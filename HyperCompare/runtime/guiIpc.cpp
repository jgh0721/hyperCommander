#include "stdafx.h"
#include "guiIpc.hpp"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonDocument>
#include <QProcess>
#include <QThread>
#include <QUuid>
#include <Windows.h>
#include <sddl.h>

namespace hc::compare
{
    namespace
    {
        QByteArray securityIdentity(HANDLE Process)
        {
            HANDLE token = nullptr; if (!OpenProcessToken(Process, TOKEN_QUERY, &token)) return {};
            DWORD size = 0; GetTokenInformation(token, TokenUser, nullptr, 0, &size);
            std::vector<std::byte> user(size); TOKEN_STATISTICS stats{}; TOKEN_ELEVATION elevation{}; DWORD session = 0;
            const bool ok = size && GetTokenInformation(token, TokenUser, user.data(), size, &size) &&
                GetTokenInformation(token, TokenStatistics, &stats, sizeof(stats), &size) &&
                GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size) &&
                GetTokenInformation(token, TokenSessionId, &session, sizeof(session), &size);
            CloseHandle(token); if (!ok) return {};
            wchar_t* sid = nullptr; if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid, &sid)) return {};
            auto result = QString::fromWCharArray(sid).toUtf8(); LocalFree(sid);
            return result + ':' + QByteArray::number(stats.AuthenticationId.HighPart) + ':' + QByteArray::number(stats.AuthenticationId.LowPart)
                + ':' + QByteArray::number(elevation.TokenIsElevated) + ':' + QByteArray::number(session);
        }
        QString executable() { return QCoreApplication::applicationDirPath() + "/HyperCompare.exe"; }
    }
    QString GuiEndpoint()
    {
        const auto identity = securityIdentity(GetCurrentProcess()); if (identity.isEmpty()) throw Failure("GuiIdentityUnavailable");
        return "HyperCompare.gui.v1." + QString::fromLatin1(QCryptographicHash::hash(
            QFileInfo(executable()).absoluteFilePath().toCaseFolded().toUtf8() + ':' + identity, QCryptographicHash::Sha256).toHex());
    }
    bool GuiPeerAllowed(QLocalSocket& Socket, bool ServerPeer)
    {
        ULONG pid = 0; const auto pipe = reinterpret_cast<HANDLE>(Socket.socketDescriptor());
        if (!(ServerPeer ? GetNamedPipeServerProcessId(pipe, &pid) : GetNamedPipeClientProcessId(pipe, &pid))) return false;
        const auto process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid); if (!process) return false;
        const auto own = securityIdentity(GetCurrentProcess()), peer = securityIdentity(process);
        bool valid = !own.isEmpty() && own == peer;
        if (valid && ServerPeer)
        {
            wchar_t path[32768]{}; DWORD count = DWORD(std::size(path));
            valid = QueryFullProcessImageNameW(process, 0, path, &count) &&
                QFileInfo(QString::fromWCharArray(path, count)).absoluteFilePath().compare(QFileInfo(executable()).absoluteFilePath(), Qt::CaseInsensitive) == 0;
        }
        CloseHandle(process); return valid;
    }
    QByteArray GuiFrame(const QJsonObject& Value)
    {
        auto bytes = QJsonDocument(Value).toJson(QJsonDocument::Compact) + '\n';
        if (bytes.size() > guiMessageLimit) throw Failure("GuiMessageLimit"); return bytes;
    }
    int SubmitGuiRequest(const QStringList& Arguments, const Request& Input,
        const std::shared_ptr<std::atomic_bool>& Canceled, QString& Error)
    {
        try
        {
            QStringList args{"HyperCompare"};
            const QStringList paths{"file1", "file2", "file3", "base", "output", "request"};
            for (int i = 1; i < Arguments.size(); ++i)
            {
                auto name = Arguments[i]; while (name.startsWith('-')) name.remove(0, 1);
                const auto equal = name.indexOf('='); const auto value = equal < 0 ? QString{} : name.mid(equal + 1); if (equal >= 0) name.truncate(equal);
                if (name == "reuse" || name == "wait" || name == "exitcode") continue;
                if (name == "wait-timeout-ms") { if (equal < 0) ++i; continue; }
                if (paths.contains(name) && equal >= 0) { args << '-' + name << QFileInfo(value).absoluteFilePath(); continue; }
                args << Arguments[i];
                if (paths.contains(name) && i + 1 < Arguments.size()) args << QFileInfo(Arguments[++i]).absoluteFilePath();
            }
            if (!args.contains("-gui") && !args.contains("--gui")) args << "-gui";
            const auto token = QUuid::createUuid().toString(QUuid::Id128);
            const auto frame = GuiFrame({{"version", 1}, {"token", token}, {"wait", Input.wait_}, {"arguments", QJsonArray::fromStringList(args)}});
            QLocalSocket socket; socket.setReadBufferSize(guiMessageLimit + 1); const auto endpoint = GuiEndpoint();
            QElapsedTimer timer; timer.start(); bool launched = false;
            while (socket.state() != QLocalSocket::ConnectedState)
            {
                if (Canceled->load()) return 3;
                socket.abort(); socket.connectToServer(endpoint);
                if (socket.state() == QLocalSocket::ConnectedState || socket.waitForConnected(100)) break;
                if (socket.error() == QLocalSocket::SocketAccessError) throw Failure("GuiAccessDenied");
                if (!launched)
                {
                    if (!QProcess::startDetached(executable(), {"-internal-gui-host"})) throw Failure("GuiLaunchFailed");
                    launched = true;
                }
                if (timer.elapsed() > 5000) throw Failure("GuiConnectTimeout");
                QThread::msleep(25);
            }
            if (!GuiPeerAllowed(socket, true)) throw Failure("GuiPeerRejected");
            if (socket.write(frame) != frame.size()) throw Failure("GuiWriteFailed");
            socket.flush(); timer.restart(); QByteArray pending; bool accepted = false;
            for (;;)
            {
                if (Canceled->load()) { socket.abort(); return 3; }
                if ((!accepted && timer.elapsed() > 15000) || (Input.guiWaitMs_ && timer.elapsed() > Input.guiWaitMs_)) throw Failure("GuiWaitTimeout");
                pending += socket.readAll(); if (pending.size() > guiMessageLimit) throw Failure("GuiMessageLimit");
                for (;;)
                {
                    const auto end = pending.indexOf('\n'); if (end < 0) break;
                    QJsonParseError parse; const auto object = QJsonDocument::fromJson(pending.left(end), &parse).object(); pending.remove(0, end + 1);
                    if (parse.error != QJsonParseError::NoError || object["version"] != 1 || object["token"] != token) throw Failure("GuiProtocolMismatch");
                    const auto kind = object["kind"].toString();
                    if (kind == "accepted" && !accepted) { accepted = true; if (!Input.wait_) return 0; }
                    else if (kind == "result" && accepted) { const int code = object["exitCode"].toInt(-1); if (code < 0 || code > 3) throw Failure("GuiProtocolMismatch"); return code; }
                    else if (kind == "error") throw Failure(object["error"].toString("GuiRejected").toStdString());
                    else throw Failure("GuiProtocolMismatch");
                }
                if (socket.state() == QLocalSocket::UnconnectedState) throw Failure("GuiDisconnected");
                socket.waitForReadyRead(50);
            }
        }
        catch (const std::exception& failure) { Error = QString::fromUtf8(failure.what()); return 2; }
    }
}
