#include "stdafx.h"
#include "epubSourceEditor.hpp"
#include "epubProcessView.hpp"
#include "text/QTextView.h"
#include "text/ScintillaQtDirectBackend.h"
#include "commonLibs/document/epub/epubSourceMap.hpp"
#include <QJsonArray>
#include <QStackedWidget>
#include <Scintilla.h>

namespace hc::document {
namespace {
ScintillaQtDirectBackend* direct(QTextView* View){return static_cast<ScintillaQtDirectBackend*>(View->Backend());}
QString languageOf(const QString& Media){return Media=="application/xhtml+xml"?QStringLiteral("HTML"):Media=="text/css"?QStringLiteral("CSS"):Media.endsWith("+xml") || Media.endsWith("/xml")?QStringLiteral("XML"):QStringLiteral("None");}
}
// Drafts, the source map and diagnostics count UTF-16 code units; Scintilla counts UTF-8 bytes.
int EpubSourceEditor::toUtf16(qintptr Position)const{return int(direct(text_)->Send(SCI_COUNTCODEUNITS,0,qBound<qintptr>(0,Position,direct(text_)->Send(SCI_GETLENGTH))));}
qintptr EpubSourceEditor::fromUtf16(int Offset)const{return direct(text_)->Send(SCI_POSITIONRELATIVECODEUNITS,0,qMax(0,Offset));}
QByteArray EpubSourceEditor::sourceBytes()const {
    const auto Length=direct(text_)->Send(SCI_GETLENGTH);QByteArray Bytes(Length+1,Qt::Uninitialized);direct(text_)->Send(SCI_GETTEXT,quintptr(Length+1),reinterpret_cast<qintptr>(Bytes.data()));Bytes.resize(Length);return Bytes;
}
QString EpubSourceEditor::sourceText()const{return QString::fromUtf8(sourceBytes());}
bool EpubSourceEditor::sourceModified()const{return direct(text_)->Send(SCI_GETMODIFY)!=0;}
bool EpubSourceEditor::sourceShown()const{return sourceStack_ && sourceStack_->currentWidget()==text_ && !bufferResource_.isEmpty() && bufferResource_==current_;}
int EpubSourceEditor::sourcePosition()const{return toUtf16(direct(text_)->Send(SCI_GETCURRENTPOS));}
int EpubSourceEditor::sourceAnchor()const{return toUtf16(direct(text_)->Send(SCI_GETANCHOR));}
void EpubSourceEditor::setSourceSelection(int Anchor,int Position,bool Reveal) {
    direct(text_)->Send(SCI_SETSEL,quintptr(fromUtf16(Anchor)),fromUtf16(Position));if(Reveal)direct(text_)->Send(SCI_SCROLLCARET);
}
int EpubSourceEditor::firstVisibleLine()const{return int(direct(text_)->Send(SCI_GETFIRSTVISIBLELINE));}
int EpubSourceEditor::horizontalOffset()const{return int(direct(text_)->Send(SCI_GETXOFFSET));}
void EpubSourceEditor::setScroll(int Line,int Offset){direct(text_)->Send(SCI_SETFIRSTVISIBLELINE,quintptr(qMax(0,Line)));direct(text_)->Send(SCI_SETXOFFSET,quintptr(qMax(0,Offset)));}
void EpubSourceEditor::setSourceReadOnly(bool ReadOnly){sourceReadOnly_=ReadOnly;text_->setReadOnly(ReadOnly);updateActions();}
void EpubSourceEditor::focusSource(){if(sourceShown())SourceInput()->setFocus(Qt::OtherFocusReason);}
void EpubSourceEditor::insertSource(const QString& Text){const auto Bytes=Text.toUtf8();direct(text_)->Send(SCI_REPLACESEL,0,reinterpret_cast<qintptr>(Bytes.constData()));}
void EpubSourceEditor::saveSourceState() {
    if(bufferResource_.isEmpty() || !sourceBuffers_.contains(bufferResource_) || sourceStack_->currentWidget()!=text_)return;
    auto& State=sourceBuffers_[bufferResource_];State.position_=toUtf16(direct(text_)->Send(SCI_GETCURRENTPOS));State.anchor_=toUtf16(direct(text_)->Send(SCI_GETANCHOR));State.line_=firstVisibleLine();State.offset_=horizontalOffset();
}
void EpubSourceEditor::releaseSource(const QString& Path) {
    if(!sourceBuffers_.contains(Path))return;const auto State=sourceBuffers_.take(Path);
    if(Path==bufferResource_)bufferResource_.clear();
    // The view keeps its own reference to the shown document.
    if(State.document_)direct(text_)->Send(SCI_RELEASEDOCUMENT,0,State.document_);
}
void EpubSourceEditor::activateSource(const QByteArray& Data,const QString& Media) {
    auto& State=sourceBuffers_[current_];bool Fresh=false;
    if(!State.document_){State.document_=direct(text_)->Send(SCI_CREATEDOCUMENT,0,SC_DOCUMENTOPTION_DEFAULT);State.serial_=++bufferSerial_;Fresh=true;}
    text_->AttachDocument(State.document_,languageOf(Media));bufferResource_=current_;
    // Scintilla keeps read-only per document and ignores replacements while it is set.
    if(Fresh || State.applied_!=Data) {text_->setReadOnly(false);
        if(!Fresh && sourceBytes()==Data){State.applied_=Data;text_->CommitContent();}
        else {
            direct(text_)->SetTextUtf8(Data);State.applied_=Data;
            direct(text_)->Send(SCI_SETEOLMODE,Data.contains("\r\n")?SC_EOL_CRLF:Data.contains('\r')?SC_EOL_CR:SC_EOL_LF);
            if(!Fresh)State.serial_=++bufferSerial_;
        }
    }
    text_->setReadOnly(sourceReadOnly_);setSourceSelection(State.anchor_,State.position_,false);setScroll(State.line_,State.offset_);
    const auto Resource=current_;const int Line=State.line_,Offset=State.offset_;
    QTimer::singleShot(0,this,[this,Resource,Line,Offset]{if(bufferResource_==Resource && sourceStack_->currentWidget()==text_)setScroll(Line,Offset);});
    for(const auto& Path:sourceBuffers_.keys())if(Path!=current_ && !model_.IsEditable(Path))releaseSource(Path);
}
void EpubSourceEditor::locateSource() {
    if(!sourceSyncPending_ || sourceComposing_ || !previewView_ || !ready_ || previewClosing_ || !sourceShown())return;
    if(previewActiveToken_!=Token() || ++sourceSyncAttempts_>30){sourceSyncPending_=false;return;}
    const auto State=previewView_->ViewState();if(State["readerBusy"].toBool()){sourceSyncTimer_.start();return;}
    int Chapter=-1;const auto Spine=model_.Publication()["spine"].toArray();for(int I=0;I<Spine.size();++I)if(Spine[I].toObject()["path"]==current_)Chapter=I;if(Chapter<0){sourceSyncPending_=false;return;}
    if(State["chapterIndex"].toInt(-1)!=Chapter){previewView_->ReaderAction("chapter",Chapter);sourceSyncTimer_.start();return;}
    epub::SourceMap Map;sourceSyncPending_=false;if(!Map.Load(sourceText()))return;const auto Anchor=Map.Anchor(sourcePosition());if(!Anchor.isEmpty())previewView_->ReaderAction("source-position",Anchor);
}
bool EpubSourceEditor::selectPreviewSource(const QJsonObject& Requested) {
    if(sourceComposing_ || !ready_ || !previewView_ || previewActiveToken_!=Token())return false;
    auto Anchor=Requested;
    if(Anchor.isEmpty()){const auto State=previewView_->ViewState();Anchor=State["sourceSelection"].toObject();const auto Serial=Anchor["serial"].toInteger();if(Serial<=sourceSelectionSerial_)return false;sourceSelectionSerial_=Serial;}
    const auto Spine=model_.Publication()["spine"].toArray();const int Chapter=Anchor["chapter"].toInt(-1);if(Chapter<0 || Chapter>=Spine.size())return false;const auto Path=Spine[Chapter].toObject()["path"].toString();
    if(current_!=Path){previewSyncing_=true;const bool Opened=openResource(Path);previewSyncing_=false;if(!Opened)return false;}if(!sourceShown())return false;
    epub::SourceMap Map;if(!Map.Load(sourceText()))return false;const int Position=Map.Position(Anchor);if(Position<0)return false;
    updating_=true;setSourceSelection(Position,Position);updating_=false;return true;
}
}
