#include "stdafx.h"
#include "epubSourceEditor.hpp"
#include <QApplication>
#include <Windows.h>
#include "epubProcessView.hpp"
#include "epubSourceTabs.hpp"
#include "commonLibs/document/epub/epubSourceLines.hpp"
#include "core/AppSettings.h"
#include "text/QTextView.h"
#include "text/ScintillaQtDirectBackend.h"
#include <Scintilla.h>
#include <QAction>
#include <QCryptographicHash>
#include <QDir>
#include <QDateTime>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QLabel>
#include <QPainter>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStackedLayout>
#include <QTemporaryDir>
#include <QToolButton>
#include <QVBoxLayout>
#include <QtConcurrentRun>
#include <algorithm>
#include <fmstyle/ThemeManager.h>
namespace hc::document {
namespace {
class EpubPreviewHeader final:public QWidget {
public:
    using QWidget::QWidget;
protected:
    void paintEvent(QPaintEvent* Event)override {
        QWidget::paintEvent(Event);QPainter Painter(this);
        Painter.fillRect(QRect(0,height()-1,width(),1),fm::style::themeColorsFor(this)[fm::style::Token::Line]);
    }
};
ScintillaQtDirectBackend* direct(QTextView* View){return static_cast<ScintillaQtDirectBackend*>(View->Backend());}
// A refused patch that a reload of the book resolves: a change the chapter checks leave to the whole
// publication (head, navigation, styles, layout), or a checked chapter the reader could not show. Any
// other error is in the text itself, and the preview waits for the next edit.
bool needsReload(const QJsonObject& Result){
    return Result["error"].toString().startsWith("preview-full-required") || Result["validated"].toInteger(-1)==Result["accepted"].toInteger(-2);
}
}
// Follow the actual input surface when the ruler, tabs or recovery notice changes.
void EpubSourceEditor::alignPreview() {
    auto* Header=previewPanel_->findChild<QWidget*>("epubPreviewHeader");
    if(!Header)return;
    auto* Controls=Header->findChild<QWidget*>("epubPreviewControls");if(!Controls)return;
    if(auto* Refresh=Controls->findChild<QToolButton*>())Refresh->setFixedHeight(Refresh->fontMetrics().height()+6);
    const bool Beside=previewSplitter_->orientation()==Qt::Horizontal && IsSourceVisible() && sourceShown();
    const int RowHeight=Controls->sizeHint().height();Controls->setFixedHeight(RowHeight);
    // The source header also contains a ruler/recovery notice. Align the status row with
    // the active tab's primary line while keeping the preview body level with the editor input.
    const auto TitleRect=static_cast<EpubSourceTabs*>(tabs_)->PrimaryTextRect(tabs_->currentIndex());
    const int TitleY=tabs_->mapTo(sourcePanel_,TitleRect.center()).y();
    const int Top=Beside && TitleRect.isValid()?qMax(0,TitleY-RowHeight/2):2;
    Header->layout()->setContentsMargins(0,Top,0,1);
    const int Height=Beside?qMax(SourceInput()->mapTo(sourcePanel_,QPoint{}).y(),Top+RowHeight+1):Top+RowHeight+1;
    if(Header->height()!=Height || Header->minimumHeight()!=Height)Header->setFixedHeight(Height);
}
void EpubSourceEditor::ConfigurePreview(const WebProcessRuntime& Runtime,int Chapter) {
    if(previewRuntime_)return;if(workspaceRestored_)Chapter=spineIndex(current_);previewRuntime_=Runtime;previewPreferredChapter_=previewRestoreChapter_=Chapter;
    auto* Panel=previewPanel_;auto* Layout=new QVBoxLayout(Panel);Layout->setContentsMargins(0,0,0,0);Layout->setSpacing(0);
    auto* Header=new EpubPreviewHeader(Panel);Header->setObjectName("epubPreviewHeader");
    auto* HeaderLayout=new QVBoxLayout(Header);HeaderLayout->setContentsMargins(0,2,0,1);HeaderLayout->setSpacing(0);
    auto* ControlRow=new QWidget(Header);ControlRow->setObjectName("epubPreviewControls");HeaderLayout->addWidget(ControlRow);HeaderLayout->addStretch();
    auto* Controls=new QHBoxLayout(ControlRow);Controls->setContentsMargins(8,2,4,2);Controls->setSpacing(4);
    connect(&fm::style::ThemeManager::instance(),&fm::style::ThemeManager::changed,Header,[Header]{Header->update();});
    // Keep layout and preview toggles in the View menu so the header can stay compact.
    auto* Title=new QLabel(tr("미리보기"),ControlRow);Title->setObjectName("epubPreviewTitle");auto Font=Title->font();Font.setWeight(QFont::DemiBold);Title->setFont(Font);Title->setMinimumWidth(1);Controls->addWidget(Title);
    previewChip_=new QLabel(Header);previewChip_->setObjectName("epubPreviewState");previewChip_->setTextFormat(Qt::PlainText);previewChip_->setAccessibleName(tr("EPUB 미리보기 상태"));previewChip_->setMinimumWidth(24);Controls->addWidget(previewChip_);Controls->addStretch();
    auto* Refresh=new QToolButton(Header);Refresh->setDefaultAction(action("epubPreviewRefresh"));Refresh->setAutoRaise(true);Refresh->setToolButtonStyle(Qt::ToolButtonTextOnly);Controls->addWidget(Refresh);
    previewChapterLabel_=new QLabel(Header);previewChapterLabel_->setObjectName("epubPreviewChapter");previewChapterLabel_->setForegroundRole(QPalette::PlaceholderText);previewChapterLabel_->setMinimumWidth(1);Controls->addWidget(previewChapterLabel_);Layout->addWidget(Header);
    auto* Frame=new QWidget(Panel);Frame->setMinimumSize(200,140);Frame->setObjectName("epubPreviewFrame");previewStack_=new QStackedLayout(Frame);previewStack_->setContentsMargins(0,0,0,0);previewStack_->setStackingMode(QStackedLayout::StackAll);Layout->addWidget(Frame,1);
    QTimer::singleShot(0,this,&EpubSourceEditor::alignPreview);
    reportPreview(tr("저장 전 미리보기 · 공통 읽기 설정 · 사용자 읽기 설정과 기록은 유지됩니다."));
    {AppSettings Settings;setPanelVisible("preview",Settings.value("epubEditor/layout/preview").toBool());}
    previewTimer_.setSingleShot(true);previewTimer_.setInterval(650);
    // Foreign Qt windows can receive activation while their HWND is reparented,
    // even with WS_EX_NOACTIVATE. Repair only a focus loss caused by our passive
    // candidate, or by the reader a reload replaces inside the shown preview's helper
    // (until shortly after it took over); an explicit control choice or another application always wins.
    connect(qApp,&QApplication::focusChanged,this,[this](QWidget* Before,QWidget* After){
        auto* Passive=previewCandidate_?previewCandidate_
            :previewView_ && previewReloadFocus_.isValid() && (previewReloadPending_ || previewReloadFocus_.elapsed()<3000)?previewView_:nullptr;
        if(!previewOwnerActive()) {
            DWORD ForegroundPid=0;GetWindowThreadProcessId(GetForegroundWindow(),&ForegroundPid);
            if(!Passive || ForegroundPid!=Passive->ViewState()["processId"].toInteger()){suspendInactivePreview();return;}
        }
        if(After || !Before || !Passive || !(sourcePanel_->isAncestorOf(Before) || treePanel_->isAncestorOf(Before)))return;
        const QPointer<QWidget> Input=Before;const QPointer<EpubProcessView> Candidate=Passive;
        auto* Retry=new QTimer(this);Retry->setInterval(40);
        // Reparenting and activation are asynchronous. The candidate may already
        // be the displayed preview when Windows finally transfers keyboard input.
        connect(qApp,&QApplication::focusChanged,Retry,[Retry,Input](QWidget*,QWidget* Next){
            if(Next && Next!=Input){Retry->stop();Retry->deleteLater();}
        });
        connect(Retry,&QTimer::timeout,this,[this,Input,Candidate,Retry,Attempts=0]()mutable{
            const auto Finish=[Retry]{Retry->stop();Retry->deleteLater();};
            if(++Attempts>50 || !Input || !Candidate || (Candidate!=previewCandidate_ && Candidate!=previewView_)
                || previewClosing_ || !IsSourceVisible() || !Input->isVisible() || !Input->isEnabled()
                || (QApplication::focusWidget() && QApplication::focusWidget()!=Input)
                || QApplication::activePopupWidget() || QApplication::activeModalWidget()
                || GetAsyncKeyState(VK_LBUTTON)<0 || GetAsyncKeyState(VK_RBUTTON)<0){Finish();return;}
            const auto Foreground=GetForegroundWindow();const auto Root=GetAncestor(reinterpret_cast<HWND>(window()->winId()),GA_ROOT);
            DWORD Pid=0;GetWindowThreadProcessId(Foreground,&Pid);
            const auto PreviewPid=Candidate->ViewState()["processId"].toInteger();
            if(!Root || !Foreground)return;
            // A foreign helper can temporarily own activation before it is parented.
            // Never activate the editor after the user has switched applications.
            if(GetAncestor(Foreground,GA_ROOT)!=Root && (!PreviewPid || Pid!=PreviewPid)){Finish();return;}
            if(GetAncestor(Foreground,GA_ROOT)!=Root)return;
            SetActiveWindow(Root);QApplication::setActiveWindow(window());
            if(Input){SetFocus(reinterpret_cast<HWND>(Input->effectiveWinId()));Input->setFocus(Qt::OtherFocusReason);}
            if(Input && Input->hasFocus() && GetFocus()==reinterpret_cast<HWND>(Input->effectiveWinId()))Finish();
        });
        Retry->start();
    });
    connect(qApp,&QGuiApplication::applicationStateChanged,this,[this]{suspendInactivePreview();});
    connect(&previewTimer_,&QTimer::timeout,this,[this]{requestPreview();});
    connect(text_->Backend(),&ITextEditorBackend::textEdited,this,&EpubSourceEditor::previewSourceEdited);
    connect(&previewVerify_,&QFutureWatcherBase::finished,this,[this]{
        // A different text than the helper's (an edit the notifications missed) resends the whole chapter.
        if(previewVerify_.result().toHex()==previewVerifySha_.toLatin1() || !previewSync_.contains(previewVerifyPath_))return;
        auto& Sync=previewSync_[previewVerifyPath_];if(Sync.revision_!=previewVerifyRevision_)return;
        Sync.revision_=-1;++previewResyncs_;previewActiveToken_.clear();previewTimer_.start();
    });
    connect(this,&EpubSourceEditor::Changed,this,[this]{
        // flush() moves the draft into the model on save and on chapter switches; the content
        // identity stays the same. Retiring the candidate then would restart a helper process
        // whose input queue is joined with this thread's while it starts and while it closes.
        const auto Identity=Token();
        if(Identity==previewEpochToken_) {
            // requestPreview() still settles a preview that is not current; it never retires one that is.
            if(action("epubAutoPreview")->isChecked() && !previewClosing_ && !previewPanel_->isHidden())previewTimer_.start();
            updateStatus();return;
        }
        previewEpochToken_=Identity;
        ++previewEpoch_;previewPending_=action("epubAutoPreview")->isChecked();if(previewCancel_ && (!nativeValidating_ || validationToken_!=Token() || previewCancel_!=validationCancel_))previewCancel_->store(true);
        if(previewCandidate_){auto* Old=std::exchange(previewCandidate_,nullptr);retirePreview(Old);}
        if(action("epubAutoPreview")->isChecked() && !previewClosing_ && !previewPanel_->isHidden())previewTimer_.start();updateStatus();
    });
    connect(&worker_,&QFutureWatcherBase::finished,this,[this]{if((action("epubAutoPreview")->isChecked() || previewMissing()) && !previewClosing_)previewTimer_.start();});
    connect(&previewWorker_,&QFutureWatcherBase::finished,this,[this]{
        previewExportMs_=previewBuildClock_.isValid()?previewBuildClock_.elapsed():0;
        const auto Result=previewWorker_.result();if(previewClosing_){RequestPreviewClose();emit StateChanged();return;}
        if(previewWorkEpoch_!=previewEpoch_ || previewWorkToken_!=Token()){if(action("epubAutoPreview")->isChecked() || previewPending_ || previewMissing())previewTimer_.start();emit StateChanged();return;}
        if(!Result){previewError_=Result.error_+": "+Result.location_;reportPreview(tr("소스 검사 실패 · 마지막 정상 미리보기를 유지합니다. %1").arg(previewError_));}
        else {cachedExport_=Result;cachedExportToken_=previewWorkToken_;previewError_.clear();if(!reloadPreview(Result.bytes_))showPreview(Result.bytes_);}emit StateChanged();
    });
    // Preview navigation state lives in a private directory. Reading appearance
    // follows the global settings through the helper configuration. No
    // candidate publication can add entries to the user's reading history.
    previewDirectory_=std::make_unique<QTemporaryDir>(QDir::tempPath()+"/HyperCommander-epub-preview-XXXXXX");previewDirectory_->setAutoRemove(false);
    if(!previewDirectory_->isValid()){previewError_="epub-preview-directory";reportPreview(tr("미리보기 임시 폴더를 만들지 못했습니다. 소스 편집은 계속할 수 있습니다."));return;}
    previewSettings_=previewDirectory_->path();
    if(ready_)requestPreview();
}
bool EpubSourceEditor::previewOwnerActive()const {
    if(!window()->isVisible() || window()->isMinimized() || !window()->internalWinId())return false;
    const auto Root=GetAncestor(reinterpret_cast<HWND>(window()->internalWinId()),GA_ROOT);
    return Root && GetAncestor(GetForegroundWindow(),GA_ROOT)==Root;
}
void EpubSourceEditor::suspendInactivePreview() {
    if(previewClosing_ || previewOwnerActive() || !previewCandidate_)return;
    DWORD ForegroundPid=0;GetWindowThreadProcessId(GetForegroundWindow(),&ForegroundPid);
    if(ForegroundPid==previewCandidate_->ViewState()["processId"].toInteger())return;
    auto* Old=std::exchange(previewCandidate_,nullptr);retirePreview(Old);previewPending_=true;previewTimer_.start();
}
QMap<QString,QByteArray> EpubSourceEditor::previewChanges()const {
    auto Changes=model_.Changes();const auto Draft=draft();
    for(auto At=Draft.begin();At!=Draft.end();++At)Changes[At.key()]=At.value();
    return Changes;
}
bool EpubSourceEditor::patchPreview() {
    if(!previewView_ || previewCandidate_ || previewReloadPending_ || previewPatchRejected_==Token()
        || previewEdition_!=model_.Publication()["sourceSha256"].toString())return false;
    const auto State=previewView_->ViewState();const int Chapter=spineIndex(current_);
    // A chapter that is not shown is kept by the helper, which loads it when it is displayed.
    if(Chapter<0 || State["status"]!="ready" || State["readerBusy"].toBool() || mediaOf(current_)!="application/xhtml+xml")return false;
    auto Changes=previewChanges();auto Before=previewChanges_,After=Changes;
    Before.remove(current_);After.remove(current_);if(Before!=After)return false;
    if(previewSyncView_!=previewView_){previewSync_.clear();previewSyncView_=previewView_;}
    auto& Sync=previewSync_[current_];auto* Backend=direct(text_);
    const qintptr Document=sourceShown()?qintptr(Backend->Send(SCI_GETDOCPOINTER)):0;const qint64 Length=Document?qint64(Backend->Send(SCI_GETLENGTH)):-1;
    previewPatchToken_=Token();
    auto Scroll=scrollSyncActive()?sourceScrollTarget(false):QJsonObject{};
    if(!Scroll.isEmpty())Scroll["serial"]=++sourceScrollSerial_;
    QJsonObject Patch{{"serial",++previewPatchSerial_},{"chapter",Chapter},{"path",current_},{"scroll",Scroll},{"revision",++previewSourceRevision_}};
    QString Action="preview-chapter";
    if(Sync.revision_>=0 && Document && Sync.document_==Document && Sync.range_.Valid() && Sync.range_.Length()==Length) {
        // Only the bytes changed since the helper's revision travel, at UTF-8 offsets of the raw buffer text.
        const auto Start=Sync.range_.Start(),End=Sync.range_.InsertedEnd();
        QByteArray Inserted(End-Start+1,Qt::Uninitialized);
        Sci_TextRangeFull Range{};Range.chrg.cpMin=Start;Range.chrg.cpMax=End;Range.lpstrText=Inserted.data();
        Backend->Send(SCI_GETTEXTRANGEFULL,0,reinterpret_cast<qintptr>(&Range));Inserted.resize(End-Start);
        // Bytes that are not UTF-8 would change on the way; the chapter goes whole then.
        const auto Text=QString::fromUtf8(Inserted);
        if(Text.toUtf8()==Inserted) {
            Action="preview-edit";Patch["baseRevision"]=Sync.revision_;Patch["baseLength"]=Sync.range_.BaseLength();
            Patch["start"]=Start;Patch["removed"]=Sync.range_.Removed();Patch["inserted"]=Text;
        }
    }
    if(Action=="preview-chapter")Patch["text"]=QString::fromUtf8(Changes.contains(current_)?Changes.value(current_):model_.Read(current_));
    if(!previewView_->ReaderAction(Action,Patch))return false;
    // The helper holds this revision unless it answers otherwise; later edits count from it.
    Sync.revision_=-1;Sync.document_=Document;Sync.range_.Reset(Length);
    previewPatchPath_=current_;previewPatchRevision_=Patch["revision"].toInteger();if(Action=="preview-edit")++previewPatchEdits_;
    previewPatchShiftCounts_.clear();for(auto At=sourceBuffers_.cbegin();At!=sourceBuffers_.cend();++At)previewPatchShiftCounts_[At.key()]=At->shifts_.size();
    previewPatchChanges_=std::move(Changes);previewPatchPending_=true;previewPatchClock_.start();++previewPatchRuns_;
    QTimer::singleShot(15000,this,[this]{finishPreviewPatch();});
    return true;
}
void EpubSourceEditor::finishPreviewPatch() {
    if(!previewPatchPending_ || previewClosing_)return;
    const auto Result=previewView_?previewView_->ViewState()["previewPatch"].toObject():QJsonObject{};
    const bool Answered=Result["serial"].toInteger(-1)==previewPatchSerial_ && Result["status"]=="done";
    if(!Answered && previewView_ && previewPatchClock_.elapsed()<15000)return;
    previewPatchPending_=false;
    const auto Error=Result["error"].toString();
    if(Answered && Result["accepted"].toInteger(-1)==previewPatchRevision_) {
        auto& Sync=previewSync_[previewPatchPath_];Sync.revision_=previewPatchRevision_;
        if(!Sync.range_.Changed())verifyPreviewSource(previewPatchPath_,previewPatchRevision_,Result["sha256"].toString());
    }
    // No answer: the next request reloads the book in the helper.
    if(!Answered)previewPatchRejected_=previewPatchToken_;
    else if(Error=="preview-resync")++previewResyncs_;
    else if(!Error.isEmpty()) {
        if(needsReload(Result))previewPatchRejected_=previewPatchToken_;
        else {
            // The helper keeps the invalid text for the next edit; the reader keeps its last good frame.
            previewPatchInvalid_=previewPatchToken_;previewError_=Error;
            reportPreview(tr("소스 검사 실패 · 마지막 정상 미리보기를 유지합니다. %1").arg(Error));
        }
    }
    else {
        previewActiveToken_=previewPatchToken_;previewChanges_=previewPatchChanges_;previewMatchesSource_=previewActiveToken_==Token();
        previewReadyMs_=previewPatchClock_.elapsed();previewExportMs_=0;previewError_.clear();previewPatchInvalid_.clear();
        previewUpdatedAt_=QDateTime::currentMSecsSinceEpoch();
        // Keep line shifts from edits typed while this asynchronous update was in flight.
        for(auto At=sourceBuffers_.begin();At!=sourceBuffers_.end();++At) {
            At->shifts_.remove(0,qMin(previewPatchShiftCounts_.value(At.key()),At->shifts_.size()));
            if(previewActiveToken_==Token())At->shiftsOverflow_=false;
        }
        lastScrollLine_=-1;previewScrollSeen_=-1;
        syncPreviewScroll();emit PreviewCommitted();
    }
    if(previewActiveToken_!=Token() && previewPatchInvalid_!=Token())previewTimer_.start();
    emit StateChanged();
}
void EpubSourceEditor::previewSourceEdited(qint64 Position,qint64 Removed,qint64 Inserted) {
    if(bufferResource_.isEmpty())return;
    const auto Sync=previewSync_.find(bufferResource_);if(Sync==previewSync_.end())return;
    // A buffer that replaced the chapter's document has no common base with the helper's text.
    if(Sync->document_!=qintptr(direct(text_)->Send(SCI_GETDOCPOINTER)))Sync->range_.Invalidate();
    else Sync->range_.Apply(Position,Removed,Inserted);
}
void EpubSourceEditor::verifyPreviewSource(const QString& Path,qint64 Revision,const QString& Sha256) {
    if(Path!=current_ || !sourceShown() || previewVerify_.isRunning() || Sha256.size()!=64)return;
    previewVerifyPath_=Path;previewVerifyRevision_=Revision;previewVerifySha_=Sha256;
    previewVerify_.setFuture(QtConcurrent::run([Bytes=sourceBytes()]{return QCryptographicHash::hash(Bytes,QCryptographicHash::Sha256);}));
}
// The export opens in the shown preview's helper, which replaces its reader after restoring the reading
// settings, the chapter and the source position (OwnerController::reload in HyperEpubWeb). Only a lost
// helper starts a new process (showPreview).
bool EpubSourceEditor::reloadPreview(const QByteArray& Bytes) {
    if(!previewView_ || !previewView_->IsLoaded() || previewClosing_ || previewReloadPending_)return false;
    const auto State=previewView_->ViewState();if(State["status"]!="ready")return false;
    const int Target=action("epubPreviewScrollSync")->isChecked()?spineIndex(current_):-1;
    QJsonObject Scroll;if(Target>=0 && scrollSyncActive() && sourceShown()){Scroll=sourceScrollTarget(false);Scroll["serial"]=++sourceScrollSerial_;}
    const auto Serial=previewView_->Reload(Bytes,{{"chapter",Target},{"locator",State["locator"].toString()},{"scroll",Scroll}});
    if(!Serial)return false;
    previewReloadSerial_=Serial;previewReloadPending_=true;++previewReloadRuns_;previewReloadFocus_.start();
    previewReloadToken_=previewWorkToken_;previewReloadChanges_=previewWorkChanges_;previewReloadEdition_=previewWorkEdition_;
    reportPreview(tr("새 본문·내장 글꼴·이미지를 준비하고 있습니다…"));
    return true;
}
void EpubSourceEditor::finishPreviewReload() {
    if(!previewReloadPending_ || previewClosing_ || !previewView_)return;
    const auto Reload=previewView_->ViewState()["reload"].toObject();
    if(Reload["serial"].toInteger(-1)!=previewReloadSerial_)return;
    if(Reload["status"]=="failed") {
        // The shown reader stays: a refresh error, not a lost preview.
        previewReloadPending_=false;previewError_=Reload["error"].toString();
        reportPreview(tr("미리보기를 갱신하지 못했습니다: %1 · 마지막 정상 화면을 유지합니다.").arg(previewError_));
        emit StateChanged();return;
    }
    if(Reload["status"]!="ready")return;
    previewReloadPending_=false;previewReloadFocus_.start();
    const auto State=previewView_->ViewState();
    previewChanges_=previewReloadChanges_;previewEdition_=previewReloadEdition_;previewReadyMs_=previewBuildClock_.isValid()?previewBuildClock_.elapsed():0;
    previewUpdatedAt_=QDateTime::currentMSecsSinceEpoch();previewActiveToken_=previewReloadToken_;previewMatchesSource_=previewActiveToken_==Token();
    sourceSelectionSerial_=State["sourceSelection"].toObject()["serial"].toInteger(-1);contextRequestSerial_=State["contextRequest"].toObject()["serial"].toInteger(-1);
    // The new reader shows the exported text and holds no chapter text of earlier patches.
    clearLineShifts();previewScrollSeen_=-1;lastScrollLine_=-1;lastScrollChapter_=-1;scrollSyncPending_=true;previewChapter_=State["chapterIndex"].toInt(-1);
    previewNavigationSerial_=-1;previewRecoveryPosition_=false;previewSync_.clear();previewPatchRejected_.clear();previewPatchInvalid_.clear();previewError_.clear();
    syncPreviewScroll();
    reportPreview(tr("저장 전 미리보기 · 공통 읽기 설정 · 표시된 내용은 아직 파일에 저장되지 않았습니다."));emit PreviewCommitted();
    if(previewActiveToken_!=Token())previewTimer_.start();
    emit StateChanged();
}
void EpubSourceEditor::requestPreview(bool Force) {
    if(sourceComposing_){previewPending_=true;previewTimer_.start();return;}
    previewTimer_.stop();if(!previewRuntime_ || previewSettings_.isEmpty() || !ready_ || previewClosing_)return;
    if(!previewOwnerActive()){suspendInactivePreview();previewPending_=true;previewTimer_.start();return;}
    if(previewPreferredChapter_>=0){if(!(sourceShown() && sourceModified()) && recoveredFrom_.isEmpty()){const auto Spine=model_.Publication()["spine"].toArray();if(previewPreferredChapter_<Spine.size()){const auto Path=Spine[previewPreferredChapter_].toObject()["path"].toString();if(model_.IsEditable(Path) && Path!=current_){current_=Path;select();}}}previewPreferredChapter_=-1;}
    if(previewPatchPending_){finishPreviewPatch();if(previewPatchPending_)return;}
    if(previewReloadPending_){finishPreviewReload();if(previewReloadPending_){previewPending_=true;return;}}
    if(previewWorker_.isRunning()){previewPending_=true;return;}
    if(!Force && previewView_ && previewActiveToken_==Token()){syncPreviewSource(true);return;}
    // The helper found this text invalid and keeps showing the last good frame until the next edit.
    if(!Force && previewView_ && previewPatchInvalid_==Token())return;
    if(previewCandidate_ && !Force && previewCandidateToken_==Token())return;
    if(previewCandidate_){auto* Old=std::exchange(previewCandidate_,nullptr);retirePreview(Old);}
    if(!Force && patchPreview())return;
    previewWorkChanges_=previewChanges();previewWorkEdition_=model_.Publication()["sourceSha256"].toString();previewBuildClock_.start();
    previewPending_=false;previewWorkToken_=Token();previewWorkEpoch_=previewEpoch_;previewError_.clear();reportPreview(tr("편집 내용을 검사하고 있습니다… 이전 미리보기는 유지됩니다."));
    if(cachedExportToken_==previewWorkToken_ && cachedExport_){if(!reloadPreview(cachedExport_.bytes_))showPreview(cachedExport_.bytes_);emit StateChanged();return;}
    if(nativeValidating_ && validationToken_==previewWorkToken_ && validationCancel_ && !validationCancel_->load()) {
        previewCancel_=validationCancel_;++exportJoins_;previewWorker_.setFuture(validation_.future());emit StateChanged();return;
    }
    ++exportRuns_;auto Model=model_;const auto Draft=draft();previewCancel_=std::make_shared<std::atomic_bool>(false);
    previewWorker_.setFuture(QtConcurrent::run([Model=std::move(Model),Draft,Cancel=previewCancel_]{return Model.Export(Draft,[Cancel]{return Cancel->load();});}));emit StateChanged();
}
void EpubSourceEditor::retirePreview(EpubProcessView* View) {
    if(!View)return;previewRetiring_.append(View);View->hide();View->RequestClose();
}
void EpubSourceEditor::showPreview(const QByteArray& Bytes) {
    if(previewClosing_)return;if(sourceComposing_){previewPending_=true;previewTimer_.start();return;}
    auto* Candidate=new EpubProcessView(*previewRuntime_,{0,0,20000},previewStack_->parentWidget());Candidate->setObjectName("epubDraftPreview");
    previewCandidateChanges_=previewWorkChanges_;previewCandidateEdition_=previewWorkEdition_;
    previewCandidate_=Candidate;previewCandidateToken_=previewWorkToken_;previewCandidateEpoch_=previewWorkEpoch_;previewCandidateSerial_=-2;previewCandidateInputRequested_=false;previewCandidateScrollSerial_=-1;previewCandidateScrollRetries_=0;previewStack_->addWidget(Candidate);
    if(previewView_)previewStack_->setCurrentWidget(previewView_);else previewStack_->setCurrentWidget(Candidate);
    Candidate->show();reportPreview(tr("새 본문·내장 글꼴·이미지를 준비하고 있습니다…"));
    connect(Candidate,&EpubProcessView::EscapeRequested,this,&EpubSourceEditor::EscapeRequested);connect(Candidate,&EpubProcessView::FullscreenRequested,this,&EpubSourceEditor::FullscreenRequested);
    connect(Candidate,&EpubProcessView::ViewStateChanged,this,[this,Candidate]{
        if(Candidate==previewCandidate_)admitPreview(Candidate);
        if(Candidate==previewView_){finishPreviewPatch();finishPreviewReload();syncPreviewSource();previewContextRequest(Candidate);previewScrolled(Candidate->ViewState()["previewScroll"].toObject());if(scrollSyncPending_)syncPreviewScroll();}
        emit StateChanged();});
    connect(Candidate,&EpubProcessView::Closed,this,[this,Candidate]{
        // A shown or pending preview that ends on its own would leave the panel empty; ask for a new one.
        const bool Lost=!previewClosing_ && (previewView_==Candidate || previewCandidate_==Candidate);
        if(previewView_==Candidate){previewView_=nullptr;previewReloadPending_=false;}if(previewCandidate_==Candidate)previewCandidate_=nullptr;
        previewRetiring_.removeIf([Candidate](const auto& View){return !View || View==Candidate;});previewStack_->removeWidget(Candidate);Candidate->deleteLater();if(previewClosing_)RequestPreviewClose();
        if(Lost && previewError_.isEmpty() && !previewPanel_->isHidden()){previewPending_=true;previewTimer_.start();}
        emit StateChanged();
    });
    connect(Candidate,&EpubProcessView::Failed,this,[this,Candidate](const QString& Error){
        if(Candidate==previewCandidate_ || Candidate==previewView_){if(Candidate==previewCandidate_)previewCandidate_=nullptr;else{previewView_=nullptr;previewReloadPending_=false;}previewError_=Error;reportPreview(tr("미리보기를 표시하지 못했습니다: %1 · 소스와 남아 있는 정상 화면을 유지합니다.").arg(Error));retirePreview(Candidate);}emit StateChanged();
    });
    connect(Candidate,&EpubProcessView::Loaded,this,[this,Candidate]{admitPreview(Candidate);});
    if(!Candidate->Load(Bytes,qint64(previewCandidateEpoch_+1),previewSettings_,false,true)){previewCandidate_=nullptr;previewError_="epub-preview-start";retirePreview(Candidate);}
}
void EpubSourceEditor::admitPreview(EpubProcessView* Candidate) {
    if(sourceComposing_ || Candidate!=previewCandidate_ || !Candidate->IsLoaded())return;
    if(previewClosing_ || previewCandidateEpoch_!=previewEpoch_ || previewCandidateToken_!=Token()){previewCandidate_=nullptr;retirePreview(Candidate);if(!previewClosing_ && (action("epubAutoPreview")->isChecked() || previewMissing()))previewTimer_.start();return;}
    const auto State=Candidate->ViewState();if(State["status"]!="ready" || State["readerBusy"].toBool() || State["settingsBusy"].toBool() || !State["readingStateLoaded"].toBool() || !State["readingProfile"].toObject()["privateReading"].toBool())return;
    if(!State["annotationsTransient"].toBool()) {if(!Candidate->property("epubTransientRequested").toBool())Candidate->setProperty("epubTransientRequested",Candidate->ReaderAction("annotations-transient",true));return;}
    // Editing previews own their context menu and width; readers keep the defaults.
    if(!Candidate->property("epubEditingRequested").toBool()){Candidate->setProperty("epubEditingRequested",true);Candidate->ReaderAction("context-menu",true);if(previewWidth_>0)Candidate->ReaderAction("preview-width",previewWidth_);}
    if(previewCandidateSerial_==-2) {
        const int OldChapter=!previewRecoveryPosition_ && previewView_?previewView_->ViewState()["chapterIndex"].toInt(-1):previewRestoreChapter_;const auto Locator=!previewRecoveryPosition_ && previewView_?previewView_->ViewState()["locator"].toString():recoveryLocator_;
        int Target=-1;const auto Spine=model_.Publication()["spine"].toArray();if(action("epubPreviewScrollSync")->isChecked())for(int I=0;I<Spine.size();++I)if(Spine[I].toObject()["path"]==current_)Target=I;
        if(Target<0 && previewRecoveryPosition_)Target=previewRestoreChapter_;
        const bool Located=Target>=0 && (Target!=OldChapter || Locator.isEmpty() && Target!=State["chapterIndex"].toInt(-1))?Candidate->ReaderAction("chapter",Target):!Locator.isEmpty()?Candidate->ReaderAction("preview-location",Locator):false;
        previewCandidateSerial_=Located?State["readerAction"].toObject()["serial"].toInteger()+1:-1;if(Located)return;
    }
    if(previewCandidateSerial_>=0 && State["readerAction"].toObject()["serial"].toInteger()<previewCandidateSerial_)return;
    if(!State["inputEnabled"].toBool()) {if(!previewCandidateInputRequested_)previewCandidateInputRequested_=Candidate->ReaderAction("enable-input",true);return;}
    // A replacement is aligned with the editor while it is still behind the shown preview, so the swap does not jump.
    // It shows the current text, so no line shifts apply. 0 means aligned or skipped.
    if(previewCandidateScrollSerial_!=0 && scrollSyncActive() && sourceShown() && State["chapterIndex"].toInt(-1)==spineIndex(current_) && State["previewScroll"].toObject()["supported"].toBool()) {
        if(previewCandidateScrollSerial_<0) {
            auto Target=sourceScrollTarget(false);Candidate->setProperty("epubSourceScrollTarget",QVariant::fromValue(Target));
            previewCandidateScrollSerial_=++sourceScrollSerial_;Target["serial"]=previewCandidateScrollSerial_;
            if(Candidate->RequestSourceScroll(Target)){previewCandidateScrollClock_.start();previewCandidateScrollTimer_.start();return;}
            previewCandidateScrollSerial_=0;
        }
        else if(State["previewScroll"].toObject()["applied"].toInteger()<previewCandidateScrollSerial_) {
            // Never expose a frame whose editor position has not been restored.
            if(previewCandidateScrollClock_.elapsed()<5000)return;
            previewCandidate_=nullptr;previewError_="epub-preview-scroll-timeout";
            reportPreview(tr("미리보기 위치를 복원하지 못해 이전 화면을 유지합니다. 갱신을 다시 실행해 주세요."));
            retirePreview(Candidate);emit StateChanged();return;
        }
        else if(previewCandidateScrollRetries_<CandidateScrollRetries && Candidate->property("epubSourceScrollTarget").value<QJsonObject>()!=sourceScrollTarget(false)){
            // Input can move while the renderer acknowledges its first scroll.
            // Keep the old preview visible until the replacement reaches the latest anchor.
            // Held keys move it on every acknowledgement; after a few rounds the swap
            // proceeds and syncPreviewScroll() below sends the anchor of that moment.
            ++previewCandidateScrollRetries_;previewCandidateScrollSerial_=-1;admitPreview(Candidate);return;
        }
    }
    previewCandidateScrollTimer_.stop();
    previewChanges_=previewCandidateChanges_;previewEdition_=previewCandidateEdition_;previewReadyMs_=previewBuildClock_.isValid()?previewBuildClock_.elapsed():0;
    previewUpdatedAt_=QDateTime::currentMSecsSinceEpoch();
    auto* Old=previewView_;previewView_=Candidate;previewCandidate_=nullptr;previewActiveToken_=previewCandidateToken_;previewMatchesSource_=previewActiveToken_==Token();sourceSelectionSerial_=State["sourceSelection"].toObject()["serial"].toInteger(-1);
    // The new preview shows the current text: earlier line shifts no longer apply and the first report is new.
    clearLineShifts();previewScrollSeen_=-1;lastScrollLine_=-1;lastScrollChapter_=-1;scrollSyncPending_=true;contextRequestSerial_=State["contextRequest"].toObject()["serial"].toInteger(-1);if((sourceSyncPending_ || SourceInput()->hasFocus()) && action("epubPreviewScrollSync")->isChecked()){sourceSyncPending_=true;sourceSyncAttempts_=0;sourceSyncTimer_.start();}previewRecoveryPosition_=false;previewChapter_=State["chapterIndex"].toInt(-1);previewNavigationSerial_=-1;previewStack_->setCurrentWidget(Candidate);Candidate->show();
    if(Old)retirePreview(Old);
    syncPreviewScroll();
    const auto LocationError=State["readerAction"].toObject()["error"].toString();
    reportPreview(LocationError.isEmpty()?tr("저장 전 미리보기 · 공통 읽기 설정 · 표시된 내용은 아직 파일에 저장되지 않았습니다."):tr("미리보기를 갱신했습니다. 이전 읽기 위치를 복원하지 못해 장 위치를 확인해 주세요."));emit PreviewCommitted();emit StateChanged();
}
void EpubSourceEditor::syncPreviewSource(bool Force) {
    if(!previewView_ || !ready_ || previewClosing_)return;const auto State=previewView_->ViewState();
    if(State["status"]!="ready" || State["readerBusy"].toBool() || !State["readingStateLoaded"].toBool())return;
    if(previewNavigationSerial_>=0){if(State["readerAction"].toObject()["serial"].toInteger()<previewNavigationSerial_)return;previewNavigationSerial_=-1;previewChapter_=State["chapterIndex"].toInt(-1);if(previewActiveToken_==Token())recoveryLocator_=State["locator"].toString();return;}
    const auto Spine=model_.Publication()["spine"].toArray();const int Chapter=State["chapterIndex"].toInt(-1);if(Chapter<0 || Chapter>=Spine.size())return;
    if(previewActiveToken_==Token())recoveryLocator_=State["locator"].toString();
    if(!action("epubPreviewScrollSync")->isChecked())return;
    if(!Force && !SourceInput()->hasFocus()) {
        // Dialogs and toolbar clicks do not authorize an old preview selection
        // to move the source. Embedded WebEngine input has a different Win32 owner.
        GUITHREADINFO Input{sizeof(GUITHREADINFO)};DWORD FocusPid=0;
        if(!GetGUIThreadInfo(0,&Input) || !Input.hwndFocus)return;
        GetWindowThreadProcessId(Input.hwndFocus,&FocusPid);
        if(FocusPid!=State["processId"].toInteger())return;
        if(selectPreviewSource())return;
    }
    if(Force){int Index=-1;for(int I=0;I<Spine.size();++I)if(Spine[I].toObject()["path"]==current_)Index=I;
        if(Index>=0 && Index!=Chapter && previewView_->ReaderAction("chapter",Index))previewNavigationSerial_=State["readerAction"].toObject()["serial"].toInteger()+1;return;}
    if(Chapter==previewChapter_)return;previewChapter_=Chapter;
    if(SourceInput()->hasFocus() || previewActiveToken_!=Token())return;
    const auto Path=Spine[Chapter].toObject()["path"].toString();if(!model_.IsEditable(Path) || Path==current_)return;
    previewSyncing_=true;openResource(Path);previewSyncing_=false;
}
QJsonObject EpubSourceEditor::previewSnapshot()const {
    return {{"configured",previewRuntime_.has_value()},{"epoch",qint64(previewEpoch_)},{"busy",previewPatchPending_||previewReloadPending_||previewWorker_.isRunning()||previewCandidate_!=nullptr},{"performance",QJsonObject{{"patchRuns",previewPatchRuns_},{"editPatches",previewPatchEdits_},{"resyncs",previewResyncs_},{"reloads",previewReloadRuns_},{"exportMs",previewExportMs_},{"readyMs",previewReadyMs_}}},{"invalid",!previewPatchInvalid_.isEmpty() && previewPatchInvalid_==Token()},{"waitingForFocus",previewPending_ && !previewOwnerActive()},{"candidate",previewCandidate_?previewCandidate_->ViewState():QJsonObject{}},{"error",previewError_},{"current",previewView_ && previewActiveToken_==Token()},{"ready",previewView_ && previewView_->IsLoaded()},{"token",QString::fromLatin1(previewActiveToken_.toHex())},{"state",previewView_?previewView_->ViewState():QJsonObject{}},{"retiring",previewRetiring_.size()},{"hidden",previewPanel_->isHidden()},{"width",previewWidth_}};
}
bool EpubSourceEditor::RequestPreviewClose() {
    previewClosing_=true;previewPatchPending_=false;previewReloadPending_=false;previewTimer_.stop();if(previewCancel_ && (!nativeValidating_ || previewCancel_!=validationCancel_))previewCancel_->store(true);
    if(previewView_){auto* Old=std::exchange(previewView_,nullptr);retirePreview(Old);}if(previewCandidate_){auto* Old=std::exchange(previewCandidate_,nullptr);retirePreview(Old);}
    const bool Closed=IsPreviewClosed();
    if(Closed && previewDirectory_)previewDirectory_->remove();return Closed;
}
bool EpubSourceEditor::IsPreviewClosed()const{return previewClosing_ && !previewWorker_.isRunning() && !previewView_ && !previewCandidate_ && std::all_of(previewRetiring_.begin(),previewRetiring_.end(),[](const auto& View){return !View || View->ViewState()["retired"].toBool();});}
void EpubSourceEditor::SetSourceVisible(bool Visible) {
    if(!Visible && sourceComposing_)return;sourcePanel_->setVisible(Visible);setPanelVisible("tree",action("epubToggleTree")->isChecked());
    setFocusProxy(Visible?SourceInput():static_cast<QWidget*>(previewView_));
    if(Visible){focusSource();if(action("epubAutoPreview")->isChecked() || previewMissing())previewTimer_.start();}else if(previewView_)previewView_->FocusInput();updateActions();emit StateChanged();
}
bool EpubSourceEditor::IsSourceVisible()const{return sourcePanel_ && !sourcePanel_->isHidden();}
}
