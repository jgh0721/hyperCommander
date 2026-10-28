#include "stdafx.h"
#include "epubEditModel.hpp"
#include "epubArchive.hpp"
#include <QCryptographicHash>
#include <QJsonArray>
#include <QBuffer>
#include <QImageReader>
#include <QXmlStreamReader>
namespace hc::document::epub {
namespace {
QString cssProblem(const QByteArray& Bytes,int& Line,int& Column,const Cancellation& Cancel) {
    const auto Text=QString::fromUtf8(Bytes);qsizetype Index=0;int CurrentLine=1,CurrentColumn=1,StartLine=1,StartColumn=1;QChar Quote;bool Comment=false,Url=false;
    struct Delimiter {QChar value_;int line_,column_;};QList<Delimiter> Stack;
    const auto Take=[&]{const auto Char=Text[Index++];if(Char=='\n'){++CurrentLine;CurrentColumn=1;}else ++CurrentColumn;return Char;};
    while(Index<Text.size()) {
        if(Index%1024==0 && Cancel && Cancel())return "cancelled";const int AtLine=CurrentLine,AtColumn=CurrentColumn;const auto Char=Take();
        if(Comment){if(Char=='*' && Index<Text.size() && Text[Index]=='/'){Take();Comment=false;}continue;}
        if(!Quote.isNull()){if(Char=='\\'){if(Index<Text.size())Take();}else if(Char==Quote)Quote={};else if(Char=='\n' || Char=='\r'){Line=StartLine;Column=StartColumn;return "CSS string contains an unescaped line break";}continue;}
        if(Url){if(Char=='\\'){if(Index<Text.size())Take();}else if(Char=='\'' || Char=='"'){Quote=Char;StartLine=AtLine;StartColumn=AtColumn;}else if(Char==')'){Url=false;Stack.removeLast();}continue;}
        if(Char=='(' && Index>=4 && Text.mid(Index-4,3).compare("url",Qt::CaseInsensitive)==0 && (Index==4 || (!Text[Index-5].isLetterOrNumber() && Text[Index-5]!='_' && Text[Index-5]!='-')))Url=true;
        if(Char=='/' && Index<Text.size() && Text[Index]=='*'){Take();Comment=true;StartLine=AtLine;StartColumn=AtColumn;continue;}
        if(Char=='\'' || Char=='"'){Quote=Char;StartLine=AtLine;StartColumn=AtColumn;continue;}
        if(Char=='\\'){if(Index<Text.size())Take();continue;}
        if(Char=='{' || Char=='(' || Char=='['){Stack.append({Char,AtLine,AtColumn});continue;}
        if(Char=='}' || Char==')' || Char==']'){
            const auto Expected=Char=='}'?QChar('{'):Char==')'?QChar('('):QChar('[');
            if(Stack.isEmpty() || Stack.last().value_!=Expected){Line=AtLine;Column=AtColumn;return "CSS closing delimiter does not match";}Stack.removeLast();
        }
    }
    if(Comment || !Quote.isNull()){Line=StartLine;Column=StartColumn;return Comment?"CSS comment is not closed":"CSS string is not closed";}
    if(!Stack.isEmpty()){Line=Stack.last().line_;Column=Stack.last().column_;return "CSS delimiter is not closed";}return {};
}
bool imageValid(const QByteArray& Bytes,const QString& Media) {
    if(!QStringList{"image/png","image/jpeg","image/gif","image/webp"}.contains(Media))return true;
    QBuffer Buffer;Buffer.setData(Bytes);Buffer.open(QIODevice::ReadOnly);QImageReader Reader(&Buffer);const auto Size=Reader.size();
    const auto Expected=Media=="image/png"?QByteArray("png"):Media=="image/jpeg"?QByteArray("jpeg"):Media=="image/webp"?QByteArray("webp"):QByteArray("gif");
    return Reader.format()==Expected && Size.isValid() && Size.width()<=4096 && Size.height()<=4096 && qint64(Size.width())*Size.height()<=4194304 && !Reader.read().isNull();
}
}

bool EditModel::Load(QByteArray Bytes,const Cancellation& Cancel) {
    const auto Session=PublicationSession::Open(Bytes,Cancel);
    const auto Prepared=Session->Index();if(!Prepared){error_=Prepared.error_+": "+Prepared.location_;return false;}
    const auto Validated=Session->ValidateAll();if(!Validated){error_=Validated.error_+": "+Validated.location_;return false;}
    accept(std::move(Bytes),Prepared.publication_);return true;
}
void EditModel::accept(QByteArray Bytes,const QJsonObject& Publication) {
    original_=Bytes;edition_=std::move(Bytes);originalHash_=Publication["sourceSha256"].toString();publication_=Publication;error_.clear();media_.clear();replacements_.clear();history_.clear();position_=0;
    QJsonArray Metadata;for(const auto& Value:publication_["resources"].toArray()){auto Item=Value.toObject();Item.remove("base64");Metadata.append(Item);}publication_["resources"]=Metadata;
    for(const auto& V:publication_["resources"].toArray()){const auto R=V.toObject();const auto M=R["mediaType"].toString();if(QStringList{"application/xhtml+xml","application/oebps-package+xml","application/x-dtbncx+xml","text/css","image/svg+xml","image/png","image/jpeg","image/gif","image/webp"}.contains(M))media_[R["path"].toString()]=M;}
}
QByteArray EditModel::Read(const QString& Path)const {
    if(!media_.contains(Path))return {};if(replacements_.contains(Path))return replacements_[Path];
    try {const Cancellation Cancel;return detail::Archive(edition_,Cancel,32*1024*1024).Read(Path);}catch(const detail::Rejected&){return {};}
}
void EditModel::assign(const QString& Path,const QByteArray& Bytes) {
    const Cancellation Cancel;const detail::Archive Archive(edition_,Cancel,32*1024*1024);if(Archive.Read(Path)==Bytes)replacements_.remove(Path);else replacements_[Path]=Bytes;
}
QString EditModel::Replace(const QString& Path,const QByteArray& Bytes) {
    if(!media_.contains(Path) || Bytes.isEmpty() || Bytes.size()>(media_[Path].startsWith("image/")?8*1024*1024:2*1024*1024))return "epub-edit-resource-budget";
    if(!imageValid(Bytes,media_[Path]))return "epub-edit-image-invalid-or-over-budget";
    qint64 Total=Bytes.size();for(auto I=replacements_.begin();I!=replacements_.end();++I)if(I.key()!=Path)Total+=I.value().size();if(Total>32*1024*1024)return "epub-edit-overlay-budget";
    if(Read(Path)==Bytes)return {};auto Before=state();assign(Path,Bytes);record(std::move(Before));return {};
}
EditModel::State EditModel::state()const{return {edition_,publication_,media_,replacements_};}
void EditModel::restore(const State& Value){edition_=Value.edition_;publication_=Value.publication_;media_=Value.media_;replacements_=Value.changes_;}
void EditModel::record(State Before) {
    while(history_.size()>position_)history_.removeLast();history_.append({std::move(Before),state()});++position_;
    const auto Memory=[this](const State& Value){qint64 Size=Value.publication_["sourceSha256"].toString()==originalHash_?0:Value.edition_.size();for(const auto& Bytes:Value.changes_)Size+=Bytes.size();return Size;};
    qint64 Total=0;for(const auto& Change:history_)Total+=Memory(Change.before_)+Memory(Change.after_);
    while(history_.size()>1 && (history_.size()>32 || Total>128LL*1024*1024)){Total-=Memory(history_.first().before_)+Memory(history_.first().after_);history_.removeFirst();--position_;}
}
void EditModel::Undo(){if(CanUndo())restore(history_[--position_].before_);}
void EditModel::Redo(){if(CanRedo())restore(history_[position_++].after_);}
QString EditModel::RestoreStructuralBytes(const QByteArray& Bytes) {
    if(Bytes.isEmpty())return {};EditModel Candidate;if(!Candidate.Load(Bytes))return Candidate.Error();
    edition_=Bytes;publication_=Candidate.publication_;media_=Candidate.media_;replacements_.clear();return {};
}
QByteArray EditModel::Fingerprint(const QMap<QString,QByteArray>& Draft)const {
    auto Changes=replacements_;for(auto I=Draft.begin();I!=Draft.end();++I)Changes[I.key()]=I.value();
    if(Changes.isEmpty())return QByteArray::fromHex(publication_["sourceSha256"].toString().toLatin1());
    const auto Edition=publication_["sourceSha256"].toString();
    // Content equality includes the immutable edition, resource paths and bytes;
    // undo, structural edits and drafts cannot inherit another revision's hash.
    if(fingerprintEdition_==Edition && fingerprintChanges_==Changes)return fingerprint_;
    QCryptographicHash Hash(QCryptographicHash::Sha256);Hash.addData(Edition.toLatin1());
    for(auto I=Changes.begin();I!=Changes.end();++I){Hash.addData(I.key().toUtf8());Hash.addData(QByteArray(1,0));Hash.addData(QCryptographicHash::hash(I.value(),QCryptographicHash::Sha256).toHex());}fingerprintEdition_=Edition;fingerprintChanges_=std::move(Changes);fingerprint_=Hash.result();return fingerprint_;
}
EditOutput EditModel::Export(const QMap<QString,QByteArray>& Draft,const Cancellation& Cancel)const {
    auto Changes=replacements_;for(auto I=Draft.begin();I!=Draft.end();++I){if(!media_.contains(I.key()) || I.value().isEmpty() || I.value().size()>(media_[I.key()].startsWith("image/")?8*1024*1024:2*1024*1024))return {{},"epub-edit-resource-budget",I.key()};Changes[I.key()]=I.value();}
    try {
        const detail::Archive Source(edition_,Cancel,32*1024*1024);
        for(auto I=media_.begin();I!=media_.end();++I) {
            if(Cancel && Cancel())return {{},"cancelled",I.key()};
            const bool XmlMedia=QStringList{"application/xhtml+xml","application/oebps-package+xml","application/x-dtbncx+xml","image/svg+xml"}.contains(I.value());
            if(!XmlMedia && I.value()!="text/css")continue;
            const auto Raw=Changes.contains(I.key())?Changes[I.key()]:Source.Read(I.key());
            if(I.value()=="text/css") {int Line=1,Column=1;const auto Problem=cssProblem(Raw,Line,Column,Cancel);if(!Problem.isEmpty())return {{},Problem,QString("%1:%2:%3").arg(I.key()).arg(Line).arg(Column)};continue;}
            QXmlStreamReader Xml(Raw);detail::XhtmlEntities Entities;Xml.setEntityResolver(&Entities);while(!Xml.atEnd()){if(Cancel && Cancel())return {{},"cancelled",I.key()};const auto Token=Xml.readNext();if(Token==QXmlStreamReader::DTD){if(!detail::PassiveDoctype(Xml))return {{},"xml-dtd-or-entity",I.key()};Entities.enabled_=Xml.dtdName()=="html";}else if(Token==QXmlStreamReader::EntityReference)return {{},"xml-dtd-or-entity",I.key()};}
            if(Xml.hasError())return {{},"XML syntax: "+Xml.errorString(),QString("%1:%2:%3").arg(I.key()).arg(Xml.lineNumber()).arg(Xml.columnNumber())};
        }
        if(Changes.isEmpty()) {
            if(publication_["recovery"].toObject()["missingSpineItems"].toInt()>0)return {{},"epub-edit-spine-repair-required",publication_["rootfile"].toString()};
            auto Prepared=std::make_shared<EditModel>();Prepared->accept(edition_,publication_);
            return {edition_,{},{},std::move(Prepared)};
        }
        for(auto I=Changes.begin();I!=Changes.end();++I)if(!imageValid(I.value(),media_[I.key()]))return {{},"epub-edit-image-invalid-or-over-budget",I.key()};
        const auto Bytes=Source.Rewrite(Changes);const auto Session=PublicationSession::Open(Bytes,Cancel);
        const auto Checked=Session->Index();if(!Checked)return {{},Checked.error_,Checked.location_};
        const auto Validated=Session->ValidateAll();if(!Validated)return {{},Validated.error_,Validated.location_};
        if(Checked.publication_["recovery"].toObject()["missingSpineItems"].toInt()>0)return {{},"epub-edit-spine-repair-required",Checked.publication_["rootfile"].toString()};
        for(const auto& Value:Checked.publication_["resources"].toArray()) {const auto Item=Value.toObject();const auto Path=Item["path"].toString(),Media=Item["mediaType"].toString();
            if(Changes.contains(Path) || media_.value(Path)!=Media) {const auto Raw=Session->ReadValidatedResource(Path);if(!Raw || !imageValid(Raw.bytes_,Media))return {{},"epub-edit-image-media-mismatch",Path};}}
        auto Prepared=std::make_shared<EditModel>();Prepared->accept(Bytes,Checked.publication_);
        return {Bytes,{},{},std::move(Prepared)};
    }catch(const detail::Rejected& Error){return {{},Error.code_,Error.location_};}
}
}
