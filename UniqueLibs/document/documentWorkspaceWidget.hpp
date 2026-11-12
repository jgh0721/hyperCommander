#pragma once
#include "documentStorageAdapter.hpp"
#include "commonLibs/document/documentWorkspace.hpp"
#include "documentRecoveryStore.hpp"
#include "workspaceChromeController.hpp"
#include "documentViewKind.hpp"
#include "hwpOpenWindow.hpp"
#include "newDocumentChooser.hpp"
#include "newEpubDialog.hpp"
#include <functional>
#include <QWidget>
#include <QJsonObject>
#include <QPlainTextEdit>
#include <QTabWidget>
#include <QPointer>
#include <memory>

class QDialog;
class QLockFile;
class QProgressDialog;
class QMenuBar;
class QMenu;
class QToolBar;
class QContextMenuEvent;
class QLabel;
class QStackedWidget;
class QFileSystemWatcher;
class QShortcut;
class QMainWindow;
class QDockWidget;
class QResizeEvent;
class QDragEnterEvent;
class QDropEvent;
namespace mrst { class ExternalChangeWatcher; }
namespace hc::document
{
    namespace search { class Panel; }
    class DocumentStatusBar;
    class DocumentProgressOverlay;
    class RecentDocumentStore;
    class RecentDocumentsDialog;
    class DocumentStartPage;
    struct RecentDocument;
    class DocumentShortcutRouter;
    class DocumentSettingsDialog;
    class DocumentWindowSize;
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
        // Reopens a session's documents. Documents limits the preserved edits to those document ids (unchanged
        // documents always reopen); the others stay in the recovery list once this window publishes its session.
        bool RestoreHotExit(const QString& SessionId, const std::optional<QStringList>& Documents = std::nullopt);
        // Filter: all · documents · epub · archive.
        void ShowRecoveryManager(const QString& Filter = {});
        // Opens an archive copy (…/archive-drafts/<id>) as a separate document.
        QJsonObject OpenArchiveDraftCopy(const QString& Directory);
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
        void RestoreWindowSize();
        void OpenSearchContext(const QJsonObject& Context);
        DocumentViewKind CurrentKind() const;
        // Opens an EPUB and enters edit mode when it is ready (--epub-edit, a new EPUB, HotExit).
        // Resource/Position select a source resource and caret; a draft path applies an EPUB draft.
        QJsonObject OpenForEpubEdit(const QString& Path, const QString& Locator = {}, int Chapter = -1,
            const QString& Resource = {}, int Position = -1, const QString& DraftPath = {});
        // ‘새 EPUB 만들기’ (HyperEditor --new-epub, Ctrl+N › EPUB). Closing it with no document closes the window.
        void ShowNewEpubDialog(bool CloseWhenCancelled = false);
    protected:
        bool eventFilter(QObject* Watched, QEvent* Event) override;
        // Surfaces own their context menus; anything else ends here instead of falling through to every action.
        void contextMenuEvent(QContextMenuEvent* Event) override;
        void resizeEvent(QResizeEvent* Event) override;
        void dragEnterEvent(QDragEnterEvent* Event) override;
        void dropEvent(QDropEvent* Event) override;
    signals:
        void StateChanged();
        void ChromeSettingsChanged(const QJsonObject& Settings);
    private:
        void buildMenus();
        QAction* scoped(QAction* Action, quint32 Kinds);
        void applyKind(DocumentViewKind Kind);
        void setChromePart(WorkspaceChromeController::Part Which, bool Visible);
        void refreshChromeChecks();
        void updateTitle();
        QList<QPointer<QAction>> kindActions_;
        QPointer<QMenu> fileMenu_, editMenu_, viewMenu_, toolsMenu_, themeMenu_, chromeMenu_;
        QPointer<QAction> recentAnchor_;
        // 최근 항목 (plan 6): shared with HyperViewer through recent-documents.json.
        RecentDocumentStore* recent_ = nullptr;
        DocumentStartPage* startPage_ = nullptr;
        QPointer<RecentDocumentsDialog> recentDialog_;
        QList<QPointer<QAction>> recentActions_;
        QString pendingOpenEncoding_;
        void refreshRecentMenu();
        void openRecent(const RecentDocument& Document);
        void showRecentDialog();
        void clearRecent();
        void placeStartPage();
        // One row (default): the active pane's toolbar starts with the shared common actions and the top
        // toolbar shows only without a document. Two rows ("split"): common above the tabs, the view's below.
        bool toolbarVisible_ = true;
        void syncToolbarRow();
        void showChromeMenu(const QPoint& Global, bool Toolbar);
        void showTabMenu(const QPoint& Global, int Index);
        void showStatusMenu(const QPoint& Global);
        // id · text of the fields the active view offers (before hiding), for the status bar menu.
        QList<QPair<QString, QString>> statusFieldNames_;
        DocumentViewKind appliedKind_ = DocumentViewKind::None;
        bool kindApplied_ = false;
        void openSearch(bool Replace);
        void placeSearch(const QString& Location);
        QJsonArray searchSources() const;
        void navigateSearch(const QJsonObject& Row);
        search::Panel* searchPanel_ = nullptr;
        QMainWindow* searchHost_ = nullptr;
        QDockWidget* searchDock_ = nullptr;
        QString searchDocument_, searchPlacement_;
        // The EPUB editor whose problems panel hosts 파일에서 찾기 ("epubProblems" placement).
        QPointer<QWidget> searchEpubHost_;
        void syncSearchHost();
        std::unique_ptr<DocumentWindowSize> windowSize_;
        // Synchronous: startup, a saved settings dialog and Escape need the result at once.
        void refreshPreferences();
        // Reads and parses on a worker, then applies here; for the folder watcher and activation.
        void refreshPreferencesAsync();
        void applyPreferences(const QJsonObject& Product, bool Loaded, const QJsonObject& Root, const QString& Error);
        void watchSettingsFolder();
        bool preferencesReading_ = false, preferencesAgain_ = false;
        quint64 preferencesGeneration_ = 0;
        void externalChange(const QString& Path, bool Missing);
        mrst::ExternalChangeWatcher* externalWatcher_ = nullptr;
        void showPreferences();
        QPointer<DocumentSettingsDialog> preferences_;
        QJsonObject newDocument(NewDocumentKind Kind = NewDocumentKind::Text);
        // Ctrl+N: the ‘새 문서’ dialog, or a kind at once per documentViews/common/newDocumentAction.
        void requestNewDocument(QWidget* Anchor, bool ImagePage = false);
        QPointer<NewDocumentChooser> chooser_;
        QPointer<NewImageDialog> imageDialog_;
        void showNewImageDialog();
        QJsonObject newImage(const NewImageSpec& Spec);
        void createEpub(const NewEpubRequest& Request, bool CloseWhenCancelled);
        bool epubCreating_ = false;
        // Untitled documents: "새 문서 N" (text, Markdown) and "새 이미지 N" (images), numbered per window.
        struct Tab;
        QString documentName(const Tab& Value) const;
        QString untitledFileName(const Tab& Value) const;
        int nextUntitledNumber(bool Image, int Preferred = 0) const;
        void nameUntitled(const std::shared_ptr<Tab>& Value, bool Image, int Preferred = 0);
        QString lastSaveFolder() const;
        // Save As start: the file itself; for untitled and recovered copies a real folder and name, never the recovery folder.
        QString saveAsStart(const Tab& Value) const;
        QJsonObject openEpubRecovery(const QString& Path);
        QPointer<QDialog> recoveryManager_;
        DocumentShortcutRouter* shortcutRouter_ = nullptr;
        QString settingsPath_;
        QJsonObject appliedPreferences_;
        QFileSystemWatcher* settingsWatcher_ = nullptr;
        // Existence, size and modification time of the files refreshPreferences() reads, as of its
        // last run. The folder watcher also fires for recent documents and EPUB workspaces, which
        // are written on every save; those must not re-read and parse the settings on the GUI thread.
        QString settingsSignature_;
        QString settingsSignature() const;
        bool settingsChanged() const;
        struct LocalSave;
        std::shared_ptr<Tab> active() const;
        std::shared_ptr<Tab> find(const QString& Id) const;
        QJsonObject open(const QString& Path, bool HostSlot, const QString& SourceToken, bool Broker,
            const QString& RecoveryPath = {}, const QString& DisplayName = {}, QJsonObject ReadPrefix = {});
        void save(const std::shared_ptr<Tab>& Tab, const QString& Destination, bool Overwrite = false, bool SaveEpubOriginal = false);
        void startSave(const std::shared_ptr<Tab>& Tab, std::shared_ptr<LocalSave> Request);
        void load(const std::shared_ptr<Tab>& Tab, bool Discard = false, const QString& RecoveryPath = {});
        void navigateImage(const std::shared_ptr<Tab>& Tab, const QString& Path);
        void retire(std::shared_ptr<Tab> Tab, bool Discard, bool PreserveRecovery = false);
        // Opens the popover of an active status field; a hidden field falls back to its command.
        void openStatusPicker(const QString& Id);
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
        // 미리보기 전체 화면( .md · .rst ): 창을 전체 화면으로 하고 활성 탭의 미리보기만 남긴다.
        void togglePreviewFullscreen();
        void showNewSphinxProjectDialog();
        bool previewFullscreen_=false;
        QList<QPointer<QWidget>> previewHidden_;
        bool epubFullscreen_=false,epubWasMaximized_=false;
        QRect epubWindowGeometry_;
        bool hotExitEnabled() const;
        // The window publishes its session for HotExit, and also for periodic recovery so that an
        // abnormal exit is offered at the next start even with HotExit off.
        bool sessionPublishing() const;
        bool hotExitEligible(const std::shared_ptr<Tab>& Tab) const;
        // D9 / D10: EPUB tabs keep unsaved edits in their EPUB draft; PDF tabs in a full-PDF snapshot.
        bool hotExitEpub(const std::shared_ptr<Tab>& Tab) const;
        bool hotExitPdf(const std::shared_ptr<Tab>& Tab) const;
        // EPUB drafts restored one at a time; a changed original asks first (EpubRestoreConflictDialog).
        QList<QJsonObject> epubRestoreQueue_;
        bool epubRestoreBusy_ = false;
        // The close attempt whose EPUB editor workspaces were already flushed.
        quint64 epubFlushEpoch_ = 0;
        void nextEpubRestore();
        // A PDF snapshot reattaches to its original only when the original still has this SHA-256.
        QString pendingRecoveryBaseline_;
        bool requestHotExitClose();
        void publishHotExitSession(bool Closing);
        void cancelHotExitClose(const QString& Error = {});
        std::shared_ptr<QLockFile> hotExitLock_;
        std::shared_ptr<QLockFile> restoringLock_;
        std::vector<std::shared_ptr<QLockFile>> manualRecoveryLocks_;
        QString restoringSessionId_;
        bool restorationIncomplete_ = false;
        QJsonObject publishedSession_;
        std::vector<RecoverySnapshot> restoredOriginals_;
        bool pendingHotExitRestore_ = false, hotExitClosing_ = false, hotExitPublishing_ = false, hotExitReady_ = false;
        QPointer<QProgressDialog> hotExitProgress_;
        quint64 hotExitCloseEpoch_ = 0;
        QString restoredActive_;
        QMap<QString, QString> restoredIds_;
        QStringList hotExitDiscard_;
        bool closingWorkspace_ = false;
        bool closePromptActive_ = false;
        bool pendingActionsScheduled_ = false;
        bool closeViewerOnEscape_ = true, viewerCloseRequested_ = false;
        QShortcut* viewerEscape_ = nullptr;
        int recoveryWorkers_ = 0;
        QMenuBar* menu_;
        QToolBar* toolbar_;
        QTabWidget* tabs_;
        DocumentStatusBar* statusBar_ = nullptr;
        // File read and write progress over the left of the status bar; follows the active tab.
        DocumentProgressOverlay* progressOverlay_ = nullptr;
        QString progressTab_;
        void refreshProgress(const std::shared_ptr<Tab>& Tab);
        void recordRecent(const std::shared_ptr<Tab>& Tab);
        void finishProgress(const std::shared_ptr<Tab>& Tab, bool Success, const QString& Code = {});
        QWidget* statusSlot_ = nullptr;
        std::vector<std::shared_ptr<Tab>> documents_;
    };
}
