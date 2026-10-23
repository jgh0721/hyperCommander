#include "stdafx.h"
#include "guiRequestServer.hpp"
#include "compareWindow.hpp"
#include "runtime/guiIpc.hpp"
#include "runtime/decisionPlan.hpp"
#include <QFutureWatcher>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QTimer>
#include <QtConcurrent/QtConcurrentRun>
#include <Windows.h>

namespace hc::compare
{
    GuiRequestServer::GuiRequestServer(QObject* Parent) : QObject(Parent)
    {
        server_.setSocketOptions(QLocalServer::UserAccessOption); server_.setMaxPendingConnections(8);
        connect(&server_, &QLocalServer::newConnection, this, &GuiRequestServer::accept);
    }
    GuiRequestServer::~GuiRequestServer() { server_.close(); if (lease_) CloseHandle(lease_); }
    bool GuiRequestServer::Listen(const QString& Endpoint)
    {
        const auto endpoint = Endpoint.isEmpty() ? GuiEndpoint() : Endpoint;
        lease_ = CreateMutexW(nullptr, FALSE, reinterpret_cast<const wchar_t*>(("Local\\" + endpoint).utf16()));
        if (!lease_) return false;
        if (GetLastError() == ERROR_ALREADY_EXISTS || !server_.listen(endpoint)) { CloseHandle(lease_); lease_ = nullptr; return false; }
        return true;
    }
    void GuiRequestServer::accept()
    {
        while (auto* socket = server_.nextPendingConnection())
        {
            socket->setParent(this); socket->setReadBufferSize(guiMessageLimit + 1);
            windows_.removeIf([](const auto& Window) { return Window.isNull(); });
            if (connections_ >= 8 || parsing_ + windows_.size() >= 16 || !GuiPeerAllowed(*socket, false))
            { socket->abort(); socket->deleteLater(); continue; }
            ++connections_;
            struct State { QByteArray pending_; QString token_; bool submitted_ = false, wait_ = false; QPointer<CompareWindow> window_; };
            auto state = std::make_shared<State>();
            connect(socket, &QLocalSocket::disconnected, this, [this, socket, state] {
                --connections_; if (state->wait_ && state->window_) state->window_->CancelRemoteRequest(); socket->deleteLater();
            });
            const auto reject = [socket, state](const QString& Error) {
                if (socket->state() != QLocalSocket::ConnectedState) return;
                socket->write(GuiFrame({{"version", 1}, {"token", state->token_}, {"kind", "error"}, {"error", Error.left(512)}})); socket->disconnectFromServer();
            };
            QTimer::singleShot(5000, socket, [socket, state] { if (!state->submitted_) socket->abort(); });
            connect(socket, &QLocalSocket::readyRead, this, [this, socket, state, reject] {
                if (state->submitted_) { socket->abort(); return; }
                state->pending_ += socket->readAll();
                if (state->pending_.size() > guiMessageLimit) { socket->abort(); return; }
                const auto end = state->pending_.indexOf('\n'); if (end < 0) return;
                state->submitted_ = true;
                try
                {
                    windows_.removeIf([](const auto& Window) { return Window.isNull(); });
                    if (parsing_ + windows_.size() >= 16) throw Failure("GuiRequestLimit");
                    if (end != state->pending_.size() - 1) throw Failure("GuiProtocolMismatch");
                    const auto message = ReadCompareJson(state->pending_.left(end), {}); state->pending_.clear();
                    if (message.size() != 4 || message["version"] != 1 || !message["wait"].isBool() || !message["arguments"].isArray()) throw Failure("GuiProtocolMismatch");
                    state->token_ = message["token"].toString();
                    if (!QRegularExpression("^[a-f0-9]{32}$").match(state->token_).hasMatch()) throw Failure("GuiProtocolMismatch");
                    state->wait_ = message["wait"].toBool(); const auto array = message["arguments"].toArray();
                    if (array.isEmpty() || array.size() > 128) throw Failure("GuiArgumentLimit");
                    QStringList args; for (const auto& item : array) { if (!item.isString() || item.toString().contains(QChar(0))) throw Failure("GuiProtocolMismatch"); args << item.toString(); }
                    auto* watcher = new QFutureWatcher<Parsed>(this); ++parsing_;
                    const QPointer<QLocalSocket> peer(socket);
                    connect(watcher, &QFutureWatcher<Parsed>::finished, this, [this, watcher, peer, state] {
                        --parsing_; const auto parsed = watcher->result(); watcher->deleteLater(); if (!peer || peer->state() != QLocalSocket::ConnectedState) return;
                        auto request = parsed.request_; QString error = parsed.error_;
                        if (error.isEmpty() && (request.headless_ || request.help_ || request.version_ || request.reuse_ ||
                            !QStringList{"auto", "compare", "merge", "directory", "directory-merge"}.contains(request.mode_))) error = "GuiModeUnsupported";
                        if (!error.isEmpty()) { peer->write(GuiFrame({{"version", 1}, {"token", state->token_}, {"kind", "error"}, {"error", error.left(512)}})); peer->disconnectFromServer(); return; }
                        auto* window = new CompareWindow(request); window->setAttribute(Qt::WA_DeleteOnClose); state->window_ = window; windows_.append(window);
                        window->BindRequest([peer, state](int Code) {
                            state->window_.clear();
                            if (state->wait_ && peer && peer->state() == QLocalSocket::ConnectedState)
                            {
                                peer->write(GuiFrame({{"version", 1}, {"token", state->token_}, {"kind", "result"}, {"exitCode", Code}}));
                                peer->flush(); if (peer->bytesToWrite()) peer->waitForBytesWritten(200); peer->disconnectFromServer();
                            }
                        });
                        peer->write(GuiFrame({{"version", 1}, {"token", state->token_}, {"kind", "accepted"}})); peer->flush();
                        window->show(); window->raise(); window->activateWindow();
                    });
                    watcher->setFuture(QtConcurrent::run([args] { return Parse(args, false); }));
                }
                catch (const std::exception& error) { reject(QString::fromUtf8(error.what())); }
            });
            if (socket->bytesAvailable()) QMetaObject::invokeMethod(socket, "readyRead", Qt::QueuedConnection);
        }
    }
}
