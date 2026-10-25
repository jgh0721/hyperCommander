#include "stdafx.h"
#include "epubSourceEditor.hpp"
#include "UniqueLibs/settings/appSettings.hpp"
#include "UniqueLibs/settings/unifiedConfig.hpp"
#include <QApplication>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QJsonArray>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QUuid>
#include <QVBoxLayout>
#include <QtConcurrentRun>
namespace hc::document {
QString EpubSourceEditor::RecoveryDirectory() {
    auto Config=qApp->property("documentViewsConfigDir").toString();if(Config.isEmpty())Config=hc::settings::DocumentPaths().directory_;
    return QDir(Config).absoluteFilePath("epub-drafts");
}
void EpubSourceEditor::resetRecoverySession() {
    draftSession_=std::make_shared<DraftSession>();recoverySession_=QUuid::createUuid().toString(QUuid::Id128);
    recoveryPath_.clear();draftWrittenToken_.clear();discardedRecovery_=false;
}
void EpubSourceEditor::setupRecovery(QVBoxLayout* Layout) {
    auto Config=qApp->property("documentViewsConfigDir").toString();if(Config.isEmpty())Config=hc::settings::DocumentPaths().directory_;
    recoveryRoot_=RecoveryDirectory();resetRecoverySession();
    QJsonObject Preferences;QString Error;
    if(hc::settings::UnifiedConfig::Load(hc::settings::UnifiedConfig::FileName(Config),Preferences,Error))
        recoveryEnabled_=Preferences["documentViews"].toObject()["textView"].toObject()["recoveryEnabled"].toBool(true);
    recoveryStatus_=new QLabel(this);recoveryStatus_->setTextFormat(Qt::PlainText);recoveryStatus_->setWordWrap(true);recoveryStatus_->setAccessibleName(tr("편집 초안 자동 복구 상태"));Layout->addWidget(recoveryStatus_);
    recoveryPanel_=new QWidget(this);auto* Row=new QHBoxLayout(recoveryPanel_);Row->setContentsMargins(0,0,0,0);
    recoveries_=new QComboBox(recoveryPanel_);recoveries_->setObjectName("epubRecoveryEntries");recoveries_->setAccessibleName(tr("복원할 EPUB 편집 초안"));recoveries_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);recoveries_->setMinimumContentsLength(16);Row->addWidget(recoveries_,1);
    recoverDraft_=new QPushButton(tr("초안 복원"),recoveryPanel_);removeDraft_=new QPushButton(tr("선택 초안 삭제"),recoveryPanel_);Row->addWidget(recoverDraft_);Row->addWidget(removeDraft_);Layout->addWidget(recoveryPanel_);recoveryPanel_->hide();
    status_->setTextFormat(Qt::PlainText);draftTimer_.setSingleShot(true);draftTimer_.setInterval(500);
    connect(&draftTimer_,&QTimer::timeout,this,&EpubSourceEditor::Checkpoint);
    connect(this,&EpubSourceEditor::Changed,this,[this]{if(recoveryEnabled_ && !discardedRecovery_ && ready_)draftTimer_.start();});
    connect(qApp,&QCoreApplication::aboutToQuit,this,&EpubSourceEditor::Checkpoint);
    connect(recoverDraft_,&QPushButton::clicked,this,[this]{restoreRecovery(recoveries_->currentData().toString());});
    connect(removeDraft_,&QPushButton::clicked,this,[this]{
        const auto Path=recoveries_->currentData().toString();if(Path.isEmpty())return;
        auto* Box=new QMessageBox(QMessageBox::Question,tr("복구 초안 삭제"),tr("선택한 복구 초안을 삭제하시겠습니까? 원본 EPUB은 변경하지 않습니다."),QMessageBox::Yes|QMessageBox::No,this);Box->setAttribute(Qt::WA_DeleteOnClose);
        connect(Box,&QMessageBox::finished,this,[this,Path](int Answer){if(Answer==QMessageBox::Yes){if(!epub::DraftStore::Remove(recoveryRoot_,Path))recoveryStatus_->setText(tr("초안을 삭제하지 못했습니다. 파일을 유지합니다."));refreshRecovery();}});Box->open();
    });
    connect(&draftFinder_,&QFutureWatcher<QList<epub::DraftInfo>>::finished,this,[this]{
        const auto Entries=draftFinder_.result();const QSignalBlocker Block(recoveries_);recoveries_->clear();
        const auto Own=QDir(recoveryRoot_).filePath(recoverySession_+".hcedraft");
        for(const auto& Entry:Entries)if(Entry.path_!=Own) {
            const auto Label=Entry.error_.isEmpty()?tr("%1 · %2").arg(QFileInfo(Entry.sourcePath_).fileName(),QDateTime::fromMSecsSinceEpoch(Entry.utcMs_).toString("yyyy-MM-dd HH:mm:ss")):tr("검사할 수 없는 초안 · %1").arg(QFileInfo(Entry.path_).fileName());
            recoveries_->addItem(Label,Entry.path_);
        }
        recoveryPanel_->setVisible(recoveries_->count()>0);emit StateChanged();
    });
    connect(&draftWriter_,&QFutureWatcher<epub::DraftWrite>::finished,this,[this]{
        const auto Result=draftWriter_.result();const auto Own=QDir(recoveryRoot_).filePath(recoverySession_+".hcedraft");
        if(Result && Result.path_==Own && !discardedRecovery_){recoveryPath_=Result.path_;draftWrittenToken_=draftWritingToken_;recoveryError_.clear();recoveryStatus_->setText(tr("편집 초안을 자동 복구용으로 보관했습니다."));}
        else if(!Result && Result.error_!="epub-draft-cancelled"){recoveryError_=Result.error_;recoveryStatus_->setText(tr("초안 자동 보관 실패: %1 · 편집 내용은 유지됩니다.").arg(recoveryError_));}
        if(!pendingRecoveryPath_.isEmpty()){const auto Path=std::exchange(pendingRecoveryPath_,{});if(Result)beginRecoveryRead(Path);else{text_->setReadOnly(false);status_->setText(tr("현재 편집 내용을 보관하지 못해 복원을 중단했습니다."));}}
        else if(Result && ready_ && recoveryEnabled_ && !discardedRecovery_ && draftWrittenToken_!=Token())draftTimer_.start();
        emit StateChanged();
    });
    connect(&draftReader_,&QFutureWatcher<epub::DraftRead>::finished,this,[this]{
        const auto Result=draftReader_.result();if(!Result){recoveryError_=Result.error_;recoveryStatus_->setText(tr("복구 초안을 읽지 못했습니다: %1 · 기존 편집과 초안 파일은 유지됩니다.").arg(recoveryError_));text_->setReadOnly(false);emit StateChanged();return;}
        pendingRecovery_=Result.snapshot_;recoveryReadingSourceChanged_=Result.sourceChanged_;recoveryError_.clear();load(Result.snapshot_.original_);
    });
    recoveryStatus_->setText(recoveryEnabled_?tr("편집 초안 자동 복구가 켜져 있습니다."):tr("편집 초안 자동 복구가 꺼져 있습니다. 기존 초안은 복구 목록에서 관리할 수 있습니다."));
}
void EpubSourceEditor::modelLoaded(epub::EditModel Model) {
    const bool Restoring=pendingRecovery_.has_value();QString Error=Model.Error();
    if(Restoring && Error.isEmpty())Error=Model.RestoreStructuralBytes(pendingRecovery_->structuralBytes_);
    if(Restoring && Error.isEmpty())for(auto I=pendingRecovery_->changes_.begin();I!=pendingRecovery_->changes_.end();++I){Error=Model.Replace(I.key(),I.value());if(!Error.isEmpty())break;}
    if(Restoring && Error.isEmpty() && pendingRecovery_->hasCurrentDraft_) {
        const auto& Current=pendingRecovery_->currentResource_;QString Media;
        for(const auto& Value:Model.Publication()["resources"].toArray())if(Value.toObject()["path"]==Current)Media=Value.toObject()["mediaType"].toString();
        if(!Model.IsEditable(Current) || QStringList{"image/png","image/jpeg","image/gif","image/webp"}.contains(Media) || QString::fromUtf8(pendingRecovery_->currentDraft_).toUtf8()!=pendingRecovery_->currentDraft_)Error="epub-draft-selected-resource";
    }
    if(Restoring && !Error.isEmpty()) {
        pendingRecovery_.reset();ready_=!model_.Publication().isEmpty();setEnabled(true);text_->setReadOnly(false);recoveryError_=Error;recoveryStatus_->setText(tr("복구 내용 검사 실패: %1 · 현재 편집과 초안 파일을 유지합니다.").arg(Error));emit StateChanged();return;
    }
    model_=std::move(Model);ready_=!model_.Publication().isEmpty();error_=model_.Error();setEnabled(true);resources_->setEnabled(ready_);text_->setEnabled(ready_);image_->setEnabled(ready_);
    for(auto* Button:findChildren<QPushButton*>())if(!recoveryPanel_->isAncestorOf(Button))Button->setEnabled(ready_);
    if(checkerCancel_)checkerCancel_->setEnabled(checkerState_=="running");
    status_->setText(ready_?tr("변경 내용은 사본으로 저장합니다. EPUBCheck 검사는 설정의 Java/JAR를 사용합니다."):error_);
    if(ready_) {
        reference_=Restoring?QCryptographicHash::hash(pendingRecovery_->original_,QCryptographicHash::Sha256):model_.Fingerprint();updating_=true;resources_->clear();
        for(const auto& Value:model_.Publication()["resources"].toArray()){const auto Resource=Value.toObject();if(model_.IsEditable(Resource["path"].toString()))resources_->addItem(Resource["path"].toString(),Resource["mediaType"].toString());}
        QStringList SpinePaths;for(const auto& Value:model_.Publication()["spine"].toArray())SpinePaths.append(Value.toObject()["path"].toString());resources_->setProperty("epubEditionHash",model_.Publication()["sourceSha256"].toString());resources_->setProperty("epubSpinePaths",SpinePaths);
        updating_=false;current_.clear();select();
        if(Restoring) {
            const auto Snapshot=std::exchange(pendingRecovery_,std::nullopt).value();baseToken_=QCryptographicHash::hash(Snapshot.original_,QCryptographicHash::Sha256);
            const int Index=resources_->findText(Snapshot.currentResource_);if(Index>=0){const QSignalBlocker Block(resources_);resources_->setCurrentIndex(Index);select();}
            if(Snapshot.hasCurrentDraft_) {
                if(Index<0 || text_->isHidden()){recoveryError_="epub-draft-selected-resource";recoveryStatus_->setText(tr("선택한 리소스 초안은 수동 확인이 필요합니다."));}
                else {updating_=true;text_->setPlainText(QString::fromUtf8(Snapshot.currentDraft_));text_->document()->setModified(true);auto Cursor=text_->textCursor();Cursor.setPosition(qBound(0,Snapshot.cursor_,text_->document()->characterCount()-1));text_->setTextCursor(Cursor);text_->verticalScrollBar()->setValue(Snapshot.scroll_);updating_=false;}
            }
            recoveryLocator_=Snapshot.locator_;recoverySourceChanged_=recoveryReadingSourceChanged_;recoveredFrom_=readingRecoveryPath_;
            previewPreferredChapter_=-1;previewRestoreChapter_=Snapshot.readingChapter_;previewRecoveryPosition_=true;
            const auto Spine=model_.Publication()["spine"].toArray();
            if(previewRestoreChapter_<0 && !recoveryLocator_.isEmpty())for(int I=0;I<Spine.size();++I)if(Spine[I].toObject()["path"]==current_)previewRestoreChapter_=I;
            protectSourcePath(Snapshot.sourcePath_);sourcePath_=Snapshot.sourcePath_;
            resetRecoverySession();text_->setReadOnly(false);
            recoveryStatus_->setText(recoverySourceChanged_?tr("원본의 변경 또는 삭제를 감지했습니다. 보관된 판본의 초안을 복원했습니다. 다른 이름으로 저장하여 비교하세요."):tr("보관된 편집 초안을 복원했습니다. 원본 EPUB은 변경하지 않았습니다."));
            emit Changed();
        }
    }
    if(ready_ && commitRestore_ && !Restoring) {
        const int Index=resources_->findText(commitResource_);if(Index>=0){const QSignalBlocker Block(resources_);resources_->setCurrentIndex(Index);select();}
        if(!text_->isHidden()){auto Cursor=text_->textCursor();const int Last=text_->document()->characterCount()-1;Cursor.setPosition(qBound(0,commitAnchor_,Last));Cursor.setPosition(qBound(0,commitPosition_,Last),QTextCursor::KeepAnchor);text_->setTextCursor(Cursor);}
        QTimer::singleShot(0,this,[this]{text_->verticalScrollBar()->setValue(commitVertical_);text_->horizontalScrollBar()->setValue(commitHorizontal_);if(commitFocus_ && previewOwnerActive())text_->setFocus(Qt::OtherFocusReason);});
    }
    commitRestore_=false;refreshRecovery();emit StateChanged();
}
void EpubSourceEditor::refreshRecovery() {
    if(draftFinder_.isRunning())return;
    draftFinder_.setFuture(QtConcurrent::run([Root=recoveryRoot_,Source=sourcePath_,Hash=sourceHashAtOpen_.toHex()]{return epub::DraftStore::List(Root,Source,Hash);}));
}
void EpubSourceEditor::Checkpoint() {
    draftTimer_.stop();if(!ready_ || !recoveryEnabled_ || discardedRecovery_ || draftWriter_.isRunning() || pendingRecovery_ || draftReader_.isRunning())return;
    if(Token()==baseToken_) {if(!recoveryPath_.isEmpty()){DiscardRecovery();resetRecoverySession();}return;}
    if(draftWrittenToken_==Token())return;
    epub::DraftSnapshot Snapshot;Snapshot.sourcePath_=sourcePath_;Snapshot.original_=model_.OriginalBytes();Snapshot.structuralBytes_=model_.StructuralBytes();Snapshot.changes_=model_.Changes();Snapshot.currentResource_=current_;Snapshot.locator_=recoveryLocator_;Snapshot.readingChapter_=previewRecoveryPosition_?previewRestoreChapter_:previewSnapshot()["state"].toObject()["chapterIndex"].toInt(previewRestoreChapter_);
    const auto Current=draft();Snapshot.hasCurrentDraft_=Current.contains(current_);Snapshot.currentDraft_=Current.value(current_);Snapshot.cursor_=text_->textCursor().position();Snapshot.scroll_=text_->verticalScrollBar()->value();
    draftWritingToken_=Token();recoveryStatus_->setText(tr("편집 초안을 자동 복구용으로 보관하고 있습니다…"));
    draftWriter_.setFuture(QtConcurrent::run([Root=recoveryRoot_,Session=recoverySession_,Snapshot=std::move(Snapshot),State=draftSession_]{
        auto Result=epub::DraftStore::Write(Root,Session,Snapshot,[State]{return State->cancelled_.load();});
        if(State->discard_.load())epub::DraftStore::Remove(Root,QDir(Root).filePath(Session+".hcedraft"));return Result;
    }));emit StateChanged();
}
void EpubSourceEditor::DiscardRecovery() {
    if(discardedRecovery_)return;
    discardedRecovery_=true;draftTimer_.stop();pendingRecoveryPath_.clear();draftSession_->discard_.store(true);draftSession_->cancelled_.store(true);
    const auto Own=QDir(recoveryRoot_).filePath(recoverySession_+".hcedraft"),Recovered=recoveredFrom_;
    (void)QtConcurrent::run([Root=recoveryRoot_,Own,Recovered]{epub::DraftStore::Remove(Root,Own);if(!Recovered.isEmpty() && Recovered!=Own)epub::DraftStore::Remove(Root,Recovered);});
    recoveryPath_.clear();recoveredFrom_.clear();draftWrittenToken_.clear();
}
void EpubSourceEditor::SetRecoveryEnabled(bool Enabled) {
    if(recoveryEnabled_==Enabled)return;recoveryEnabled_=Enabled;
    if(!Enabled){draftTimer_.stop();draftSession_->cancelled_.store(true);recoveryStatus_->setText(tr("자동 복구가 꺼져 있습니다. 기존 초안 파일은 유지합니다."));}
    else {draftSession_=std::make_shared<DraftSession>();if(ready_ && Token()!=baseToken_)draftTimer_.start();}
    emit StateChanged();
}
void EpubSourceEditor::restoreRecovery(const QString& Path) {
    if(structuring_){status_->setText(tr("구조 작업이 끝난 뒤 초안을 복원해 주세요."));return;}
    if(Path.isEmpty() || pendingRecovery_ || draftReader_.isRunning() || worker_.isRunning() || validation_.isRunning())return;
    if(ready_ && Token()!=baseToken_ && draftWrittenToken_!=Token()) {
        if(!recoveryEnabled_){recoveryStatus_->setText(tr("현재 편집 내용이 있습니다. 사본을 저장하거나 자동 복구를 켠 뒤 복원하세요."));return;}
        pendingRecoveryPath_=Path;text_->setReadOnly(true);if(!draftWriter_.isRunning())Checkpoint();return;
    }
    beginRecoveryRead(Path);
}
void EpubSourceEditor::beginRecoveryRead(const QString& Path) {
    readingRecoveryPath_=Path;text_->setReadOnly(true);recoveryStatus_->setText(tr("보관된 초안을 검사하고 있습니다…"));
    draftReader_.setFuture(QtConcurrent::run([Root=recoveryRoot_,Path,Source=sourcePath_,AtOpen=sourceHashAtOpen_]{
        auto Result=epub::DraftStore::Read(Root,Path);if(!Result)return Result;
        const auto SavedHash=QCryptographicHash::hash(Result.snapshot_.original_,QCryptographicHash::Sha256);
        if(SavedHash!=AtOpen && QDir::cleanPath(Source).compare(QDir::cleanPath(Result.snapshot_.sourcePath_),Qt::CaseInsensitive)!=0)return epub::DraftRead{{},"epub-draft-book-mismatch"};
        QFile Current(Result.snapshot_.sourcePath_);
        Result.sourceChanged_=!Current.open(QIODevice::ReadOnly) || Current.size()>64LL*1024*1024 || QCryptographicHash::hash(Current.read(64LL*1024*1024+1),QCryptographicHash::Sha256)!=SavedHash;
        return Result;
    }));
}
}
