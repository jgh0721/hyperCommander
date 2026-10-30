#include "stdafx.h"
#include "epubSourceEditor.hpp"
#include "UniqueLibs/nativeFilePath.hpp"
#include "epubResourceTree.hpp"
#include "text/QTextView.h"
#include "text/ITextEditorBackend.h"
#include "text/ScintillaQtDirectBackend.h"
#include <Windows.h>
#include <QAction>
#include <QBuffer>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QInputMethodEvent>
#include <QJsonArray>
#include <QLabel>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStackedWidget>
#include <QTabBar>
#include <QTemporaryDir>
#include <QTreeView>
#include <QtConcurrentRun>
#include <Scintilla.h>
namespace hc::document {
namespace {
QByteArray identity(const QString& Path) {
    const auto Native=QDir::toNativeSeparators(QFileInfo(Path).absoluteFilePath());
    const auto Handle=CreateFileW(reinterpret_cast<LPCWSTR>(hc::storage::native::FilePath(Native).utf16()),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,nullptr);
    if(Handle==INVALID_HANDLE_VALUE)return {};FILE_ID_INFO Info{};const bool Ok=GetFileInformationByHandleEx(Handle,FileIdInfo,&Info,sizeof(Info));CloseHandle(Handle);return Ok?QByteArray(reinterpret_cast<const char*>(&Info),sizeof(Info)):QByteArray{};
}
bool raster(const QString& Media){return QStringList{"image/png","image/jpeg","image/gif","image/webp"}.contains(Media);}
}
void EpubSourceEditor::protectSourcePath(const QString& Path){protectedPaths_.append(QFileInfo(Path).absoluteFilePath().toCaseFolded());const auto Id=identity(Path);if(!Id.isEmpty())identities_.append(Id);}
EpubSourceEditor::EpubSourceEditor(QByteArray Bytes,const QString& Path,QWidget* Parent,const QByteArray& SourceHash):QWidget(Parent) {
    protectSourcePath(Path);
    baseToken_=SourceHash.isEmpty()?QCryptographicHash::hash(Bytes,QCryptographicHash::Sha256):SourceHash;sourceHashAtOpen_=baseToken_;sourcePath_=Path;
    setupLayout();setupActions();setupStatus();setupScrollSync();
    sourceSyncTimer_.setSingleShot(true);sourceSyncTimer_.setInterval(180);connect(&sourceSyncTimer_,&QTimer::timeout,this,&EpubSourceEditor::locateSource);
    connect(text_->Backend(),&ITextEditorBackend::cursorPositionChanged,this,[this]{if(!updating_ && SourceInput()->hasFocus() && action("epubPreviewSync")->isChecked()){sourceSyncPending_=true;sourceSyncAttempts_=0;sourceSyncTimer_.start();}updateStatus();});
    connect(text_->Backend(),&ITextEditorBackend::selectionChanged,this,&EpubSourceEditor::updateActions);
    connect(text_,&QTextView::ContentChanged,this,[this]{
        if(updating_ || !sourceShown())return;bookUndoPriority_=bookRedoPriority_=false;
        if(text_->Backend()->documentLength()>2*1024*1024){const QSignalBlocker Block(text_);static_cast<ScintillaQtDirectBackend*>(text_->Backend())->Send(SCI_UNDO);report(tr("소스 편집은 리소스당 2 MiB까지 지원합니다."));}
        refreshTabs();emit Changed();
    });
    setupRecovery();setupValidation();setupStructure();
    connect(this,&EpubSourceEditor::Changed,this,[this]{if(validationCancel_ && validationToken_!=Token())validationCancel_->store(true);});
    connect(this,&EpubSourceEditor::StateChanged,this,[this]{updateActions();updateStatus();});
    connect(&worker_,&QFutureWatcher<epub::EditModel>::finished,this,[this]{modelLoaded(worker_.result());});
    connect(&validation_,&QFutureWatcher<epub::EditOutput>::finished,this,[this]{
        const auto Result=validation_.result();nativeValidating_=false;if(validationToken_!=Token()){report(tr("검사 중 소스가 변경되었습니다. 다시 검사해 주세요."));emit StateChanged();return;}
        nativeDiagnostic(Result);
        if(!Result){error_=Result.error_+": "+Result.location_;report(Result.error_=="epub-edit-spine-repair-required"?tr("읽기 순서에 누락된 참조가 있어 저장할 수 없습니다. OPF에서 해당 참조를 수정한 후 다시 검사해 주세요. 원본은 변경하지 않았습니다."):tr("소스 검사를 통과하지 못했습니다. %1").arg(error_));validatedBytes_.clear();}
        else {cachedExport_=Result;cachedExportToken_=validationToken_;error_.clear();validatedToken_=validationToken_;validatedBytes_=Result.bytes_;report(tr("기본 구조·XML·CSS 괄호 검사 통과 · EPUBCheck 검사로 표준 오류를 확인할 수 있습니다."));}
        if(Result && checkerRequested_){startEpubCheck();return;}
        emit StateChanged();if(Result && IsValidated() && saveAfter_)emit SaveRequested();saveAfter_=false;
    });load(std::move(Bytes));
}
EpubSourceEditor::~EpubSourceEditor(){
    if(validationCancel_)validationCancel_->store(true);if(previewCancel_)previewCancel_->store(true);if(structureCancel_)structureCancel_->store(true);stopEpubCheck();RequestPreviewClose();if(ready_ && !discardedRecovery_ && recoveryEnabled_)Checkpoint();if(cancel_)cancel_->store(true);
    saveLayout();for(const auto& Path:sourceBuffers_.keys())releaseSource(Path);
    // The workspace may have adopted the strip into its status row.
    if(statusStrip_ && statusStrip_->parent()!=this)delete statusStrip_;
}
void EpubSourceEditor::load(QByteArray Bytes) {
    saveSourceState();bookUndoPriority_=bookRedoPriority_=false;ready_=false;setEnabled(false);report(tr("EPUB 편집 구조를 검사하고 있습니다…"));cancel_=std::make_shared<std::atomic_bool>(false);auto Cancel=cancel_;
    worker_.setFuture(QtConcurrent::run([Bytes=std::move(Bytes),Cancel]{epub::EditModel Model;Model.Load(Bytes,[Cancel]{return Cancel->load();});return Model;}));
}
void EpubSourceEditor::report(const QString& Text){message_=Text;updateStatus();}
void EpubSourceEditor::reportDraft(const QString& Text){draftMessage_=Text;updateStatus();}
void EpubSourceEditor::reportPreview(const QString& Text){previewMessage_=Text;updateStatus();}
QWidget* EpubSourceEditor::SourceInput()const{return text_->Backend()->widget();}
QString EpubSourceEditor::mediaOf(const QString& Path)const{for(const auto& Value:model_.Publication()["resources"].toArray())if(Value.toObject()["path"]==Path)return Value.toObject()["mediaType"].toString();return {};}
QMap<QString,QByteArray> EpubSourceEditor::draft()const {if(ready_ && !current_.isEmpty() && sourceShown() && sourceModified())return {{current_,sourceBytes()}};return {};}
QByteArray EpubSourceEditor::Token()const {if(!ready_)return baseToken_;const auto Value=model_.Fingerprint(draft());return Value==reference_?baseToken_:Value;}
bool EpubSourceEditor::flush(){
    if(!ready_ || sourceComposing_)return false;const auto Draft=draft();
    for(auto I=Draft.begin();I!=Draft.end();++I){const auto Error=model_.Replace(I.key(),I.value());if(!Error.isEmpty()){report(Error);return false;}if(sourceBuffers_.contains(I.key()))sourceBuffers_[I.key()].applied_=I.value();touchedResources_.insert(I.key());}
    if(!Draft.isEmpty()){text_->CommitContent();refreshTabs();if(resourceModel_)resourceModel_->Refresh();emit Changed();}return true;
}
bool EpubSourceEditor::openResource(const QString& Path) {
    if(!ready_ || !model_.IsEditable(Path))return false;
    if(Path==current_){selectResource(Path);refreshTabs();return true;}
    if(!flush()){selectResource(current_);refreshTabs();return false;}
    current_=Path;select();
    // A resource chosen in the tree or tabs takes a linked preview to its caret.
    if(!previewSyncing_ && action("epubPreviewSync")->isChecked()){sourceSyncPending_=true;sourceSyncAttempts_=0;sourceSyncTimer_.start();}
    return true;
}
void EpubSourceEditor::select() {
    if(!ready_ || current_.isEmpty())return;saveSourceState();const auto Data=model_.Read(current_);const auto Media=mediaOf(current_);const bool Raster=raster(Media);
    updating_=true;sourceStack_->setCurrentWidget(Raster?static_cast<QWidget*>(image_):static_cast<QWidget*>(text_));
    if(Raster){QBuffer Buffer;Buffer.setData(Data);Buffer.open(QIODevice::ReadOnly);QImageReader Reader(&Buffer);Reader.setAutoTransform(true);const auto Size=Reader.size();const auto Image=Size.isValid() && Size.width()<=4096 && Size.height()<=4096 && qint64(Size.width())*Size.height()<=4194304?Reader.read():QImage{};image_->setPixmap(QPixmap::fromImage(Image.scaled(640,420,Qt::KeepAspectRatio,Qt::SmoothTransformation)));image_->setAccessibleName(tr("이미지 미리보기: %1").arg(current_));}
    else activateSource(Data,Media);updating_=false;
    if(!openResources_.contains(current_))openResources_.append(current_);
    // A newly shown resource is aligned once its chapter is in the preview.
    scrollSyncPending_=true;lastScrollLine_=-1;
    refreshTabs();selectResource(current_);markDiagnostics();updateActions();updateStatus();
}
void EpubSourceEditor::importImage() {
    if(structuring_)return;
    if(!ready_ || !raster(mediaOf(current_))){report(tr("교체할 PNG·JPEG·GIF·WebP 리소스를 먼저 선택해 주세요. 경로와 형식은 유지합니다."));return;}
    const auto Path=QFileDialog::getOpenFileName(this,tr("같은 형식의 이미지로 교체"),{},tr("이미지 (*.png *.jpg *.jpeg *.gif *.webp)"));if(Path.isEmpty())return;QFile File(Path);if(!File.open(QIODevice::ReadOnly) || File.size()>8*1024*1024){report(tr("이미지 입력 한도는 8 MiB입니다."));return;}
    const auto Error=model_.Replace(current_,File.read(8*1024*1024+1));if(!Error.isEmpty()){report(Error);return;}touchedResources_.insert(current_);select();emit Changed();
}
bool EpubSourceEditor::CanUndo()const{return ready_ && !structuring_ && ((sourceShown() && static_cast<ScintillaQtDirectBackend*>(text_->Backend())->Send(SCI_CANUNDO)) || model_.CanUndo());}
bool EpubSourceEditor::CanRedo()const{return ready_ && !structuring_ && ((sourceShown() && static_cast<ScintillaQtDirectBackend*>(text_->Backend())->Send(SCI_CANREDO)) || model_.CanRedo());}
void EpubSourceEditor::Undo(){
    if(!ready_ || structuring_)return;auto* Backend=static_cast<ScintillaQtDirectBackend*>(text_->Backend());
    if(!(bookUndoPriority_ && model_.CanUndo()) && sourceShown() && Backend->Send(SCI_CANUNDO))Backend->Send(SCI_UNDO);else{model_.Undo();bookUndoPriority_=model_.CanUndo();bookRedoPriority_=true;refreshResources(current_);emit Changed();}
}
void EpubSourceEditor::Redo(){
    if(!ready_ || structuring_)return;auto* Backend=static_cast<ScintillaQtDirectBackend*>(text_->Backend());
    if(!(bookRedoPriority_ && model_.CanRedo()) && sourceShown() && Backend->Send(SCI_CANREDO))Backend->Send(SCI_REDO);else{model_.Redo();bookUndoPriority_=true;bookRedoPriority_=model_.CanRedo();refreshResources(current_);emit Changed();}
}
QJsonObject EpubSourceEditor::Snapshot()const {
    QJsonArray Rows;for(const auto& Value:model_.Publication()["resources"].toArray()){const auto Row=Value.toObject();if(model_.IsEditable(Row["path"].toString()))Rows.append(QJsonObject{{"path",Row["path"]},{"mediaType",Row["mediaType"]}});}
    const auto State=sourceBuffers_.value(current_);
    const QJsonObject Source{{"resource",current_},{"buffer",QString::number(State.serial_)},{"shown",sourceShown()},{"modified",ready_ && sourceShown() && sourceModified()},{"readOnly",sourceReadOnly_},{"language",text_->currentLanguage()}};
    const QJsonObject Layout{{"orientation",previewSplitter_->orientation()==Qt::Horizontal?"horizontal":"vertical"},{"tree",!treePanel_->isHidden()},{"preview",!previewPanel_->isHidden()},{"problems",!problemsPanel_->isHidden()}};
    const QJsonObject Status{{"message",message_},{"draft",draftMessage_},{"preview",previewMessage_},{"validation",statusValidation_->text()}};
    return {{"exportWork",QJsonObject{{"runs",exportRuns_},{"joins",exportJoins_}}},{"ready",ready_},{"error",error_},{"resources",Rows},{"current",current_},{"tabs",QJsonArray::fromStringList(openResources_)},{"source",Source},{"layout",Layout},{"status",Status},{"canUndo",CanUndo()},{"canRedo",CanRedo()},{"saveAsOnly",true},{"structuring",structuring_},{"structureError",structureError_},{"validating",nativeValidating_ || checkerState_=="running" || structuring_},{"standardValidation",validationSnapshot()},{"validated",IsValidated()},{"preview",previewSnapshot()},{"scrollSync",QJsonObject{{"active",scrollSyncActive()},{"requests",sourceScrollSerial_},{"line",lastScrollLine_},{"pending",scrollSyncPending_},{"stale",sourceBuffers_.value(current_).shiftsOverflow_}}},{"sourceVisible",IsSourceVisible()},{"recoveryEnabled",recoveryEnabled_},{"recoveryBusy",draftWriter_.isRunning()||draftReader_.isRunning()||draftFinder_.isRunning()||pendingRecovery_.has_value()},{"recoverySaved",!recoveryPath_.isEmpty()&&draftWrittenToken_==Token()},{"recoveryPath",recoveryPath_},{"recoveryCandidates",recoveryEntries_.size()},{"recoveryError",recoveryError_},{"recoverySourceChanged",recoverySourceChanged_}};
}
QJsonObject EpubSourceEditor::Execute(const QJsonObject& Request) {
    const auto Action=Request["action"].toString();
    if(Action=="epub.edit.preview"){requestPreview(true);return {{"accepted",previewRuntime_.has_value()}};}
    if(Action=="epub.edit.source-visible"){SetSourceVisible(Request["visible"].toBool(true));return {{"accepted",true}};}
    if(Action=="epub.edit.recover"){restoreRecovery(Request["path"].toString());return {{"accepted",true}};}
    if(Action=="epub.edit.structure-cancel"){if(structureCancel_)structureCancel_->store(true);return {{"accepted",true}};}
    if(Action=="epub.edit.layout") {
        if(Request.contains("orientation"))applyOrientation(Request["orientation"]=="vertical"?Qt::Vertical:Qt::Horizontal);
        for(const auto& Panel:{QString("tree"),QString("preview"),QString("problems")})if(Request.contains(Panel))setPanelVisible(Panel,Request[Panel].toBool());
        return {{"accepted",true}};
    }
    if(Action=="epub.edit.source-state") {
        const auto Text=sourceText();bool Styled=false;
        if(sourceShown()){auto* Backend=static_cast<ScintillaQtDirectBackend*>(text_->Backend());const auto End=qMin<qintptr>(Backend->Send(SCI_GETLENGTH),4096);Backend->Send(SCI_COLOURISE,0,End);for(qintptr I=0;I<End && !Styled;++I)Styled=Backend->Send(SCI_GETSTYLEAT,I)!=0;}
        return {{"resource",current_},{"text",Text},{"position",sourcePosition()},{"anchor",sourceAnchor()},{"modified",sourceShown() && sourceModified()},{"buffer",QString::number(sourceBuffers_.value(current_).serial_)},{"styled",Styled},{"focus",SourceInput()->hasFocus()},{"shown",sourceShown()}};
    }
    if(Action=="epub.edit.focus-source"){focusSource();return {{"accepted",sourceShown()}};}
    if(!ready_ || structuring_)return {{"error","epub-edit-not-ready-or-busy"}};
    if(Action=="epub.edit.select")return openResource(Request["resource"].toString())?QJsonObject{{"accepted",true}}:QJsonObject{{"error",model_.IsEditable(Request["resource"].toString())?"epub-edit-draft-invalid":"epub-edit-resource"}};
    if(Action=="epub.edit.cursor") {
        if(!sourceShown())return {{"error","epub-edit-source-hidden"}};const int Position=Request["position"].toInt(-1);if(Position<0)return {{"error","epub-edit-cursor"}};
        // Automation moves the caret without asking the preview to follow.
        updating_=true;setSourceSelection(Request["anchor"].toInt(Position),Position);updating_=false;updateStatus();return {{"accepted",true}};
    }
    if(Action=="epub.edit.insert"){if(!sourceShown() || sourceReadOnly_)return {{"error","epub-edit-source-hidden"}};insertSource(Request["text"].toString());return {{"accepted",true}};}
    if(Action=="epub.edit.close-resource") {
        const auto Path=Request["resource"].toString();const auto Mode=Request["mode"].toString("apply");if(!openResources_.contains(Path))return {{"error","epub-edit-resource"}};
        if(Mode=="discard")revertResource(Path);else if(Path==current_ && !flush())return {{"error","epub-edit-draft-invalid"}};
        return closeResource(Path,false)?QJsonObject{{"accepted",true}}:QJsonObject{{"error","epub-edit-resource"}};
    }
    if(Action=="epub.edit.command")return RunCommand(Request["command"].toString())?QJsonObject{{"accepted",true}}:QJsonObject{{"error","epub-edit-command"}};
    if(Action=="epub.edit.find-all"){findInResources(Request["text"].toString());return {{"accepted",true},{"results",searchResults_->topLevelItemCount()}};}
    if(Action=="epub.edit.wrap"){if(!sourceShown() || text_->Backend()->selectedText().isEmpty())return {{"error","epub-edit-selection"}};wrapSelection(Request["tag"].toString());return {{"accepted",true}};}
    if(Action=="epub.edit.quick-fix"){const auto Fix=quickFixAt(sourcePosition());if(Fix.isEmpty())return {{"error","epub-edit-quick-fix"}};quickFix();return {{"accepted",true}};}
    if(Action=="epub.edit.open-link")return openLink()?QJsonObject{{"accepted",true}}:QJsonObject{{"error","epub-edit-link"}};
    if(Action=="epub.edit.metadata-inspect"){if(!flush())return {{"error","epub-edit-draft-invalid"}};return model_.Metadata();}
    if(Action=="epub.edit.image-inspect"){if(!flush())return {{"error","epub-edit-draft-invalid"}};return model_.InspectImage(Request["resource"].toString());}
    if(Action=="epub.edit.preview-source")return {{"accepted",selectPreviewSource()}};
    if(Action=="epub.edit.source-locate"){sourceSyncPending_=true;sourceSyncAttempts_=0;locateSource();return {{"accepted",true}};}
    if(Action=="epub.edit.structure")return structure(Request)?QJsonObject{{"accepted",true}}:QJsonObject{{"error","epub-edit-structure-not-ready"}};
    if(Action=="epub.edit.checkpoint"){Checkpoint();return {{"accepted",true}};}
    if(Action=="epub.edit.recovery-list"){QJsonArray Entries;for(const auto& Entry:recoveryEntries_)Entries.append(QJsonObject{{"path",Entry.second},{"label",Entry.first}});return {{"entries",Entries}};}
    if(Action=="epub.edit.validate"){validate(false,Request["standard"].toBool());return {{"accepted",true}};}
    if(Action=="epub.edit.validation-cancel"){cancelEpubCheck("cancelled");return {{"accepted",true}};}
    if(Action=="epub.edit.diagnostic")return {{"accepted",selectDiagnostic(Request["index"].toInt(-1))}};
    if(Action=="epub.edit.replace") {
        if(!flush())return {{"error","epub-edit-draft-invalid"}};const auto Path=Request["resource"].toString();if(!model_.IsEditable(Path))return {{"error","epub-edit-resource"}};
        QByteArray Data;if(raster(mediaOf(Path))){const auto Decoded=QByteArray::fromBase64Encoding(Request["base64"].toString().toLatin1(),QByteArray::AbortOnBase64DecodingErrors);if(!Decoded)return {{"error","epub-edit-image-encoding"}};Data=Decoded.decoded;}else Data=Request["text"].toString().toUtf8();
        const auto Error=model_.Replace(Path,Data);if(!Error.isEmpty())return {{"error",Error}};
        touchedResources_.insert(Path);current_=Path;select();emit Changed();return {{"accepted",true}};
    }
    return {{"error","epub-edit-action"}};
}
storage::Result<QByteArray> EpubSourceEditor::Export(const QString& Destination)const {
    if(!ready_ || QFileInfo(Destination).suffix().compare("epub",Qt::CaseInsensitive)!=0)return storage::Error{storage::ErrorCategory::Unsupported,"epub-edit-save-as-epub-required"};
    if(protectedPaths_.contains(QFileInfo(Destination).absoluteFilePath().toCaseFolded()) || (!identity(Destination).isEmpty() && identities_.contains(identity(Destination))))return storage::Error{storage::ErrorCategory::AccessDenied,"epub-edit-source-overwrite-denied"};
    if(!IsValidated())return storage::Error{storage::ErrorCategory::InvalidRequest,"epub-edit-validation-required"};return validatedBytes_;
}
void EpubSourceEditor::validate(bool SaveAfter,bool Standard) {
    ReloadValidationSettings();
    if(sourceComposing_ || !ready_ || draftReader_.isRunning() || pendingRecovery_ || !pendingRecoveryPath_.isEmpty() || nativeValidating_ || checkerState_=="running" || structuring_)return;checkerRequested_=Standard || standardValidationRequired();validatedBytes_.clear();validationToken_=Token();saveAfter_=SaveAfter;report(tr("패키지·XML·CSS 괄호·참조·이미지를 검사하고 있습니다…"));auto Model=model_;const auto Draft=draft();
    if(previewWorker_.isRunning() && previewWorkToken_==validationToken_ && previewCancel_ && !previewCancel_->load()) {
        validationCancel_=previewCancel_;nativeValidating_=true;++exportJoins_;validation_.setFuture(previewWorker_.future());emit StateChanged();return;
    }
    validationCancel_=std::make_shared<std::atomic_bool>(false);const auto Cancel=validationCancel_;
    const auto Cached=cachedExportToken_==validationToken_?cachedExport_:epub::EditOutput{};
    if(!Cached)++exportRuns_;nativeValidating_=true;validation_.setFuture(QtConcurrent::run([Model=std::move(Model),Draft,Cancel,Cached]{if(Cancel->load())return epub::EditOutput{{},"cancelled",{}};return Cached?Cached:Model.Export(Draft,[Cancel]{return Cancel->load();});}));emit StateChanged();
}
bool EpubSourceEditor::eventFilter(QObject* Object,QEvent* Event) {
    if(Object==SourceInput() && Event->type()==QEvent::InputMethod) {
        sourceComposing_=!static_cast<QInputMethodEvent*>(Event)->preeditString().isEmpty();text_->setProperty("epubSourceComposing",sourceComposing_);
        if(!sourceComposing_)QTimer::singleShot(0,this,[this]{if(sourceComposing_)return;if(previewCandidate_)admitPreview(previewCandidate_);if(action("epubAutoPreview")->isChecked())previewTimer_.start();});
    }
    if(Object==SourceInput() && Event->type()==QEvent::FocusIn)updateActions();
    return QWidget::eventFilter(Object,Event);
}
void EpubSourceEditor::Commit(const QString& Path,const QByteArray& Bytes){commitRestore_=true;commitResource_=current_;commitPosition_=sourcePosition();commitAnchor_=sourceAnchor();commitVertical_=firstVisibleLine();commitHorizontal_=horizontalOffset();commitFocus_=SourceInput()->hasFocus();DiscardRecovery();resetRecoverySession();sourcePath_=Path;baseToken_=Token();protectSourcePath(Path);touchedResources_.clear();
    // Only the immutable model produced by validation of these exact bytes may
    // bypass loading. Saving a stale revision still follows the normal load path.
    const auto Prepared=cachedExport_.prepared_;
    if(Prepared && cachedExport_.bytes_==Bytes) {
        sourceHashAtOpen_=QByteArray::fromHex(Prepared->Publication()["sourceSha256"].toString().toLatin1());
        saveSourceState();bookUndoPriority_=bookRedoPriority_=false;modelLoaded(*Prepared);
    } else load(Bytes);}
}
