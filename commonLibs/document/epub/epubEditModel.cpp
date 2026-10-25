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
bool imageValid(const QByteArray& Bytes,const QString& Media) {
    if(!QStringList{"image/png","image/jpeg","image/gif"}.contains(Media))return true;
    QBuffer Buffer;Buffer.setData(Bytes);Buffer.open(QIODevice::ReadOnly);QImageReader Reader(&Buffer);const auto Size=Reader.size();
    const auto Expected=Media=="image/png"?QByteArray("png"):Media=="image/jpeg"?QByteArray("jpeg"):QByteArray("gif");
    return Reader.format()==Expected && Size.isValid() && Size.width()<=4096 && Size.height()<=4096 && qint64(Size.width())*Size.height()<=4194304 && !Reader.read().isNull();
}
}

bool EditModel::Load(QByteArray Bytes,const Cancellation& Cancel) {
    const auto Prepared=Prepare(Bytes,Cancel);if(!Prepared){error_=Prepared.error_+": "+Prepared.location_;return false;}
    original_=std::move(Bytes);publication_=Prepared.publication_;error_.clear();media_.clear();replacements_.clear();history_.clear();position_=0;
    QJsonArray Metadata;for(const auto& Value:publication_["resources"].toArray()){auto Item=Value.toObject();Item.remove("base64");Metadata.append(Item);}publication_["resources"]=Metadata;
    for(const auto& V:publication_["resources"].toArray()){const auto R=V.toObject();const auto M=R["mediaType"].toString();if(QStringList{"application/xhtml+xml","application/oebps-package+xml","text/css","image/svg+xml","image/png","image/jpeg","image/gif"}.contains(M))media_[R["path"].toString()]=M;}
    return true;
}
QByteArray EditModel::Read(const QString& Path)const {
    if(!media_.contains(Path))return {};if(replacements_.contains(Path))return replacements_[Path];
    try {const Cancellation Cancel;return detail::Archive(original_,Cancel).Read(Path);}catch(const detail::Rejected&){return {};}
}
void EditModel::assign(const QString& Path,const QByteArray& Bytes) {
    const Cancellation Cancel;const detail::Archive Archive(original_,Cancel);if(Archive.Read(Path)==Bytes)replacements_.remove(Path);else replacements_[Path]=Bytes;
}
QString EditModel::Replace(const QString& Path,const QByteArray& Bytes) {
    if(!media_.contains(Path) || Bytes.isEmpty() || Bytes.size()>(media_[Path].startsWith("image/")?8*1024*1024:2*1024*1024))return "epub-edit-resource-budget";
    if(!imageValid(Bytes,media_[Path]))return "epub-edit-image-invalid-or-over-budget";
    qint64 Total=Bytes.size();for(auto I=replacements_.begin();I!=replacements_.end();++I)if(I.key()!=Path)Total+=I.value().size();if(Total>32*1024*1024)return "epub-edit-overlay-budget";
    const auto Before=Read(Path);if(Before==Bytes)return {};
    while(history_.size()>position_)history_.removeLast();
    history_.append({Path,Before,Bytes});++position_;assign(Path,Bytes);
    qint64 Memory=0;for(const auto& C:history_)Memory+=C.before_.size()+C.after_.size();
    while(history_.size()>1 && (history_.size()>32 || Memory>64*1024*1024)){Memory-=history_.first().before_.size()+history_.first().after_.size();history_.removeFirst();--position_;}
    return {};
}
void EditModel::Undo(){if(CanUndo()){const auto& C=history_[--position_];assign(C.path_,C.before_);}}
void EditModel::Redo(){if(CanRedo()){const auto& C=history_[position_++];assign(C.path_,C.after_);}}
QByteArray EditModel::Fingerprint(const QMap<QString,QByteArray>& Draft)const {
    auto Changes=replacements_;for(auto I=Draft.begin();I!=Draft.end();++I)Changes[I.key()]=I.value();
    if(Changes.isEmpty())return QByteArray::fromHex(publication_["sourceSha256"].toString().toLatin1());
    QCryptographicHash Hash(QCryptographicHash::Sha256);Hash.addData(publication_["sourceSha256"].toString().toLatin1());
    for(auto I=Changes.begin();I!=Changes.end();++I){Hash.addData(I.key().toUtf8());Hash.addData(QByteArray(1,0));Hash.addData(QCryptographicHash::hash(I.value(),QCryptographicHash::Sha256).toHex());}return Hash.result();
}
EditOutput EditModel::Export(const QMap<QString,QByteArray>& Draft,const Cancellation& Cancel)const {
    auto Changes=replacements_;for(auto I=Draft.begin();I!=Draft.end();++I){if(!media_.contains(I.key()) || I.value().isEmpty() || I.value().size()>(media_[I.key()].startsWith("image/")?8*1024*1024:2*1024*1024))return {{},"epub-edit-resource-budget",I.key()};Changes[I.key()]=I.value();}
    for(auto I=Changes.begin();I!=Changes.end();++I)if(QStringList{"application/xhtml+xml","application/oebps-package+xml","image/svg+xml"}.contains(media_[I.key()])) {
        QXmlStreamReader Xml(I.value());detail::XhtmlEntities Entities;Xml.setEntityResolver(&Entities);while(!Xml.atEnd()){if(Cancel && Cancel())return {{},"cancelled",I.key()};const auto Token=Xml.readNext();if(Token==QXmlStreamReader::DTD){if(!detail::PassiveDoctype(Xml))return {{},"xml-dtd-or-entity",I.key()};Entities.enabled_=Xml.dtdName()=="html";}else if(Token==QXmlStreamReader::EntityReference)return {{},"xml-dtd-or-entity",I.key()};}if(Xml.hasError())return {{},"XML syntax: "+Xml.errorString(),QString("%1:%2:%3").arg(I.key()).arg(Xml.lineNumber()).arg(Xml.columnNumber())};
    }
    if(Changes.isEmpty()) {
        if(publication_["recovery"].toObject()["missingSpineItems"].toInt()>0)return {{},"epub-edit-spine-repair-required",publication_["rootfile"].toString()};
        return {original_,{},{}};
    }
    for(auto I=Changes.begin();I!=Changes.end();++I)if(!imageValid(I.value(),media_[I.key()]))return {{},"epub-edit-image-invalid-or-over-budget",I.key()};
    try {
        const auto Bytes=detail::Archive(original_,Cancel).Rewrite(Changes);const auto Checked=Prepare(Bytes,Cancel);if(!Checked)return {{},Checked.error_,Checked.location_};
        if(Checked.publication_["recovery"].toObject()["missingSpineItems"].toInt()>0)return {{},"epub-edit-spine-repair-required",Checked.publication_["rootfile"].toString()};
        for(const auto& Value:Checked.publication_["resources"].toArray()) {const auto Item=Value.toObject();const auto Path=Item["path"].toString(),Media=Item["mediaType"].toString();
            if(Changes.contains(Path) || media_.value(Path)!=Media) {const auto Raw=QByteArray::fromBase64(Item["base64"].toString().toLatin1());if(!imageValid(Raw,Media))return {{},"epub-edit-image-media-mismatch",Path};}}
        return {Bytes,{},{}};
    }catch(const detail::Rejected& Error){return {{},Error.code_,Error.location_};}
}
}
