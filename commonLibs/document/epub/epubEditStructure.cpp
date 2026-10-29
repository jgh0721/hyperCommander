#include "stdafx.h"
#include "epubEditModel.hpp"
#include "epubArchive.hpp"
#include <QBuffer>
#include <QCryptographicHash>
#include <QDomDocument>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QJsonArray>
#include <QPainter>
#include <QRegularExpression>
#include <QUrl>
#include <QXmlStreamReader>
#include <QUuid>
#include <new>
namespace hc::document::epub {
namespace {
using namespace detail;
const QString Opf="http://www.idpf.org/2007/opf",Xhtml="http://www.w3.org/1999/xhtml",Ops="http://www.idpf.org/2007/ops",Ncx="http://www.daisy.org/z3986/2005/ncx/";
QStringList tokens(const QString& Text){return Text.split(QRegularExpression("\\s+"),Qt::SkipEmptyParts);}
QString resolve(const QString& Base,const QString& Href) {
    const QUrl Url(Href,QUrl::TolerantMode);if(Href.isEmpty() || !Url.isValid() || !Url.scheme().isEmpty() || !Url.authority().isEmpty() || Url.hasQuery())return {};
    const auto Path=QUrl::fromPercentEncoding(Href.section('#',0,0).toUtf8());if(Path.isEmpty())return Base;
    if(Path.startsWith('/') || Path.contains(':') || Path.contains('\\'))return {};
    return Canonical(QDir::cleanPath(QFileInfo(Base).path()+"/"+Path));
}
QString relative(const QString& Base,const QString& Target,const QString& Fragment={}) {
    auto BaseParts=Base.split('/');BaseParts.removeLast();const auto TargetParts=Target.split('/');qsizetype Common=0;while(Common<BaseParts.size() && Common<TargetParts.size() && BaseParts[Common]==TargetParts[Common])++Common;QStringList Parts;for(auto I=Common;I<BaseParts.size();++I)Parts.append("..");for(auto I=Common;I<TargetParts.size();++I)Parts.append(TargetParts[I]);auto Path=Parts.join('/');QUrl Url;Url.setPath(Path);if(!Fragment.isEmpty())Url.setFragment(Fragment);return Url.toString(QUrl::FullyEncoded);
}
QDomDocument xml(const QByteArray& Bytes,const QString& Path) {
    if(Bytes.size()>2*1024*1024)Reject("xml-limit",Path);QXmlStreamReader Reader(Bytes);XhtmlEntities Entities;Entities.enabled_=true;Reader.setEntityResolver(&Entities);QDomDocument Document;
    const auto Result=Document.setContent(&Reader,QDomDocument::ParseOptions(QDomDocument::ParseOption::UseNamespaceProcessing)|QDomDocument::ParseOption::PreserveSpacingOnlyNodes);
    if(!Result)Reject("edit-xml-syntax",QString("%1:%2:%3").arg(Path).arg(Result.errorLine).arg(Result.errorColumn));return Document;
}
QDomElement child(const QDomNode& Parent,const QString& Ns,const QString& Name) {
    for(auto Node=Parent.firstChild();!Node.isNull();Node=Node.nextSibling()){const auto Element=Node.toElement();if(Element.namespaceURI()==Ns && Element.localName()==Name)return Element;}return {};
}
QDomElement element(QDomDocument& Document,const QString& Ns,const QString& Name,const QString& Text={}) {
    auto Result=Document.createElementNS(Ns,Name);if(!Text.isNull())Result.appendChild(Document.createTextNode(Text));return Result;
}
QByteArray bytes(const QDomDocument& Document){const auto Bytes=Document.toByteArray(-1);if(Bytes.size()>2*1024*1024)Reject("edit-xml-limit");return Bytes;}
QString uniqueId(){return "hc-"+QUuid::createUuid().toString(QUuid::Id128);}
QString escaped(const QString& Text,qsizetype& At) {
    if(At>=Text.size())return {};const auto Char=Text[At++];if(Char=='\n')return {};if(Char=='\r'){if(At<Text.size() && Text[At]=='\n')++At;return {};}
    if(!(Char>='0' && Char<='9') && !(Char.toLower()>='a' && Char.toLower()<='f'))return Char;
    QString Hex(Char);while(At<Text.size() && Hex.size()<6 && ((Text[At]>='0' && Text[At]<='9') || Text[At].toLower()>='a' && Text[At].toLower()<='f'))Hex+=Text[At++];
    if(At<Text.size() && Text[At].isSpace()){if(Text[At++]=='\r' && At<Text.size() && Text[At]=='\n')++At;}
    bool Ok=false;char32_t Code=Hex.toUInt(&Ok,16);if(!Ok || !Code || Code>0x10ffff || Code>=0xd800 && Code<=0xdfff)Code=0xfffd;return QString::fromUcs4(&Code,1);
}
QString quoted(const QString& Text,qsizetype& At) {
    const auto Quote=Text[At++];QString Value;
    while(At<Text.size()){const auto Char=Text[At++];if(Char==Quote)return Value;if(Char=='\\')Value+=escaped(Text,At);else Value+=Char;}Reject("edit-css-reference-syntax");
}
QString cssReferences(const QString& Text,const std::function<QString(const QString&)>& Map) {
    struct Change{qsizetype first_,last_;QString value_;};QList<Change> Changes;QList<QString> Functions;qsizetype At=0;bool Import=false;
    const auto String=[&](bool Reference){const auto First=At;const auto Value=quoted(Text,At);if(Reference){const auto Next=Map(Value);if(Next!=Value)Changes.append({First,At,"\""+Next+"\""});}};
    const auto Name=[&]{QString Value;while(At<Text.size()){const auto Char=Text[At];if(Char=='\\'){++At;Value+=escaped(Text,At);}else if(Char.isLetterOrNumber() || Char=='_' || Char=='-' || Char.unicode()>=128){Value+=Char;++At;}else break;}return Value.toLower();};
    while(At<Text.size()) {
        const auto Char=Text[At];
        if(Char=='/' && At+1<Text.size() && Text[At+1]=='*'){const auto End=Text.indexOf("*/",At+2);if(End<0)Reject("edit-css-reference-syntax");At=End+2;continue;}
        if(Char=='\'' || Char=='"'){String(Import || !Functions.isEmpty() && QStringList{"image-set","-webkit-image-set","image","src"}.contains(Functions.last()));Import=false;continue;}
        if(Char=='@'){++At;const auto Rule=Name();if(Rule=="import"){Import=true;while(At<Text.size() && Text[At].isSpace())++At;if(At<Text.size() && (Text[At]=='\'' || Text[At]=='"')){String(true);Import=false;}}continue;}
        if(Char.isLetterOrNumber() || Char=='_' || Char=='-' || Char=='\\' || Char.unicode()>=128) {
            const auto First=At;const auto Id=Name();
            if(At<Text.size() && Text[At]=='(') {
                ++At;
                if(Id=="url") {
                    Import=false;
                    while(At<Text.size() && Text[At].isSpace())++At;QString Value;
                    if(At<Text.size() && (Text[At]=='\'' || Text[At]=='"'))Value=quoted(Text,At);
                    else while(At<Text.size() && Text[At]!=')'){if(Text[At]=='\\'){++At;Value+=escaped(Text,At);}else Value+=Text[At++];}
                    while(At<Text.size() && Text[At].isSpace())++At;if(At>=Text.size() || Text[At++]!=')')Reject("edit-css-reference-syntax");Value=Value.trimmed();const auto Next=Map(Value);if(Next!=Value)Changes.append({First,At,"url(\""+Next+"\")"});
                }else Functions.append(Id);
            }continue;
        }
        if(Char==';')Import=false;
        if(Char=='(')Functions.append(QString{});else if(Char==')' && !Functions.isEmpty())Functions.removeLast();++At;
    }
    auto Result=Text;for(auto I=Changes.crbegin();I!=Changes.crend();++I)Result.replace(I->first_,I->last_-I->first_,I->value_);return Result;
}
QString srcsetReferences(const QString& Text,const std::function<QString(const QString&)>& Map) {
    struct Change{qsizetype first_,last_;QString value_;};QList<Change> Changes;qsizetype At=0;
    while(At<Text.size()) {
        while(At<Text.size() && (Text[At].isSpace() || Text[At]==','))++At;const auto First=At;while(At<Text.size() && !Text[At].isSpace())++At;auto End=At;while(End>First && Text[End-1]==',')--End;
        const auto Value=Text.mid(First,End-First);if(!Value.isEmpty()){const auto Next=Map(Value);if(Next!=Value)Changes.append({First,End,Next});}
        if(End!=At)continue;int Parentheses=0;while(At<Text.size()){const auto Char=Text[At++];if(Char=='(')++Parentheses;else if(Char==')')--Parentheses;else if(Char==',' && Parentheses<=0)break;}
    }
    auto Result=Text;for(auto I=Changes.crbegin();I!=Changes.crend();++I)Result.replace(I->first_,I->last_-I->first_,I->value_);return Result;
}
QByteArray imageData(const QJsonObject& Request,QString& Media) {
    const auto Encoded=Request["base64"].toString().toLatin1();if(Encoded.size()>12*1024*1024)Reject("edit-image-budget");const auto Decoded=QByteArray::fromBase64Encoding(Encoded,QByteArray::AbortOnBase64DecodingErrors);if(!Decoded || Decoded.decoded.isEmpty() || Decoded.decoded.size()>8*1024*1024)Reject("edit-image-budget");
    QBuffer Buffer;Buffer.setData(Decoded.decoded);Buffer.open(QIODevice::ReadOnly);QImageReader Reader(&Buffer);const auto Format=Reader.format();const auto Size=Reader.size();
    if(!QList<QByteArray>{"png","jpeg","gif","webp"}.contains(Format) || !Size.isValid() || Size.width()>4096 || Size.height()>4096 || qint64(Size.width())*Size.height()>4194304 || Reader.read().isNull())Reject("edit-image-format-or-size");
    Media=Format=="jpeg"?"image/jpeg":"image/"+QString::fromLatin1(Format);return Decoded.decoded;
}
struct Package {
    QByteArray source_;Cancellation cancel_;QJsonObject publication_;QDomDocument opf_;QString rootfile_;Archive archive_;
    QMap<QString,QByteArray> replacements_,additions_;QSet<QString> removals_;QMap<QString,QString> media_;QJsonArray toc_;
    Package(QByteArray Source,QJsonObject Publication,Cancellation Cancel):source_(std::move(Source)),cancel_(std::move(Cancel)),publication_(std::move(Publication)),archive_(source_,cancel_,32*1024*1024) {
        rootfile_=publication_["rootfile"].toString();opf_=xml(read(rootfile_),rootfile_);toc_=publication_["navigation"].toArray();
        for(const auto& Value:publication_["resources"].toArray()){const auto Row=Value.toObject();media_[Row["path"].toString()]=Row["mediaType"].toString();}
    }
    QByteArray read(const QString& Path)const {if(additions_.contains(Path))return additions_[Path];if(replacements_.contains(Path))return replacements_[Path];return archive_.Read(Path);}
    void store(const QString& Path,const QByteArray& Data){if(additions_.contains(Path))additions_[Path]=Data;else replacements_[Path]=Data;}
    QDomElement manifest(){return child(opf_.documentElement(),Opf,"manifest");}
    QDomElement spine(){return child(opf_.documentElement(),Opf,"spine");}
    QDomElement item(const QString& Path){for(auto Node=manifest().firstChildElement();!Node.isNull();Node=Node.nextSiblingElement())if(resolve(rootfile_,Node.attribute("href"))==Path)return Node;return {};}
    void add(const QString& Path,const QString& Media,const QByteArray& Data) {
        if(Canonical(Path)!=Path || media_.contains(Path) || archive_.Contains(Path))Reject("edit-resource-exists",Path);
        additions_[Path]=Data;media_[Path]=Media;auto Item=element(opf_,Opf,"item");Item.setAttribute("id",uniqueId());Item.setAttribute("href",relative(rootfile_,Path));Item.setAttribute("media-type",Media);manifest().appendChild(Item);
    }
    void toc(const QJsonArray& Rows);
    void references(const QString& Old,const QString& New,bool RemoveImages=false,bool Rebase=true);
    void rename(const QString& Old,const QString& New);
    void remove(const QString& Path,bool RemoveImages);
    void cover(const QString& Path);
    void chapter(const QString& Path,const QString& Title,int After);
    void move(const QString& Path,int Delta);
    QByteArray finish(){
        if(opf_.documentElement().attribute("version")!="2.0") {auto Metadata=child(opf_.documentElement(),Opf,"metadata");QDomElement Modified;for(auto Node=Metadata.firstChildElement();!Node.isNull();Node=Node.nextSiblingElement())if(Node.localName()=="meta" && Node.attribute("property")=="dcterms:modified"){Modified=Node;break;}if(Modified.isNull()){Modified=element(opf_,Opf,"meta");Modified.setAttribute("property","dcterms:modified");Metadata.appendChild(Modified);}while(!Modified.firstChild().isNull())Modified.removeChild(Modified.firstChild());Modified.appendChild(opf_.createTextNode(QDateTime::currentDateTimeUtc().toString("yyyy-MM-dd'T'HH:mm:ss'Z'")));}
        replacements_[rootfile_]=bytes(opf_);return archive_.Rewrite(replacements_,additions_,removals_);
    }
};
void Package::toc(const QJsonArray& Rows) {
    if(Rows.isEmpty() || Rows.size()>1024)Reject("edit-toc-budget");int Previous=-1;
    for(const auto& Value:Rows){const auto Row=Value.toObject();const int Depth=Row["depth"].toInt(-1);const auto Path=Row["path"].toString();
        if(!Value.isObject() || !Row["label"].isString() || Row["label"].toString().trimmed().isEmpty() || Row["label"].toString().size()>1024 || Depth<0 || Depth>32 || Depth>Previous+1 || !Path.isEmpty() && media_.value(Path)!="application/xhtml+xml" || Row["fragment"].toString().size()>1024)Reject("edit-toc-entry");Previous=Depth;}
    for(const auto& Value:Rows){const auto Target=Value.toObject()["path"].toString();if(Target.isEmpty())continue;const auto Id=item(Target).attribute("id");bool InSpine=false;for(auto Ref=spine().firstChildElement();!Ref.isNull();Ref=Ref.nextSiblingElement())InSpine|=Ref.attribute("idref")==Id;if(!InSpine)Reject("edit-toc-target-not-in-spine",Target);}
    const bool Epub2=opf_.documentElement().attribute("version")=="2.0";QString Path;
    for(auto I=media_.begin();I!=media_.end();++I)if(Epub2?I.value()=="application/x-dtbncx+xml":tokens(item(I.key()).attribute("properties")).contains("nav")){Path=I.key();break;}
    QDomDocument Document;QDomElement Container;
    if(Path.isEmpty()) {
        Path=QFileInfo(rootfile_).path()+"/hc-toc-"+QUuid::createUuid().toString(QUuid::Id128)+(Epub2?".ncx":".xhtml");
        auto Root=element(Document,Epub2?Ncx:Xhtml,Epub2?"ncx":"html");Document.appendChild(Root);
        if(Epub2) {
            Root.setAttribute("version","2005-1");auto Head=element(Document,Ncx,"head");Root.appendChild(Head);auto Meta=element(Document,Ncx,"meta");Meta.setAttribute("name","dtb:uid");
            const auto Unique=opf_.documentElement().attribute("unique-identifier");const auto Ids=opf_.elementsByTagNameNS("http://purl.org/dc/elements/1.1/","identifier");QString Identifier;
            for(int I=0;I<Ids.size();++I)if(Ids.at(I).toElement().attribute("id")==Unique)Identifier=Ids.at(I).toElement().text();Meta.setAttribute("content",Identifier);Head.appendChild(Meta);
            auto Title=element(Document,Ncx,"docTitle");Title.appendChild(element(Document,Ncx,"text",publication_["title"].toString()));Root.appendChild(Title);Container=element(Document,Ncx,"navMap");Root.appendChild(Container);
        } else {
            Root.setAttribute("xmlns:epub",Ops);auto Head=element(Document,Xhtml,"head");Head.appendChild(element(Document,Xhtml,"title",QStringLiteral("목차")));Root.appendChild(Head);auto Body=element(Document,Xhtml,"body");Root.appendChild(Body);Container=element(Document,Xhtml,"nav");Container.setAttributeNS(Ops,"epub:type","toc");Body.appendChild(Container);
        }
        add(Path,Epub2?"application/x-dtbncx+xml":"application/xhtml+xml",{});auto Item=item(Path);if(Epub2)spine().setAttribute("toc",Item.attribute("id"));else Item.setAttribute("properties","nav");
    } else {
        Document=xml(read(Path),Path);
        if(Epub2)Container=child(Document.documentElement(),Ncx,"navMap");
        else {const auto Navs=Document.elementsByTagNameNS(Xhtml,"nav");for(int I=0;I<Navs.size();++I){const auto Node=Navs.at(I).toElement();if(tokens(Node.attributeNS(Ops,"type")).contains("toc")){Container=Node;break;}}}
        if(Container.isNull())Reject("edit-toc-container",Path);while(!Container.firstChild().isNull())Container.removeChild(Container.firstChild());
    }
    QList<QDomElement> Parents;QDomElement Last;
    if(Epub2)Parents.append(Container);else {Container.appendChild(element(Document,Xhtml,"h2",QStringLiteral("목차")));auto Ol=element(Document,Xhtml,"ol");Container.appendChild(Ol);Parents.append(Ol);}
    int Order=0;
    for(int I=0;I<Rows.size();++I) {
        const auto Row=Rows[I].toObject();const int Depth=Row["depth"].toInt();while(Parents.size()>Depth+1)Parents.removeLast();
        if(Parents.size()==Depth){if(Last.isNull())Reject("edit-toc-depth");if(Epub2)Parents.append(Last);else {auto Ol=element(Document,Xhtml,"ol");Last.appendChild(Ol);Parents.append(Ol);}}
        auto Target=Row["path"].toString();auto Fragment=Row["fragment"].toString();
        if(Epub2 && Target.isEmpty())for(int J=I+1;J<Rows.size() && Rows[J].toObject()["depth"].toInt()>Depth;++J)if(!Rows[J].toObject()["path"].toString().isEmpty()){Target=Rows[J].toObject()["path"].toString();Fragment=Rows[J].toObject()["fragment"].toString();break;}
        Last=element(Document,Epub2?Ncx:Xhtml,Epub2?"navPoint":"li");Parents.last().appendChild(Last);
        if(Epub2){if(Target.isEmpty())Reject("edit-toc-group-target");Last.setAttribute("id",uniqueId());Last.setAttribute("playOrder",++Order);auto Label=element(Document,Ncx,"navLabel");Label.appendChild(element(Document,Ncx,"text",Row["label"].toString()));Last.appendChild(Label);auto Content=element(Document,Ncx,"content");Content.setAttribute("src",relative(Path,Target,Fragment));Last.appendChild(Content);}
        else {auto Link=element(Document,Xhtml,Target.isEmpty()?"span":"a",Row["label"].toString());if(!Target.isEmpty())Link.setAttribute("href",relative(Path,Target,Fragment));Last.appendChild(Link);}
    }
    store(Path,bytes(Document));toc_=Rows;
}
void Package::references(const QString& Old,const QString& New,bool RemoveImages,bool Rebase) {
    for(auto I=media_.begin();I!=media_.end();++I) {
        const auto Path=I.key(),Media=I.value();if(Path==rootfile_ || Path==Old && (New.isEmpty() || !Rebase))continue;Check(cancel_);
        const auto Base=Rebase && Path==Old?New:Path;
        const auto Map=[&](const QString& Value){if(Value.startsWith('#'))return Value;const auto Target=resolve(Path,Value);if(Target.isEmpty() || Target!=Old && Base==Path || Target==Old && New==Old && Base==Path)return Value;
            if(Target==Old && New.isEmpty())Reject("edit-reference-still-used",Path+" -> "+Old);return relative(Base,Target==Old?New:Target,QUrl(Value).fragment());};
        const auto Raw=read(Path);
        if(Media=="text/css"){const auto Text=QString::fromUtf8(Raw);const auto Next=cssReferences(Text,Map);if(Next!=Text)store(Path,Next.toUtf8());continue;}
        if(!QStringList{"application/xhtml+xml","image/svg+xml","application/x-dtbncx+xml"}.contains(Media))continue;
        auto Document=xml(Raw,Path);QList<QDomElement> Nodes,Pending{Document.documentElement()};while(!Pending.isEmpty()){auto Node=Pending.takeLast();Nodes.append(Node);for(auto Child=Node.firstChildElement();!Child.isNull();Child=Child.nextSiblingElement())Pending.append(Child);}bool Changed=false;
        for(auto Node:Nodes) {
            auto Connected=QDomNode(Node);while(!Connected.isNull() && !Connected.isDocument())Connected=Connected.parentNode();if(Connected.isNull())continue;
            const bool Image=(Node.namespaceURI()==Xhtml && Node.localName()=="img") || (Node.namespaceURI()=="http://www.w3.org/2000/svg" && Node.localName()=="image");
            if(Image && RemoveImages && New.isEmpty()) {
                bool Uses=false;const auto Detect=[&](const QString& Value){Uses|=resolve(Path,Value)==Old;return Value;};
                Detect(Node.attribute("src"));Detect(Node.attribute("href"));Detect(Node.attributeNS("http://www.w3.org/1999/xlink","href"));srcsetReferences(Node.attribute("srcset"),Detect);cssReferences(Node.attribute("style"),Detect);
                if(Uses){Node.parentNode().removeChild(Node);Changed=true;continue;}
            }
            bool Removed=false;const auto Attributes=Node.attributes();
            for(int J=0;J<Attributes.size();++J) {
                auto Attr=Attributes.item(J).toAttr();const auto Name=Attr.localName().isEmpty()?Attr.name():Attr.localName();const auto Value=Attr.value();QString Next=Value;
                if(QStringList{"href","src","poster","data"}.contains(Name)) {
                    if(New.isEmpty() && resolve(Path,Value)==Old) {
                        bool Navigation=false;auto Parent=Node.parentNode().toElement();
                        for(auto Ancestor=Parent;!Ancestor.isNull();Ancestor=Ancestor.parentNode().toElement())if(Ancestor.namespaceURI()==Xhtml && Ancestor.localName()=="nav" && tokens(Ancestor.attributeNS(Ops,"type")).contains("landmarks"))Navigation=true;
                        if(Image && RemoveImages){Node.parentNode().removeChild(Node);Removed=Changed=true;break;}
                        if(Navigation){auto Target=Parent.localName()=="li"?QDomNode(Parent):QDomNode(Node);Target.parentNode().removeChild(Target);Removed=Changed=true;break;}
                    }
                    Next=Map(Value);
                }else if(Name=="srcset")Next=srcsetReferences(Value,Map);else if(Name=="style")Next=cssReferences(Value,Map);
                if(Next!=Value){Attr.setValue(Next);Changed=true;}
            }
            if(Removed)continue;
            if(Node.localName()=="style"){const auto Text=Node.text(),Next=cssReferences(Text,Map);if(Next!=Text){while(!Node.firstChild().isNull())Node.removeChild(Node.firstChild());Node.appendChild(Document.createTextNode(Next));Changed=true;}}
        }
        if(Changed)store(Path,bytes(Document));
    }
    const auto Guide=opf_.elementsByTagNameNS(Opf,"reference");QList<QDomElement> References;for(int I=0;I<Guide.size();++I)References.append(Guide.at(I).toElement());
    for(auto Node:References)if(resolve(rootfile_,Node.attribute("href"))==Old){if(New.isEmpty())Node.parentNode().removeChild(Node);else Node.setAttribute("href",relative(rootfile_,New,QUrl(Node.attribute("href")).fragment()));}
}
void Package::rename(const QString& Old,const QString& New) {
    if(Old==rootfile_ || !media_.contains(Old) || Canonical(New)!=New || Old==New)Reject("edit-rename-resource",Old);
    const auto Folded=New.toCaseFolded();for(auto I=media_.begin();I!=media_.end();++I)if(I.key()!=Old && I.key().toCaseFolded()==Folded)Reject("edit-resource-exists",New);
    if(archive_.Contains(New) && New!=Old)Reject("edit-resource-exists",New);
    references(Old,New);auto Item=item(Old);if(Item.isNull())Reject("edit-manifest-resource",Old);Item.setAttribute("href",relative(rootfile_,New));
    const auto Raw=read(Old);replacements_.remove(Old);if(additions_.contains(Old))additions_.remove(Old);else removals_.insert(Old);additions_[New]=Raw;media_[New]=media_.take(Old);
    for(int I=0;I<toc_.size();++I){auto Row=toc_[I].toObject();if(Row["path"]==Old){Row["path"]=New;toc_[I]=Row;}}
}
void Package::remove(const QString& Path,bool RemoveImages) {
    auto Item=item(Path);if(Item.isNull() || Path==rootfile_ || tokens(Item.attribute("properties")).contains("nav") || media_.value(Path)=="application/x-dtbncx+xml")Reject("edit-remove-package-resource",Path);
    const auto Id=Item.attribute("id");QList<QDomElement> Spine;for(auto Ref=spine().firstChildElement();!Ref.isNull();Ref=Ref.nextSiblingElement())if(Ref.localName()=="itemref")Spine.append(Ref);
    for(auto Ref:Spine)if(Ref.attribute("idref")==Id){if(Spine.size()<=1)Reject("edit-last-chapter",Path);Ref.parentNode().removeChild(Ref);}
    QJsonArray Rows;bool NavigationChanged=false;
    for(int I=0;I<toc_.size();++I){auto Row=toc_[I].toObject();if(Row["path"]==Path){NavigationChanged=true;if(I+1<toc_.size() && toc_[I+1].toObject()["depth"].toInt()>Row["depth"].toInt()){Row["path"]="";Row["fragment"]="";}else continue;}Rows.append(Row);}
    if(NavigationChanged)toc(Rows);references(Path,{},RemoveImages);
    const auto Metas=opf_.elementsByTagNameNS(Opf,"meta");QList<QDomElement> Cover;for(int I=0;I<Metas.size();++I){const auto Meta=Metas.at(I).toElement();if(Meta.attribute("name")=="cover" && Meta.attribute("content")==Id)Cover.append(Meta);}for(auto Meta:Cover)Meta.parentNode().removeChild(Meta);
    Item.parentNode().removeChild(Item);media_.remove(Path);replacements_.remove(Path);if(additions_.contains(Path))additions_.remove(Path);else removals_.insert(Path);
}
void Package::cover(const QString& Path) {
    if(!media_.value(Path).startsWith("image/"))Reject("edit-cover-image",Path);auto Selected=item(Path);if(Selected.isNull())Reject("edit-cover-image",Path);QString Old;
    for(auto Node=manifest().firstChildElement();!Node.isNull();Node=Node.nextSiblingElement()){auto Properties=tokens(Node.attribute("properties"));if(Properties.removeAll("cover-image")){Old=resolve(rootfile_,Node.attribute("href"));if(Properties.isEmpty())Node.removeAttribute("properties");else Node.setAttribute("properties",Properties.join(' '));}}
    auto Metadata=child(opf_.documentElement(),Opf,"metadata");QList<QDomElement> Metas;for(auto Node=Metadata.firstChildElement();!Node.isNull();Node=Node.nextSiblingElement())if(Node.namespaceURI()==Opf && Node.localName()=="meta" && Node.attribute("name")=="cover")Metas.append(Node);
    for(auto Node:Metas){if(Old.isEmpty()){const auto Id=Node.attribute("content");for(auto Entry=manifest().firstChildElement();!Entry.isNull();Entry=Entry.nextSiblingElement())if(Entry.attribute("id")==Id)Old=resolve(rootfile_,Entry.attribute("href"));}Node.parentNode().removeChild(Node);}
    if(opf_.documentElement().attribute("version")!="2.0"){auto Properties=tokens(Selected.attribute("properties"));Properties.append("cover-image");Selected.setAttribute("properties",Properties.join(' '));}
    auto Meta=element(opf_,Opf,"meta");Meta.setAttribute("name","cover");Meta.setAttribute("content",Selected.attribute("id"));Metadata.appendChild(Meta);
    if(!Old.isEmpty() && Old!=Path)references(Old,Path,false,false);
}
void Package::chapter(const QString& Path,const QString& Title,int After) {
    if(Title.trimmed().isEmpty() || Title.size()>1024)Reject("edit-chapter-title");QDomDocument Document;auto Html=element(Document,Xhtml,"html");const auto Languages=publication_["languages"].toArray();const auto Language=Languages.isEmpty()?QString{}:Languages.first().toString();if(!Language.isEmpty())Html.setAttributeNS("http://www.w3.org/XML/1998/namespace","xml:lang",Language);Document.appendChild(Html);
    auto Head=element(Document,Xhtml,"head");Head.appendChild(element(Document,Xhtml,"title",Title));Html.appendChild(Head);auto Body=element(Document,Xhtml,"body");Body.appendChild(element(Document,Xhtml,"h1",Title));Body.appendChild(element(Document,Xhtml,"p",QStringLiteral("새 문단")));Html.appendChild(Body);add(Path,"application/xhtml+xml",bytes(Document));
    auto Ref=element(opf_,Opf,"itemref");Ref.setAttribute("idref",item(Path).attribute("id"));QList<QDomElement> Spine;for(auto Node=spine().firstChildElement();!Node.isNull();Node=Node.nextSiblingElement())if(Node.localName()=="itemref")Spine.append(Node);
    if(After>=0 && After<Spine.size())spine().insertAfter(Ref,Spine[After]);else spine().appendChild(Ref);
    auto Rows=toc_;Rows.append(QJsonObject{{"label",Title},{"path",Path},{"fragment",""},{"depth",0}});toc(Rows);
}
void Package::move(const QString& Path,int Delta) {
    const auto Id=item(Path).attribute("id");QList<QDomElement> Refs;for(auto Ref=spine().firstChildElement();!Ref.isNull();Ref=Ref.nextSiblingElement())if(Ref.localName()=="itemref")Refs.append(Ref);int Index=-1;for(int I=0;I<Refs.size();++I)if(Refs[I].attribute("idref")==Id)Index=I;
    if(Index<0 || (Delta!=-1 && Delta!=1) || Index+Delta<0 || Index+Delta>=Refs.size())Reject("edit-chapter-move",Path);if(Delta<0)spine().insertBefore(Refs[Index],Refs[Index-1]);else spine().insertAfter(Refs[Index],Refs[Index+1]);
}
}
QString EditModel::Structure(const QJsonObject& Request,const Cancellation& Cancel) {
    try {
        if(Request["operation"]=="metadata" || Request["operation"]=="image-attributes")return modifyProperties(Request,Cancel);
        const auto Current=Export({},Cancel);if(!Current)return Current.error_+": "+Current.location_;const auto Prepared=PublicationSession::Open(Current.bytes_,Cancel)->Index();if(!Prepared)return Prepared.error_+": "+Prepared.location_;
        Package Book(Current.bytes_,Prepared.publication_,Cancel);const auto Action=Request["operation"].toString(),Path=Request["resource"].toString();
        if(Action=="image-add") {
            QString Media;const auto Data=imageData(Request,Media);Book.add(Path,Media,Data);const auto Chapter=Request["chapter"].toString();
            if(!Chapter.isEmpty()) {
                if(Book.media_.value(Chapter)!="application/xhtml+xml")Reject("edit-image-chapter",Chapter);auto Document=xml(Book.read(Chapter),Chapter);auto Body=child(Document.documentElement(),Xhtml,"body");if(Body.isNull())Reject("edit-image-chapter",Chapter);
                auto Figure=element(Document,Xhtml,"div");Figure.setAttribute("id",uniqueId());auto Image=element(Document,Xhtml,"img");Image.setAttribute("src",relative(Chapter,Path));const auto Alt=Request["alt"].toString();if(Alt.size()>2048)Reject("edit-image-alt-limit");Image.setAttribute("alt",Alt);Figure.appendChild(Image);Body.appendChild(Figure);Book.store(Chapter,bytes(Document));
            }
            if(Request["cover"].toBool())Book.cover(Path);
        } else if(Action=="rename")Book.rename(Path,Request["destination"].toString());
        else if(Action=="remove")Book.remove(Path,Request["removeImageReferences"].toBool());
        else if(Action=="cover")Book.cover(Path);
        else if(Action=="chapter-add")Book.chapter(Path,Request["title"].toString(),Request["after"].toInt(-1));
        else if(Action=="chapter-move")Book.move(Path,Request["delta"].toInt());
        else if(Action=="toc")Book.toc(Request["entries"].toArray());
        else if(Action=="duplicate") {
            // A copy stays in its folder so its relative references keep resolving.
            const auto Target=Request["destination"].toString();const auto Media=Book.media_.value(Path);
            if(Path==Book.rootfile_ || Media.isEmpty() || Media.startsWith("image/") || Media=="application/x-dtbncx+xml" || tokens(Book.item(Path).attribute("properties")).contains("nav"))Reject("edit-duplicate-resource",Path);
            if(QFileInfo(Target).path()!=QFileInfo(Path).path())Reject("edit-duplicate-folder",Target);
            Book.add(Target,Media,Book.read(Path));const auto Id=Book.item(Path).attribute("id");
            for(auto Ref=Book.spine().firstChildElement();!Ref.isNull();Ref=Ref.nextSiblingElement())if(Ref.localName()=="itemref" && Ref.attribute("idref")==Id){auto Copy=element(Book.opf_,Opf,"itemref");Copy.setAttribute("idref",Book.item(Target).attribute("id"));Book.spine().insertAfter(Copy,Ref);break;}
        } else if(Action=="resource-add") {
            const auto Media=Request["mediaType"].toString();if(!QStringList{"text/css","font/ttf","font/otf","font/woff","font/woff2"}.contains(Media))Reject("edit-resource-media",Media);
            const auto Encoded=Request["base64"].toString().toLatin1();const auto Decoded=QByteArray::fromBase64Encoding(Encoded,QByteArray::AbortOnBase64DecodingErrors);
            if(!Decoded || Decoded.decoded.isEmpty() || Decoded.decoded.size()>(Media=="text/css"?2*1024*1024:16*1024*1024))Reject("edit-resource-budget",Path);
            if(Media=="text/css"){const auto Text=QString::fromUtf8(Decoded.decoded);if(Text.toUtf8()!=Decoded.decoded)Reject("edit-resource-encoding",Path);cssReferences(Text,[](const QString& Value){return Value;});}
            Book.add(Path,Media,Decoded.decoded);
        }
        else if(Action=="image-convert") {
            if(!QStringList{"image/png","image/jpeg","image/gif","image/webp"}.contains(Book.media_.value(Path)))Reject("edit-image-convert-format",Path);
            const auto Format=Request["format"].toString();if(Format!="png" && Format!="jpeg")Reject("edit-image-convert-format");QBuffer Input;Input.setData(Book.read(Path));Input.open(QIODevice::ReadOnly);QImageReader Reader(&Input);Reader.setAutoTransform(true);const auto Size=Reader.size();
            if(!Size.isValid() || Size.width()>4096 || Size.height()>4096 || qint64(Size.width())*Size.height()>4194304)Reject("edit-image-format-or-size");
            if(Reader.supportsAnimation() && Reader.imageCount()>1 && !Request["flattenAnimation"].toBool())Reject("edit-image-animation-flatten-required");auto Image=Reader.read();if(Image.isNull())Reject("edit-image-decode");
            if(Format=="jpeg" && Image.hasAlphaChannel()){if(!Request["flattenAlpha"].toBool())Reject("edit-image-alpha-flatten-required");QImage Flattened(Image.size(),QImage::Format_RGB32);Flattened.fill(Qt::white);QPainter Paint(&Flattened);Paint.drawImage(0,0,Image);Paint.end();Image=std::move(Flattened);}
            QByteArray Data;QBuffer Output(&Data);Output.open(QIODevice::WriteOnly);const auto Quality=Request["quality"].toInt(92);if(Quality<1 || Quality>100 || !Image.save(&Output,Format.toLatin1().constData(),Quality) || Data.size()>8*1024*1024)Reject("edit-image-encode");
            const auto Target=Request["destination"].toString();if(Target.isEmpty() || QFileInfo(Target).suffix().compare(Format=="jpeg"?"jpg":"png",Qt::CaseInsensitive)!=0 && !(Format=="jpeg" && QFileInfo(Target).suffix().compare("jpeg",Qt::CaseInsensitive)==0))Reject("edit-image-extension");
            Book.store(Path,Data);Book.media_[Path]="image/"+Format;Book.item(Path).setAttribute("media-type","image/"+Format);if(Target!=Path)Book.rename(Path,Target);
        } else Reject("edit-structure-action");
        Check(Cancel);const auto CandidateBytes=Book.finish();EditModel Candidate;if(!Candidate.Load(CandidateBytes,Cancel))return Candidate.Error();const auto Valid=Candidate.Export({},Cancel);if(!Valid)return Valid.error_+": "+Valid.location_;
        auto Before=state();edition_=CandidateBytes;publication_=Candidate.publication_;media_=Candidate.media_;replacements_.clear();record(std::move(Before));return {};
    }catch(const Rejected& Error){return Error.code_+(Error.location_.isEmpty()?QString{}:": "+Error.location_);}catch(const std::bad_alloc&){return "edit-structure-memory-limit";}
}
namespace {
const QString Dc="http://purl.org/dc/elements/1.1/";
QDomDocument propertyXml(const QByteArray& Bytes,const QString& Path) {
    QXmlStreamReader Reader(Bytes);XhtmlEntities Entities;Entities.enabled_=true;Reader.setEntityResolver(&Entities);int Nodes=0,Depth=0;
    while(!Reader.atEnd()){const auto Token=Reader.readNext();if(Token==QXmlStreamReader::StartElement && (++Nodes>32768 || ++Depth>128))Reject("edit-property-xml-budget",Path);if(Token==QXmlStreamReader::EndElement)--Depth;if(Token==QXmlStreamReader::DTD && !PassiveDoctype(Reader))Reject("xml-dtd-or-entity",Path);}
    if(Reader.hasError())Reject("edit-xml-syntax",Path);return xml(Bytes,Path);
}
QString contentHash(const QByteArray& Bytes){return QString::fromLatin1(QCryptographicHash::hash(Bytes,QCryptographicHash::Sha256).toHex());}
QDomElement atPath(QDomDocument& Document,const QJsonArray& Path) {
    auto Node=Document.documentElement();if(Path.size()>128)Reject("edit-image-position");
    for(const auto& Part:Path){if(!Part.isDouble() || Part.toDouble()!=Part.toInt(-1) || Part.toInt(-1)<0)Reject("edit-image-position");int I=0;auto Child=Node.firstChildElement();for(;!Child.isNull() && I<Part.toInt();++I)Child=Child.nextSiblingElement();if(Child.isNull())Reject("edit-image-position");Node=Child;}return Node;
}
QJsonObject captionInfo(const QDomElement& Image) {
    const auto Parent=Image.parentNode().toElement();const bool Figure=Parent.namespaceURI()==Xhtml && (Parent.localName()=="figure" || Parent.localName()=="div" && tokens(Parent.attribute("class")).contains("hc-image-figure"));
    auto Caption=Figure?child(Parent,Xhtml,"figcaption"):QDomElement{};if(Figure && Caption.isNull())for(auto Node=Parent.firstChildElement();!Node.isNull();Node=Node.nextSiblingElement())if(Node.localName()=="p" && tokens(Node.attribute("class")).contains("hc-image-caption")){Caption=Node;break;}
    const bool Allowed=Figure?Parent.elementsByTagNameNS(Xhtml,"img").size()==1:Parent.namespaceURI()==Xhtml && QStringList{"body","div","section","article","li","td","aside"}.contains(Parent.localName());
    return {{"caption",Caption.text()},{"captionEditable",Allowed && Caption.firstChildElement().isNull()}};
}
bool imageUses(const QString& Document,const QDomElement& Node,const QString& Path) {
    bool Uses=false;const auto Check=[&](const QString& Value){Uses|=resolve(Document,Value)==Path;return Value;};
    for(const auto& Name:{"src","href"})Check(Node.attribute(Name));Check(Node.attributeNS("http://www.w3.org/1999/xlink","href"));srcsetReferences(Node.attribute("srcset"),Check);return Uses;
}
}
QJsonObject EditModel::Metadata()const {
    try {const auto Path=publication_["rootfile"].toString();const auto Data=Read(Path);auto Document=propertyXml(Data,Path);const auto Root=child(Document.documentElement(),Opf,"metadata");QJsonObject Values,Counts;
        for(const auto& Name:{"title","creator","language","publisher","description","identifier"}){const auto Elements=Root.elementsByTagNameNS(Dc,Name);Values[Name]=Elements.isEmpty()?QString{}:Elements.at(0).toElement().text();Counts[Name]=Elements.size();}
        return {{"resource",Path},{"sha256",contentHash(Data)},{"values",Values},{"counts",Counts}};
    }catch(const Rejected& Error){return {{"error",Error.code_},{"location",Error.location_}};}catch(const std::bad_alloc&){return {{"error","edit-property-memory-limit"}};}
}
QJsonObject EditModel::InspectImage(const QString& Path,const Cancellation& Cancel)const {
    try {
        if(!media_.value(Path).startsWith("image/"))Reject("edit-image-resource",Path);QJsonArray Rows;
        for(auto I=media_.begin();I!=media_.end();++I){
            Check(Cancel);const auto Resource=I.key(),Media=I.value();
            if(Media=="text/css"){bool Uses=false;cssReferences(QString::fromUtf8(Read(Resource)),[&](const QString& Url){Uses|=resolve(Resource,Url)==Path;return Url;});if(Uses)Rows.append(QJsonObject{{"document",Resource},{"editable",false},{"kind","css"}});continue;}
            if(Media!="application/xhtml+xml" && Media!="image/svg+xml")continue;const auto Data=Read(Resource);auto Document=propertyXml(Data,Resource);const auto Hash=contentHash(Data);QJsonArray Stylesheets;const auto Links=Document.elementsByTagNameNS(Xhtml,"link");for(int J=0;J<Links.size();++J){const auto Link=Links.at(J).toElement();if(tokens(Link.attribute("rel").toLower()).contains("stylesheet")){const auto Css=resolve(Resource,Link.attribute("href"));if(media_.value(Css)=="text/css" && !Stylesheets.contains(Css))Stylesheets.append(Css);}}
            struct Location {QDomElement node_;QJsonArray path_;};QList<Location> Pending{{Document.documentElement(),{}}};
            while(!Pending.isEmpty()) {
                Check(Cancel);auto Location=Pending.takeLast();const auto Node=Location.node_;const bool Direct=imageUses(Resource,Node,Path);bool Css=false;cssReferences(Node.attribute("style"),[&](const QString& Url){Css|=resolve(Resource,Url)==Path;return Url;});
                if(Node.localName()=="style")cssReferences(Node.text(),[&](const QString& Url){Css|=resolve(Resource,Url)==Path;return Url;});
                if(Direct || Css){auto Row=captionInfo(Node);Row["document"]=Resource;Row["elementPath"]=Location.path_;Row["sha256"]=Hash;Row["stylesheets"]=Stylesheets;Row["editable"]=Direct && Node.namespaceURI()==Xhtml && Node.localName()=="img";Row["kind"]=Direct?Node.tagName():"css";Row["alt"]=Node.attribute("alt");Row["hasAlt"]=Node.hasAttribute("alt");Row["width"]=Node.attribute("width");Row["height"]=Node.attribute("height");Row["style"]=Node.attribute("style");Rows.append(Row);if(Rows.size()>4096)Reject("edit-image-occurrence-limit");}
                int Index=0;for(auto Child=Node.firstChildElement();!Child.isNull();Child=Child.nextSiblingElement()){auto Next=Location.path_;Next.append(Index++);if(Next.size()>128)Reject("edit-image-position");Pending.append({Child,Next});}
            }
        }
        QString Width,Height;if(media_.value(Path)=="image/svg+xml"){const auto Svg=propertyXml(Read(Path),Path).documentElement();Width=Svg.attribute("width");Height=Svg.attribute("height");}else{QBuffer Buffer;Buffer.setData(Read(Path));Buffer.open(QIODevice::ReadOnly);QImageReader Reader(&Buffer);const auto Size=Reader.size();if(Size.isValid()){Width=QString::number(Size.width());Height=QString::number(Size.height());}}
        return {{"resource",Path},{"width",Width},{"height",Height},{"occurrences",Rows}};
    }catch(const Rejected& Error){return {{"error",Error.code_},{"location",Error.location_}};}catch(const std::bad_alloc&){return {{"error","edit-property-memory-limit"}};}
}
QString EditModel::modifyProperties(const QJsonObject& Request,const Cancellation& Cancel) {
    const auto Action=Request["operation"].toString();const auto Path=Action=="metadata"?publication_["rootfile"].toString():Request["document"].toString();
    if(!media_.contains(Path))Reject("edit-property-resource",Path);const auto Raw=Read(Path);if(contentHash(Raw)!=Request["expectedSha256"].toString())Reject("edit-property-revision-changed",Path);auto Document=propertyXml(Raw,Path);
    if(Action=="metadata") {
        const auto Values=Request["values"].toObject();auto Metadata=child(Document.documentElement(),Opf,"metadata");if(Metadata.isNull() || Values.isEmpty())Reject("edit-metadata-values");
        for(auto I=Values.begin();I!=Values.end();++I){const auto Key=I.key(),Value=I.value().toString();if(!I.value().isString() || !QStringList{"title","creator","language","publisher","description"}.contains(Key) || Value.size()>(Key=="description"?8192:1024) || Value.contains(QChar::Null))Reject("edit-metadata-value",Key);
            if(Key=="title" && Value.trimmed().isEmpty())Reject("edit-metadata-title");if(Key=="language" && !QRegularExpression("^[A-Za-z]{2,8}(?:-[A-Za-z0-9]{1,8})*$").match(Value).hasMatch())Reject("edit-metadata-language");
            auto Field=child(Metadata,Dc,Key);if(Field.isNull()){Field=Document.createElementNS(Dc,"dc:"+Key);Metadata.appendChild(Field);}while(!Field.firstChild().isNull())Field.removeChild(Field.firstChild());Field.appendChild(Document.createTextNode(Value));
        }
        if(Document.documentElement().attribute("version")!="2.0"){QDomElement Modified;for(auto Node=Metadata.firstChildElement();!Node.isNull();Node=Node.nextSiblingElement())if(Node.attribute("property")=="dcterms:modified"){Modified=Node;break;}if(Modified.isNull()){Modified=element(Document,Opf,"meta");Modified.setAttribute("property","dcterms:modified");Metadata.appendChild(Modified);}while(!Modified.firstChild().isNull())Modified.removeChild(Modified.firstChild());Modified.appendChild(Document.createTextNode(QDateTime::currentDateTimeUtc().toString("yyyy-MM-dd'T'HH:mm:ss'Z'")));}
    }else {
        if(media_.value(Path)!="application/xhtml+xml" || !Request["elementPath"].isArray())Reject("edit-image-position");auto Image=atPath(Document,Request["elementPath"].toArray());if(Image.namespaceURI()!=Xhtml || Image.localName()!="img" || !imageUses(Path,Image,Request["resource"].toString()))Reject("edit-image-position");
        if(Request.contains("alt")){if(!Request["alt"].isString() || Request["alt"].toString().size()>2048)Reject("edit-image-alt-limit");Image.setAttribute("alt",Request["alt"].toString());}
        for(const auto& Key:QStringList{"width","height"})if(Request.contains(Key)) {
            if(!Request[Key].isString())Reject("edit-image-dimension");const auto Value=Request[Key].toString();
            if(Value.isEmpty())Image.removeAttribute(Key);else {if(!QRegularExpression("^[0-9]{1,6}$").match(Value).hasMatch())Reject("edit-image-dimension");Image.setAttribute(Key,Value);}
        }
        if(Request.contains("caption")) {
            if(!Request["caption"].isString() || Request["caption"].toString().size()>4096 || !captionInfo(Image)["captionEditable"].toBool())Reject("edit-image-caption-structure");const auto Text=Request["caption"].toString();auto Figure=Image.parentNode().toElement();
            const bool Epub2=publication_["packageVersion"]=="2.0";const bool Wrapped=Figure.localName()=="figure" || Figure.localName()=="div" && tokens(Figure.attribute("class")).contains("hc-image-figure");if(!Wrapped && !Text.isEmpty()){auto Next=element(Document,Xhtml,Epub2?"div":"figure");if(Epub2)Next.setAttribute("class","hc-image-figure");Figure.replaceChild(Next,Image);Next.appendChild(Image);Figure=Next;}
            auto Caption=child(Figure,Xhtml,"figcaption");if(Caption.isNull())for(auto Node=Figure.firstChildElement();!Node.isNull();Node=Node.nextSiblingElement())if(Node.localName()=="p" && tokens(Node.attribute("class")).contains("hc-image-caption")){Caption=Node;break;}if(!Caption.isNull() && Text.isEmpty())Figure.removeChild(Caption);else if(!Text.isEmpty()){if(Caption.isNull()){Caption=element(Document,Xhtml,Epub2?"p":"figcaption");if(Epub2)Caption.setAttribute("class","hc-image-caption");Figure.appendChild(Caption);}while(!Caption.firstChild().isNull())Caption.removeChild(Caption.firstChild());Caption.appendChild(Document.createTextNode(Text));}
        }
    }
    Check(Cancel);const auto Next=bytes(Document);if(Next==Raw)return {};auto Candidate=*this;const auto Error=Candidate.Replace(Path,Next);if(!Error.isEmpty())return Error;const auto Verified=Candidate.Export({},Cancel);if(!Verified)return Verified.error_+": "+Verified.location_;auto Before=state();replacements_=Candidate.replacements_;record(std::move(Before));return {};
}

}
