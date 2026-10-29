#include "stdafx.h"
#include "epubSourceEditor.hpp"
#include "epubProcessView.hpp"
#include "text/QTextView.h"
#include "text/ScintillaQtDirectBackend.h"
#include <QAction>
#include <QDateTime>
#include <QJsonArray>
#include <Scintilla.h>

namespace hc::document {
namespace {
// After one side moves the other, the other side's echo of that move is ignored this long.
constexpr qint64 SyncGuardMs=250;
// The line at this height ratio of the editor is kept at the same ratio of the preview.
constexpr double AnchorRatio=0.5;
qint64 nowMs(){return QDateTime::currentMSecsSinceEpoch();}
ScintillaQtDirectBackend* direct(QTextView* View){return static_cast<ScintillaQtDirectBackend*>(View->Backend());}
}
void EpubSourceEditor::setupScrollSync() {
    scrollRetryTimer_.setSingleShot(true);connect(&scrollRetryTimer_,&QTimer::timeout,this,&EpubSourceEditor::syncPreviewScroll);
    connect(text_->Backend(),&ITextEditorBackend::viewportScrolled,this,&EpubSourceEditor::syncPreviewScroll);
    // Edits after the preview was built move its lines. Buffer replacements made while updating are not edits.
    connect(text_->Backend(),&ITextEditorBackend::linesShifted,this,[this](int Line,int Delta){
        if(updating_ || bufferResource_.isEmpty() || !sourceBuffers_.contains(bufferResource_))return;
        auto& State=sourceBuffers_[bufferResource_];if(State.shifts_.size()<256)State.shifts_.append({Line,Delta});else State.shiftsOverflow_=true;
    });
    connect(this,&EpubSourceEditor::Changed,this,[this]{if(previewView_ && previewActiveToken_==Token())clearLineShifts();});
}
bool EpubSourceEditor::scrollSyncActive()const{return action("epubPreviewSync")->isChecked() && action("epubPreviewScrollSync")->isChecked();}
int EpubSourceEditor::spineIndex(const QString& Path)const{const auto Spine=model_.Publication()["spine"].toArray();for(int I=0;I<Spine.size();++I)if(Spine[I].toObject()["path"]==Path)return I;return -1;}
void EpubSourceEditor::clearLineShifts(){for(auto& State:sourceBuffers_){State.shifts_.clear();State.shiftsOverflow_=false;}}
double EpubSourceEditor::toPreviewLine(double Line)const{return qMax(1.0,ToRevisionLine(Line,sourceBuffers_.value(current_).shifts_));}
double EpubSourceEditor::fromPreviewLine(double Line)const{return qMax(1.0,FromRevisionLine(Line,sourceBuffers_.value(current_).shifts_));}
QJsonObject EpubSourceEditor::sourceScrollTarget(bool Revision)const {
    auto* Backend=text_->Backend();const bool Top=Backend->firstVisibleLine()==0;
    // The top stays the top: a ratio would push the chapter's first heading out of the preview.
    const double Line=Top?1.0:Backend->fractionalLineAtViewportRatio(AnchorRatio);
    return {{"chapter",spineIndex(current_)},{"line",Top || !Revision?Line:toPreviewLine(Line)},{"ratio",AnchorRatio},{"end",!Top && Backend->isScrolledToEnd()}};
}
void EpubSourceEditor::syncPreviewScroll() {
    if(!scrollSyncActive() || updating_ || sourceComposing_ || !ready_ || !sourceShown() || !previewView_ || previewClosing_ || previewPanel_->isHidden())return;
    const int Chapter=spineIndex(current_);if(Chapter<0)return;
    // Position sync brings the preview to this chapter first; while a sync is pending its state changes call back here.
    const auto State=previewView_->ViewState();
    if(State["chapterIndex"].toInt(-1)!=Chapter || !State["previewScroll"].toObject()["supported"].toBool())return;
    const qint64 Now=nowMs();
    // The preview has just moved the editor, so answering now would bounce. Send the final position once the guard ends.
    if(Now<suppressSyncUntil_){if(!scrollRetryTimer_.isActive())scrollRetryTimer_.start(int(suppressSyncUntil_-Now)+16);return;}
    scrollRetryTimer_.stop();scrollSyncPending_=false;
    auto Target=sourceScrollTarget(true);const double Line=Target["line"].toDouble();const bool End=Target["end"].toBool();
    // Viewport signals also follow changes that keep the line, such as wrap toggles. An unchanged target sends
    // nothing and sets no guard, which would otherwise swallow the user's next preview scroll.
    if(Chapter==lastScrollChapter_ && End==lastScrollEnd_ && qFuzzyCompare(Line,lastScrollLine_))return;
    lastScrollChapter_=Chapter;lastScrollEnd_=End;lastScrollLine_=Line;Target["serial"]=++sourceScrollSerial_;
    if(previewView_->RequestSourceScroll(Target))previewDrivenIgnoreUntil_=Now+SyncGuardMs;
}
void EpubSourceEditor::previewScrolled(const QJsonObject& Report) {
    const auto Serial=Report["serial"].toInteger(0);if(Serial<=previewScrollSeen_)return;previewScrollSeen_=Serial;
    // Only the user's scroll moves the editor; the page also scrolls by itself while images and fonts settle.
    if(!scrollSyncActive() || !Report["user"].toBool() || !Report["supported"].toBool() || sourceComposing_ || !ready_ || !sourceShown())return;
    const qint64 Now=nowMs();if(Now<previewDrivenIgnoreUntil_ || Report["chapter"].toInt(-1)!=spineIndex(current_))return;
    suppressSyncUntil_=Now+SyncGuardMs;auto* Backend=direct(text_);
    if(Report["top"].toBool())Backend->Send(SCI_SETFIRSTVISIBLELINE,0);
    else if(Report["bottom"].toBool())Backend->Send(SCI_SETFIRSTVISIBLELINE,quintptr(Backend->Send(SCI_VISIBLEFROMDOCLINE,quintptr(Backend->lineCount()))));
    else Backend->scrollFractionalLineToViewportRatio(fromPreviewLine(Report["line"].toDouble()),Report["ratio"].toDouble(AnchorRatio));
    // The editor scrolls whole rows. Remembering where it landed keeps the retry after the guard from
    // sending that rounded position back and nudging the preview the user just placed.
    const auto Landed=sourceScrollTarget(true);lastScrollChapter_=Landed["chapter"].toInt();lastScrollLine_=Landed["line"].toDouble();lastScrollEnd_=Landed["end"].toBool();
}
void EpubSourceEditor::placeSourceLine(int Position,double Ratio) {
    // A preview click keeps its height in the editor. The editor's resulting scroll must not echo back.
    suppressSyncUntil_=nowMs()+SyncGuardMs;auto* Backend=direct(text_);
    Backend->scrollFractionalLineToViewportRatio(double(Backend->Send(SCI_LINEFROMPOSITION,quintptr(fromUtf16(Position))))+1,qBound(0.0,Ratio,0.9));
    const auto Landed=sourceScrollTarget(true);lastScrollChapter_=Landed["chapter"].toInt();lastScrollLine_=Landed["line"].toDouble();lastScrollEnd_=Landed["end"].toBool();
}
}
