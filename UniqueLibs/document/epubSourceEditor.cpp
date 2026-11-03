#include "stdafx.h"
#include "epubSourceEditor.hpp"
#include "documentTextCodec.hpp"
#include "UniqueLibs/settings/unifiedConfig.hpp"
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
#include <QJsonDocument>
#include <QProcess>
#include <QSaveFile>
#include <QStandardPaths>
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
    sourceSyncTimer_.setSingleShot(true);sourceSyncTimer_.setInterval(180);connect(&sourceSyncTimer_,&QTimer::timeout,this,[this]{locateSource();});
    connect(text_->Backend(),&ITextEditorBackend::cursorPositionChanged,this,[this]{if(!updating_ && SourceInput()->hasFocus() && action("epubPreviewScrollSync")->isChecked()){sourceSyncPending_=true;sourceSyncAttempts_=0;sourceSyncTimer_.start();}updateStatus();});
    connect(text_->Backend(),&ITextEditorBackend::selectionChanged,this,&EpubSourceEditor::updateActions);
    connect(text_,&QTextView::ContentChanged,this,[this]{
        invalidateSourceCache();
        if(updating_ || !sourceShown())return;bookUndoPriority_=bookRedoPriority_=false;
        if(text_->Backend()->documentLength()>2*1024*1024){const QSignalBlocker Block(text_);static_cast<ScintillaQtDirectBackend*>(text_->Backend())->Send(SCI_UNDO);invalidateSourceCache();report(tr("소스 편집은 리소스당 2 MiB까지 지원합니다."));}
        refreshTabs();emit Changed();
    });
    setupWorkspace();
    workspaceTimer_.setSingleShot(true);workspaceTimer_.setInterval(700);
    connect(&workspaceTimer_,&QTimer::timeout,this,&EpubSourceEditor::saveWorkspace);
    connect(text_->Backend(),&ITextEditorBackend::cursorPositionChanged,this,&EpubSourceEditor::scheduleWorkspace);
    connect(text_->Backend(),&ITextEditorBackend::selectionChanged,this,&EpubSourceEditor::scheduleWorkspace);
    connect(text_->Backend(),&ITextEditorBackend::viewportScrolled,this,&EpubSourceEditor::scheduleWorkspace);
    connect(this,&EpubSourceEditor::StateChanged,this,&EpubSourceEditor::scheduleWorkspace);
    connect(this,&EpubSourceEditor::Changed,this,&EpubSourceEditor::scheduleWorkspace);
    setupRecovery();setupValidation();setupStructure();
    connect(this,&EpubSourceEditor::Changed,this,[this]{if(validationCancel_ && validationToken_!=Token())validationCancel_->store(true);});
    connect(this,&EpubSourceEditor::StateChanged,this,[this]{updateActions();updateStatus();});
    connect(&worker_,&QFutureWatcher<LoadOutput>::finished,this,[this]{
        const auto Result=worker_.result();
        if(Result.workspace_){workspaceSaved_=Result.workspace_->document_;workspaceError_=Result.workspace_->error_;}
        modelLoaded(Result.model_);
    });
    connect(&validation_,&QFutureWatcher<epub::EditOutput>::finished,this,[this]{
        const auto Result=validation_.result();nativeValidating_=false;if(validationToken_!=Token()){report(tr("검사 중 소스가 변경되었습니다. 다시 검사해 주세요."));emit StateChanged();return;}
        nativeDiagnostic(Result);
        if(!Result){error_=Result.error_+": "+Result.location_;report(Result.error_=="epub-edit-spine-repair-required"?tr("읽기 순서에 누락된 참조가 있어 저장할 수 없습니다. OPF에서 해당 참조를 수정한 후 다시 검사해 주세요. 원본은 변경하지 않았습니다."):tr("소스 검사를 통과하지 못했습니다. %1").arg(error_));validatedBytes_.clear();}
        else {cachedExport_=Result;cachedExportToken_=validationToken_;error_.clear();validatedToken_=validationToken_;validatedBytes_=Result.bytes_;report(tr("기본 구조·XML·CSS 괄호 검사 통과 · EPUBCheck 검사로 표준 오류를 확인할 수 있습니다."));}
        if(Result && checkerRequested_){startEpubCheck();return;}
        emit StateChanged();if(Result && IsValidated() && saveAfter_)emit SaveRequested(saveAsAfter_);saveAfter_=false;
    });load(std::move(Bytes));
}
EpubSourceEditor::~EpubSourceEditor(){
    if(validationCancel_)validationCancel_->store(true);if(previewCancel_)previewCancel_->store(true);if(structureCancel_)structureCancel_->store(true);stopEpubCheck();RequestPreviewClose();if(ready_ && !discardedRecovery_ && recoveryEnabled_)Checkpoint();if(cancel_)cancel_->store(true);
    saveWorkspace();saveLayout();for(const auto& Path:sourceBuffers_.keys())releaseSource(Path);
    // The workspace may have adopted the strip into its status row.
    if(statusStrip_ && statusStrip_->parent()!=this)delete statusStrip_;
}
void EpubSourceEditor::load(QByteArray Bytes) {
    saveSourceState();bookUndoPriority_=bookRedoPriority_=false;ready_=false;setEnabled(false);report(tr("EPUB 편집 구조를 검사하고 있습니다…"));cancel_=std::make_shared<std::atomic_bool>(false);auto Cancel=cancel_;
    const bool Restore=model_.Publication().isEmpty() && !pendingRecovery_ && !commitRestore_;
    const auto Load=[Bytes=std::move(Bytes),Cancel](std::optional<WorkspaceRead> Workspace){
        LoadOutput Result;Result.model_.Load(Bytes,[Cancel]{return Cancel->load();});
        Result.workspace_=std::move(Workspace);return Result;
    };
    // A continuation preserves the session queue's ordering without a blocking result()
    // that could steal a queued read and run it ahead of the previous editor's final save.
    if(Restore)worker_.setFuture(readWorkspace(workspaceFile_,sourcePath_).then(QtFuture::Launch::Async,
        [Load](WorkspaceRead Workspace){return Load(std::move(Workspace));}));
    else worker_.setFuture(QtConcurrent::run([Load]{return Load(std::nullopt);}));
}
void EpubSourceEditor::report(const QString& Text){message_=Text;updateStatus();}
void EpubSourceEditor::reportDraft(const QString& Text){draftMessage_=Text;updateStatus();}
void EpubSourceEditor::reportPreview(const QString& Text){previewMessage_=Text;updateStatus();}
QWidget* EpubSourceEditor::SourceInput()const{return text_->Backend()->widget();}
QString EpubSourceEditor::mediaOf(const QString& Path)const{for(const auto& Value:model_.Publication()["resources"].toArray())if(Value.toObject()["path"]==Path)return Value.toObject()["mediaType"].toString();return {};}
QMap<QString,QByteArray> EpubSourceEditor::draft()const {
    // A disk savepoint does not apply/reload the live model. Keep its source overlay
    // even after Scintilla's savepoint moves, until flush applies it to the model.
    if(ready_ && !current_.isEmpty() && sourceShown() && sourceBytes()!=sourceBuffers_.value(current_).applied_)return {{current_,sourceBytes()}};
    return {};
}
QByteArray EpubSourceEditor::Token()const {if(!ready_)return baseToken_;const auto Value=model_.Fingerprint(draft());return Value==reference_?baseToken_:Value;}
std::function<QJsonArray()> EpubSourceEditor::CaptureSearchSources(const QString& Document) const {
    const auto Model = model_; const auto Draft = draft();
    return [Model, Draft, Document] { return BuildSearchSources(Model, Draft, Document); };
}
QJsonArray EpubSourceEditor::BuildSearchSources(const epub::EditModel& Model, const QMap<QString, QByteArray>& Draft, const QString& Document) {
        QJsonArray Result;
        for (const auto& Item : Model.Publication()["resources"].toArray()) {
            const auto Resource = Item.toObject(); const auto Path = Resource["path"].toString();
            const auto Media = Resource["mediaType"].toString();
            if (!Model.IsEditable(Path) || (Media.startsWith("image/") && Media != "image/svg+xml")) continue;
            const auto Bytes = Draft.contains(Path) ? Draft[Path] : Model.Read(Path);
            const auto Decoded = DecodeDocumentText(Bytes); if (!Decoded.HasValue()) continue;
            int Chapter=-1;const auto Spine=Model.Publication()["spine"].toArray();
            for(int Index=0;Index<Spine.size();++Index)if(Spine[Index].toObject()["path"]==Path){Chapter=Index;break;}
            Result.append(QJsonObject{{"path", Path}, {"resource", Path}, {"chapter",Chapter}, {"document", Document}, {"text", Decoded.Value().text_}, {"encoding", Decoded.Value().encoding_}, {"bom", Decoded.Value().bom_}});
        }
        return Result;
}
QString EpubSourceEditor::ApplySearchBatch(const QJsonArray& Changes) {
    if (!ready_ || structuring_ || sourceComposing_) return tr("책을 지금 편집할 수 없습니다.");
    const auto Draft = draft(); QMap<QString, QByteArray> Replacements;
    for (const auto& Value : Changes) {
        const auto Change = Value.toObject(); const auto Source = Change["source"].toObject(); const auto Path = Source["resource"].toString();
        const auto Current = DecodeDocumentText(Draft.contains(Path) ? Draft[Path] : model_.Read(Path), Source["encoding"].toString());
        if (!Current.HasValue()) return tr("리소스를 읽을 수 없습니다: %1").arg(Path);
        if (Source["undo"].toBool() && Current.Value().text_ == Change["after"].toString()) continue;
        if (Current.Value().text_ != Change["before"].toString()) return tr("검색 후 리소스가 변경되었습니다: %1").arg(Path);
        const auto Encoded = EncodeDocumentText(Change["after"].toString(), Current.Value().encoding_, Current.Value().bom_);
        if (!Encoded.HasValue()) return tr("리소스의 원래 인코딩으로 바꿀 수 없습니다: %1").arg(Path);
        Replacements.insert(Path, Encoded.Value());
    }
    if (!flush()) return tr("현재 소스의 편집 내용을 반영하지 못했습니다.");
    const auto Error = model_.ReplaceBatch(Replacements, model_.Fingerprint()); if (!Error.isEmpty()) return Error;
    bookUndoPriority_ = true; bookRedoPriority_ = false; refreshResources(current_); emit Changed(); return {};
}
void EpubSourceEditor::SelectSearchRange(const QString& Resource, qint64 Start, qint64 Length) {
    SetSourceVisible(true); if (!openResource(Resource)) return;
    const auto Text = sourceText(); if (Start < 0 || Length < 0 || Start > Text.size() || Length > Text.size() - Start) return;
    auto* Backend = static_cast<ScintillaQtDirectBackend*>(text_->Backend());
    Backend->Send(SCI_SETSEL, fromUtf16(int(Start)), fromUtf16(int(Start + Length))); Backend->Send(SCI_SCROLLCARET); focusSource();
}
bool EpubSourceEditor::flush(){
    if(!ready_ || sourceComposing_)return false;const auto Draft=draft();saveSourceState();
    for(auto I=Draft.begin();I!=Draft.end();++I){const auto Error=model_.Replace(I.key(),I.value());if(!Error.isEmpty()){report(Error);return false;}if(sourceBuffers_.contains(I.key()))sourceBuffers_[I.key()].applied_=I.value();touchedResources_.insert(I.key());}
    if(!Draft.isEmpty()){text_->CommitContent();refreshTabs();if(resourceModel_)resourceModel_->Refresh();emit Changed();}return true;
}
bool EpubSourceEditor::openResource(const QString& Path) {
    if(!ready_ || !model_.IsEditable(Path))return false;
    if(Path==current_){selectResource(Path);refreshTabs();return true;}
    if(!flush()){selectResource(current_);refreshTabs();return false;}
    current_=Path;select();
    // A resource chosen in the tree or tabs takes a linked preview to its caret.
    if(!previewSyncing_ && action("epubPreviewScrollSync")->isChecked()){sourceSyncPending_=true;sourceSyncAttempts_=0;sourceSyncTimer_.start();}
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
    refreshTabs();selectResource(current_);markDiagnostics();updateActions();updateStatus();scheduleWorkspace();
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
    return {{"sourceCache",QJsonObject{{"reads",sourceReads_},{"maps",sourceMapBuilds_}}},{"workspaceError",workspaceError_},{"workspaceFile",workspaceFile_},{"workspaceSaving",workspaceWriter_.isRunning()},{"exportWork",QJsonObject{{"runs",exportRuns_},{"joins",exportJoins_}}},{"ready",ready_},{"error",error_},{"resources",Rows},{"current",current_},{"tabs",QJsonArray::fromStringList(openResources_)},{"source",Source},{"layout",Layout},{"status",Status},{"canUndo",CanUndo()},{"canRedo",CanRedo()},{"saveAsOnly",false},{"structuring",structuring_},{"structureError",structureError_},{"validating",nativeValidating_ || checkerState_=="running" || structuring_},{"standardValidation",validationSnapshot()},{"validated",IsValidated()},{"preview",previewSnapshot()},{"scrollSync",QJsonObject{{"active",scrollSyncActive()},{"requests",sourceScrollSerial_},{"line",lastScrollLine_},{"pending",scrollSyncPending_},{"stale",sourceBuffers_.value(current_).shiftsOverflow_},{"target",lastScrollTarget_}}},{"sourceVisible",IsSourceVisible()},{"recoveryEnabled",recoveryEnabled_},{"recoveryBusy",draftWriter_.isRunning()||draftReader_.isRunning()||draftFinder_.isRunning()||pendingRecovery_.has_value()},{"recoverySaved",!recoveryPath_.isEmpty()&&draftWrittenToken_==Token()},{"recoveryPath",recoveryPath_},{"recoveryCandidates",recoveryEntries_.size()},{"recoveryError",recoveryError_},{"recoverySourceChanged",recoverySourceChanged_}};
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
    if(Action=="epub.edit.find-all"){findInResources(Request["text"].toString());return {{"accepted",true},{"pending",true}};}
    if(Action=="epub.edit.wrap"){if(!sourceShown() || text_->Backend()->selectedText().isEmpty())return {{"error","epub-edit-selection"}};wrapSelection(Request["tag"].toString());return {{"accepted",true}};}
    if(Action=="epub.edit.quick-fix"){const auto Fix=quickFixAt(sourcePosition());if(Fix.isEmpty())return {{"error","epub-edit-quick-fix"}};quickFix();return {{"accepted",true}};}
    if(Action=="epub.edit.open-link")return openLink()?QJsonObject{{"accepted",true}}:QJsonObject{{"error","epub-edit-link"}};
    if(Action=="epub.edit.metadata-inspect"){if(!flush())return {{"error","epub-edit-draft-invalid"}};return model_.Metadata();}
    if(Action=="epub.edit.image-inspect"){if(!flush())return {{"error","epub-edit-draft-invalid"}};return model_.InspectImage(Request["resource"].toString());}
    if(Action=="epub.edit.preview-source")return {{"accepted",selectPreviewSource()}};
    if(Action=="epub.edit.source-locate"){sourceSyncPending_=true;sourceSyncAttempts_=0;locateSource(true);return {{"accepted",true}};}
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
bool EpubSourceEditor::PrepareSave(bool SaveAs) {
    // Applying source edits is shared by both save commands, before validation.
    if(!flush())return false;
    if(IsValidated())return true;
    saveAsAfter_=SaveAs;
    if((nativeValidating_ || checkerState_=="running") && validationToken_==Token())saveAfter_=true;
    else validate(true);
    return false;
}
DocumentEncoder EpubSourceEditor::CaptureSave(const QString& Destination,bool SaveOriginal) {
    const bool Available=ready_ && !sourceComposing_ && !structuring_ && !draftReader_.isRunning() && !pendingRecovery_ && pendingRecoveryPath_.isEmpty();
    return [Model=model_,Draft=draft(),Destination,SaveOriginal,Available,Paths=protectedPaths_,Ids=identities_,Source=sourcePath_,Settings=checkerSettings_,SettingsFile=checkerSettingsFile_,
            RequiredDefault=hc::defaults::edit::ViewValue("epubEditor/requireEpubCheck").toBool(),Cached=cachedExportToken_==Token()?cachedExport_:epub::EditOutput{},
            Checked=checkerVerifiedToken_==Token()](storage::OperationContext Context) mutable ->storage::Result<QByteArray> {
        using namespace storage;
        if(!Available)return Error{ErrorCategory::InvalidRequest,"epub-edit-save-busy"};
        if(QFileInfo(Destination).suffix().compare("epub",Qt::CaseInsensitive)!=0)return Error{ErrorCategory::Unsupported,"epub-edit-save-as-epub-required"};
        if(auto Stop=Context.Check({"document"}))return *Stop;
        // Read current validation policy in the worker too: config paths can be
        // redirected or remote. A changed policy cannot reuse an old EPUBCheck verdict.
        QJsonObject Root;QString SettingsError;
        if(hc::settings::UnifiedConfig::Load(SettingsFile,Root,SettingsError)){
            auto Next=Root["documentViews"].toObject()["epubEditor"].toObject();Next.remove("layout");
            Checked=Checked && Next==Settings;Settings=std::move(Next);
        }
        const bool Required=Settings["requireEpubCheck"].toBool(RequiredDefault);
        if(auto Stop=Context.Check({"document"}))return *Stop;
        // Network identity probes belong to the worker, including hard-link aliases.
        if(!SaveOriginal && (Paths.contains(QFileInfo(Destination).absoluteFilePath().toCaseFolded()) || Ids.contains(identity(Destination)) || (!identity(Source).isEmpty() && identity(Source)==identity(Destination))))
            return Error{ErrorCategory::AccessDenied,"epub-edit-source-overwrite-denied"};
        const auto Output=Cached?Cached:Model.Export(Draft,[Context]{return Context.Check({"document"}).has_value();});
        if(auto Stop=Context.Check({"document"}))return *Stop;
        if(!Output)return Error{ErrorCategory::InvalidRequest,(Output.error_+": "+Output.location_).toStdString()};
        if(Required && !Checked) {
            auto Java=Settings["javaPath"].toString().trimmed();if(Java.isEmpty())Java=QStandardPaths::findExecutable("java");
            const auto Jar=Settings["epubcheckJar"].toString().trimmed();
            if(!QFileInfo(Java).isAbsolute() || !QFileInfo(Java).isFile() || !QFileInfo(Jar).isAbsolute() || !QFileInfo(Jar).isFile()
                || QFileInfo(Jar).suffix().compare("jar",Qt::CaseInsensitive)!=0)return Error{ErrorCategory::InvalidRequest,"epubcheck-configure-java-and-epubcheck"};
            QTemporaryDir Directory;if(!Directory.isValid())return Error{ErrorCategory::Unavailable,"epubcheck-temporary-directory"};
            const auto Candidate=Directory.filePath("candidate.epub");QSaveFile File(Candidate);File.setDirectWriteFallback(false);
            if(!File.open(QIODevice::WriteOnly) || File.write(Output.bytes_)!=Output.bytes_.size() || !File.commit())return Error{ErrorCategory::Unavailable,"epubcheck-temporary-publication"};
            QProcess Checker;Checker.setProcessChannelMode(QProcess::MergedChannels);
            Checker.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* Args){Args->flags|=CREATE_NO_WINDOW;});
            Checker.setWorkingDirectory(Directory.path());Checker.start(Java,{"-Xmx256m","-Djava.awt.headless=true","-jar",Jar,Candidate,"--json",Directory.filePath("report.json"),"--locale","en"});
            if(!Checker.waitForStarted(5000))return Error{ErrorCategory::Unavailable,"epubcheck-start-failed"};
            QByteArray Log;
            while(Checker.state()!=QProcess::NotRunning) {
                Checker.waitForFinished(50);Log+=Checker.readAll();
                const auto Stop=Context.Check({"document"});
                if(Stop || Log.size()>128*1024 || QFileInfo(Directory.filePath("report.json")).size()>8*1024*1024) {
                    Checker.kill();Checker.waitForFinished(1000);
                    if(Stop)return *Stop;
                    return Error{ErrorCategory::BudgetExceeded,"epubcheck-output-limit"};
                }
            }
            QFile Report(Directory.filePath("report.json"));
            if(Checker.exitStatus()!=QProcess::NormalExit || Checker.exitCode()!=0 || Report.size()<=0 || Report.size()>8*1024*1024 || !Report.open(QIODevice::ReadOnly))
                return Error{ErrorCategory::InvalidRequest,"epubcheck-failed-or-invalid-report"};
            QJsonParseError Parse;const auto Json=QJsonDocument::fromJson(Report.readAll(),&Parse);const auto Root=Json.object(),Summary=Root["checker"].toObject();
            if(Parse.error!=QJsonParseError::NoError || !Json.isObject() || !Root["messages"].isArray() || Root["messages"].toArray().size()>2000
                || Summary["checkerVersion"].toString().isEmpty() || Summary["nError"].toInt(-1)!=0 || Summary["nFatal"].toInt(-1)!=0)
                return Error{ErrorCategory::InvalidRequest,"epubcheck-failed-or-invalid-report"};
            for(const auto& Item:Root["messages"].toArray()) {
                const auto Row=Item.toObject();const auto Severity=Row["severity"].toString();
                if(Severity.isEmpty() || !Row["ID"].isString() || !Row["message"].isString() || Severity=="ERROR" || Severity=="FATAL")
                    return Error{ErrorCategory::InvalidRequest,"epubcheck-failed-or-invalid-report"};
            }
        }
        return Output.bytes_;
    };
}
void EpubSourceEditor::CommitSaved(const QString& Path,const QByteArray& Content,const QByteArray& Hash) {
    if(Path!=sourcePath_)saveWorkspace();
    savedToken_=QByteArray::fromHex(Content.mid(5));sourcePath_=Path;sourceHashAtOpen_=Hash;
    const auto Target=QFileInfo(Path).absoluteFilePath().toCaseFolded();if(!protectedPaths_.contains(Target))protectedPaths_.append(Target);
    // Never reload the model/documents or move focus on save completion. A newer
    // draft and its recovery record remain owned by the current editor.
    if(!IsModified()){error_.clear();if(sourceShown())text_->CommitContent();DiscardRecovery();resetRecoverySession();}
    else {draftWrittenToken_.clear();draftTimer_.start();}
    report(IsModified()?tr("저장 완료 · 추가 변경 있음"):tr("저장 완료"));
    // The save worker read the current verdict policy; follow it here without a GUI-thread read.
    reloadValidationSettingsAsync();emit StateChanged();
}
void EpubSourceEditor::ReportSaveFailure(const QByteArray& Content,const QString& Error) {
    // The failing snapshot can be older than the displayed source. Do not mark
    // the newer source invalid, but retain a visible failure for this request.
    if(Content=="epub:"+Token().toHex())error_=Error;
    report(tr("저장 실패: %1 · 편집 내용은 유지됩니다. 소스와 검사 설정을 확인한 뒤 다시 저장하십시오.").arg(Error));reloadValidationSettingsAsync();emit StateChanged();
}
storage::Result<QByteArray> EpubSourceEditor::Export(const QString& Destination,bool SaveOriginal)const {
    if(!ready_ || QFileInfo(Destination).suffix().compare("epub",Qt::CaseInsensitive)!=0)return storage::Error{storage::ErrorCategory::Unsupported,"epub-edit-save-as-epub-required"};
    const auto Target=QFileInfo(Destination).absoluteFilePath().toCaseFolded();
    // The workspace establishes the current target and original-write authority.
    // A Save As that finishes during further typing has already adopted its new path.
    if(!SaveOriginal && (protectedPaths_.contains(Target) || (!identity(Destination).isEmpty() && identities_.contains(identity(Destination)))))return storage::Error{storage::ErrorCategory::AccessDenied,"epub-edit-source-overwrite-denied"};
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
    if((Object==SourceInput() || Object==sourcePanel_) &&
       (Event->type()==QEvent::Move || Event->type()==QEvent::Resize || Event->type()==QEvent::Show || Event->type()==QEvent::LayoutRequest))
        QTimer::singleShot(0,this,&EpubSourceEditor::alignPreview);
    if(Object==SourceInput() && Event->type()==QEvent::InputMethod) {
        sourceComposing_=!static_cast<QInputMethodEvent*>(Event)->preeditString().isEmpty();text_->setProperty("epubSourceComposing",sourceComposing_);setProperty("epubSourceComposing",sourceComposing_);
        if(!sourceComposing_)QTimer::singleShot(0,this,[this]{if(sourceComposing_)return;if(previewCandidate_)admitPreview(previewCandidate_);if(action("epubAutoPreview")->isChecked())previewTimer_.start();});
    }
    if(Object==SourceInput() && Event->type()==QEvent::FocusIn)updateActions();
    return QWidget::eventFilter(Object,Event);
}
void EpubSourceEditor::Commit(const QString& Path,const QByteArray& Bytes){saveWorkspace();commitRestore_=true;commitResource_=current_;commitPosition_=sourcePosition();commitAnchor_=sourceAnchor();commitTop_=scrollTop();commitHorizontal_=horizontalOffset();commitFocus_=SourceInput()->hasFocus();DiscardRecovery();resetRecoverySession();sourcePath_=Path;baseToken_=Token();protectSourcePath(Path);touchedResources_.clear();
    // Only the immutable model produced by validation of these exact bytes may
    // bypass loading. Saving a stale revision still follows the normal load path.
    const auto Prepared=cachedExport_.prepared_;
    if(Prepared && cachedExport_.bytes_==Bytes) {
        sourceHashAtOpen_=QByteArray::fromHex(Prepared->Publication()["sourceSha256"].toString().toLatin1());
        saveSourceState();bookUndoPriority_=bookRedoPriority_=false;modelLoaded(*Prepared);
    } else load(Bytes);}
}
