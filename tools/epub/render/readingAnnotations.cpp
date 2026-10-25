#include "stdafx.h"
#include "readingAnnotations.hpp"
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
namespace hc::epub::poc {
namespace {
constexpr qint64 MaxBytes=256*1024;
bool text(const QJsonValue& Value,int Limit,bool Empty=true) {
    if(!Value.isString() || Value.toString().size()>Limit || (!Empty && Value.toString().trimmed().isEmpty()))return false;
    for(const auto Character:Value.toString())if(Character.category()==QChar::Other_Control && Character!='\n' && Character!='\t')return false;return true;
}
bool identifier(const QString& Value,int Length) {return Value.size()==Length && QRegularExpression("^[a-f0-9]+$").match(Value).hasMatch();}
bool locator(const QJsonValue& Value) {
    return Value.isString() && Value.toString().size()<=2048 && QRegularExpression("^epubcfi\\([^\\x{0}-\\x{1f}\\x{7f}-\\x{9f}\\x{2028}\\x{2029}]+\\)$").match(Value.toString()).hasMatch();
}
}
bool ReadingAnnotations::Valid(const QJsonObject& Annotation) {
    static const QSet<QString> Keys{"id","locator","quote","prefix","suffix","note","color","unresolved","reason","sourceBook","sourceLocator"};
    for(auto I=Annotation.begin();I!=Annotation.end();++I)if(!Keys.contains(I.key()))return false;
    if(!identifier(Annotation["id"].toString(),32) || !text(Annotation["quote"],512,false) || !text(Annotation["prefix"],128) || !text(Annotation["suffix"],128) || !text(Annotation["note"],2048)
        || !QRegularExpression("^#[a-fA-F0-9]{6}$").match(Annotation["color"].toString()).hasMatch())return false;
    if(Annotation.contains("unresolved") && !Annotation["unresolved"].isBool())return false;
    if(Annotation["unresolved"].toBool()) {if(Annotation["locator"]!=QJsonValue(""))return false;}
    else if(!locator(Annotation["locator"]))return false;
    if(Annotation.contains("reason") && !text(Annotation["reason"],256))return false;
    if(Annotation.contains("sourceBook") && !identifier(Annotation["sourceBook"].toString(),64))return false;
    if(Annotation.contains("sourceLocator") && !locator(Annotation["sourceLocator"]))return false;
    return true;
}
bool ReadingAnnotations::Valid(const QJsonArray& Annotations) {
    if(Annotations.size()>32)return false;QSet<QString> Ids;
    for(const auto& Value:Annotations){if(!Value.isObject() || !Valid(Value.toObject()))return false;const auto Id=Value.toObject()["id"].toString();if(Ids.contains(Id))return false;Ids.insert(Id);}return true;
}
QString ReadingAnnotations::Export(const QString& Destination,const QString& Book,const QString& Title,const QJsonArray& Annotations) {
    if(!QFileInfo(Destination).isAbsolute() || QFileInfo(Destination).suffix().compare("json",Qt::CaseInsensitive)!=0)return QStringLiteral("형광펜·메모는 별도의 JSON 파일로 내보내세요.");
    if(!identifier(Book,64) || Title.size()>512 || !Valid(Annotations))return QStringLiteral("내보낼 메모 또는 책 식별자가 올바르지 않습니다.");
    if(QFileInfo(Destination).isSymLink())return QStringLiteral("연결된 파일에는 메모를 내보내지 않습니다.");
    const auto Bytes=QJsonDocument(QJsonObject{{"schemaVersion",1},{"kind","hypercommander.epub.annotations"},{"book",Book},{"title",Title},{"annotations",Annotations}}).toJson();
    if(Bytes.size()>MaxBytes)return QStringLiteral("메모 내보내기 크기 제한을 초과했습니다.");QSaveFile File(Destination);File.setDirectWriteFallback(false);
    if(!File.open(QIODevice::WriteOnly) || File.write(Bytes)!=Bytes.size() || !File.commit())return QStringLiteral("메모를 내보내지 못했습니다. 기존 파일은 유지됩니다.");return {};
}
ReadingAnnotationTransfer ReadingAnnotations::Import(const QString& Input) {
    QFile File(Input);if(!File.open(QIODevice::ReadOnly) || File.size()>MaxBytes)return {{},{},{},QStringLiteral("메모 파일을 읽을 수 없거나 크기 제한을 초과했습니다.")};
    const auto Bytes=File.read(MaxBytes+1);QJsonParseError Error;const auto Doc=QJsonDocument::fromJson(Bytes,&Error);const auto Root=Doc.object();
    if(Bytes.size()>MaxBytes || Error.error!=QJsonParseError::NoError || !Doc.isObject() || Root["schemaVersion"]!=QJsonValue(1) || Root["kind"]!="hypercommander.epub.annotations"
        || !identifier(Root["book"].toString(),64) || !text(Root["title"],512) || !Root["annotations"].isArray() || !Valid(Root["annotations"].toArray()))return {{},{},{},QStringLiteral("메모 파일의 내용 또는 버전을 지원하지 않습니다. 기존 메모는 유지됩니다.")};
    return {Root["annotations"].toArray(),Root["book"].toString(),Root["title"].toString(),{}};
}
}
