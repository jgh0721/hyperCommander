#pragma once
#include "commonLibs/document/epub/epubEditModel.hpp"
#include "commonLibs/document/epub/epubDraftStore.hpp"
#include "commonLibs/storage/storage.hpp"
#include "documentWebProcess.hpp"
#include <QWidget>
#include <QFutureWatcher>
#include <QTimer>
#include <QPointer>
#include <QElapsedTimer>
#include <QJsonArray>
#include <atomic>
#include <optional>
#include <utility>
class QScrollArea;class QEvent;class QToolButton;class QProcess;class QTreeWidget;
class QTextDocument;
class QComboBox;class QPlainTextEdit;class QLabel;class QVBoxLayout;class QPushButton;class QSplitter;class QStackedLayout;class QCheckBox;class QTemporaryDir;class QResizeEvent;
namespace hc::document {
class EpubProcessView;
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
signals:void Changed();void StateChanged();void SaveRequested();void PreviewCommitted();void EscapeRequested();void FullscreenRequested();
protected:bool eventFilter(QObject* Object,QEvent* Event)override;void resizeEvent(QResizeEvent* Event)override;
private:
    void load(QByteArray Bytes);void validate(bool SaveAfter,bool Standard=false);void select();bool flush();void importImage();
    QMap<QString,QByteArray> draft()const;
    struct StructureOutput {epub::EditModel model_;QString error_,selected_;};
    QFutureWatcher<StructureOutput> structureWorker_;std::shared_ptr<std::atomic_bool> structureCancel_;
    QToolButton* structureActions_=nullptr;QPushButton* tocEdit_=nullptr;QString structureError_;bool structuring_=false;
    QJsonArray tocDraft_;QByteArray tocDraftToken_;
    void setupStructure(QVBoxLayout* Layout);bool structure(QJsonObject Request);void refreshResources(const QString& Preferred);
    void addImageResource();void convertImageResource();void addChapterResource();void editNavigation();
    void setupValidation(QVBoxLayout* Layout);void nativeDiagnostic(const epub::EditOutput& Result);
    void setDiagnostics(QJsonArray Rows);bool selectDiagnostic(int Index);
    bool standardValidationRequired()const;bool standardValidationAccepted()const;
    void startEpubCheck();void cancelEpubCheck(const QString& Reason);void finishEpubCheck(int Code,bool Normal);void stopEpubCheck();
    QJsonObject validationSnapshot()const;
    QProcess* checkerProcess_=nullptr;QTreeWidget* diagnostics_=nullptr;QPushButton* checkerCancel_=nullptr;
    QTimer checkerTimer_;QElapsedTimer checkerElapsed_;std::unique_ptr<QTemporaryDir> checkerDirectory_;
    QJsonObject checkerSettings_;QJsonArray diagnosticRows_;QByteArray checkerWorkToken_,checkerVerifiedToken_,checkerLog_;
    QString checkerState_="idle",checkerReason_,checkerVersion_;bool checkerRequested_=false;
    void setupRecovery(QVBoxLayout* Layout);
    void modelLoaded(epub::EditModel Model);
    void refreshRecovery();
    void restoreRecovery(const QString& Path);
    void beginRecoveryRead(const QString& Path);
    void resetRecoverySession();
    void protectSourcePath(const QString& Path);
    bool previewOwnerActive()const;
    void suspendInactivePreview();
    void requestPreview(bool Force=false);
    void showPreview(const QByteArray& Bytes);
    void admitPreview(EpubProcessView* Candidate);
    void retirePreview(EpubProcessView* View);
    void syncPreviewSource(bool Force=false);
    void saveSourceState();void activateSource(const QByteArray& Data,const QString& Media);
    void rehighlightSources();void locateSource();bool selectPreviewSource();
    void editMetadata();void inspectImage();void editImageProperties(const QJsonObject& Inspection);
    struct SourceState {QPointer<QTextDocument> document_;QByteArray applied_;int position_=0,anchor_=0,vertical_=0,horizontal_=0;};
    QMap<QString,SourceState> sourceBuffers_;QString bufferResource_;
    QTimer sourceSyncTimer_;bool sourceSyncPending_=false;int sourceSyncAttempts_=0;qint64 sourceSelectionSerial_=-1;
    QFutureWatcher<QJsonObject> imageInspection_;QByteArray imageInspectionToken_;
    QJsonObject previewSnapshot()const;
    epub::EditOutput cachedExport_;QByteArray cachedExportToken_;
    epub::EditModel model_;QFutureWatcher<epub::EditModel> worker_;
    QFutureWatcher<epub::EditOutput> validation_;QByteArray validationToken_,validatedToken_,validatedBytes_;bool saveAfter_=false,nativeValidating_=false;
    std::shared_ptr<std::atomic_bool> cancel_,validationCancel_;
    qint64 exportRuns_=0,exportJoins_=0;
    QComboBox* resources_=nullptr;QPlainTextEdit* text_=nullptr;QLabel *image_=nullptr,*status_=nullptr;
    QString current_,error_;QStringList protectedPaths_;QList<QByteArray> identities_;QByteArray baseToken_,reference_;
    bool ready_=false,updating_=false,sourceComposing_=false,commitRestore_=false,commitFocus_=false;
    bool bookUndoPriority_=false,bookRedoPriority_=false;
    QString commitResource_;int commitPosition_=0,commitAnchor_=0,commitVertical_=0,commitHorizontal_=0;QScrollArea* sourceScroll_=nullptr;
    struct DraftSession {std::atomic_bool cancelled_=false,discard_=false;};
    std::shared_ptr<DraftSession> draftSession_=std::make_shared<DraftSession>();
    QFutureWatcher<epub::DraftWrite> draftWriter_;
    QFutureWatcher<epub::DraftRead> draftReader_;
    QFutureWatcher<QList<epub::DraftInfo>> draftFinder_;
    QTimer draftTimer_;
    QString recoveryRoot_,sourcePath_,recoverySession_,recoveryPath_,recoveryLocator_,recoveryError_,pendingRecoveryPath_,readingRecoveryPath_,recoveredFrom_;
    QByteArray sourceHashAtOpen_,draftWrittenToken_,draftWritingToken_;
    std::optional<epub::DraftSnapshot> pendingRecovery_;
    QComboBox* recoveries_=nullptr;QLabel* recoveryStatus_=nullptr;QWidget* recoveryPanel_=nullptr;
    QPushButton *recoverDraft_=nullptr,*removeDraft_=nullptr;
    bool recoveryEnabled_=true,recoverySourceChanged_=false,recoveryReadingSourceChanged_=false,discardedRecovery_=false;
    QWidget* sourcePanel_=nullptr;QSplitter* previewSplitter_=nullptr;QStackedLayout* previewStack_=nullptr;
    QLabel* previewStatus_=nullptr;QCheckBox *previewAutomatic_=nullptr,*previewSynchronize_=nullptr;
    std::optional<WebProcessRuntime> previewRuntime_;std::unique_ptr<QTemporaryDir> previewDirectory_;
    QFutureWatcher<epub::EditOutput> previewWorker_;QTimer previewTimer_;
    EpubProcessView *previewView_=nullptr,*previewCandidate_=nullptr;QList<QPointer<EpubProcessView>> previewRetiring_;
    std::shared_ptr<std::atomic_bool> previewCancel_;QByteArray previewWorkToken_,previewCandidateToken_,previewActiveToken_;
    quint64 previewEpoch_=0,previewWorkEpoch_=0,previewCandidateEpoch_=0;bool previewClosing_=false,previewPending_=false,previewSyncing_=false;
    QString previewError_,previewSettings_;int previewChapter_=-1,previewPreferredChapter_=-1,previewRestoreChapter_=-1;qint64 previewNavigationSerial_=-1,previewCandidateSerial_=-2;
    bool previewCandidateInputRequested_=false,previewRecoveryPosition_=false;
};
}
