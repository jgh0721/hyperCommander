#pragma once
#include "commonLibs/document/epub/epubEditModel.hpp"
#include "commonLibs/document/epub/epubDraftStore.hpp"
#include "commonLibs/storage/storage.hpp"
#include "documentWebProcess.hpp"
#include "epubLineShifts.hpp"
#include <QWidget>
#include <QFutureWatcher>
#include <QTimer>
#include <QPointer>
#include <QElapsedTimer>
#include <QJsonArray>
#include <atomic>
#include <optional>
#include <utility>
class QEvent;class QToolButton;class QProcess;class QTreeWidget;class QTreeWidgetItem;class QTreeView;class QTabBar;class QTabWidget;class QStackedWidget;
class QLabel;class QVBoxLayout;class QPushButton;class QSplitter;class QStackedLayout;class QTemporaryDir;class QLineEdit;class QMenu;class QAction;class QPlainTextEdit;class QTextView;class QModelIndex;
namespace hc::document {
class EpubProcessView;class EpubResourceModel;class EpubResourceFilter;class EpubSplitter;
class EpubSourceEditor final:public QWidget {
    Q_OBJECT
public:
    EpubSourceEditor(QByteArray Bytes,const QString& Path,QWidget* Parent=nullptr,const QByteArray& SourceHash={});
    ~EpubSourceEditor()override;
    QJsonObject Snapshot()const;
    QJsonObject Execute(const QJsonObject& Request);
    QByteArray Token()const;
    storage::Result<QByteArray> Export(const QString& Destination)const;
    bool CanUndo()const;bool CanRedo()const;void Undo();void Redo();
    bool IsReady()const{return ready_;}
    bool IsModified()const{return ready_ && Token()!=baseToken_;}
    bool IsValidated()const{return ready_ && !draftReader_.isRunning() && !pendingRecovery_ && pendingRecoveryPath_.isEmpty() && !nativeValidating_ && !structuring_ && validatedToken_==Token() && !validatedBytes_.isEmpty() && checkerState_!="running" && standardValidationAccepted();}
    static QString RecoveryDirectory();
    void Commit(const QString& Path,const QByteArray& Bytes);
    void RequestSave(){validate(true);}
    void ReloadValidationSettings();
    void Checkpoint();
    void DiscardRecovery();
    void SetRecoveryEnabled(bool Enabled);
    void SetReadingLocator(QString Locator){recoveryLocator_=std::move(Locator);}
    void ConfigurePreview(const WebProcessRuntime& Runtime,int Chapter=-1);
    EpubProcessView* PreviewView()const{return previewView_;}
    void SetSourceVisible(bool Visible);
    bool IsSourceVisible()const;
    bool RequestPreviewClose();
    bool IsPreviewClosed()const;
    QTextView* SourceView()const{return text_;}
    QWidget* SourceInput()const;
    // Commands shared by the toolbar, menus, context menus and the workspace shortcut router.
    QList<QAction*> ToolbarActions()const;
    QList<QAction*> MenuActions()const;
    QWidget* StatusStrip()const;
    bool RunCommand(const QString& Id);
    bool CanRunCommand(const QString& Id)const;
    void SetShortcutBindings(const QJsonObject& Bindings);
signals:void Changed();void StateChanged();void SaveRequested();void PreviewCommitted();void EscapeRequested();void FullscreenRequested();
protected:bool eventFilter(QObject* Object,QEvent* Event)override;
private:
    void load(QByteArray Bytes);void validate(bool SaveAfter,bool Standard=false);void select();bool flush();void importImage();
    QMap<QString,QByteArray> draft()const;
    void report(const QString& Text);void reportDraft(const QString& Text);void reportPreview(const QString& Text);
    // Layout, panels and commands.
    void setupLayout();void setupActions();void setupStatus();void setupProblems(QWidget* Panel);void setupResources(QWidget* Panel);
    void restoreLayout();void saveLayout();void applyOrientation(Qt::Orientation Value,bool Save=true);void setPanelVisible(const QString& Panel,bool Visible);
    QAction* action(const QString& Id)const;void updateActions();void updateStatus();
    // Resource tree, editor tabs and the source buffers.
    void refreshTree();void selectResource(const QString& Path);bool openResource(const QString& Path);QString mediaOf(const QString& Path)const;QString treeResource()const;
    void refreshTabs();void closeTab(int Index);void closeTabs(const QList<int>& Indexes);bool closeResource(const QString& Path,bool Ask);void revertResource(const QString& Path);
    QStringList openResources_;QSet<QString> touchedResources_;
    // Scintilla position helpers: model and draft positions stay UTF-16 offsets.
    int toUtf16(qintptr Position)const;qintptr fromUtf16(int Offset)const;
    QByteArray sourceBytes()const;QString sourceText()const;bool sourceModified()const;bool sourceShown()const;
    int sourcePosition()const;int sourceAnchor()const;void setSourceSelection(int Anchor,int Position,bool Reveal=true);
    int firstVisibleLine()const;int horizontalOffset()const;void setScroll(int Line,int Offset);void setSourceReadOnly(bool ReadOnly);
    void focusSource();void insertSource(const QString& Text);
    struct StructureOutput {epub::EditModel model_;QString error_,selected_;};
    QFutureWatcher<StructureOutput> structureWorker_;std::shared_ptr<std::atomic_bool> structureCancel_;
    QString structureError_;bool structuring_=false;
    QJsonArray tocDraft_;QByteArray tocDraftToken_;
    void setupStructure();bool structure(QJsonObject Request);void refreshResources(const QString& Preferred);
    void addImageResource(const QString& Chapter={});void convertImageResource(const QString& Path);void addChapterResource(const QString& After={});void editNavigation();
    void renameResource(const QString& Path);void removeResource(const QString& Path);void moveChapter(const QString& Path,int Delta);
    void setupValidation();void nativeDiagnostic(const epub::EditOutput& Result);
    void setDiagnostics(QJsonArray Rows);bool selectDiagnostic(int Index);void markDiagnostics();
    bool standardValidationRequired()const;bool standardValidationAccepted()const;
    void startEpubCheck();void cancelEpubCheck(const QString& Reason);void finishEpubCheck(int Code,bool Normal);void stopEpubCheck();
    QJsonObject validationSnapshot()const;
    QProcess* checkerProcess_=nullptr;QTreeWidget* diagnostics_=nullptr;QPlainTextEdit* checkerOutput_=nullptr;QTreeWidget* searchResults_=nullptr;QTabWidget* problems_=nullptr;QLabel* problemsSummary_=nullptr;QWidget* problemsPanel_=nullptr;
    QTimer checkerTimer_;QElapsedTimer checkerElapsed_;std::unique_ptr<QTemporaryDir> checkerDirectory_;
    QJsonObject checkerSettings_;QJsonArray diagnosticRows_;QByteArray checkerWorkToken_,checkerVerifiedToken_,checkerLog_;
    QString checkerState_="idle",checkerReason_,checkerVersion_;bool checkerRequested_=false;QStringList problemFilter_;QString problemResource_;
    void setupRecovery();
    void modelLoaded(epub::EditModel Model);
    void refreshRecovery();
    void restoreRecovery(const QString& Path);
    void beginRecoveryRead(const QString& Path);
    void resetRecoverySession();
    void removeRecovery(const QString& Path);
    void protectSourcePath(const QString& Path);
    bool previewOwnerActive()const;
    // No preview exists or is being made. The first one is shown even when live preview is off.
    bool previewMissing()const{return !previewView_ && !previewCandidate_ && !previewWorker_.isRunning();}
    void suspendInactivePreview();
    void requestPreview(bool Force=false);
    void showPreview(const QByteArray& Bytes);
    void admitPreview(EpubProcessView* Candidate);
    void retirePreview(EpubProcessView* View);
    void syncPreviewSource(bool Force=false);
    void previewContextRequest(EpubProcessView* View);
    void saveSourceState();void activateSource(const QByteArray& Data,const QString& Media);void releaseSource(const QString& Path);
    void locateSource();bool selectPreviewSource(const QJsonObject& Anchor={});
    // Scroll synchronisation (epubSourceScroll.cpp): the line at half the editor height stays at half the preview height.
    void setupScrollSync();bool scrollSyncActive()const;void syncPreviewScroll();void previewScrolled(const QJsonObject& Report);
    QJsonObject sourceScrollTarget(bool Revision)const;void placeSourceLine(int Position,double Ratio);int spineIndex(const QString& Path)const;
    double toPreviewLine(double Line)const;double fromPreviewLine(double Line)const;void clearLineShifts();
    QTimer scrollRetryTimer_;qint64 previewDrivenIgnoreUntil_=0,suppressSyncUntil_=0,previewScrollSeen_=-1,sourceScrollSerial_=0,previewCandidateScrollSerial_=-1;
    double lastScrollLine_=-1;int lastScrollChapter_=-1;bool lastScrollEnd_=false,scrollSyncPending_=false;QElapsedTimer previewCandidateScrollClock_;
    void editMetadata();void inspectImage(const QString& Path);void editImageProperties(const QJsonObject& Inspection);
    // Context menus and source tools.
    void showTreeMenu(const QPoint& Position);void showSourceMenu(const QPoint& Position);void showTabMenu(const QPoint& Position);void showProblemMenu(const QPoint& Position);
    void quickFix();QJsonObject quickFixAt(int Position)const;QString linkAt(int Position)const;bool openLink();void wrapSelection(const QString& Tag);
    void findInResources(const QString& Text);void findSelectionInSource(const QString& Text);void copyPath(const QString& Path,bool Relative);
    void insertImageReference(const QString& Path);void exportResource(const QString& Path);void duplicateResource(const QString& Path);void importResources();
    // shifts_: line changes since the shown preview was built, so scroll positions map onto it.
    struct SourceState {qintptr document_=0;QByteArray applied_;int position_=0,anchor_=0,line_=0,offset_=0;quint64 serial_=0;QList<LineShift> shifts_;bool shiftsOverflow_=false;};
    QMap<QString,SourceState> sourceBuffers_;QString bufferResource_;quint64 bufferSerial_=0;
    QTimer sourceSyncTimer_;bool sourceSyncPending_=false;int sourceSyncAttempts_=0;qint64 sourceSelectionSerial_=-1;qint64 contextRequestSerial_=-1;
    QFutureWatcher<QJsonObject> imageInspection_;QByteArray imageInspectionToken_;
    QJsonObject previewSnapshot()const;
    epub::EditOutput cachedExport_;QByteArray cachedExportToken_;
    epub::EditModel model_;QFutureWatcher<epub::EditModel> worker_;
    QFutureWatcher<epub::EditOutput> validation_;QByteArray validationToken_,validatedToken_,validatedBytes_;bool saveAfter_=false,nativeValidating_=false;
    std::shared_ptr<std::atomic_bool> cancel_,validationCancel_;
    qint64 exportRuns_=0,exportJoins_=0;
    QTextView* text_=nullptr;QLabel* image_=nullptr;QStackedWidget* sourceStack_=nullptr;QWidget* sourcePanel_=nullptr;QWidget* recoveryBar_=nullptr;QLabel* recoveryText_=nullptr;
    QString current_,error_,message_,draftMessage_,previewMessage_;QStringList protectedPaths_;QList<QByteArray> identities_;QByteArray baseToken_,reference_;
    bool ready_=false,updating_=false,sourceComposing_=false,commitRestore_=false,commitFocus_=false,sourceReadOnly_=false;
    bool bookUndoPriority_=false,bookRedoPriority_=false,recoveryDismissed_=false;
    QString commitResource_;int commitPosition_=0,commitAnchor_=0,commitVertical_=0,commitHorizontal_=0;
    EpubResourceModel* resourceModel_=nullptr;EpubResourceFilter* resourceFilter_=nullptr;QTreeView* tree_=nullptr;QLineEdit* treeFilter_=nullptr;QLabel* treeSummary_=nullptr;QTabBar* tabs_=nullptr;
    QSplitter *outerSplitter_=nullptr,*workSplitter_=nullptr,*previewSplitter_=nullptr;QWidget *treePanel_=nullptr,*previewPanel_=nullptr;QTimer layoutTimer_;bool layoutRestored_=false;
    QMap<QString,QAction*> actions_;QList<QAction*> toolbarActions_,menuActions_;QMenu *validateMenu_=nullptr,*structureMenu_=nullptr,*draftMenu_=nullptr,*layoutMenu_=nullptr;
    QWidget* statusStrip_=nullptr;QLabel *statusValidation_=nullptr,*statusMessage_=nullptr,*statusDraft_=nullptr,*statusCaret_=nullptr,*statusFormat_=nullptr;QToolButton* statusProblems_=nullptr;
    struct DraftSession {std::atomic_bool cancelled_=false,discard_=false;};
    std::shared_ptr<DraftSession> draftSession_=std::make_shared<DraftSession>();
    QFutureWatcher<epub::DraftWrite> draftWriter_;
    QFutureWatcher<epub::DraftRead> draftReader_;
    QFutureWatcher<QList<epub::DraftInfo>> draftFinder_;
    QTimer draftTimer_;
    QString recoveryRoot_,sourcePath_,recoverySession_,recoveryPath_,recoveryLocator_,recoveryError_,pendingRecoveryPath_,readingRecoveryPath_,recoveredFrom_;
    QByteArray sourceHashAtOpen_,draftWrittenToken_,draftWritingToken_;QList<QPair<QString,QString>> recoveryEntries_;qint64 draftSavedAt_=0;
    std::optional<epub::DraftSnapshot> pendingRecovery_;
    bool recoveryEnabled_=true,recoverySourceChanged_=false,recoveryReadingSourceChanged_=false,discardedRecovery_=false;
    QStackedLayout* previewStack_=nullptr;QLabel *previewChip_=nullptr,*previewChapterLabel_=nullptr;
    std::optional<WebProcessRuntime> previewRuntime_;std::unique_ptr<QTemporaryDir> previewDirectory_;
    QFutureWatcher<epub::EditOutput> previewWorker_;QTimer previewTimer_;
    EpubProcessView *previewView_=nullptr,*previewCandidate_=nullptr;QList<QPointer<EpubProcessView>> previewRetiring_;
    std::shared_ptr<std::atomic_bool> previewCancel_;QByteArray previewWorkToken_,previewCandidateToken_,previewActiveToken_;
    quint64 previewEpoch_=0,previewWorkEpoch_=0,previewCandidateEpoch_=0;bool previewClosing_=false,previewPending_=false,previewSyncing_=false;
    QString previewError_,previewSettings_;int previewChapter_=-1,previewPreferredChapter_=-1,previewRestoreChapter_=-1,previewWidth_=0;qint64 previewNavigationSerial_=-1,previewCandidateSerial_=-2;
    bool previewCandidateInputRequested_=false,previewRecoveryPosition_=false;
};
}
