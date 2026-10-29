#pragma once
#include "documentStorageAdapter.hpp"
#include "commonLibs/document/documentWorkspace.hpp"
#include "documentRecoveryStore.hpp"
#include "workspaceChromeController.hpp"
#include "hwpOpenWindow.hpp"
#include <functional>
#include <QWidget>
#include <QJsonObject>
#include <QPlainTextEdit>
#include <QTabWidget>
#include <QPointer>
#include <memory>

class QMenuBar;
class QToolBar;
class QLabel;
class QStackedWidget;
class QFileSystemWatcher;
class QShortcut;
namespace mrst { class ExternalChangeWatcher; }
namespace hc::document
{
    class DocumentStatusBar;
    class DocumentShortcutRouter;
    class DocumentSettingsDialog;
    class DocumentWorkspaceWidget : public QWidget
    {
        Q_OBJECT
    public:
        explicit DocumentWorkspaceWidget(QString RecoveryRoot, bool Hosted = false, QWidget* Parent = nullptr);
        ~DocumentWorkspaceWidget() override;
        QJsonObject Handle(const QJsonObject& Request);
        QJsonObject HandleMessage(const QJsonObject& Envelope);
        QJsonObject Snapshot() const;
        bool RequestClose();
        bool RequestViewerClose();
        bool IsIdle() const;
        bool SetTabbed(bool Enabled);
        bool ApplyChromeSettings(const QJsonObject& Settings);
        QJsonObject ChromeSettings() const;
        void ShowShellError(const QString& Message);
        void SetClosePrompt(std::function<int(const QString&)> Prompt) { closePrompt_ = std::move(Prompt); }
        void Checkpoint();
        QString RecoveryRoot() const { return recoveryRoot_; }
        void SetClipboardBroker(bool Enabled);
        void ConfigureEpub(WebProcessRuntime Runtime) { epubRuntime_ = std::move(Runtime); }
        void ConfigureHwp(WebProcessRuntime Runtime) { hwpRuntime_ = std::move(Runtime); }
    protected:
        bool eventFilter(QObject* Watched, QEvent* Event) override;
    signals:
        void StateChanged();
        void ChromeSettingsChanged(const QJsonObject& Settings);
    private:
        void refreshPreferences();
        void externalChange(const QString& Path, bool Missing);
        mrst::ExternalChangeWatcher* externalWatcher_ = nullptr;
        void showPreferences();
        QPointer<DocumentSettingsDialog> preferences_;
        QJsonObject newDocument();
        void showEpubRecovery();
        QJsonObject openEpubRecovery(const QString& Path);
        DocumentShortcutRouter* shortcutRouter_ = nullptr;
        QString settingsPath_;
        QJsonObject appliedPreferences_;
        QFileSystemWatcher* settingsWatcher_ = nullptr;
        struct Tab;
        std::shared_ptr<Tab> active() const;
        std::shared_ptr<Tab> find(const QString& Id) const;
        QJsonObject open(const QString& Path, bool HostSlot, const QString& SourceToken, bool Broker,
            const QString& RecoveryPath = {}, const QString& DisplayName = {});
        void save(const std::shared_ptr<Tab>& Tab, const QString& Destination, bool Overwrite = false);
        void load(const std::shared_ptr<Tab>& Tab, bool Discard = false, const QString& RecoveryPath = {});
        void navigateImage(const std::shared_ptr<Tab>& Tab, const QString& Path);
        void retire(std::shared_ptr<Tab> Tab, bool Discard, bool PreserveRecovery = false);
        void cleanupRecovery(const std::shared_ptr<Tab>& Tab, std::uint64_t ThroughRevision);
        QWidget* currentPane() const;
        void activate(const std::shared_ptr<Tab>& Tab);
        QJsonObject prepareSave(const std::shared_ptr<Tab>& Tab);
        void update();
        QJsonObject editHwp(const std::shared_ptr<Tab>& Tab);
        QJsonObject editEpub(const std::shared_ptr<Tab>& Tab);
        void tryFinishClose();
        void escapeViewer();
        void finishWindowClose();
        void closeTab(int Index);
        QJsonObject clipboard(const std::shared_ptr<Tab>& Tab, const QString& Mode);
        QJsonObject clipboardRequest_, pendingHostOpen_, recoveryList_, clipboardSelection_;
        QJsonObject editRequest_;
        WebProcessRuntime hwpRuntime_;
        std::optional<WebProcessRuntime> epubRuntime_;
        QTextCursor clipboardCursor_;
        bool clipboardBroker_ = false, negotiated_ = false;
        quint64 lastMessage_ = 0;
        QString recoveryRoot_, workspaceId_;
        bool hosted_, tabbed_ = true;
        DocumentWorkspace workspace_;
        std::shared_ptr<DocumentRecoveryStore> recovery_;
        WorkspaceChromeController* chrome_;
        QStackedWidget* singlePane_;
        std::function<int(const QString&)> closePrompt_;
        QString shellError_;
        void toggleEpubFullscreen();
        bool epubFullscreen_=false,epubWasMaximized_=false;
        QRect epubWindowGeometry_;
        bool closingWorkspace_ = false;
        bool pendingActionsScheduled_ = false;
        bool closeViewerOnEscape_ = true, viewerCloseRequested_ = false;
        QShortcut* viewerEscape_ = nullptr;
        int recoveryWorkers_ = 0;
        QMenuBar* menu_;
        QToolBar* toolbar_;
        QTabWidget* tabs_;
        DocumentStatusBar* statusBar_ = nullptr;
        QWidget* statusSlot_ = nullptr;
        std::vector<std::shared_ptr<Tab>> documents_;
    };
}
