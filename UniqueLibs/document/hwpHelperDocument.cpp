#include "hwpStartupTrace.hpp"
#include "hwpHelperDocument.hpp"
#include "hwpInputInspection.hpp"
#include <QWebEngineView>
#include <QWebEnginePage>
#include <QJsonArray>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
namespace hc::document
{
    HwpHelperDocument::HwpHelperDocument(std::shared_ptr<const HwpBundle> Bundle,
        std::uintptr_t ParentWindow, int LoadTimeoutMs, bool AllowEdit)
        : bundle_(std::move(Bundle)), input_(bool(bundle_)), allowEdit_(AllowEdit), parent_(ParentWindow), timeoutMs_(LoadTimeoutMs) {}
    HwpHelperDocument::~HwpHelperDocument()
    {
        // QObject disconnects receivers only in its base destructor, after
        // derived members have died. Endpoint teardown can emit Failed while
        // the bridge is loading, so disconnect while our state is still alive.
        if(view_) {
            disconnect(view_->LoadBridge(), nullptr, this, nullptr);
            disconnect(view_->View()->page(), nullptr, this, nullptr);
            view_.reset();
        }
    }
    void HwpHelperDocument::fail(const QString& Code)
    {
        state_ = "failed"; error_ = Code;
        // The native owner will terminate the entire Job. No local Retire ACK.
    }
    QJsonObject HwpHelperDocument::status() const
    {
        QJsonObject Out{{"type", "DocumentStatus"}, {"version", 1}, {"identity", identity_},
            {"state", state_}, {"error", error_}};
        if (editing_) Out["editor"] = editor_.State();
        return Out;
    }
    void HwpHelperDocument::Refresh()
    {
        input_.Expire();
        if(!view_ || !parent_ || state_ == "failed") return;
        const auto Parent = reinterpret_cast<HWND>(parent_);
        const auto Child = reinterpret_cast<HWND>(child_);
        RECT Rect{};
        if(!IsWindow(Parent) || !IsWindow(Child) || GetParent(Child) != Parent || !GetClientRect(Parent, &Rect)) {
            fail("rhwp-renderer-surface"); return;
        }
        // Win32 client extents are physical pixels; QWidget/QWindow geometry is
        // logical. A foreign-parented top-level widget also needs an explicit
        // origin, otherwise show() can retain its former desktop position.
        const auto Dpr = view_->devicePixelRatioF();
        const QRect Geometry(0, 0, qRound(Rect.right / Dpr), qRound(Rect.bottom / Dpr));
        if(view_->geometry() != Geometry) view_->setGeometry(Geometry);
        if(view_->windowHandle()->geometry() != Geometry) view_->windowHandle()->setGeometry(Geometry);
    }
    QJsonObject HwpHelperDocument::Handle(const QJsonObject& Request)
    {
        const auto Reject = [](const char* Code) { return QJsonObject{{"version", 1}, {"error", Code}}; };
        if(!bundle_) return Reject("rhwp-input-package-not-approved");
        const auto Type = Request["type"].toString();
        const auto PrintReply = [this] {
            QJsonObject Out{{"printIdle",print_.Idle()},{"printRetired",print_.Retired()}};
            if(print_.TakeAction())Out["action"]="print";
            const auto Message=print_.TakeMessage();if(!Message.isEmpty())Out["printMessage"]=Message;
            return Out;
        };
        if(Type.startsWith("DocumentPrint")) {
            if(state_!="ready"||Request["version"]!=QJsonValue(1)||Request["identity"]!=QJsonValue(identity_))return Reject("rhwp-print-unavailable");
            if(Type=="DocumentPrintPoll"&&Request.size()==3)return PrintReply();
            if(Type=="DocumentPrintCancel"&&Request.size()==4){const auto Ok=print_.Cancel(Request["printIdentity"].toObject());return QJsonObject{{"ok",Ok},{"printIdle",print_.Idle()},{"printRetired",print_.Retired()}};}
            if(Type=="DocumentPrint"&&Request.size()==5) {
                const auto Identity=Request["printIdentity"].toObject();
                const auto Model=editor_.Model();
                if(!editing_&&(Identity["changeSequence"]!="0"||Identity["savepoint"]!=QString::fromLatin1(inputSha_)))return Reject("rhwp-print-stale-model");
                if(editing_&&(Identity["changeSequence"]!=QString::number(Model.changeSequence_)||Identity["savepoint"].toString().toStdString()!=Model.savepoint_))return Reject("rhwp-print-stale-model");
                return {{"ok",print_.Begin(Identity,Request["pages"].toArray())}};
            }
            return Reject("rhwp-print-schema");
        }
        if(Type.startsWith("Input")) {
            auto Reply = input_.Handle(Request);
            if(Reply["type"] == QJsonValue("InputAccepted")) {
                startup::Data().SetBinding(Reply["identity"].toObject());startup::Data().Begin("helper.inputReceive");
            }
            if(Reply["type"] == QJsonValue("InputVerified")) startup::Data().EndNamed("helper.inputReceive");
            if(Reply["type"] == QJsonValue("InputVerified")) identity_ = Reply["identity"].toObject();
            return Reply;
        }
        if (Type == "DocumentEditorExport" || Type == "DocumentEditorPoll" || Type == "DocumentEditorSaved" || Type == "DocumentEditorDiscard" || Type == "DocumentEditorSynchronize" || Type == "DocumentEditorResume") {
            if (!editing_ || state_ != "ready" || Request["version"] != QJsonValue(1)
                || Request["identity"] != QJsonValue(identity_)) return Reject("rhwp-editor-unavailable");
            if (Type == "DocumentEditorSynchronize" && Request.size() == 4)
                return {{"ok", editor_.Synchronize(Request["id"].toString())}};
            if (Type == "DocumentEditorResume" && Request.size() == 3) return {{"ok", editor_.Resume()}};
            if (Type == "DocumentEditorDiscard" && Request.size() == 3) { editor_.DiscardExport(); return {{"ok", true}}; }
            if (Type == "DocumentEditorExport" && Request.size() == 5)
                return {{"ok", editor_.BeginExport(Request["exportIdentity"].toObject(), Request["format"].toString())}};
            if (Type == "DocumentEditorSaved" && Request.size() == 5) {
                auto Model = editor_.Model();
                if (Request["sequence"] != QString::number(Model.changeSequence_)
                    || Request["savepoint"].toString().toStdString() != Model.savepoint_) return Reject("rhwp-editor-stale-save");
                return {{"ok", editor_.NotifySaved(Model)}};
            }
            if (Type == "DocumentEditorPoll" && Request.size() == 3) {
                QJsonObject Out=PrintReply(); Out["editor"]=editor_.State();
                const auto Action = editor_.TakeUserAction(); if (!Action.isEmpty()) Out["action"] = Action;
                if (auto Message = editor_.TakeExportMessage()) Out["message"] = *Message;
                return Out;
            }
            return Reject("rhwp-editor-schema");
        }
        if(Request.size() != 3 || Request["version"] != QJsonValue(1) || !Request["identity"].isObject())
            return Reject("rhwp-document-schema");
        if(identity_.isEmpty() || Request["identity"].toObject() != identity_) return Reject("rhwp-document-stale");
        if(Type == "DocumentStatus") {
            Refresh();
            if(state_ == "loading" && view_ && view_->LoadBridge()->CurrentState() == HwpLoadBridge::State::Parsed)
                state_ = "parsed";
            return status();
        }
        if((Type != "DocumentStart" && Type != "DocumentStartEdit") || state_ != "idle") return Reject("rhwp-document-state");
        startup::Scope Start("helper.documentStart");
        auto Input = input_.Take();
        if(!Input) return Reject("rhwp-document-input");
        inputSha_=Input->sha256_;
        QString Error;
        if (Type == "DocumentStartEdit") {
            const auto Inspection = InspectHwpInput(Input->bytes_, Input->format_);
            DocumentOperationTicket Load;
            Load.documentId_ = identity_["documentId"].toString().toStdString();
            Load.generation_ = identity_["generation"].toString().toULongLong();
            Load.token_ = identity_["token"].toString().toULongLong();
            Load.operation_ = DocumentOperation::Loading;
            if (!allowEdit_ || !Inspection.accepted_ || Inspection.restricted_
                || !editor_.Activate(Load, identity_["bindingOwner"].toString().toULongLong(),
                    identity_["bindingGeneration"].toString().toULongLong(), Input->sha256_, true)) {
                fail("rhwp-edit-input-denied"); return status();
            }
            editing_ = true;
        }
        print_.Activate({{"documentId",identity_["documentId"]},{"generation",identity_["generation"]},
            {"bindingOwner",identity_["bindingOwner"]},{"bindingGeneration",identity_["bindingGeneration"]},{"epoch",identity_["epoch"]}});
        view_ = HwpBundleView::Create(bundle_, Error, editing_ ? &editor_ : nullptr, &print_);
        if(!view_) { fail(Error); return status(); }
        auto* Bridge = view_->LoadBridge();
        connect(Bridge, &HwpLoadBridge::Ready, this, [this] { if(state_ != "failed") state_ = "ready"; });
        connect(Bridge, &HwpLoadBridge::ViewFailed, this, [this](const QString& Code) { fail(Code); });
        connect(Bridge, &HwpLoadBridge::Failed, this, [this](const QString& Code) { fail(Code); });
        // Bridge load failure is intentionally terminal before Ready; process
        // health after Ready also belongs to this native controller.
        connect(view_->View()->page(), &QWebEnginePage::renderProcessTerminated, this,
            [this] { fail("rhwp-renderer-load"); });
        connect(view_->View()->page(), &QWebEnginePage::loadFinished, this,
            [this](bool Ok) { if(!Ok) fail("rhwp-renderer-load"); });
        HwpNativeLoad Load{identity_["documentId"].toString(),
            identity_["generation"].toString().toULongLong(), identity_["bindingOwner"].toString().toULongLong(),
            identity_["bindingGeneration"].toString().toULongLong(), identity_["epoch"].toString().toULongLong(),
            Input->format_, Input->sha256_};
        state_ = "loading";
        if(!Bridge->Begin(Load, std::move(Input->bytes_), Error, timeoutMs_, !editing_)) { fail(Error); return status(); }
        view_->setWindowFlags(Qt::Window | Qt::FramelessWindowHint);
        if(parent_) {
            const auto Parent = reinterpret_cast<HWND>(parent_);
            DWORD Pid = 0; GetWindowThreadProcessId(Parent, &Pid);
            if(Pid != GetCurrentProcessId()) { fail("rhwp-renderer-surface"); return status(); }
            // Keep Qt's native parent model in sync. Raw SetParent alone can
            // be undone by QWidget::show when it restores its QWindow state.
            parentWindow_.reset(QWindow::fromWinId(static_cast<WId>(parent_)));
            if(!parentWindow_) { fail("rhwp-renderer-surface"); return status(); }
            child_ = static_cast<std::uintptr_t>(view_->winId());
            view_->windowHandle()->setParent(parentWindow_.get());
            Refresh();
            ShowWindow(Parent, SW_SHOWNOACTIVATE);
        } else view_->resize(900, 800); // Standalone native tests.
        if(state_ != "failed") { view_->show(); Refresh(); }
        return status();
    }
}
