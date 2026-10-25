#pragma once
#include "commonLibs/document/epub/epubEditModel.hpp"
#include "commonLibs/document/epub/epubDraftStore.hpp"
#include "commonLibs/storage/storage.hpp"
#include <QWidget>
#include <QFutureWatcher>
#include <QTimer>
#include <atomic>
#include <optional>
#include <utility>
class QComboBox;class QPlainTextEdit;class QLabel;class QVBoxLayout;class QPushButton;
namespace hc::document {
class EpubSourceEditor final:public QWidget {
    Q_OBJECT
public:
    EpubSourceEditor(QByteArray Bytes,const QString& Path,QWidget* Parent=nullptr);
    ~EpubSourceEditor()override;
    QJsonObject Snapshot()const;
    QJsonObject Execute(const QJsonObject& Request);
    QByteArray Token()const;
    storage::Result<QByteArray> Export(const QString& Destination)const;
    bool CanUndo()const;bool CanRedo()const;void Undo();void Redo();
    bool IsReady()const{return ready_;}
    bool IsValidated()const{return ready_ && !draftReader_.isRunning() && !pendingRecovery_ && pendingRecoveryPath_.isEmpty() && !validation_.isRunning() && validatedToken_==Token() && !validatedBytes_.isEmpty();}
    static QString RecoveryDirectory();
    void Commit(const QString& Path,const QByteArray& Bytes);
    void RequestSave(){validate(true);}
    void Checkpoint();
    void DiscardRecovery();
    void SetRecoveryEnabled(bool Enabled);
    void SetReadingLocator(QString Locator){recoveryLocator_=std::move(Locator);}
signals:void Changed();void StateChanged();void SaveRequested();
private:
    void load(QByteArray Bytes);void validate(bool SaveAfter);void select();bool flush();void importImage();
    QMap<QString,QByteArray> draft()const;
    void setupRecovery(QVBoxLayout* Layout);
    void modelLoaded(epub::EditModel Model);
    void refreshRecovery();
    void restoreRecovery(const QString& Path);
    void beginRecoveryRead(const QString& Path);
    void resetRecoverySession();
    void protectSourcePath(const QString& Path);
    epub::EditModel model_;QFutureWatcher<epub::EditModel> worker_;
    QFutureWatcher<epub::EditOutput> validation_;QByteArray validationToken_,validatedToken_,validatedBytes_;bool saveAfter_=false;
    std::shared_ptr<std::atomic_bool> cancel_;
    QComboBox* resources_=nullptr;QPlainTextEdit* text_=nullptr;QLabel *image_=nullptr,*status_=nullptr;
    QString current_,error_;QStringList protectedPaths_;QList<QByteArray> identities_;QByteArray baseToken_,reference_;
    bool ready_=false,updating_=false;
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
};
}
