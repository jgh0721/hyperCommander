#include "viewerHostBridge.hpp"
#include "UniqueLibs/document/documentWorkspaceWidget.hpp"
#include "UniqueLibs/document/documentThemeAdapter.hpp"
#include "UniqueLibs/document/hwpHostSession.hpp"
#include "hwpProductPolicy.hpp"
#include "UniqueLibs/settings/appSettings.hpp"
#include <QApplication>
#include <QCloseEvent>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QFileInfo>
#include <QStandardPaths>
#include <QTimer>
#include <qt_windows.h>

namespace hc::document
{
    namespace
    {
        class ViewerWindow final : public DocumentWorkspaceWidget
        {
        public:
            explicit ViewerWindow(const QString& Root) : DocumentWorkspaceWidget(Root, true) {}
        protected:
            void closeEvent(QCloseEvent* Event) override
            {
                if (RequestViewerClose()) hide();
                Event->ignore();
            }
        };
    }

    struct ViewerHostBridge::State
    {
        std::unique_ptr<ViewerWindow> window_;
        std::unique_ptr<HwpHostSession> rhwp_;
        HANDLE abort_ = nullptr;
        QTimer* abortTimer_ = nullptr;
        bool failed_ = false;
    };

    ViewerHostBridge::ViewerHostBridge(HWND) : state_(std::make_unique<State>()) {}
    ViewerHostBridge::~ViewerHostBridge() { Stop(); }

    void ViewerHostBridge::Start()
    {
        if (!qobject_cast<QApplication*>(QCoreApplication::instance())) { state_->failed_ = true; return; }
        QApplication::setOrganizationName("HyperCommander");
        QApplication::setApplicationName("HyperViewer");
        const auto ConfigDirectory = qEnvironmentVariable("HC_DOCUMENT_CONFIG_DIR");
        if (!ConfigDirectory.isEmpty()) qApp->setProperty("documentViewsConfigDir", ConfigDirectory);
        const auto Root = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/recovery/viewer";
        state_->window_ = std::make_unique<ViewerWindow>(Root);
        state_->rhwp_ = std::make_unique<HwpHostSession>(
            QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/rhwp",
            MakeProductHwpRuntime(QCoreApplication::applicationDirPath(), QByteArray(HC_RHWP_APPROVED_INVENTORY_SHA256)));
        state_->window_->winId();
        QObject::connect(state_->window_.get(), &DocumentWorkspaceWidget::StateChanged,
            state_->window_.get(), [this] {
                if (state_->window_->Snapshot()["closeRequested"].toBool()) state_->window_->hide();
            });
        const auto Config = ConfigDirectory.isEmpty() ? hc::settings::DocumentPaths()
            : hc::settings::ConfigPaths::Resolve({"HyperViewer", "--config-dir", ConfigDirectory},
                QCoreApplication::applicationDirPath(), {}, true);
        if (!Config.error_.isEmpty()) state_->window_->ShowShellError(Config.error_);
        else {
            hc::settings::JsonStore Settings(hc::settings::UnifiedConfig::FileName(Config.directory_),
                {{"schemaVersion", 2}}, nullptr, hc::settings::JsonStore::Scope::Product);
            const auto Appearance = Settings.Object()["appearance"].toObject();
            if (Appearance.contains("documentWorkspace"))
                state_->window_->ApplyChromeSettings(Appearance["documentWorkspace"].toObject());
        }
        JOBOBJECT_BASIC_UI_RESTRICTIONS Restrictions{};
        if (QueryInformationJobObject(nullptr, JobObjectBasicUIRestrictions, &Restrictions, sizeof(Restrictions), nullptr))
            state_->window_->SetClipboardBroker((Restrictions.UIRestrictionsClass
                & (JOB_OBJECT_UILIMIT_READCLIPBOARD | JOB_OBJECT_UILIMIT_WRITECLIPBOARD)) != 0);
    }

    void ViewerHostBridge::Stop()
    {
        if (state_->abortTimer_) { delete state_->abortTimer_; state_->abortTimer_ = nullptr; }
        if (state_->abort_) { CloseHandle(state_->abort_); state_->abort_ = nullptr; }
        if (state_->rhwp_) { state_->rhwp_->Shutdown(); state_->rhwp_.reset(); }
        if (state_->window_) { state_->window_->Checkpoint(); state_->window_.reset(); }
    }

    bool ViewerHostBridge::Load(const wchar_t* Path)
    {
        if (!state_->window_ || !Path) return false;
        const auto LocalPath = QString::fromWCharArray(Path);
        const auto Suffix = QFileInfo(LocalPath).suffix().toLower();
        if (Suffix == "hwp" || Suffix == "hwpx") {
#ifdef HC_RHWP_HOST_RUNTIME
            return state_->rhwp_ && !state_->rhwp_->OpenHost(LocalPath).contains("error");
#else
            return false;
#endif
        }
        const auto Result = state_->rhwp_->Dispatch({{"action", "openHost"}, {"path", LocalPath}},
            [this](const QJsonObject& Request) { return state_->window_->Handle(Request); });
        return !Result.contains("error");
    }

    void ViewerHostBridge::Focus()
    {
        if (state_->window_ && state_->window_->isVisible()) state_->window_->Handle({{"action", "focus"}});
    }

    void ViewerHostBridge::SetAbort(HANDLE Event)
    {
        if (state_->abortTimer_) { delete state_->abortTimer_; state_->abortTimer_ = nullptr; }
        if (state_->abort_) { CloseHandle(state_->abort_); state_->abort_ = nullptr; }
        if (!Event || !state_->window_) return;
        if (!DuplicateHandle(GetCurrentProcess(), Event, GetCurrentProcess(), &state_->abort_, SYNCHRONIZE, FALSE, 0)) return;
        state_->abortTimer_ = new QTimer(state_->window_.get());
        state_->abortTimer_->setInterval(50);
        QObject::connect(state_->abortTimer_, &QTimer::timeout, state_->window_.get(), [this] {
            if (state_->abort_ && WaitForSingleObject(state_->abort_, 0) == WAIT_OBJECT_0) {
                state_->abortTimer_->stop(); state_->window_->Handle({{"action", "cancelHost"}});
                if (state_->rhwp_) state_->rhwp_->CancelHost();
            }
        });
        state_->abortTimer_->start();
    }

    bool ViewerHostBridge::Exchange(const char* Input, unsigned Size, std::string& Output)
    {
        if (!state_->window_ || !Input || !Size || Size > HostPayloadLimit) return false;
        QJsonParseError Error;
        const auto Request = QJsonDocument::fromJson(QByteArray(Input, qsizetype(Size)), &Error);
        if (Error.error != QJsonParseError::NoError || !Request.isObject()) return false;
        const auto Object = Request.object();
        if (Object["action"] == "hostRuntime" && Object.size() == 1) {
            BOOL InJob = FALSE; JOBOBJECT_EXTENDED_LIMIT_INFORMATION Limits{};
            JOBOBJECT_BASIC_UI_RESTRICTIONS Ui{};
            const bool Valid = IsProcessInJob(GetCurrentProcess(), nullptr, &InJob) && InJob
                && QueryInformationJobObject(nullptr, JobObjectExtendedLimitInformation, &Limits, sizeof(Limits), nullptr)
                && QueryInformationJobObject(nullptr, JobObjectBasicUIRestrictions, &Ui, sizeof(Ui), nullptr);
            const auto Runtime = MakeProductHwpRuntime(QCoreApplication::applicationDirPath(),
                QByteArray(HC_RHWP_APPROVED_INVENTORY_SHA256));
            const QJsonObject Reply{{"version", 1}, {"jobPolicyAvailable", Valid},
                {"processLimit", Valid ? int(Limits.BasicLimitInformation.ActiveProcessLimit) : 0},
                {"processMemoryBytes", Valid ? QString::number(Limits.ProcessMemoryLimit) : QString()},
                {"jobMemoryBytes", Valid ? QString::number(Limits.JobMemoryLimit) : QString()},
                {"killOnClose", Valid && bool(Limits.BasicLimitInformation.LimitFlags & JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE)},
                {"breakaway", Valid && bool(Limits.BasicLimitInformation.LimitFlags & (JOB_OBJECT_LIMIT_BREAKAWAY_OK | JOB_OBJECT_LIMIT_SILENT_BREAKAWAY_OK))},
                {"uiRestricted", Valid && bool(Ui.UIRestrictionsClass)},
                {"rhwpRuntimeRoot", Runtime.root_},
                {"rhwpPolicyAvailable", !Runtime.approvedInventorySha256_->isEmpty()},
                {"rhwpHostCommands", true}, {"rhwpHostEdit", false}};
            Output = QJsonDocument(Reply).toJson(QJsonDocument::Compact).toStdString();
            return true;
        }
        const auto Dispatch = [this](const QJsonObject& Command) {
            const auto Action = (Command.contains("messageType") ? Command["payload"].toObject() : Command)["action"].toString();
            if (Action == "present") {
                state_->window_->show();
                // The isolated host starts with STARTF_USESHOWWINDOW/SW_HIDE.
                // The first native show consumes that startup state.
                ShowWindow(reinterpret_cast<HWND>(state_->window_->winId()), SW_RESTORE);
                state_->window_->raise(); state_->window_->activateWindow();
                QTimer::singleShot(0, state_->window_.get(), [this] { Focus(); });
            } else if (Action == "conceal") state_->window_->hide();
            auto Reply = Command.contains("messageType") ? state_->window_->HandleMessage(Command) : state_->window_->Handle(Command);
            if (Action == "present") {
                const auto NativeWindow = QString::number(static_cast<quintptr>(state_->window_->winId()), 16);
                if (Command.contains("messageType")) {
                    auto Body = Reply["payload"].toObject();
                    if (Body["accepted"].toBool()) {
                        Body.insert("nativeWindow", NativeWindow);
                        Reply.insert("payload", Body);
                        Reply.insert("payloadLength", QJsonDocument(Body).toJson(QJsonDocument::Compact).size());
                    }
                } else if (Reply["accepted"].toBool()) Reply.insert("nativeWindow", NativeWindow);
            }
            return Reply;
        };
        const auto Reply = state_->rhwp_ ? state_->rhwp_->Dispatch(Object, Dispatch) : Dispatch(Object);
        Output = QJsonDocument(Reply).toJson(QJsonDocument::Compact).toStdString();
        return true;
    }

    HWND ViewerHostBridge::Child() const { return nullptr; }
    bool ViewerHostBridge::Failed() const { return state_->failed_; }
    std::string ViewerHostBridge::Diagnostic() const { return R"({"error":"viewer-not-ready"})"; }
}
