#include "hwpHostSession.hpp"
#include <QFileInfo>
#include <QUuid>
#include <QTimer>
#include <utility>
namespace hc::document
{
    HwpHostSession::HwpHostSession(QString CacheRoot, WebProcessRuntime Runtime,
        HwpOpenWindow::BackendFactory Factory, QObject* Parent)
        : QObject(Parent), cacheRoot_(std::move(CacheRoot)), runtime_(std::move(Runtime)), factory_(std::move(Factory)) {}
    HwpHostSession::~HwpHostSession() { delete window_.data(); }
    QJsonObject HwpHostSession::snapshot() const
    {
        QString State = documentId_.isEmpty() ? "idle" : "closed", Error = terminalError_;
        bool View = false, Print = false;
        if (window_) {
            const bool Closing = closing_ || window_->IsClosing();
            State = Closing ? "closing" : "opening";
            Error = window_->Error();
            if (!Closing && !Error.isEmpty()) State = "failed";
            if (auto* Document = window_->Document()) {
                const auto Value = Document->Snapshot();
                if (Value.error_) Error = QString::fromStdString(Value.error_->code_);
                if (!Closing) {
                    if (Value.lifecycle_ == DocumentLifecycle::Ready) State = "ready";
                    else if (Value.lifecycle_ == DocumentLifecycle::Failed) State = "failed";
                }
                View = !Closing && Document->Capabilities().view_;
                Print = !Closing && Document->Capabilities().print_;
            }
        }
        return {{"version", 1}, {"documentId", documentId_}, {"state", State}, {"error", Error},
            {"capabilities", QJsonObject{{"view", View}, {"edit", false}, {"save", false}, {"print", Print}}}};
    }
    QJsonObject HwpHostSession::Handle(const QJsonObject& Request)
    {
        const auto Fail = [](const char* Error) { return QJsonObject{{"version", 1}, {"error", Error}}; };
        if (Request.value("version") != QJsonValue(1) || !Request.value("action").isString()) return Fail("rhwp-host-schema");
        const auto Action = Request.value("action").toString();
        if (Action == "rhwpHostState") {
            if (Request.size() != 2) return Fail("rhwp-host-schema");
            auto State = snapshot(); State["pendingOpen"] = !pendingPath_.isEmpty();
            return State;
        }
        if (Action == "rhwpOpen") {
            if (Request.size() != 3 || !Request.value("path").isString()) return Fail("rhwp-host-schema");
            if (stopping_) return Fail("rhwp-host-stopping");
            if (window_) return Fail("rhwp-host-busy");
            const auto Path = Request.value("path").toString();
            const auto Format = QFileInfo(Path).suffix().toLower();
            if (Path.contains(QChar(0)) || !QFileInfo(Path).isAbsolute() || (Format != "hwp" && Format != "hwpx")) return Fail("rhwp-host-path");
            const auto Pin = runtime_.approvedInventorySha256_.value_or(QByteArray{});
            if (!runtime_.approvedInventorySha256_ || !runtime_.sha256_.isEmpty()
                || Pin.size() != 64 || QByteArray::fromHex(Pin).toHex() != Pin) return Fail("rhwp-runtime-policy-unavailable");
            documentId_ = QUuid::createUuid().toString(QUuid::Id128); closing_ = false; terminalError_.clear(); pendingPath_.clear();
            auto* Window = new HwpOpenWindow(Path, cacheRoot_, runtime_, factory_); window_ = Window;
            connect(Window, &HwpOpenWindow::StateChanged, this, [this, Window] {
                if (window_ != Window) return;
                const auto Error = snapshot().value("error").toString();
                if (!Error.isEmpty()) terminalError_ = Error;
            });
            connect(Window, &HwpOpenWindow::Closed, this, [this, Window] {
                if (window_ == Window) { if (!Window->Error().isEmpty()) terminalError_ = Window->Error(); window_ = nullptr; closing_ = false; }
                Window->deleteLater();
                if (!pendingPath_.isEmpty()) QTimer::singleShot(0, this, [this] {
                    if (pendingPath_.isEmpty() || stopping_) return;
                    const auto Path = std::exchange(pendingPath_, {});
                    OpenHost(Path);
                });
            });
            // Closing this auxiliary SDI must not end the still-connected host.
            Window->setAttribute(Qt::WA_QuitOnClose, false);
            Window->show(); Window->Open();
            return snapshot();
        }
        if (Action != "rhwpState" && Action != "rhwpClose") return Fail("rhwp-host-command-unavailable");
        if (Request.size() != 3 || !Request.value("documentId").isString()) return Fail("rhwp-host-schema");
        if (documentId_.isEmpty() || Request.value("documentId").toString() != documentId_) return Fail("rhwp-host-stale-document");
        if (Action == "rhwpClose") CancelHost();
        return snapshot();
    }
    QJsonObject HwpHostSession::Dispatch(const QJsonObject& Request,
        const std::function<QJsonObject(const QJsonObject&)>& NativeDispatch)
    {
        const auto Action = Request.value("action").toString();
        if (Action.startsWith("rhwp")) return Handle(Request);
        const bool Legacy = !Request.contains("messageType");
        if (Legacy && Action == "openHost") {
            const auto Path = Request.value("path").toString();
            const auto Format = QFileInfo(Path).suffix().toLower();
            if (Format == "hwp" || Format == "hwpx") {
                if (Request.size() != 2) return {{"error", "rhwp-host-schema"}};
                return OpenHost(Path);
            }
        }
        if (!NativeDispatch) return {{"error", "rhwp-native-dispatch-unavailable"}};
        const QPointer<HwpHostSession> Guard(this);
        const auto Reply = NativeDispatch(Request);
        if (Guard && Legacy && (Action == "openHost" || Action == "cancelHost") && !Reply.contains("error")) Guard->CancelHost();
        return Reply;
    }
    QJsonObject HwpHostSession::OpenHost(const QString& Path)
    {
        // SDK load is latest-selection-wins, but retirement still owns the slot.
        const auto Format = QFileInfo(Path).suffix().toLower();
        if (Path.contains(QChar(0)) || !QFileInfo(Path).isAbsolute() || (Format != "hwp" && Format != "hwpx"))
            return {{"error", "rhwp-host-path"}};
        if (stopping_) return {{"error", "rhwp-host-stopping"}};
        if (window_) {
            pendingPath_ = Path; closing_ = true; window_->RequestClose();
            return {{"accepted", true}, {"state", "closing"}};
        }
        pendingPath_.clear();
        return Handle({{"action", "rhwpOpen"}, {"version", 1}, {"path", Path}});
    }
    void HwpHostSession::CancelHost()
    {
        pendingPath_.clear();
        if (window_) { closing_ = true; window_->RequestClose(); }
    }
    void HwpHostSession::Shutdown()
    {
        stopping_ = true; CancelHost();
    }
}
