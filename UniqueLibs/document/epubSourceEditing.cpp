#include "stdafx.h"
#include "epubSourceEditor.hpp"
#include "epubProcessView.hpp"
#include "commonLibs/document/epub/epubSourceMap.hpp"
#include <QComboBox>
#include <QFont>
#include <QJsonArray>
#include <QPlainTextEdit>
#include <QPlainTextDocumentLayout>
#include <QRegularExpression>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSyntaxHighlighter>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextCharFormat>

namespace hc::document {
namespace {
class SourceHighlight final:public QSyntaxHighlighter {
public:
    SourceHighlight(QTextDocument* Document,QWidget* Owner,bool Css):QSyntaxHighlighter(Document),owner_(Owner),css_(Css){}
protected:
    void highlightBlock(const QString& Text)override {
        if(!owner_)return;
        const auto Palette=owner_->palette();
        const auto Apply=[&](const QString& Pattern,QPalette::ColorRole Role,bool Bold=false){QTextCharFormat Format;Format.setForeground(Palette.color(Role));if(Bold)Format.setFontWeight(QFont::DemiBold);auto Matches=QRegularExpression(Pattern).globalMatch(Text);while(Matches.hasNext()){const auto Match=Matches.next();setFormat(Match.capturedStart(),Match.capturedLength(),Format);}};
        Apply(css_?QStringLiteral(R"([\w-]+(?=\s*:))"):QStringLiteral(R"(</?[\w:.-]+|/?>)"),QPalette::Link,true);
        Apply(QStringLiteral(R"("[^"\n]*"|'[^'\n]*')"),QPalette::Link);
        Apply(css_?QStringLiteral(R"(#[\da-fA-F]{3,8}\b|\b\d+(?:\.\d+)?(?:px|em|rem|%)?)"):QStringLiteral(R"(&[\w#]+;|[\w:.-]+(?=\s*=))"),QPalette::Highlight);
        const QString Begin=css_?"/*":"<!--",End=css_?"*/":"-->";int Position=previousBlockState()==1?0:Text.indexOf(Begin);setCurrentBlockState(0);
        while(Position>=0){const int Close=Text.indexOf(End,Position+(previousBlockState()==1 && Position==0?0:Begin.size()));const int Length=Close<0?Text.size()-Position:Close+End.size()-Position;setFormat(Position,Length,Palette.color(QPalette::PlaceholderText));if(Close<0){setCurrentBlockState(1);break;}Position=Text.indexOf(Begin,Close+End.size());}
    }
private:QPointer<QWidget> owner_;bool css_;
};
}
void EpubSourceEditor::saveSourceState() {
    if(bufferResource_.isEmpty() || !sourceBuffers_.contains(bufferResource_))return;
    auto& State=sourceBuffers_[bufferResource_];State.position_=text_->textCursor().position();State.anchor_=text_->textCursor().anchor();State.vertical_=text_->verticalScrollBar()->value();State.horizontal_=text_->horizontalScrollBar()->value();
}
void EpubSourceEditor::activateSource(const QByteArray& Data,const QString& Media) {
    auto& State=sourceBuffers_[current_];QTextDocument* Retired=nullptr;
    if(State.document_ && State.applied_!=Data && State.document_->toRawText().replace(QChar(0x2029),QChar('\n')).toUtf8()==Data){State.applied_=Data;State.document_->setModified(false);}
    if(!State.document_ || State.applied_!=Data) {
        auto* Previous=State.document_.data();auto* Document=new QTextDocument(this);Document->setDocumentLayout(new QPlainTextDocumentLayout(Document));Document->setDefaultFont(text_->font());Document->setPlainText(QString::fromUtf8(Data));Document->setModified(false);new SourceHighlight(Document,text_,Media=="text/css");State.document_=Document;State.applied_=Data;
        Retired=Previous;
    }
    text_->setDocument(State.document_);if(Retired)delete Retired;bufferResource_=current_;auto Cursor=text_->textCursor();const int Last=text_->document()->characterCount()-1;Cursor.setPosition(qBound(0,State.anchor_,Last));Cursor.setPosition(qBound(0,State.position_,Last),QTextCursor::KeepAnchor);text_->setTextCursor(Cursor);text_->verticalScrollBar()->setValue(State.vertical_);text_->horizontalScrollBar()->setValue(State.horizontal_);
    const auto Resource=current_;const QPointer<QTextDocument> Document=State.document_;const int Vertical=State.vertical_,Horizontal=State.horizontal_,Revision=Document->revision();QTimer::singleShot(0,this,[this,Resource,Document,Vertical,Horizontal,Revision]{if(Document && bufferResource_==Resource && text_->document()==Document && Document->revision()==Revision){text_->verticalScrollBar()->setValue(Vertical);text_->horizontalScrollBar()->setValue(Horizontal);}});
    for(auto I=sourceBuffers_.begin();I!=sourceBuffers_.end();)if(I.key()!=current_ && !model_.IsEditable(I.key())){delete I->document_.data();I=sourceBuffers_.erase(I);}else ++I;
}
void EpubSourceEditor::rehighlightSources(){for(const auto& State:std::as_const(sourceBuffers_))if(State.document_)for(auto* Highlighter:State.document_->findChildren<QSyntaxHighlighter*>())Highlighter->rehighlight();}
void EpubSourceEditor::locateSource() {
    if(!sourceSyncPending_ || sourceComposing_ || !previewView_ || !ready_ || previewClosing_)return;
    if(previewActiveToken_!=Token() || ++sourceSyncAttempts_>30){sourceSyncPending_=false;return;}
    const auto State=previewView_->ViewState();if(State["readerBusy"].toBool()){sourceSyncTimer_.start();return;}
    int Chapter=-1;const auto Spine=model_.Publication()["spine"].toArray();for(int I=0;I<Spine.size();++I)if(Spine[I].toObject()["path"]==current_)Chapter=I;if(Chapter<0){sourceSyncPending_=false;return;}
    if(State["chapterIndex"].toInt(-1)!=Chapter){previewView_->ReaderAction("chapter",Chapter);sourceSyncTimer_.start();return;}
    epub::SourceMap Map;sourceSyncPending_=false;if(!Map.Load(text_->toPlainText()))return;const auto Anchor=Map.Anchor(text_->textCursor().position());if(!Anchor.isEmpty())previewView_->ReaderAction("source-position",Anchor);
}
bool EpubSourceEditor::selectPreviewSource() {
    if(sourceComposing_ || !ready_ || !previewView_ || previewActiveToken_!=Token())return false;
    const auto State=previewView_->ViewState(),Anchor=State["sourceSelection"].toObject();const auto Serial=Anchor["serial"].toInteger();if(Serial<=sourceSelectionSerial_)return false;sourceSelectionSerial_=Serial;
    const auto Spine=model_.Publication()["spine"].toArray();const int Chapter=Anchor["chapter"].toInt(-1);if(Chapter<0 || Chapter>=Spine.size())return false;const auto Path=Spine[Chapter].toObject()["path"].toString();const int Index=resources_->findText(Path);if(Index<0 || !flush())return false;
    if(current_!=Path){const QSignalBlocker Block(resources_);resources_->setCurrentIndex(Index);select();}
    epub::SourceMap Map;if(!Map.Load(text_->toPlainText()))return false;const int Position=Map.Position(Anchor);if(Position<0)return false;const QSignalBlocker Block(text_);auto Cursor=text_->textCursor();Cursor.setPosition(Position);text_->setTextCursor(Cursor);text_->ensureCursorVisible();return true;
}
}
