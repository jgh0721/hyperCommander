#include "stdafx.h"
#include "epubSourceEditor.hpp"
#include "epubProcessView.hpp"
#include "text/QTextView.h"
#include "text/ScintillaQtDirectBackend.h"
#include <QAction>
#include <QDateTime>
#include <QJsonArray>
#include <Scintilla.h>
#include <QAbstractScrollArea>
#include <QTimer>
#include <cmath>

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
    // One deadline per replacement: re-requests restart it instead of stacking timers.
    previewCandidateScrollTimer_.setSingleShot(true);previewCandidateScrollTimer_.setInterval(5000);
    connect(&previewCandidateScrollTimer_,&QTimer::timeout,this,[this]{if(previewCandidate_)admitPreview(previewCandidate_);});
    connect(text_->Backend(),&ITextEditorBackend::viewportScrolled,this,&EpubSourceEditor::syncPreviewScroll);
    connect(text_->Backend(),&ITextEditorBackend::cursorPositionChanged,this,[this]{
        if(updating_ || !SourceInput()->hasFocus())return;
        auto* Backend=direct(text_);const auto Position=Backend->Send(SCI_GETCURRENTPOS);
        const auto Y=Backend->Send(SCI_POINTYFROMPOSITION,0,Position);
        // A selection API may change an offscreen caret without scrolling the source.
        // Only a visible editing point asks the preview to follow that change.
        if(Y>=0 && Y<Backend->scrollArea()->viewport()->height())
            QTimer::singleShot(0,this,&EpubSourceEditor::syncPreviewScroll);
    });
    // Edits after the preview was built move its lines. Buffer replacements made while updating are not edits.
    connect(text_->Backend(),&ITextEditorBackend::linesShifted,this,[this](int Line,int Delta){
        if(updating_ || bufferResource_.isEmpty() || !sourceBuffers_.contains(bufferResource_))return;
        auto& State=sourceBuffers_[bufferResource_];if(State.shifts_.size()<256)State.shifts_.append({Line,Delta});else State.shiftsOverflow_=true;
    });
    connect(this,&EpubSourceEditor::Changed,this,[this]{
        previewMatchesSource_=previewView_ && previewActiveToken_==Token();
        if(previewMatchesSource_)clearLineShifts();
    });
}
double EpubSourceEditor::previewViewOffset(double SourceY)const{
    const auto* Viewport=direct(text_)->scrollArea()->viewport();const auto* Frame=previewStack_->parentWidget();
    return Viewport->mapToGlobal(QPoint(0,0)).y()+SourceY-Frame->mapToGlobal(QPoint(0,0)).y();
}
double EpubSourceEditor::sourceYForViewOffset(double Offset)const{
    const auto* Viewport=direct(text_)->scrollArea()->viewport();const auto* Frame=previewStack_->parentWidget();
    return Frame->mapToGlobal(QPoint(0,0)).y()+Offset-Viewport->mapToGlobal(QPoint(0,0)).y();
}
void EpubSourceEditor::placeSourceRow(qintptr Position,double Y){
    // The editor scrolls whole display rows: the row holding the character goes to the row nearest Y.
    // Rows above may wrap once shown, so the placement is corrected once more after that.
    auto* Backend=direct(text_);
    const auto Place=[Backend,Position,Y]{
        const auto Row=qMax<qintptr>(1,Backend->Send(SCI_TEXTHEIGHT,Backend->Send(SCI_LINEFROMPOSITION,quintptr(Position))));
        const auto Rows=qintptr(std::lround((double(Backend->Send(SCI_POINTYFROMPOSITION,0,Position))-Y)/double(Row)));
        if(Rows!=0)Backend->Send(SCI_SETFIRSTVISIBLELINE,quintptr(qMax<qintptr>(0,Backend->Send(SCI_GETFIRSTVISIBLELINE)+Rows)));
    };
    const int Line=int(Backend->Send(SCI_LINEFROMPOSITION,quintptr(Position)));const auto Row=qMax<qintptr>(1,Backend->Send(SCI_TEXTHEIGHT,quintptr(Line)));
    const auto Sub=qMax<qintptr>(0,(Backend->Send(SCI_POINTYFROMPOSITION,0,Position)-Backend->Send(SCI_POINTYFROMPOSITION,0,Backend->Send(SCI_POSITIONFROMLINE,quintptr(Line))))/Row);
    Backend->Send(SCI_SETFIRSTVISIBLELINE,quintptr(qMax<qintptr>(0,Backend->Send(SCI_VISIBLEFROMDOCLINE,quintptr(Line))+Sub-qintptr(std::lround(Y/double(Row))))));
    Place();
    QTimer::singleShot(0,this,[this,Place]{suppressSyncUntil_=nowMs()+SyncGuardMs;Place();});
}
bool EpubSourceEditor::scrollSyncActive()const{return action("epubPreviewScrollSync")->isChecked();}
int EpubSourceEditor::spineIndex(const QString& Path)const{const auto Spine=model_.Publication()["spine"].toArray();for(int I=0;I<Spine.size();++I)if(Spine[I].toObject()["path"]==Path)return I;return -1;}
void EpubSourceEditor::clearLineShifts(){for(auto& State:sourceBuffers_){State.shifts_.clear();State.shiftsOverflow_=false;}}
double EpubSourceEditor::toPreviewLine(double Line)const{return qMax(1.0,ToRevisionLine(Line,sourceBuffers_.value(current_).shifts_));}
double EpubSourceEditor::fromPreviewLine(double Line)const{return qMax(1.0,FromRevisionLine(Line,sourceBuffers_.value(current_).shifts_));}
QJsonObject EpubSourceEditor::sourceScrollTarget(bool Revision)const {
    auto* Backend=direct(text_);auto Position=Backend->Send(SCI_GETCURRENTPOS);
    int CaretLine=int(Backend->Send(SCI_LINEFROMPOSITION,Position));
    const int Height=qMax(1,Backend->scrollArea()->viewport()->height());
    // POINTY is the top of the display row holding the position.
    double Y=Backend->Send(SCI_POINTYFROMPOSITION,0,Position);
    const bool Visible=Y>=0 && Y<Height;
    if(!Visible){
        // Keep the nearest visible row when the caret leaves the viewport. Switching
        // to its centre here would jump across several tall rendered paragraphs.
        if(Backend->firstVisibleLine()==0)return {{"chapter",spineIndex(current_)},{"line",1},{"ratio",AnchorRatio},{"end",false}};
        if(Backend->isScrolledToEnd())return {{"chapter",spineIndex(current_)},{"line",Backend->lineCount()+1},{"ratio",AnchorRatio},{"end",true}};
        Position=Backend->Send(SCI_POSITIONFROMPOINT,Backend->leftMarginWidth()+1,qBound(0,int(Y),Height-1));
        CaretLine=int(Backend->Send(SCI_LINEFROMPOSITION,Position));
        Y=Backend->Send(SCI_POINTYFROMPOSITION,0,Position);
    }
    // Horizontal progress is not a vertical scroll anchor: at the next display
    // row the viewport ratio also changes, pulling the preview back down.
    // Follow the start of the display row, including wrapped rows, instead.
    // Line fraction and height ratio both name the row's top, so the preview point for
    // that row lands at the row's own height rather than half a row below it.
    const auto Start=Backend->Send(SCI_POSITIONFROMLINE,CaretLine);
    const double Row=qMax<qintptr>(1,Backend->Send(SCI_TEXTHEIGHT,CaretLine));
    const double Rows=qMax<qintptr>(1,Backend->Send(SCI_WRAPCOUNT,CaretLine));
    const double Top=Backend->Send(SCI_POINTYFROMPOSITION,0,Start);
    const double Line=CaretLine+1+qBound(0.0,(Y-Top)/(Rows*Row),.999);
    QJsonObject Target{{"chapter",spineIndex(current_)},{"line",Revision?toPreviewLine(Line):Line},{"ratio",qBound(0.0,Y/Height,1.0)},{"end",false},
        {"viewOffset",previewViewOffset(Y)}};
    // D5: the character itself, the caret's or the first one visible at the row's left edge, by its
    // source-map position. A replacement preview shows the current text; the shown one only until an edit.
    if((!Revision || previewMatchesSource_) && sourceShown())
        if(const auto Anchor=sourceAnchor(Position);!Anchor.isEmpty())Target["anchor"]=Anchor;
    return Target;
}
QJsonObject EpubSourceEditor::sourceAnchor(qintptr Position)const{
    // The caret can sit in markup (inside <p class="…">, on </p>) or after a row's last tag. There the map
    // names an element, whose box top is not its text's line, or the white space between blocks, which
    // has no box at all, so the preview falls back to its whole parent. The nearest text character of
    // the same display row stands for the caret, the next one first; a space has no rendered counterpart.
    const auto* Map=sourceMap();if(!Map)return {};
    auto* Backend=direct(text_);
    const qintptr Line=Backend->Send(SCI_LINEFROMPOSITION,quintptr(Position));
    const qintptr Start=Backend->Send(SCI_POSITIONFROMLINE,quintptr(Line)),End=Backend->Send(SCI_GETLINEENDPOSITION,quintptr(Line));
    const qintptr Row=Backend->Send(SCI_POINTYFROMPOSITION,0,Position);
    const auto Char=[Backend](qintptr At){return char(Backend->Send(SCI_GETCHARAT,quintptr(At)));};
    const auto Blank=[](char C){return C==' '||C=='\t'||C=='\r'||C=='\n';};
    const int Origin=toUtf16(Position);const auto Own=Map->Anchor(Origin);
    // Inside a tag when the nearest angle bracket to the left opens one. UTF-8 continuation bytes are never brackets.
    bool Inside=false;
    for(qintptr At=Position-1;At>=Start && Position-At<=4096;--At){const char C=Char(At);if(C=='>')break;if(C=='<'){Inside=true;break;}}
    // Tags are skipped by their brackets; only a text candidate costs a map lookup. Bounded for long attribute values.
    bool Tag=Inside;int Utf16=Origin;
    for(qintptr At=Position,Steps=0;At<End && Steps<1024;++Steps){
        if(At!=Position && Backend->Send(SCI_POINTYFROMPOSITION,0,At)!=Row)break;
        const char C=Char(At);
        if(Tag){if(C=='>')Tag=false;}
        else if(C=='<')Tag=true;
        else if(!Blank(C))if(const auto Anchor=Map->Anchor(Utf16);Anchor["child"].toInt(-1)>=0)return Anchor;
        const qintptr Next=Backend->Send(SCI_POSITIONAFTER,quintptr(At));Utf16+=int(Backend->Send(SCI_COUNTCODEUNITS,quintptr(At),Next));At=Next;
    }
    Tag=Inside;Utf16=Origin;
    for(qintptr At=Position,Steps=0;At>Start && Steps<1024;++Steps){
        const qintptr Previous=Backend->Send(SCI_POSITIONBEFORE,quintptr(At));Utf16-=int(Backend->Send(SCI_COUNTCODEUNITS,quintptr(Previous),At));At=Previous;
        if(Backend->Send(SCI_POINTYFROMPOSITION,0,At)!=Row)break;
        const char C=Char(At);
        if(Tag){if(C=='<')Tag=false;}
        else if(C=='>')Tag=true;
        else if(!Blank(C))if(const auto Anchor=Map->Anchor(Utf16);Anchor["child"].toInt(-1)>=0)return Anchor;
    }
    // A row of markup only: an element that starts on this row still names its own box. White space between
    // blocks, or a container opened rows above, names nothing here; the line table places the row instead.
    if(Own["child"].toInt(-1)<0)if(const int Begin=Map->Position(Own);Begin>=0 && Backend->Send(SCI_LINEFROMPOSITION,quintptr(fromUtf16(Begin)))==Line)return Own;
    return {};
}
void EpubSourceEditor::syncPreviewScroll() {
    if(!scrollSyncActive() || previewPatchPending_ || updating_ || sourceComposing_ || !ready_ || !sourceShown() || !previewView_ || previewClosing_ || previewPanel_->isHidden())return;
    const int Chapter=spineIndex(current_);if(Chapter<0)return;
    // Position sync brings the preview to this chapter first; while a sync is pending its state changes call back here.
    const auto State=previewView_->ViewState();
    if(State["chapterIndex"].toInt(-1)!=Chapter || !State["previewScroll"].toObject()["supported"].toBool())return;
    const qint64 Now=nowMs();
    // The preview has just moved the editor, so answering now would bounce. Send the final position once the guard ends.
    if(Now<suppressSyncUntil_){if(!scrollRetryTimer_.isActive())scrollRetryTimer_.start(int(suppressSyncUntil_-Now)+16);return;}
    scrollRetryTimer_.stop();scrollSyncPending_=false;
    auto Target=sourceScrollTarget(true);const double Line=Target["line"].toDouble();const bool End=Target["end"].toBool();const double Ratio=Target["ratio"].toDouble();
    // Viewport signals also follow changes that keep the line, such as wrap toggles. An unchanged target sends
    // nothing and sets no guard, which would otherwise swallow the user's next preview scroll.
    // The anchor character can change within a row (horizontal moves), so the whole target is compared.
    if(Chapter==lastScrollChapter_ && Target==lastScrollTarget_)return;
    lastScrollChapter_=Chapter;lastScrollEnd_=End;lastScrollLine_=Line;lastScrollRatio_=Ratio;lastScrollTarget_=Target;Target["serial"]=++sourceScrollSerial_;
    if(previewView_->RequestSourceScroll(Target))previewDrivenIgnoreUntil_=Now+SyncGuardMs;
}
void EpubSourceEditor::previewScrolled(const QJsonObject& Report) {
    const auto Serial=Report["serial"].toInteger(0);if(Serial<=previewScrollSeen_)return;previewScrollSeen_=Serial;
    // Only the user's scroll moves the editor; the page also scrolls by itself while images and fonts settle.
    if(!scrollSyncActive() || !Report["user"].toBool() || !Report["supported"].toBool() || sourceComposing_ || !ready_ || !sourceShown())return;
    const qint64 Now=nowMs();if(Now<previewDrivenIgnoreUntil_ || Report["chapter"].toInt(-1)!=spineIndex(current_))return;
    suppressSyncUntil_=Now+SyncGuardMs;auto* Backend=direct(text_);
    // The preview's reference character, by its source-map position, keeps its screen height here.
    const auto Reference=Report["reference"].toObject();const auto* Map=previewMatchesSource_?sourceMap():nullptr;
    const int Exact=Map && Reference["anchor"].isObject()?Map->Position(Reference["anchor"].toObject()):-1;
    if(Report["top"].toBool())Backend->Send(SCI_SETFIRSTVISIBLELINE,0);
    else if(Report["bottom"].toBool())Backend->Send(SCI_SETFIRSTVISIBLELINE,quintptr(Backend->Send(SCI_VISIBLEFROMDOCLINE,quintptr(Backend->lineCount()))));
    else if(Exact>=0)placeSourceRow(fromUtf16(Exact),sourceYForViewOffset(Reference["viewOffset"].toDouble()));
    else Backend->scrollFractionalLineToViewportRatio(fromPreviewLine(Report["line"].toDouble()),Report["ratio"].toDouble(AnchorRatio));
    // The editor scrolls whole rows. Remembering where it landed keeps the retry after the guard from
    // sending that rounded position back and nudging the preview the user just placed.
    const auto Landed=sourceScrollTarget(true);lastScrollChapter_=Landed["chapter"].toInt();lastScrollLine_=Landed["line"].toDouble();lastScrollEnd_=Landed["end"].toBool();lastScrollRatio_=Landed["ratio"].toDouble();lastScrollTarget_=Landed;
}
void EpubSourceEditor::placeSourceLine(int Position,double Ratio) {
    // A preview click keeps its height in the editor. The editor's resulting scroll must not echo back.
    suppressSyncUntil_=nowMs()+SyncGuardMs;auto* Backend=direct(text_);
    Backend->scrollFractionalLineToViewportRatio(double(Backend->Send(SCI_LINEFROMPOSITION,quintptr(fromUtf16(Position))))+1,qBound(0.0,Ratio,0.9));
    const auto Landed=sourceScrollTarget(true);lastScrollChapter_=Landed["chapter"].toInt();lastScrollLine_=Landed["line"].toDouble();lastScrollEnd_=Landed["end"].toBool();lastScrollRatio_=Landed["ratio"].toDouble();lastScrollTarget_=Landed;
}
}
