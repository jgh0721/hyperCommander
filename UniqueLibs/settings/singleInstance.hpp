#pragma once
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QLockFile>
#include <QObject>
#include <QTimer>
#include <functional>
#include <memory>

namespace hc::settings {
// A process lock owns the endpoint. Never remove a server while another process
// holds the same lock, including the interval before it enters its event loop.
class SingleInstance final : public QObject {
public:
    enum class Result { Primary, Forwarded, Failed };
    explicit SingleInstance(QObject* Parent = nullptr) : QObject(Parent), server_(this) {}
    ~SingleInstance() override { server_.close(); lock_.reset(); }
    static QString Key(const QString& Directory) {
        const auto Canonical = QFileInfo(Directory).canonicalFilePath();
        const auto Identity = (Canonical.isEmpty() ? QFileInfo(Directory).absoluteFilePath() : Canonical).toCaseFolded()
            + '|' + qEnvironmentVariable("USERDOMAIN") + '|' + qEnvironmentVariable("USERNAME")
            + '|' + qEnvironmentVariable("SESSIONNAME");
        return "HyperCommander-" + QString::fromLatin1(QCryptographicHash::hash(Identity.toUtf8(), QCryptographicHash::Sha256).toHex());
    }
    static QJsonArray Paths(const QStringList& Arguments) {
        QJsonArray Paths; QString Side = "active";
        for (int Index = 1; Index < Arguments.size() && Paths.size() < 64; ++Index) {
            auto Arg = Arguments[Index];
            if (Arg == "--config-dir") { ++Index; continue; }
            if (Arg == "--portable" || Arg.startsWith("--config-dir=")) continue;
            if (Arg == "--left" || Arg == "--right") {
                Side = Arg.mid(2); if (++Index >= Arguments.size()) break; Arg = Arguments[Index];
            } else if (Arg.startsWith("--left=") || Arg.startsWith("--right=")) {
                const int At = Arg.indexOf('='); Side = Arg.mid(2, At - 2); Arg = Arg.mid(At + 1);
            } else if (Arg.startsWith('-')) continue;
            if (Arg.isEmpty()) continue;
            Paths.append(QJsonObject{{"side", Side}, {"path", QDir::cleanPath(QFileInfo(Arg).absoluteFilePath())}});
            Side = "active";
        }
        return Paths;
    }
    Result Start(const QString& Directory, const QJsonArray& Paths, std::function<bool(const QJsonArray&)> Receive) {
        const auto Name = Key(Directory);
        lock_ = std::make_unique<QLockFile>(QDir::temp().filePath(Name + ".lock"));
        lock_->setStaleLockTime(0);
        if (!lock_->tryLock(0)) {
            // Concurrent launches may arrive before listen() or before the first
            // window exists. ACK only after the receiving UI accepts all paths.
            QElapsedTimer Deadline; Deadline.start();
            while (Deadline.elapsed() < 5000) {
                QLocalSocket Socket; Socket.connectToServer(Name);
                if (!Socket.waitForConnected(100)) continue;
                Socket.write(QJsonDocument(QJsonObject{{"version", 1}, {"paths", Paths}}).toJson(QJsonDocument::Compact) + '\n');
                if (Socket.bytesToWrite() && !Socket.waitForBytesWritten(1000)) break;
                QByteArray Ack = Socket.readAll();
                while (!Ack.contains('\n') && Deadline.elapsed() < 5000) {
                    if (!Socket.waitForReadyRead(int(5000 - Deadline.elapsed()))) break;
                    Ack += Socket.readAll();
                }
                if (Ack.startsWith("OK\n")) return Result::Forwarded;
                error_ = "기존 창이 경로를 받지 못했습니다. 기존 창의 대화상자나 탭 수를 확인하세요.";
                return Result::Failed;
            }
            error_ = "기존 창에 연결하지 못했습니다. 시작 또는 종료 중인지 확인한 뒤 다시 실행하세요.";
            return Result::Failed;
        }
        QLocalServer::removeServer(Name);
        server_.setSocketOptions(QLocalServer::UserAccessOption);
        if (!server_.listen(Name)) { error_ = server_.errorString(); lock_.reset(); return Result::Failed; }
        connect(&server_, &QLocalServer::newConnection, this, [this, Receive] {
            while (auto* Socket = server_.nextPendingConnection()) {
                Socket->setParent(this);
                auto Bytes = std::make_shared<QByteArray>();
                QTimer::singleShot(5000, Socket, [Socket] { Socket->disconnectFromServer(); Socket->deleteLater(); });
                connect(Socket, &QLocalSocket::disconnected, Socket, &QObject::deleteLater);
                connect(Socket, &QLocalSocket::readyRead, Socket, [Socket, Bytes, Receive] {
                    *Bytes += Socket->readAll();
                    if (Bytes->size() > 262144) { Socket->disconnectFromServer(); return; }
                    if (!Bytes->endsWith('\n')) return;
                    Socket->blockSignals(true);
                    const auto Object = QJsonDocument::fromJson(*Bytes).object();
                    bool Valid = Object["version"] == QJsonValue(1) && Object["paths"].isArray() && Object["paths"].toArray().size() <= 64;
                    for (const auto& Value : Object["paths"].toArray()) {
                        const auto Path = Value.toObject();
                        Valid = Valid && Path["path"].isString() && QDir::isAbsolutePath(Path["path"].toString())
                            && QStringList{"active", "left", "right"}.contains(Path["side"].toString());
                    }
                    const bool Accepted = Valid && Receive(Object["paths"].toArray());
                    Socket->write(Accepted ? "OK\n" : "ERROR\n"); Socket->flush(); Socket->disconnectFromServer(); Socket->deleteLater();
                });
            }
        });
        return Result::Primary;
    }
    QString Error() const { return error_; }
private:
    QLocalServer server_;
    std::unique_ptr<QLockFile> lock_;
    QString error_;
};
} // namespace hc::settings
