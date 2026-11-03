#include "stdafx.h"
#include "epubSourceEditor.hpp"
#include "UniqueLibs/ProductDefaults_Edit.hpp"
#include "UniqueLibs/settings/appSettings.hpp"
#include "epubProcessView.hpp"
#include "text/QTextView.h"
#include "text/ScintillaQtDirectBackend.h"
#include "commonLibs/document/epub/epubSourceMap.hpp"
#include <QJsonArray>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLockFile>
#include <QSaveFile>
#include <QDateTime>
#include <QLabel>
#include <QStackedWidget>
#include <Scintilla.h>
#include <QThreadPool>
#include <QtConcurrentRun>
#include <mutex>

namespace hc::document {
namespace {
ScintillaQtDirectBackend* direct(QTextView* View){return static_cast<ScintillaQtDirectBackend*>(View->Backend());}
QString languageOf(const QString& Media){return Media=="application/xhtml+xml"?QStringLiteral("HTML"):Media=="text/css"?QStringLiteral("CSS"):Media.endsWith("+xml") || Media.endsWith("/xml")?QStringLiteral("XML"):QStringLiteral("None");}
}
// Drafts, the source map, search and diagnostics count UTF-16 code units of the text
// as QString::fromUtf8 and DecodeDocumentText decode it, without a leading UTF-8 BOM.
// Scintilla keeps those three bytes in the buffer so saved bytes stay unchanged, and
// counts UTF-8 bytes; counting from the document start would include the BOM.
qintptr EpubSourceEditor::bomBytes()const {
    if(sourceBom_<0){
        auto* Backend=direct(text_);const auto Byte=[Backend](int At){return int(Backend->Send(SCI_GETCHARAT,quintptr(At))&0xff);};
        sourceBom_=Backend->Send(SCI_GETLENGTH)>=3 && Byte(0)==0xef && Byte(1)==0xbb && Byte(2)==0xbf?3:0;
    }
    return sourceBom_;
}
int EpubSourceEditor::toUtf16(qintptr Position)const {
    auto* Backend=direct(text_);const auto Bom=bomBytes();Position=qBound(qintptr(0),Position,Backend->Send(SCI_GETLENGTH));
    if(Position<=Bom)return 0;
    SourcePosition Nearest{Bom,0};
    for(const auto& Cached:sourcePositions_){
        if(Cached.byte_<0)continue;
        if(Cached.byte_==Position)return Cached.utf16_;
        if(qAbs(Position-Cached.byte_)<qAbs(Position-Nearest.byte_))Nearest=Cached;
    }
    // Status/actions ask repeatedly for the same caret and anchor. Adjacent
    // movement only counts the delta; a source edit or buffer switch clears both.
    const int Delta=int(Backend->Send(SCI_COUNTCODEUNITS,qMin(Position,Nearest.byte_),qMax(Position,Nearest.byte_)));
    const int Units=Nearest.utf16_+(Position>=Nearest.byte_?Delta:-Delta);
    sourcePositions_[1]=sourcePositions_[0];sourcePositions_[0]={Position,Units};return Units;
}
qintptr EpubSourceEditor::fromUtf16(int Offset)const{return direct(text_)->Send(SCI_POSITIONRELATIVECODEUNITS,quintptr(bomBytes()),qMax(0,Offset));}
void EpubSourceEditor::invalidateSourceCache() {
    sourceBytesCached_=sourceTextCached_=sourceMapCached_=sourceMapValid_=false;
    for(auto& Position:sourcePositions_)Position={};
    // An edit can remove the invisible U+FEFF at the start of the buffer.
    sourceBom_=-1;
}
const epub::SourceMap* EpubSourceEditor::sourceMap()const {
    if(!sourceMapCached_){sourceMapValid_=sourceMapCache_.Load(sourceText());sourceMapCached_=true;++sourceMapBuilds_;}
    return sourceMapValid_?&sourceMapCache_:nullptr;
}
QByteArray EpubSourceEditor::sourceBytes()const {
    if(sourceBytesCached_)return sourceBytesCache_;
    const auto Length=direct(text_)->Send(SCI_GETLENGTH);QByteArray Bytes(Length+1,Qt::Uninitialized);direct(text_)->Send(SCI_GETTEXT,quintptr(Length+1),reinterpret_cast<qintptr>(Bytes.data()));Bytes.resize(Length);sourceBytesCache_=std::move(Bytes);sourceBytesCached_=true;++sourceReads_;return sourceBytesCache_;
}
QString EpubSourceEditor::sourceText()const{if(!sourceTextCached_){sourceTextCache_=QString::fromUtf8(sourceBytes());sourceTextCached_=true;}return sourceTextCache_;}
bool EpubSourceEditor::sourceModified()const{return direct(text_)->Send(SCI_GETMODIFY)!=0;}
bool EpubSourceEditor::sourceShown()const{return sourceStack_ && sourceStack_->currentWidget()==text_ && !bufferResource_.isEmpty() && bufferResource_==current_;}
int EpubSourceEditor::sourcePosition()const{return toUtf16(direct(text_)->Send(SCI_GETCURRENTPOS));}
int EpubSourceEditor::sourceAnchor()const{return toUtf16(direct(text_)->Send(SCI_GETANCHOR));}
void EpubSourceEditor::setSourceSelection(int Anchor,int Position,bool Reveal) {
    // SCI_SETSELECTION restores direction without SCI_SETSEL's implicit caret scroll.
    direct(text_)->Send(SCI_SETSELECTION,quintptr(fromUtf16(Position)),fromUtf16(Anchor));if(Reveal)direct(text_)->Send(SCI_SCROLLCARET);
}
int EpubSourceEditor::firstVisibleLine()const{return int(direct(text_)->Send(SCI_GETFIRSTVISIBLELINE));}
int EpubSourceEditor::horizontalOffset()const{return int(direct(text_)->Send(SCI_GETXOFFSET));}
EpubSourceEditor::ScrollTop EpubSourceEditor::scrollTop()const{
    auto* Backend=direct(text_);const auto Display=Backend->Send(SCI_GETFIRSTVISIBLELINE);
    const auto Line=Backend->Send(SCI_DOCLINEFROMVISIBLE,quintptr(Display));
    return {int(Line),int(qMax<qintptr>(0,Display-Backend->Send(SCI_VISIBLEFROMDOCLINE,quintptr(Line))))};
}
void EpubSourceEditor::setScroll(ScrollTop Top,int Offset){
    auto* Backend=direct(text_);
    // Display rows above the top may not be wrapped yet. SCROLLVERTICAL keeps the
    // document line and sub-row as its target and re-applies it once wrapping has
    // reached them, so restoring never forces the whole resource to wrap here.
    // Scintilla clamps a sub-row beyond the line's rows at this wrap width.
    Backend->Send(SCI_SCROLLVERTICAL,quintptr(qBound(0,Top.line_,int(Backend->Send(SCI_GETLINECOUNT))-1)),qMax(0,Top.subLine_));
    Backend->Send(SCI_SETXOFFSET,quintptr(qMax(0,Offset)));
}
void EpubSourceEditor::setSourceReadOnly(bool ReadOnly){sourceReadOnly_=ReadOnly;text_->setReadOnly(ReadOnly);updateActions();}
void EpubSourceEditor::focusSource(){if(sourceShown())SourceInput()->setFocus(Qt::OtherFocusReason);}
void EpubSourceEditor::insertSource(const QString& Text){const auto Bytes=Text.toUtf8();direct(text_)->Send(SCI_REPLACESEL,0,reinterpret_cast<qintptr>(Bytes.constData()));}
void EpubSourceEditor::saveSourceState() {
    if(bufferResource_.isEmpty() || !sourceBuffers_.contains(bufferResource_) || sourceStack_->currentWidget()!=text_)return;
    auto& State=sourceBuffers_[bufferResource_];State.position_=toUtf16(direct(text_)->Send(SCI_GETCURRENTPOS));State.anchor_=toUtf16(direct(text_)->Send(SCI_GETANCHOR));
    const auto Top=scrollTop();State.line_=Top.line_;State.subLine_=Top.subLine_;State.offset_=horizontalOffset();
}
void EpubSourceEditor::releaseSource(const QString& Path) {
    if(!sourceBuffers_.contains(Path))return;const auto State=sourceBuffers_.take(Path);
    if(Path==bufferResource_)bufferResource_.clear();
    // The view keeps its own reference to the shown document.
    if(State.document_)direct(text_)->Send(SCI_RELEASEDOCUMENT,0,State.document_);
}
void EpubSourceEditor::activateSource(const QByteArray& Data,const QString& Media) {
    invalidateSourceCache();
    auto& State=sourceBuffers_[current_];bool Fresh=false;
    if(!State.document_){State.document_=direct(text_)->Send(SCI_CREATEDOCUMENT,0,SC_DOCUMENTOPTION_DEFAULT);State.serial_=++bufferSerial_;Fresh=true;}
    text_->AttachDocument(State.document_,languageOf(Media));bufferResource_=current_;
    // Scintilla keeps read-only per document and ignores replacements while it is set.
    if(Fresh || State.applied_!=Data) {text_->setReadOnly(false);
        if(!Fresh && sourceBytes()==Data){State.applied_=Data;text_->CommitContent();}
        else {
            direct(text_)->SetTextUtf8(Data);invalidateSourceCache();State.applied_=Data;
            direct(text_)->Send(SCI_SETEOLMODE,Data.contains("\r\n")?SC_EOL_CRLF:Data.contains('\r')?SC_EOL_CR:SC_EOL_LF);
            if(!Fresh)State.serial_=++bufferSerial_;
        }
    }
    if(Fresh && workspacePositions_.contains(current_)){
        const auto Saved=workspacePositions_.take(current_).toObject();const int Length=QString::fromUtf8(Data).size();
        // Older sessions counted the BOM of this resource as one unit.
        const int Legacy=workspaceLegacyUnits_ && Data.startsWith("\xef\xbb\xbf")?1:0;
        State.position_=qBound(0,Saved["position"].toInt()-Legacy,Length);State.anchor_=qBound(0,Saved["anchor"].toInt(State.position_+Legacy)-Legacy,Length);
        // Sessions before topLine stored a display row; read it as a document line.
        State.line_=qMax(0,Saved.contains("topLine")?Saved["topLine"].toInt():Saved["firstVisibleLine"].toInt());
        State.subLine_=qMax(0,Saved["topSubLine"].toInt());
        State.offset_=qBound(0,Saved["horizontalOffset"].toInt(),1000000);
    }
    const ScrollTop Top{State.line_,State.subLine_};
    text_->setReadOnly(sourceReadOnly_);setSourceSelection(State.anchor_,State.position_,false);setScroll(Top,State.offset_);
    const auto Resource=current_;const int Offset=State.offset_;
    QTimer::singleShot(0,this,[this,Resource,Top,Offset]{if(bufferResource_==Resource && sourceStack_->currentWidget()==text_)setScroll(Top,Offset);});
    for(const auto& Path:sourceBuffers_.keys())if(Path!=current_ && !model_.IsEditable(Path))releaseSource(Path);
}
void EpubSourceEditor::locateSource(bool Reveal) {
    sourceSyncReveal_|=Reveal;
    if(!sourceSyncPending_ || sourceComposing_ || !previewView_ || !ready_ || previewClosing_ || !sourceShown())return;
    if(previewActiveToken_!=Token() || ++sourceSyncAttempts_>30){sourceSyncPending_=false;sourceSyncReveal_=false;return;}
    const auto State=previewView_->ViewState();if(State["readerBusy"].toBool()){sourceSyncTimer_.start();return;}
    int Chapter=-1;const auto Spine=model_.Publication()["spine"].toArray();for(int I=0;I<Spine.size();++I)if(Spine[I].toObject()["path"]==current_)Chapter=I;if(Chapter<0){sourceSyncPending_=false;return;}
    if(State["chapterIndex"].toInt(-1)!=Chapter){previewView_->ReaderAction("chapter",Chapter);sourceSyncTimer_.start();return;}
    const bool ShouldReveal=std::exchange(sourceSyncReveal_,false);
    const auto* Map=sourceMap();sourceSyncPending_=false;if(!Map)return;auto Anchor=Map->Anchor(sourcePosition());if(Anchor.isEmpty())return;
    // Ctrl+Up/Down can bring an offscreen caret back inside the source viewport.
    // That automatic caret move must not override the viewport anchor, especially
    // when a one-line image occupies most of the preview. Explicit Locate still reveals it.
    if(scrollSyncActive())Anchor["reveal"]=!ShouldReveal && State["previewScroll"].toObject()["supported"].toBool()?"none":"nearest";
    previewView_->ReaderAction("source-position",Anchor);
}
bool EpubSourceEditor::selectPreviewSource(const QJsonObject& Requested) {
    if(sourceComposing_ || !ready_ || !previewView_ || previewActiveToken_!=Token())return false;
    auto Anchor=Requested;
    if(Anchor.isEmpty()){const auto State=previewView_->ViewState();Anchor=State["sourceSelection"].toObject();const auto Serial=Anchor["serial"].toInteger();if(Serial<=sourceSelectionSerial_)return false;sourceSelectionSerial_=Serial;}
    const auto Spine=model_.Publication()["spine"].toArray();const int Chapter=Anchor["chapter"].toInt(-1);if(Chapter<0 || Chapter>=Spine.size())return false;const auto Path=Spine[Chapter].toObject()["path"].toString();
    if(current_!=Path){previewSyncing_=true;const bool Opened=openResource(Path);previewSyncing_=false;if(!Opened)return false;}if(!sourceShown())return false;
    const auto* Map=sourceMap();if(!Map)return false;const int Position=Map->Position(Anchor);if(Position<0)return false;
    // A preview click with scroll sync puts the line at the click's height instead of merely revealing it.
    const double Ratio=Anchor["ratio"].toDouble(-1);const bool Place=Ratio>=0 && scrollSyncActive();
    updating_=true;setSourceSelection(Position,Position,!Place);if(Place)placeSourceLine(Position,Ratio);updating_=false;return true;
}
}

namespace hc::document {
namespace {
// Session reads follow already queued saves, including a just-closed editor's final
// snapshot. This pool never runs widget/model work and is independent of preview jobs.
QThreadPool* workspacePool() {
    struct Pool final:QThreadPool {Pool(){setMaxThreadCount(1);}};
    static Pool Instance;return &Instance;
}
QString workspaceFile() {
    const auto Override=QCoreApplication::instance()->property("epubWorkspaceDirectory");
    const auto Config=QCoreApplication::instance()->property("documentViewsConfigDir");
    return hc::defaults::edit::EpubWorkspaceFile(Override.isValid()?Override.toString():Config.isValid()?Config.toString():hc::settings::DocumentPaths().directory_);
}
QString workspaceKey(const QString& Path) {
    const QFileInfo Info(Path);const auto Canonical=Info.canonicalFilePath();
    return QDir::fromNativeSeparators(Canonical.isEmpty()?Info.absoluteFilePath():Canonical).toCaseFolded();
}
bool readWorkspaceFile(const QString& Path,QJsonObject& Root,QString& Error) {
    Root={{"schemaVersion",1},{"documents",QJsonObject{}}};
    QFile File(Path);if(!File.exists())return true;
    if(!File.open(QIODevice::ReadOnly) || File.size()>8*1024*1024){Error="workspace-read-failed";return false;}
    QJsonParseError Parse;const auto Json=QJsonDocument::fromJson(File.read(8*1024*1024+1),&Parse);
    if(File.error()!=QFileDevice::NoError || Parse.error!=QJsonParseError::NoError || !Json.isObject()
        || Json.object()["schemaVersion"].toInt()!=1 || !Json.object()["documents"].isObject()){
        Error="workspace-invalid-json";return false;
    }
    Root=Json.object();return true;
}
}
struct EpubSourceEditor::WorkspaceState {
    struct Snapshot {QJsonObject document_;QMap<QString,QByteArray> sources_;QString utf8Resource_;};
    std::mutex mutex_;QMap<QString,Snapshot> pending_;bool running_=false;
    // Result of the last write round. The editor reads it here rather than from the
    // future: setFuture() drops a finished signal that has not been delivered yet.
    QString error_;
};
QFuture<EpubSourceEditor::WorkspaceRead> EpubSourceEditor::readWorkspace(QString FilePath,QString Source) {
    return QtConcurrent::run(workspacePool(),[FilePath=std::move(FilePath),Source=std::move(Source)]{
        WorkspaceRead Result;QJsonObject Root;
        if(readWorkspaceFile(FilePath,Root,Result.error_))Result.document_=Root["documents"].toObject()[workspaceKey(Source)].toObject();
        return Result;
    });
}
void EpubSourceEditor::setupWorkspace() {
    workspaceFile_=workspaceFile();workspaceState_=std::make_shared<WorkspaceState>();
    connect(&workspaceWriter_,&QFutureWatcher<QString>::finished,this,[this]{
        const auto Error=[this]{const std::lock_guard Guard(workspaceState_->mutex_);return workspaceState_->error_;}();
        if(!Error.isEmpty()){
            if(Error=="workspace-lock-failed"){workspaceLastDocument_={};workspaceTimer_.start(1000);}
            if(workspaceError_!=Error)report(tr("EPUB 워크스페이스 저장 실패: %1 (%2)").arg(workspaceFile_,Error));
        }
        workspaceError_=Error;
        // Reporting session persistence must not emit StateChanged and schedule another save.
    });
}
void EpubSourceEditor::scheduleWorkspace() {
    if(ready_ && !updating_ && !workspaceRestoring_ && !sourcePath_.isEmpty())workspaceTimer_.start();
}
void EpubSourceEditor::saveWorkspace() {
    workspaceTimer_.stop();if(!ready_ || workspaceRestoring_ || sourcePath_.isEmpty())return;
    QJsonArray Tabs;WorkspaceState::Snapshot Snapshot;
    for(const auto& Path:openResources_){
        auto Saved=workspacePositions_.value(Path).toObject();
        if(sourceBuffers_.contains(Path)){
            const auto& State=sourceBuffers_[Path];
            // firstVisibleLine stays for older readers; it now holds the document line.
            Saved={{"position",State.position_},{"anchor",State.anchor_},{"topLine",State.line_},{"topSubLine",State.subLine_},
                {"firstVisibleLine",State.line_},{"horizontalOffset",State.offset_}};
            if(Path==current_ && sourceShown()){
                // Scintilla already owns byte positions. Counting UTF-16 units from
                // the document start here pauses input near the end of a large chapter.
                auto* Backend=direct(text_);Snapshot.utf8Resource_=Path;const auto Top=scrollTop();
                Saved={{"position",int(Backend->Send(SCI_GETCURRENTPOS))},{"anchor",int(Backend->Send(SCI_GETANCHOR))},
                    {"topLine",Top.line_},{"topSubLine",Top.subLine_},{"firstVisibleLine",Top.line_},{"horizontalOffset",horizontalOffset()}};
                Snapshot.sources_[Path]=sourceBytes();
            } else Snapshot.sources_[Path]=State.applied_;
        }
        Saved["resource"]=Path;Tabs.append(Saved);
    }
    Snapshot.document_={{"path",sourcePath_},{"activeResource",current_},{"tabs",Tabs},
        {"sourceSha256",QString::fromLatin1(sourceHashAtOpen_.toHex())},{"positionUnits","text"}};
    if(workspaceLastDocument_==Snapshot.document_ && workspaceLastSources_==Snapshot.sources_)return;
    workspaceLastDocument_=Snapshot.document_;workspaceLastSources_=Snapshot.sources_;
    {
        const std::lock_guard Guard(workspaceState_->mutex_);
        // Save As can leave snapshots for two paths. Coalesce only within the same book.
        workspaceState_->pending_[sourcePath_]=std::move(Snapshot);
        if(workspaceState_->running_)return;workspaceState_->running_=true;
    }
    const int LockMs=qBound(1,qApp->property("epubWorkspaceLockTimeoutMs").toInt()>0?qApp->property("epubWorkspaceLockTimeoutMs").toInt():5000,60000);
    workspaceWriter_.setFuture(QtConcurrent::run(workspacePool(),[State=workspaceState_,FilePath=workspaceFile_,LockMs]{
        QString Error;int LockAttempts=0;
        for(;;){
            QMap<QString,WorkspaceState::Snapshot> Pending;
            {
                const std::lock_guard Guard(State->mutex_);
                // A batch still held after the last lock attempt stays queued; the editor's
                // next save starts a new round with it.
                if(State->pending_.isEmpty() || LockAttempts>=3){State->running_=false;State->error_=Error;return Error;}
                Pending.swap(State->pending_);
            }
            Error.clear();
            // No filesystem operation, lock wait or atomic commit is allowed on the GUI thread.
            if(!QDir().mkpath(QFileInfo(FilePath).absolutePath()))Error="workspace-create-directory-failed";
            QLockFile Lock(FilePath+".lock");Lock.setStaleLockTime(30000);
            if(Error.isEmpty() && !Lock.tryLock(LockMs)){
                // Another writer holds the file. Requeue the batch instead of dropping it:
                // it may be the final position of an editor that has already closed, or the
                // previous path of a Save As. A newer snapshot of the same book wins.
                Error="workspace-lock-failed";++LockAttempts;
                const std::lock_guard Guard(State->mutex_);
                for(auto It=Pending.begin();It!=Pending.end();++It)if(!State->pending_.contains(It.key()))State->pending_.insert(It.key(),std::move(It.value()));
                State->error_=Error;qWarning("EPUB workspace save waits for lock: %s",qPrintable(FilePath));continue;
            }
            LockAttempts=0;
            QJsonObject Root;
            if(Error.isEmpty() && readWorkspaceFile(FilePath,Root,Error)){
                auto Documents=Root["documents"].toObject();
                for(auto It=Pending.begin();It!=Pending.end();++It){
                    auto Document=It->document_;auto Tabs=Document["tabs"].toArray();
                    for(int I=0;I<Tabs.size();++I){
                        auto Tab=Tabs[I].toObject();const auto Resource=Tab["resource"].toString();
                        if(It->sources_.contains(Resource)){
                            const auto Source=It->sources_.value(Resource);const auto Text=QString::fromUtf8(Source);
                            if(Resource==It->utf8Resource_){
                                const auto Position=qBound(qsizetype(0),qsizetype(Tab["position"].toInt()),Source.size());
                                const auto Anchor=qBound(qsizetype(0),qsizetype(Tab["anchor"].toInt()),Source.size());
                                const auto Utf16=QString::fromUtf8(Source.constData(),Position).size();
                                Tab["position"]=int(Utf16);Tab["anchor"]=int(Anchor==Position?Utf16:QString::fromUtf8(Source.constData(),Anchor).size());
                            }
                            const auto Prefix=Text.left(Tab["position"].toInt());
                            Tab["line"]=Prefix.count('\n')+1;Tab["column"]=Prefix.size()-Prefix.lastIndexOf('\n');Tabs[I]=Tab;
                        }
                    }
                    Document["tabs"]=Tabs;Document["savedAtUtc"]=QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
                    Documents[workspaceKey(It.key())]=Document;
                }
                Root["documents"]=Documents;const auto Bytes=QJsonDocument(Root).toJson();QSaveFile File(FilePath);File.setDirectWriteFallback(false);
                if(Bytes.size()>8*1024*1024 || !File.open(QIODevice::WriteOnly) || File.write(Bytes)!=Bytes.size() || !File.commit())Error="workspace-write-failed";
            }
            if(!Error.isEmpty())qWarning("EPUB workspace save failed: %s (%s)",qPrintable(FilePath),qPrintable(Error));
            {const std::lock_guard Guard(State->mutex_);State->error_=Error;}
            // The shared queue outlives the widget: closing adds the final snapshot without
            // waiting for the disk or abandoning a newer position behind an in-flight save.
        }
    }));
}
bool EpubSourceEditor::restoreWorkspace() {
    if(!workspaceError_.isEmpty()){report(tr("EPUB 워크스페이스를 읽지 못했습니다: %1").arg(workspaceFile_));return false;}
    const auto Saved=std::exchange(workspaceSaved_,{});
    if(!Saved["tabs"].isArray())return false;
    workspaceLegacyUnits_=Saved["positionUnits"].toString()!="text";
    workspaceRestoring_=true;openResources_.clear();workspacePositions_={};
    for(const auto& Value:Saved["tabs"].toArray()){
        const auto Tab=Value.toObject();const auto Path=Tab["resource"].toString();
        if(openResources_.size()>=256)break;
        if(model_.IsEditable(Path) && !openResources_.contains(Path)){openResources_.append(Path);workspacePositions_[Path]=Tab;}
    }
    current_=Saved["activeResource"].toString();if(!openResources_.contains(current_))current_=openResources_.value(0);
    // If all formerly open resources were removed externally, use the normal initial chapter.
    if(openResources_.isEmpty() && !Saved["tabs"].toArray().isEmpty()){workspaceRestoring_=false;return false;}
    if(!current_.isEmpty())select();
    else {bufferResource_.clear();sourceStack_->setCurrentWidget(sourceStack_->findChild<QLabel*>("epubSourceEmpty"));refreshTabs();}
    workspaceRestoring_=false;workspaceRestored_=true;previewPreferredChapter_=spineIndex(current_);return true;
}
}
