#include "stdafx.h"
#include "epubSourceEditor.hpp"
#include <QApplication>
#include <Windows.h>
#include "epubProcessView.hpp"
#include "core/AppSettings.h"
#include "text/QTextView.h"
#include <QAction>
#include <QDir>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QLabel>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStackedLayout>
#include <QTemporaryDir>
#include <QToolButton>
#include <QVBoxLayout>
#include <QtConcurrentRun>
#include <algorithm>
namespace hc::document {
void EpubSourceEditor::ConfigurePreview(const WebProcessRuntime& Runtime,int Chapter) {
    if(previewRuntime_)return;previewRuntime_=Runtime;previewPreferredChapter_=previewRestoreChapter_=Chapter;
    auto* Panel=previewPanel_;auto* Layout=new QVBoxLayout(Panel);Layout->setContentsMargins(0,0,0,0);Layout->setSpacing(0);
    auto* Header=new QWidget(Panel);Header->setObjectName("epubPreviewHeader");auto* Controls=new QHBoxLayout(Header);Controls->setContentsMargins(8,2,4,2);Controls->setSpacing(4);
    // The header's minimum width bounds the editor window's; in a narrow window the title gives way before the toggles.
    auto* Title=new QLabel(tr("미리보기"),Header);auto Font=Title->font();Font.setWeight(QFont::DemiBold);Title->setFont(Font);Title->setMinimumWidth(1);Controls->addWidget(Title);
    previewChip_=new QLabel(Header);previewChip_->setObjectName("epubPreviewState");previewChip_->setTextFormat(Qt::PlainText);previewChip_->setAccessibleName(tr("EPUB 미리보기 상태"));previewChip_->setMinimumWidth(24);Controls->addWidget(previewChip_);Controls->addStretch();
    for(const auto& Name:{"epubAutoPreview","epubPreviewSync","epubPreviewScrollSync","epubPreviewRefresh"}){auto* Button=new QToolButton(Header);Button->setDefaultAction(action(Name));Button->setAutoRaise(true);Button->setToolButtonStyle(Qt::ToolButtonTextOnly);Controls->addWidget(Button);}
    previewChapterLabel_=new QLabel(Header);previewChapterLabel_->setObjectName("epubPreviewChapter");previewChapterLabel_->setForegroundRole(QPalette::PlaceholderText);previewChapterLabel_->setMinimumWidth(1);Controls->addWidget(previewChapterLabel_);Layout->addWidget(Header);
    auto* Frame=new QWidget(Panel);Frame->setMinimumSize(200,140);Frame->setObjectName("epubPreviewFrame");previewStack_=new QStackedLayout(Frame);previewStack_->setContentsMargins(0,0,0,0);previewStack_->setStackingMode(QStackedLayout::StackAll);Layout->addWidget(Frame,1);
    reportPreview(tr("저장 전 미리보기 · 공통 읽기 설정 · 사용자 읽기 설정과 기록은 유지됩니다."));
    {AppSettings Settings;setPanelVisible("preview",Settings.value("epubEditor/layout/preview",true).toBool());}
    previewTimer_.setSingleShot(true);previewTimer_.setInterval(650);
    // Foreign Qt windows can receive activation while their HWND is reparented,
    // even with WS_EX_NOACTIVATE. Repair only a focus loss caused by our passive
    // candidate; an explicit control choice or another application always wins.
    connect(qApp,&QApplication::focusChanged,this,[this](QWidget* Before,QWidget* After){
        if(!previewOwnerActive()) {
            DWORD ForegroundPid=0;GetWindowThreadProcessId(GetForegroundWindow(),&ForegroundPid);
            if(!previewCandidate_ || ForegroundPid!=previewCandidate_->ViewState()["processId"].toInteger()){suspendInactivePreview();return;}
        }
        if(After || !Before || !previewCandidate_ || !(sourcePanel_->isAncestorOf(Before) || treePanel_->isAncestorOf(Before)))return;
        const QPointer<QWidget> Input=Before;const QPointer<EpubProcessView> Candidate=previewCandidate_;
        QTimer::singleShot(40,this,[this,Input,Candidate]{
            if(!Input || !Candidate || Candidate!=previewCandidate_ || previewClosing_ || !IsSourceVisible()
                || !Input->isVisible() || !Input->isEnabled() || QApplication::focusWidget()
                || QApplication::activePopupWidget() || QApplication::activeModalWidget())return;
            const auto Foreground=GetForegroundWindow();const auto Root=GetAncestor(reinterpret_cast<HWND>(window()->winId()),GA_ROOT);
            DWORD Pid=0;GetWindowThreadProcessId(Foreground,&Pid);
            if(!Root || GetAncestor(Foreground,GA_ROOT)!=Root || Pid!=Candidate->ViewState()["processId"].toInteger())return;
            SetActiveWindow(Root);QApplication::setActiveWindow(window());
            if(Input){SetFocus(reinterpret_cast<HWND>(Input->effectiveWinId()));Input->setFocus(Qt::OtherFocusReason);}
        });
    });
    connect(qApp,&QGuiApplication::applicationStateChanged,this,[this]{suspendInactivePreview();});
    connect(&previewTimer_,&QTimer::timeout,this,[this]{requestPreview();});
    connect(this,&EpubSourceEditor::Changed,this,[this]{
        ++previewEpoch_;previewPending_=action("epubAutoPreview")->isChecked();if(previewCancel_ && (!nativeValidating_ || validationToken_!=Token() || previewCancel_!=validationCancel_))previewCancel_->store(true);
        if(previewCandidate_){auto* Old=std::exchange(previewCandidate_,nullptr);retirePreview(Old);}
        if(action("epubAutoPreview")->isChecked() && !previewClosing_ && !previewPanel_->isHidden())previewTimer_.start();updateStatus();
    });
    connect(&worker_,&QFutureWatcherBase::finished,this,[this]{if((action("epubAutoPreview")->isChecked() || previewMissing()) && !previewClosing_)previewTimer_.start();});
    connect(&previewWorker_,&QFutureWatcherBase::finished,this,[this]{
        const auto Result=previewWorker_.result();if(previewClosing_){RequestPreviewClose();emit StateChanged();return;}
        if(previewWorkEpoch_!=previewEpoch_ || previewWorkToken_!=Token()){if(action("epubAutoPreview")->isChecked() || previewPending_ || previewMissing())previewTimer_.start();emit StateChanged();return;}
        if(!Result){previewError_=Result.error_+": "+Result.location_;reportPreview(tr("소스 검사 실패 · 마지막 정상 미리보기를 유지합니다. %1").arg(previewError_));}
        else {cachedExport_=Result;cachedExportToken_=previewWorkToken_;previewError_.clear();showPreview(Result.bytes_);}emit StateChanged();
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
void EpubSourceEditor::requestPreview(bool Force) {
    if(sourceComposing_){previewPending_=true;previewTimer_.start();return;}
    previewTimer_.stop();if(!previewRuntime_ || previewSettings_.isEmpty() || !ready_ || previewClosing_)return;
    if(!previewOwnerActive()){suspendInactivePreview();previewPending_=true;previewTimer_.start();return;}
    if(previewPreferredChapter_>=0){if(!(sourceShown() && sourceModified()) && recoveredFrom_.isEmpty()){const auto Spine=model_.Publication()["spine"].toArray();if(previewPreferredChapter_<Spine.size()){const auto Path=Spine[previewPreferredChapter_].toObject()["path"].toString();if(model_.IsEditable(Path) && Path!=current_){current_=Path;select();}}}previewPreferredChapter_=-1;}
    if(previewWorker_.isRunning()){previewPending_=true;return;}
    if(!Force && previewView_ && previewActiveToken_==Token()){syncPreviewSource(true);return;}
    if(previewCandidate_ && !Force && previewCandidateToken_==Token())return;
    if(previewCandidate_){auto* Old=std::exchange(previewCandidate_,nullptr);retirePreview(Old);}
    previewPending_=false;previewWorkToken_=Token();previewWorkEpoch_=previewEpoch_;previewError_.clear();reportPreview(tr("편집 내용을 검사하고 있습니다… 이전 미리보기는 유지됩니다."));
    if(cachedExportToken_==previewWorkToken_ && cachedExport_){showPreview(cachedExport_.bytes_);emit StateChanged();return;}
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
    previewCandidate_=Candidate;previewCandidateToken_=previewWorkToken_;previewCandidateEpoch_=previewWorkEpoch_;previewCandidateSerial_=-2;previewCandidateInputRequested_=false;previewCandidateScrollSerial_=-1;previewStack_->addWidget(Candidate);
    if(previewView_)previewStack_->setCurrentWidget(previewView_);else previewStack_->setCurrentWidget(Candidate);
    Candidate->show();reportPreview(tr("새 본문·내장 글꼴·이미지를 준비하고 있습니다…"));
    connect(Candidate,&EpubProcessView::EscapeRequested,this,&EpubSourceEditor::EscapeRequested);connect(Candidate,&EpubProcessView::FullscreenRequested,this,&EpubSourceEditor::FullscreenRequested);
    connect(Candidate,&EpubProcessView::ViewStateChanged,this,[this,Candidate]{
        if(Candidate==previewCandidate_)admitPreview(Candidate);
        if(Candidate==previewView_){syncPreviewSource();previewContextRequest(Candidate);previewScrolled(Candidate->ViewState()["previewScroll"].toObject());if(scrollSyncPending_)syncPreviewScroll();}
        emit StateChanged();});
    connect(Candidate,&EpubProcessView::Closed,this,[this,Candidate]{
        // A shown or pending preview that ends on its own would leave the panel empty; ask for a new one.
        const bool Lost=!previewClosing_ && (previewView_==Candidate || previewCandidate_==Candidate);
        if(previewView_==Candidate)previewView_=nullptr;if(previewCandidate_==Candidate)previewCandidate_=nullptr;
        previewRetiring_.removeIf([Candidate](const auto& View){return !View || View==Candidate;});previewStack_->removeWidget(Candidate);Candidate->deleteLater();if(previewClosing_)RequestPreviewClose();
        if(Lost && previewError_.isEmpty() && !previewPanel_->isHidden()){previewPending_=true;previewTimer_.start();}
        emit StateChanged();
    });
    connect(Candidate,&EpubProcessView::Failed,this,[this,Candidate](const QString& Error){
        if(Candidate==previewCandidate_ || Candidate==previewView_){if(Candidate==previewCandidate_)previewCandidate_=nullptr;else previewView_=nullptr;previewError_=Error;reportPreview(tr("미리보기를 표시하지 못했습니다: %1 · 소스와 남아 있는 정상 화면을 유지합니다.").arg(Error));retirePreview(Candidate);}emit StateChanged();
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
        int Target=-1;const auto Spine=model_.Publication()["spine"].toArray();if(action("epubPreviewSync")->isChecked())for(int I=0;I<Spine.size();++I)if(Spine[I].toObject()["path"]==current_)Target=I;
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
            auto Target=sourceScrollTarget(false);previewCandidateScrollSerial_=++sourceScrollSerial_;Target["serial"]=previewCandidateScrollSerial_;
            if(Candidate->RequestSourceScroll(Target)){previewCandidateScrollClock_.start();QTimer::singleShot(300,this,[this,Guard=QPointer<EpubProcessView>(Candidate)]{if(Guard)admitPreview(Guard);});return;}
            previewCandidateScrollSerial_=0;
        }
        else if(State["previewScroll"].toObject()["applied"].toInteger()<previewCandidateScrollSerial_ && previewCandidateScrollClock_.elapsed()<300)return;
    }
    auto* Old=previewView_;previewView_=Candidate;previewCandidate_=nullptr;previewActiveToken_=previewCandidateToken_;sourceSelectionSerial_=-1;
    // The new preview shows the current text: earlier line shifts no longer apply and the first report is new.
    clearLineShifts();previewScrollSeen_=-1;lastScrollLine_=-1;lastScrollChapter_=-1;scrollSyncPending_=true;contextRequestSerial_=State["contextRequest"].toObject()["serial"].toInteger(-1);if((sourceSyncPending_ || SourceInput()->hasFocus()) && action("epubPreviewSync")->isChecked()){sourceSyncPending_=true;sourceSyncAttempts_=0;sourceSyncTimer_.start();}previewRecoveryPosition_=false;previewChapter_=State["chapterIndex"].toInt(-1);previewNavigationSerial_=-1;previewStack_->setCurrentWidget(Candidate);Candidate->show();
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
    if(!action("epubPreviewSync")->isChecked())return;
    if(!Force && !SourceInput()->hasFocus() && selectPreviewSource())return;
    if(Force){int Index=-1;for(int I=0;I<Spine.size();++I)if(Spine[I].toObject()["path"]==current_)Index=I;
        if(Index>=0 && Index!=Chapter && previewView_->ReaderAction("chapter",Index))previewNavigationSerial_=State["readerAction"].toObject()["serial"].toInteger()+1;return;}
    if(Chapter==previewChapter_)return;previewChapter_=Chapter;
    if(SourceInput()->hasFocus() || previewActiveToken_!=Token())return;
    const auto Path=Spine[Chapter].toObject()["path"].toString();if(!model_.IsEditable(Path) || Path==current_)return;
    previewSyncing_=true;openResource(Path);previewSyncing_=false;
}
QJsonObject EpubSourceEditor::previewSnapshot()const {
    return {{"configured",previewRuntime_.has_value()},{"busy",previewWorker_.isRunning()||previewCandidate_!=nullptr},{"waitingForFocus",previewPending_ && !previewOwnerActive()},{"candidate",previewCandidate_?previewCandidate_->ViewState():QJsonObject{}},{"error",previewError_},{"current",previewView_ && previewActiveToken_==Token()},{"ready",previewView_ && previewView_->IsLoaded()},{"token",QString::fromLatin1(previewActiveToken_.toHex())},{"state",previewView_?previewView_->ViewState():QJsonObject{}},{"retiring",previewRetiring_.size()},{"hidden",previewPanel_->isHidden()},{"width",previewWidth_}};
}
bool EpubSourceEditor::RequestPreviewClose() {
    previewClosing_=true;previewTimer_.stop();if(previewCancel_ && (!nativeValidating_ || previewCancel_!=validationCancel_))previewCancel_->store(true);
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
