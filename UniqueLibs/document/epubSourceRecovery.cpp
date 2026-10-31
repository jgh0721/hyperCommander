#include "stdafx.h"
#include "epubSourceEditor.hpp"
#include "text/QTextView.h"
#include "text/ScintillaQtDirectBackend.h"
#include "UniqueLibs/qtMessageDialog.hpp"
#include "UniqueLibs/settings/appSettings.hpp"
#include "UniqueLibs/settings/unifiedConfig.hpp"
#include <QAction>
#include <QApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QInputDialog>
#include <QLabel>
#include <QJsonArray>
#include <QMessageBox>
#include <QToolButton>
#include <QUuid>
#include <QtConcurrentRun>
#include <Scintilla.h>
namespace hc::document {
QString EpubSourceEditor::RecoveryDirectory() {
    auto Config=qApp->property("documentViewsConfigDir").toString();if(Config.isEmpty())Config=hc::settings::DocumentPaths().directory_;
    return QDir(Config).absoluteFilePath("epub-drafts");
}
void EpubSourceEditor::resetRecoverySession() {
    draftSession_=std::make_shared<DraftSession>();recoverySession_=QUuid::createUuid().toString(QUuid::Id128);
    recoveryPath_.clear();draftWrittenToken_.clear();discardedRecovery_=false;draftSavedAt_=0;
}
void EpubSourceEditor::removeRecovery(const QString& Path) {
    if(Path.isEmpty())return;
    auto* Box=new hc::ui::MessageDialog(QMessageBox::Question,tr("복구 초안 삭제"),tr("선택한 복구 초안을 삭제하시겠습니까? 원본 EPUB은 변경하지 않습니다."),QMessageBox::Yes|QMessageBox::No,this);Box->setAttribute(Qt::WA_DeleteOnClose);
    connect(Box,&QMessageBox::finished,this,[this,Path](int Answer){if(Answer==QMessageBox::Yes){if(!epub::DraftStore::Remove(recoveryRoot_,Path))reportDraft(tr("초안을 삭제하지 못했습니다. 파일을 유지합니다."));refreshRecovery();}});Box->open();
}
void EpubSourceEditor::setupRecovery() {
    auto Config=qApp->property("documentViewsConfigDir").toString();if(Config.isEmpty())Config=hc::settings::DocumentPaths().directory_;
    recoveryRoot_=RecoveryDirectory();resetRecoverySession();
    QJsonObject Preferences;QString Error;
    if(hc::settings::UnifiedConfig::Load(hc::settings::UnifiedConfig::FileName(Config),Preferences,Error))
        recoveryEnabled_=Preferences["documentViews"].toObject()["textView"].toObject()["recoveryEnabled"].toBool(true);
    draftTimer_.setSingleShot(true);draftTimer_.setInterval(500);
    connect(&draftTimer_,&QTimer::timeout,this,&EpubSourceEditor::Checkpoint);
    connect(this,&EpubSourceEditor::Changed,this,[this]{if(recoveryEnabled_ && !discardedRecovery_ && ready_)draftTimer_.start();});
    connect(qApp,&QCoreApplication::aboutToQuit,this,&EpubSourceEditor::Checkpoint);
    // The bar appears only while another draft of this book exists; the draft menu always lists them.
    connect(recoveryBar_->findChild<QToolButton*>("epubRecoveryRestore"),&QToolButton::clicked,this,[this]{if(!recoveryEntries_.isEmpty())restoreRecovery(recoveryEntries_.first().second);});
    connect(recoveryBar_->findChild<QToolButton*>("epubRecoveryRemove"),&QToolButton::clicked,this,[this]{if(!recoveryEntries_.isEmpty())removeRecovery(recoveryEntries_.first().second);});
    connect(recoveryBar_->findChild<QToolButton*>("epubRecoveryDismiss"),&QToolButton::clicked,this,[this]{recoveryDismissed_=true;recoveryBar_->hide();});
    connect(recoveryBar_->findChild<QToolButton*>("epubRecoveryOther"),&QToolButton::clicked,this,[this]{
        QStringList Labels;for(const auto& Entry:recoveryEntries_)Labels.append(Entry.first);if(Labels.isEmpty())return;bool Ok=false;
        const auto Label=hc::ui::InputDialog::getItem(this,tr("EPUB 편집 초안 복원"),tr("복원할 초안을 고르세요. 원본 EPUB은 변경하지 않습니다."),Labels,0,false,&Ok);
        if(Ok)for(const auto& Entry:recoveryEntries_)if(Entry.first==Label){restoreRecovery(Entry.second);break;}
    });
    connect(&draftFinder_,&QFutureWatcher<QList<epub::DraftInfo>>::finished,this,[this]{
        const auto Entries=draftFinder_.result();recoveryEntries_.clear();
        const auto Own=QDir(recoveryRoot_).filePath(recoverySession_+".hcedraft");
        for(const auto& Entry:Entries)if(Entry.path_!=Own) {
            const auto Label=Entry.error_.isEmpty()?tr("%1 · %2").arg(QFileInfo(Entry.sourcePath_).fileName(),QDateTime::fromMSecsSinceEpoch(Entry.utcMs_).toString("yyyy-MM-dd HH:mm:ss")):tr("검사할 수 없는 초안 · %1").arg(QFileInfo(Entry.path_).fileName());
            recoveryEntries_.append({Label,Entry.path_});
        }
        if(!recoveryEntries_.isEmpty())recoveryText_->setText(recoveryEntries_.size()==1?tr("이전 세션의 편집 초안이 있습니다 · %1").arg(recoveryEntries_.first().first):tr("이전 세션의 편집 초안이 %1개 있습니다 · 최근 %2").arg(recoveryEntries_.size()).arg(recoveryEntries_.first().first));
        recoveryBar_->findChild<QToolButton*>("epubRecoveryOther")->setVisible(recoveryEntries_.size()>1);recoveryBar_->setVisible(!recoveryEntries_.isEmpty() && !recoveryDismissed_);emit StateChanged();
    });
    connect(&draftWriter_,&QFutureWatcher<epub::DraftWrite>::finished,this,[this]{
        const auto Result=draftWriter_.result();const auto Own=QDir(recoveryRoot_).filePath(recoverySession_+".hcedraft");
        if(Result && Result.path_==Own && !discardedRecovery_){recoveryPath_=Result.path_;draftWrittenToken_=draftWritingToken_;recoveryError_.clear();draftSavedAt_=QDateTime::currentMSecsSinceEpoch();reportDraft(tr("편집 초안을 자동 복구용으로 보관했습니다."));}
        else if(!Result && Result.error_!="epub-draft-cancelled"){recoveryError_=Result.error_;reportDraft(tr("초안 자동 보관 실패: %1 · 편집 내용은 유지됩니다.").arg(recoveryError_));report(draftMessage_);}
        if(!pendingRecoveryPath_.isEmpty()){const auto Path=std::exchange(pendingRecoveryPath_,{});if(Result)beginRecoveryRead(Path);else{setSourceReadOnly(false);report(tr("현재 편집 내용을 보관하지 못해 복원을 중단했습니다."));}}
        else if(Result && ready_ && recoveryEnabled_ && !discardedRecovery_ && draftWrittenToken_!=Token())draftTimer_.start();
        emit StateChanged();
    });
    connect(&draftReader_,&QFutureWatcher<epub::DraftRead>::finished,this,[this]{
        const auto Result=draftReader_.result();if(!Result){recoveryError_=Result.error_;reportDraft(tr("복구 초안을 읽지 못했습니다: %1 · 기존 편집과 초안 파일은 유지됩니다.").arg(recoveryError_));report(draftMessage_);setSourceReadOnly(false);emit StateChanged();return;}
        pendingRecovery_=Result.snapshot_;recoveryReadingSourceChanged_=Result.sourceChanged_;recoveryError_.clear();load(Result.snapshot_.original_);
    });
    reportDraft(recoveryEnabled_?tr("편집 초안 자동 복구가 켜져 있습니다."):tr("편집 초안 자동 복구가 꺼져 있습니다. 기존 초안은 복구 목록에서 관리할 수 있습니다."));
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
        pendingRecovery_.reset();ready_=!model_.Publication().isEmpty();setEnabled(true);setSourceReadOnly(false);recoveryError_=Error;reportDraft(tr("복구 내용 검사 실패: %1 · 현재 편집과 초안 파일을 유지합니다.").arg(Error));report(draftMessage_);emit StateChanged();return;
    }
    const bool First=model_.Publication().isEmpty();
    model_=std::move(Model);ready_=!model_.Publication().isEmpty();error_=model_.Error();setEnabled(true);
    report(ready_?tr("변경 내용은 사본으로 저장합니다. EPUBCheck 검사는 설정의 Java/JAR를 사용합니다."):error_);
    if(ready_) {
        if(!Restoring)sourceHashAtOpen_=QByteArray::fromHex(model_.Publication()["sourceSha256"].toString().toLatin1());
        reference_=Restoring?QCryptographicHash::hash(pendingRecovery_->original_,QCryptographicHash::Sha256):model_.Fingerprint();
        QStringList SpinePaths;for(const auto& Value:model_.Publication()["spine"].toArray())SpinePaths.append(Value.toObject()["path"].toString());setProperty("epubEditionHash",model_.Publication()["sourceSha256"].toString());setProperty("epubSpinePaths",SpinePaths);
        refreshTree();if(First || !layoutRestored_)restoreLayout();
        // The first editable resource in reading order opens first.
        QString Initial=current_;if(!model_.IsEditable(Initial)){Initial.clear();for(const auto& Path:SpinePaths)if(model_.IsEditable(Path)){Initial=Path;break;}if(Initial.isEmpty())for(const auto& Value:model_.Publication()["resources"].toArray())if(model_.IsEditable(Value.toObject()["path"].toString())){Initial=Value.toObject()["path"].toString();break;}}
        if(!(First && !Restoring && !commitRestore_ && restoreWorkspace())){current_=Initial;select();}
        if(Restoring) {
            const auto Snapshot=std::exchange(pendingRecovery_,std::nullopt).value();baseToken_=QCryptographicHash::hash(Snapshot.original_,QCryptographicHash::Sha256);
            // The draft is written into the shown document, which stays locked until unlocked here.
            setSourceReadOnly(false);
            if(model_.IsEditable(Snapshot.currentResource_)){current_=Snapshot.currentResource_;select();}
            if(Snapshot.hasCurrentDraft_) {
                if(current_!=Snapshot.currentResource_ || !sourceShown()){recoveryError_="epub-draft-selected-resource";reportDraft(tr("선택한 리소스 초안은 수동 확인이 필요합니다."));}
                else {updating_=true;static_cast<ScintillaQtDirectBackend*>(text_->Backend())->Send(SCI_SETTEXT,0,reinterpret_cast<qintptr>(Snapshot.currentDraft_.constData()));setSourceSelection(Snapshot.cursor_,Snapshot.cursor_);setScroll(Snapshot.scroll_,0);updating_=false;refreshTabs();}
            }
            recoveryLocator_=Snapshot.locator_;recoverySourceChanged_=recoveryReadingSourceChanged_;recoveredFrom_=readingRecoveryPath_;
            previewPreferredChapter_=-1;previewRestoreChapter_=Snapshot.readingChapter_;previewRecoveryPosition_=true;
            const auto Spine=model_.Publication()["spine"].toArray();
            if(previewRestoreChapter_<0 && !recoveryLocator_.isEmpty())for(int I=0;I<Spine.size();++I)if(Spine[I].toObject()["path"]==current_)previewRestoreChapter_=I;
            protectSourcePath(Snapshot.sourcePath_);sourcePath_=Snapshot.sourcePath_;
            resetRecoverySession();
            reportDraft(recoverySourceChanged_?tr("원본의 변경 또는 삭제를 감지했습니다. 보관된 판본의 초안을 복원했습니다. 다른 이름으로 저장하여 비교하세요."):tr("보관된 편집 초안을 복원했습니다. 원본 EPUB은 변경하지 않았습니다."));report(draftMessage_);
            emit Changed();
        }
    }
    if(ready_ && commitRestore_ && !Restoring) {
        if(model_.IsEditable(commitResource_) && commitResource_!=current_){current_=commitResource_;select();}
        if(sourceShown())setSourceSelection(commitAnchor_,commitPosition_,false);
        QTimer::singleShot(0,this,[this]{if(sourceShown())setScroll(commitVertical_,commitHorizontal_);if(commitFocus_ && previewOwnerActive())focusSource();});
    }
    commitRestore_=false;refreshRecovery();updateActions();emit StateChanged();
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
    // Draft positions stay UTF-16 offsets and first lines so earlier drafts restore unchanged.
    const auto Current=draft();Snapshot.hasCurrentDraft_=Current.contains(current_);Snapshot.currentDraft_=Current.value(current_);Snapshot.cursor_=sourceShown()?sourcePosition():0;Snapshot.scroll_=sourceShown()?firstVisibleLine():0;
    draftWritingToken_=Token();reportDraft(tr("편집 초안을 자동 복구용으로 보관하고 있습니다…"));
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
    if(!Enabled){draftTimer_.stop();draftSession_->cancelled_.store(true);reportDraft(tr("자동 복구가 꺼져 있습니다. 기존 초안 파일은 유지합니다."));}
    else {draftSession_=std::make_shared<DraftSession>();if(ready_ && Token()!=baseToken_)draftTimer_.start();reportDraft(tr("편집 초안 자동 복구가 켜져 있습니다."));}
    emit StateChanged();
}
void EpubSourceEditor::restoreRecovery(const QString& Path) {
    if(structuring_){report(tr("구조 작업이 끝난 뒤 초안을 복원해 주세요."));return;}
    if(Path.isEmpty() || pendingRecovery_ || draftReader_.isRunning() || worker_.isRunning() || validation_.isRunning())return;
    if(ready_ && Token()!=baseToken_ && draftWrittenToken_!=Token()) {
        if(!recoveryEnabled_){reportDraft(tr("현재 편집 내용이 있습니다. 사본을 저장하거나 자동 복구를 켠 뒤 복원하세요."));report(draftMessage_);return;}
        pendingRecoveryPath_=Path;setSourceReadOnly(true);if(!draftWriter_.isRunning())Checkpoint();return;
    }
    beginRecoveryRead(Path);
}
void EpubSourceEditor::beginRecoveryRead(const QString& Path) {
    readingRecoveryPath_=Path;setSourceReadOnly(true);reportDraft(tr("보관된 초안을 검사하고 있습니다…"));recoveryDismissed_=true;recoveryBar_->hide();
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
