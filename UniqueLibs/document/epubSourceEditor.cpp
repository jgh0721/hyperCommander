#include "stdafx.h"
#include "epubSourceEditor.hpp"
#include "documentThemeAdapter.hpp"
#include <Windows.h>
#include <QComboBox>
#include <QCheckBox>
#include <QDir>
#include <QImageReader>
#include <QBuffer>
#include <QCryptographicHash>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QImage>
#include <QJsonArray>
#include <QLabel>
#include <QPlainTextEdit>
#include <QInputMethodEvent>
#include <QScrollArea>
#include <QScrollBar>
#include <QTextCursor>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSplitter>
#include <QTemporaryDir>
#include <QVBoxLayout>
#include <QtConcurrentRun>
namespace hc::document {
namespace {
QByteArray identity(const QString& Path) {
    const auto Native=QDir::toNativeSeparators(QFileInfo(Path).absoluteFilePath());
    const auto Handle=CreateFileW(reinterpret_cast<LPCWSTR>(Native.utf16()),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,nullptr);
    if(Handle==INVALID_HANDLE_VALUE)return {};FILE_ID_INFO Info{};const bool Ok=GetFileInformationByHandleEx(Handle,FileIdInfo,&Info,sizeof(Info));CloseHandle(Handle);return Ok?QByteArray(reinterpret_cast<const char*>(&Info),sizeof(Info)):QByteArray{};
}
}
void EpubSourceEditor::protectSourcePath(const QString& Path){protectedPaths_.append(QFileInfo(Path).absoluteFilePath().toCaseFolded());const auto Id=identity(Path);if(!Id.isEmpty())identities_.append(Id);}
EpubSourceEditor::EpubSourceEditor(QByteArray Bytes,const QString& Path,QWidget* Parent,const QByteArray& SourceHash):QWidget(Parent) {
    protectSourcePath(Path);
    baseToken_=SourceHash.isEmpty()?QCryptographicHash::hash(Bytes,QCryptographicHash::Sha256):SourceHash;sourceHashAtOpen_=baseToken_;sourcePath_=Path;
    auto* Outer=new QVBoxLayout(this);Outer->setContentsMargins(0,0,0,0);previewSplitter_=new QSplitter(this);previewSplitter_->setChildrenCollapsible(false);Outer->addWidget(previewSplitter_);sourceScroll_=new QScrollArea(previewSplitter_);sourceScroll_->setObjectName("epubSourceScroll");sourceScroll_->setWidgetResizable(true);sourceScroll_->setMinimumSize(0,0);sourcePanel_=new QWidget;sourceScroll_->setWidget(sourcePanel_);sourceScroll_->viewport()->installEventFilter(this);auto* Layout=new QVBoxLayout(sourcePanel_);Layout->setContentsMargins(4,4,4,4);
    auto* Note=new QLabel(tr("EPUB 소스 편집 · XHTML / CSS / OPF 메타데이터 / 이미지\n편집 초안은 자동 복구 설정에 따라 보관됩니다. 미리보기는 마지막으로 검사를 통과한 편집 내용을 표시합니다. 저장은 다른 이름으로 진행합니다."),this);Note->setWordWrap(true);Layout->addWidget(Note);
    resources_=new QComboBox(this);resources_->setObjectName("epubEditResources");resources_->setAccessibleName(tr("편집할 EPUB 리소스"));resources_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);resources_->setMinimumContentsLength(24);Layout->addWidget(resources_);
    text_=new QPlainTextEdit(this);text_->setObjectName("epubEditSource");text_->setAccessibleName(tr("EPUB 리소스 소스"));text_->setLineWrapMode(QPlainTextEdit::NoWrap);text_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));DocumentThemeAdapter::Attach(text_);Layout->addWidget(text_,1);
    text_->installEventFilter(this);text_->setMinimumHeight(120);text_->setAttribute(Qt::WA_NativeWindow);setFocusProxy(text_);
    sourceSyncTimer_.setSingleShot(true);sourceSyncTimer_.setInterval(180);connect(&sourceSyncTimer_,&QTimer::timeout,this,&EpubSourceEditor::locateSource);
    connect(text_,&QPlainTextEdit::cursorPositionChanged,this,[this]{if(!updating_ && text_->hasFocus() && previewSynchronize_ && previewSynchronize_->isChecked()){sourceSyncPending_=true;sourceSyncAttempts_=0;sourceSyncTimer_.start();}});
    image_=new QLabel(this);image_->setAlignment(Qt::AlignCenter);image_->setMinimumHeight(100);image_->hide();Layout->addWidget(image_,1);
    auto* Actions=new QHBoxLayout;auto* Apply=new QPushButton(tr("리소스 변경 반영"),this);auto* Replace=new QPushButton(tr("선택 이미지 교체…"),this);auto* Save=new QPushButton(tr("검증 후 다른 이름으로 저장…"),this);Apply->setObjectName("epubApplyResource");Replace->setObjectName("epubReplaceImage");Save->setObjectName("epubSaveCopy");Actions->setObjectName("epubSourceActions");Actions->addWidget(Apply);Actions->addWidget(Replace);Actions->addWidget(Save);Layout->addLayout(Actions);
    status_=new QLabel(this);status_->setWordWrap(true);Layout->addWidget(status_);
    connect(Apply,&QPushButton::clicked,this,[this]{flush();});connect(Replace,&QPushButton::clicked,this,&EpubSourceEditor::importImage);connect(Save,&QPushButton::clicked,this,[this]{validate(true);});
    connect(resources_,&QComboBox::currentIndexChanged,this,[this]{if(!updating_){if(!flush()){const QSignalBlocker Block(resources_);resources_->setCurrentIndex(resources_->findText(current_));return;}select();}});
    connect(text_,&QPlainTextEdit::textChanged,this,[this]{if(updating_)return;bookUndoPriority_=bookRedoPriority_=false;if(text_->document()->toRawText().replace(QChar(0x2029),QChar('\n')).toUtf8().size()>2*1024*1024){const QSignalBlocker Block(text_);text_->undo();status_->setText(tr("소스 편집은 리소스당 2 MiB까지 지원합니다."));}emit Changed();});
    setupRecovery(Layout);setupValidation(Layout);setupStructure(Layout);
    connect(this,&EpubSourceEditor::Changed,this,[this]{if(validationCancel_ && validationToken_!=Token())validationCancel_->store(true);});
    connect(&worker_,&QFutureWatcher<epub::EditModel>::finished,this,[this]{modelLoaded(worker_.result());});
    connect(&validation_,&QFutureWatcher<epub::EditOutput>::finished,this,[this]{
        const auto Result=validation_.result();nativeValidating_=false;if(validationToken_!=Token()){status_->setText(tr("검사 중 소스가 변경되었습니다. 다시 검사해 주세요."));emit StateChanged();return;}
        nativeDiagnostic(Result);
        if(!Result){error_=Result.error_+": "+Result.location_;status_->setText(Result.error_=="epub-edit-spine-repair-required"?tr("읽기 순서에 누락된 참조가 있어 저장할 수 없습니다. OPF에서 해당 참조를 수정한 후 다시 검사해 주세요. 원본은 변경하지 않았습니다."):tr("소스 검사를 통과하지 못했습니다.\n%1").arg(error_));validatedBytes_.clear();}
        else {cachedExport_=Result;cachedExportToken_=validationToken_;error_.clear();validatedToken_=validationToken_;validatedBytes_=Result.bytes_;status_->setText(tr("기본 구조·XML·CSS 괄호 검사 통과 · EPUBCheck 검사로 표준 오류를 확인할 수 있습니다."));}
        if(Result && checkerRequested_){startEpubCheck();return;}
        emit StateChanged();if(Result && IsValidated() && saveAfter_)emit SaveRequested();saveAfter_=false;
    });load(std::move(Bytes));
}
EpubSourceEditor::~EpubSourceEditor(){if(validationCancel_)validationCancel_->store(true);if(previewCancel_)previewCancel_->store(true);if(structureCancel_)structureCancel_->store(true);stopEpubCheck();RequestPreviewClose();if(ready_ && !discardedRecovery_ && recoveryEnabled_)Checkpoint();if(cancel_)cancel_->store(true);}
void EpubSourceEditor::load(QByteArray Bytes) {
    saveSourceState();bookUndoPriority_=bookRedoPriority_=false;ready_=false;setEnabled(false);status_->setText(tr("EPUB 편집 구조를 검사하고 있습니다…"));cancel_=std::make_shared<std::atomic_bool>(false);auto Cancel=cancel_;
    worker_.setFuture(QtConcurrent::run([Bytes=std::move(Bytes),Cancel]{epub::EditModel Model;Model.Load(Bytes,[Cancel]{return Cancel->load();});return Model;}));
}
QMap<QString,QByteArray> EpubSourceEditor::draft()const {if(ready_ && !current_.isEmpty() && !text_->isHidden() && text_->document()->isModified())return {{current_,text_->document()->toRawText().replace(QChar(0x2029),QChar('\n')).toUtf8()}};return {};}
QByteArray EpubSourceEditor::Token()const {if(!ready_)return baseToken_;const auto Value=model_.Fingerprint(draft());return Value==reference_?baseToken_:Value;}
bool EpubSourceEditor::flush(){if(!ready_ || sourceComposing_)return false;const auto Draft=draft();for(auto I=Draft.begin();I!=Draft.end();++I){const auto Error=model_.Replace(I.key(),I.value());if(!Error.isEmpty()){status_->setText(Error);return false;}if(sourceBuffers_.contains(I.key()))sourceBuffers_[I.key()].applied_=I.value();}if(!Draft.isEmpty()){text_->document()->setModified(false);emit Changed();}return true;}
void EpubSourceEditor::select() {
    if(!ready_)return;saveSourceState();current_=resources_->currentText();const auto Data=model_.Read(current_);const auto Media=resources_->currentData().toString();const bool Raster=QStringList{"image/png","image/jpeg","image/gif","image/webp"}.contains(Media);
    updating_=true;text_->setVisible(!Raster);image_->setVisible(Raster);
    if(Raster){QBuffer Buffer;Buffer.setData(Data);Buffer.open(QIODevice::ReadOnly);QImageReader Reader(&Buffer);Reader.setAutoTransform(true);const auto Size=Reader.size();const auto Image=Size.isValid() && Size.width()<=4096 && Size.height()<=4096 && qint64(Size.width())*Size.height()<=4194304?Reader.read():QImage{};image_->setPixmap(QPixmap::fromImage(Image.scaled(640,420,Qt::KeepAspectRatio,Qt::SmoothTransformation)));image_->setAccessibleName(tr("이미지 미리보기: %1").arg(current_));}
    else activateSource(Data,Media);updating_=false;
}
void EpubSourceEditor::importImage() {
    if(structuring_)return;
    if(!ready_ || !QStringList{"image/png","image/jpeg","image/gif","image/webp"}.contains(resources_->currentData().toString())){status_->setText(tr("교체할 PNG·JPEG·GIF·WebP 리소스를 먼저 선택해 주세요. 경로와 형식은 유지합니다."));return;}
    const auto Path=QFileDialog::getOpenFileName(this,tr("같은 형식의 이미지로 교체"),{},tr("이미지 (*.png *.jpg *.jpeg *.gif *.webp)"));if(Path.isEmpty())return;QFile File(Path);if(!File.open(QIODevice::ReadOnly) || File.size()>8*1024*1024){status_->setText(tr("이미지 입력 한도는 8 MiB입니다."));return;}
    const auto Error=model_.Replace(current_,File.read(8*1024*1024+1));if(!Error.isEmpty()){status_->setText(Error);return;}select();emit Changed();
}
bool EpubSourceEditor::CanUndo()const{return ready_ && !structuring_ && ((!text_->isHidden() && text_->document()->isUndoAvailable()) || model_.CanUndo());}
bool EpubSourceEditor::CanRedo()const{return ready_ && !structuring_ && ((!text_->isHidden() && text_->document()->isRedoAvailable()) || model_.CanRedo());}
void EpubSourceEditor::Undo(){if(!ready_ || structuring_)return;if(!(bookUndoPriority_ && model_.CanUndo()) && !text_->isHidden() && text_->document()->isUndoAvailable())text_->undo();else{model_.Undo();bookUndoPriority_=model_.CanUndo();bookRedoPriority_=true;refreshResources(current_);emit Changed();}}
void EpubSourceEditor::Redo(){if(!ready_ || structuring_)return;if(!(bookRedoPriority_ && model_.CanRedo()) && !text_->isHidden() && text_->document()->isRedoAvailable())text_->redo();else{model_.Redo();bookUndoPriority_=true;bookRedoPriority_=model_.CanRedo();refreshResources(current_);emit Changed();}}
QJsonObject EpubSourceEditor::Snapshot()const {QJsonArray Rows;for(int I=0;I<resources_->count();++I)Rows.append(QJsonObject{{"path",resources_->itemText(I)},{"mediaType",resources_->itemData(I).toString()}});return {{"exportWork",QJsonObject{{"runs",exportRuns_},{"joins",exportJoins_}}},{"ready",ready_},{"error",error_},{"resources",Rows},{"current",current_},{"canUndo",CanUndo()},{"canRedo",CanRedo()},{"saveAsOnly",true},{"structuring",structuring_},{"structureError",structureError_},{"validating",nativeValidating_ || checkerState_=="running" || structuring_},{"standardValidation",validationSnapshot()},{"validated",IsValidated()},{"preview",previewSnapshot()},{"sourceVisible",IsSourceVisible()},{"recoveryEnabled",recoveryEnabled_},{"recoveryBusy",draftWriter_.isRunning()||draftReader_.isRunning()||draftFinder_.isRunning()||pendingRecovery_.has_value()},{"recoverySaved",!recoveryPath_.isEmpty()&&draftWrittenToken_==Token()},{"recoveryPath",recoveryPath_},{"recoveryCandidates",recoveries_?recoveries_->count():0},{"recoveryError",recoveryError_},{"recoverySourceChanged",recoverySourceChanged_}};}
QJsonObject EpubSourceEditor::Execute(const QJsonObject& Request) {
    const auto Action=Request["action"].toString();
    if(Action=="epub.edit.preview"){requestPreview(true);return {{"accepted",previewRuntime_.has_value()}};}
    if(Action=="epub.edit.source-visible"){SetSourceVisible(Request["visible"].toBool(true));return {{"accepted",true}};}
    if(Action=="epub.edit.recover"){restoreRecovery(Request["path"].toString());return {{"accepted",true}};}
    if(Action=="epub.edit.structure-cancel"){if(structureCancel_)structureCancel_->store(true);return {{"accepted",true}};}
    if(!ready_ || structuring_)return {{"error","epub-edit-not-ready-or-busy"}};
    if(Action=="epub.edit.select"){if(!flush())return {{"error","epub-edit-draft-invalid"}};const int Index=resources_->findText(Request["resource"].toString());if(Index<0)return {{"error","epub-edit-resource"}};const QSignalBlocker Block(resources_);resources_->setCurrentIndex(Index);select();return {{"accepted",true}};}
    if(Action=="epub.edit.metadata-inspect"){if(!flush())return {{"error","epub-edit-draft-invalid"}};return model_.Metadata();}
    if(Action=="epub.edit.image-inspect"){if(!flush())return {{"error","epub-edit-draft-invalid"}};return model_.InspectImage(Request["resource"].toString());}
    if(Action=="epub.edit.preview-source")return {{"accepted",selectPreviewSource()}};
    if(Action=="epub.edit.source-locate"){sourceSyncPending_=true;sourceSyncAttempts_=0;locateSource();return {{"accepted",true}};}
    if(Action=="epub.edit.structure")return structure(Request)?QJsonObject{{"accepted",true}}:QJsonObject{{"error","epub-edit-structure-not-ready"}};
    if(Action=="epub.edit.checkpoint"){Checkpoint();return {{"accepted",true}};}
    if(Action=="epub.edit.recovery-list"){QJsonArray Entries;for(int I=0;I<recoveries_->count();++I)Entries.append(QJsonObject{{"path",recoveries_->itemData(I).toString()},{"label",recoveries_->itemText(I)}});return {{"entries",Entries}};}
    if(Action=="epub.edit.validate"){validate(false,Request["standard"].toBool());return {{"accepted",true}};}
    if(Action=="epub.edit.validation-cancel"){cancelEpubCheck("cancelled");return {{"accepted",true}};}
    if(Action=="epub.edit.diagnostic")return {{"accepted",selectDiagnostic(Request["index"].toInt(-1))}};
    if(Action=="epub.edit.replace") {
        if(!flush())return {{"error","epub-edit-draft-invalid"}};const auto Path=Request["resource"].toString();const auto Index=resources_->findText(Path);if(Index<0)return {{"error","epub-edit-resource"}};
        const bool Raster=QStringList{"image/png","image/jpeg","image/gif","image/webp"}.contains(resources_->itemData(Index).toString());
        QByteArray Data;if(Raster){const auto Decoded=QByteArray::fromBase64Encoding(Request["base64"].toString().toLatin1(),QByteArray::AbortOnBase64DecodingErrors);if(!Decoded)return {{"error","epub-edit-image-encoding"}};Data=Decoded.decoded;}else Data=Request["text"].toString().toUtf8();
        const auto Error=model_.Replace(Path,Data);if(!Error.isEmpty())return {{"error",Error}};
        {const QSignalBlocker Block(resources_);resources_->setCurrentIndex(Index);}select();emit Changed();return {{"accepted",true}};
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
    if(sourceComposing_ || !ready_ || draftReader_.isRunning() || pendingRecovery_ || !pendingRecoveryPath_.isEmpty() || nativeValidating_ || checkerState_=="running" || structuring_)return;checkerRequested_=Standard || standardValidationRequired();validatedBytes_.clear();validationToken_=Token();saveAfter_=SaveAfter;status_->setText(tr("패키지·XML·CSS 괄호·참조·이미지를 검사하고 있습니다…"));auto Model=model_;const auto Draft=draft();
    if(previewWorker_.isRunning() && previewWorkToken_==validationToken_ && previewCancel_ && !previewCancel_->load()) {
        validationCancel_=previewCancel_;nativeValidating_=true;++exportJoins_;validation_.setFuture(previewWorker_.future());emit StateChanged();return;
    }
    validationCancel_=std::make_shared<std::atomic_bool>(false);const auto Cancel=validationCancel_;
    const auto Cached=cachedExportToken_==validationToken_?cachedExport_:epub::EditOutput{};
    if(!Cached)++exportRuns_;nativeValidating_=true;validation_.setFuture(QtConcurrent::run([Model=std::move(Model),Draft,Cancel,Cached]{if(Cancel->load())return epub::EditOutput{{},"cancelled",{}};return Cached?Cached:Model.Export(Draft,[Cancel]{return Cancel->load();});}));emit StateChanged();
}
bool EpubSourceEditor::eventFilter(QObject* Object,QEvent* Event) {
    if(Object==sourceScroll_->viewport() && Event->type()==QEvent::Resize){if(auto* Actions=sourcePanel_->findChild<QBoxLayout*>("epubSourceActions"))Actions->setDirection(sourceScroll_->viewport()->width()<760?QBoxLayout::TopToBottom:QBoxLayout::LeftToRight);}
    if(Object==text_ && Event->type()==QEvent::PaletteChange)rehighlightSources();
    if(Object==text_ && Event->type()==QEvent::InputMethod) {
        sourceComposing_=!static_cast<QInputMethodEvent*>(Event)->preeditString().isEmpty();text_->setProperty("epubSourceComposing",sourceComposing_);
        if(!sourceComposing_)QTimer::singleShot(0,this,[this]{if(sourceComposing_)return;if(previewCandidate_)admitPreview(previewCandidate_);if(previewAutomatic_ && previewAutomatic_->isChecked())previewTimer_.start();});
    }
    return QWidget::eventFilter(Object,Event);
}
void EpubSourceEditor::Commit(const QString& Path,const QByteArray& Bytes){commitRestore_=true;commitResource_=current_;commitPosition_=text_->textCursor().position();commitAnchor_=text_->textCursor().anchor();commitVertical_=text_->verticalScrollBar()->value();commitHorizontal_=text_->horizontalScrollBar()->value();commitFocus_=text_->hasFocus();DiscardRecovery();resetRecoverySession();sourcePath_=Path;baseToken_=Token();protectSourcePath(Path);
    // Only the immutable model produced by validation of these exact bytes may
    // bypass loading. Saving a stale revision still follows the normal load path.
    const auto Prepared=cachedExport_.prepared_;
    if(Prepared && cachedExport_.bytes_==Bytes) {
        sourceHashAtOpen_=QByteArray::fromHex(Prepared->Publication()["sourceSha256"].toString().toLatin1());
        saveSourceState();bookUndoPriority_=bookRedoPriority_=false;modelLoaded(*Prepared);
    } else load(Bytes);}
}
