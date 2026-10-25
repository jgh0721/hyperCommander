#include "stdafx.h"
#include "readingProfiles.hpp"
#include "readingSettings.hpp"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLockFile>
#include <QSaveFile>
#include <QSet>
namespace hc::epub::poc {
namespace {
constexpr qint64 LibraryLimit=2*1024*1024,TransferLimit=12*1024*1024;
QString libraryPath(const QString& Settings) {
    if(Settings.isEmpty())throw QStringLiteral("설정 경로가 없어 프로필을 보관할 수 없습니다.");
    return QDir(QFileInfo(Settings).absolutePath()).filePath("epub-profiles.json");
}
QString name(const QString& Value) {
    const auto Name=Value.trimmed();if(Name.isEmpty() || Name.size()>80)throw QStringLiteral("프로필 이름은 1~80자로 입력하세요.");
    for(const auto Character:Name)if(Character.category()==QChar::Other_Control)throw QStringLiteral("프로필 이름에 제어 문자를 사용할 수 없습니다.");
    return Name;
}
QJsonObject style(QJsonObject Value,bool Transfer=false) {
    if(!ReadingStore::Valid(Value))throw QStringLiteral("프로필의 설정 값 또는 버전이 올바르지 않습니다.");
    for(const auto* Key:{"resume","privateReading","historyEpoch"}) {
        if(Transfer && Value.contains(Key))throw QStringLiteral("서식 프로필에는 독서 기록·개인정보 설정을 넣을 수 없습니다.");
        Value.remove(Key);
    }
    return Value;
}
QByteArray readFile(const QString& Path,qint64 Limit) {
    QFile File(Path);if(!File.open(QIODevice::ReadOnly) || File.size()>Limit)throw QStringLiteral("프로필 파일을 읽을 수 없거나 크기 제한을 초과했습니다.");
    const auto Bytes=File.read(Limit+1);if(Bytes.size()>Limit)throw QStringLiteral("프로필 파일 크기 제한을 초과했습니다.");return Bytes;
}
QJsonObject object(const QByteArray& Bytes) {
    QJsonParseError Error;const auto Doc=QJsonDocument::fromJson(Bytes,&Error);
    if(Error.error!=QJsonParseError::NoError || !Doc.isObject())throw QStringLiteral("프로필 JSON이 손상되었습니다. 기존 프로필은 유지됩니다.");return Doc.object();
}
QJsonArray readLibrary(const QString& Path) {
    if(QFileInfo(Path).isSymLink())throw QStringLiteral("연결된 프로필 보관 경로에는 저장하지 않습니다.");
    if(!QFileInfo::exists(Path))return {};
    const auto Root=object(readFile(Path,LibraryLimit));
    if(Root["schemaVersion"]!=QJsonValue(1) || !Root["profiles"].isArray() || Root["profiles"].toArray().size()>32)throw QStringLiteral("프로필 보관 파일의 버전 또는 개수 제한이 올바르지 않습니다.");
    QSet<QString> Names;const auto Profiles=Root["profiles"].toArray();
    for(const auto& Item:Profiles){if(!Item.isObject())throw QStringLiteral("프로필 항목이 올바르지 않습니다.");const auto Row=Item.toObject();const auto Name=name(Row["name"].toString());
        if(Names.contains(Name) || !Row["profile"].isObject())throw QStringLiteral("프로필 이름이 중복되거나 설정이 없습니다.");Names.insert(Name);style(Row["profile"].toObject(),true);}
    return Profiles;
}
void writeFile(const QString& Path,const QByteArray& Bytes,qint64 Limit) {
    if(Bytes.size()>Limit || QFileInfo(Path).isSymLink() || QFileInfo(QFileInfo(Path).absolutePath()).isSymLink())throw QStringLiteral("프로필 저장 경로 또는 크기 제한을 확인하세요.");
    QSaveFile File(Path);File.setDirectWriteFallback(false);
    if(!File.open(QIODevice::WriteOnly) || File.write(Bytes)!=Bytes.size() || !File.commit())throw QStringLiteral("프로필을 저장하지 못했습니다. 기존 파일은 유지됩니다.");
}
QByteArray asset(const QString& Settings,const QJsonObject& Profile) {
    const auto Id=Profile["backgroundAsset"].toString();if(Id.isEmpty())return {};
    QString Error;const auto Bytes=ReadingAssets::Load(Settings,Id,Error);if(!Error.isEmpty())throw Error;return Bytes;
}
}
ReadingProfileResult ReadingProfiles::List(const QString& Settings) {
    try {return {readLibrary(libraryPath(Settings)),{},{},{}};}catch(const QString& Error){return {{},{},{},Error};}
}
ReadingProfileResult ReadingProfiles::Save(const QString& Settings,const QString& Name,const QJsonObject& Profile,bool Overwrite) {
    try {
        const auto Label=name(Name),Path=libraryPath(Settings);const auto Value=style(Profile);asset(Settings,Value);
        if(!QDir().mkpath(QFileInfo(Path).absolutePath()))throw QStringLiteral("프로필 보관 폴더를 만들 수 없습니다.");
        QLockFile Lock(Path+".lock");if(!Lock.tryLock(0))throw QStringLiteral("다른 창에서 프로필을 저장 중입니다. 잠시 후 다시 시도하세요.");
        auto Profiles=readLibrary(Path);int Index=-1;for(int I=0;I<Profiles.size();++I)if(Profiles[I].toObject()["name"]==Label)Index=I;
        if(Index>=0 && !Overwrite)throw QStringLiteral("같은 이름의 프로필이 있습니다. 덮어쓰기를 명시적으로 선택하세요.");
        const QJsonObject Row{{"name",Label},{"profile",Value}};if(Index>=0)Profiles[Index]=Row;else {if(Profiles.size()>=32)throw QStringLiteral("프로필은 32개까지 보관합니다.");Profiles.append(Row);}
        writeFile(Path,QJsonDocument(QJsonObject{{"schemaVersion",1},{"profiles",Profiles}}).toJson(),LibraryLimit);return {Profiles,Value,Label,{}};
    }catch(const QString& Error){return {{},{},{},Error};}
}
ReadingProfileResult ReadingProfiles::Remove(const QString& Settings,const QString& Name) {
    try {
        const auto Label=name(Name),Path=libraryPath(Settings);QLockFile Lock(Path+".lock");if(!Lock.tryLock(0))throw QStringLiteral("다른 창에서 프로필을 저장 중입니다.");
        auto Profiles=readLibrary(Path);bool Found=false;for(qsizetype I=Profiles.size();I>0;--I)if(Profiles[I-1].toObject()["name"]==Label){Profiles.removeAt(I-1);Found=true;}
        if(!Found)throw QStringLiteral("선택한 프로필을 찾을 수 없습니다.");writeFile(Path,QJsonDocument(QJsonObject{{"schemaVersion",1},{"profiles",Profiles}}).toJson(),LibraryLimit);return {Profiles,{},{},{}};
    }catch(const QString& Error){return {{},{},{},Error};}
}
ReadingProfileResult ReadingProfiles::Export(const QString& Settings,const QString& Destination,const QString& Name,const QJsonObject& Profile) {
    try {
        const QFileInfo Target(Destination);const auto Absolute=QDir::cleanPath(Target.absoluteFilePath());
        if(!Target.isAbsolute() || Target.suffix().compare("json",Qt::CaseInsensitive)!=0 || Absolute.compare(QDir::cleanPath(QFileInfo(Settings).absoluteFilePath()),Qt::CaseInsensitive)==0 || Absolute.compare(QDir::cleanPath(libraryPath(Settings)),Qt::CaseInsensitive)==0)throw QStringLiteral("프로필은 설정 파일과 별도의 JSON 파일로 내보내세요.");
        const auto Label=name(Name);const auto Value=style(Profile);const auto Background=asset(Settings,Value);
        writeFile(Destination,QJsonDocument(QJsonObject{{"schemaVersion",1},{"kind","hypercommander.epub.profile"},{"name",Label},{"profile",Value},{"backgroundPng",QString::fromLatin1(Background.toBase64())}}).toJson(),TransferLimit);return {{},Value,Label,{}};
    }catch(const QString& Error){return {{},{},{},Error};}
}
ReadingProfileResult ReadingProfiles::Import(const QString& Settings,const QString& Input,bool Overwrite) {
    try {
        const auto Root=object(readFile(Input,TransferLimit));
        if(Root["schemaVersion"]!=QJsonValue(1) || Root["kind"]!="hypercommander.epub.profile" || !Root["profile"].isObject() || !Root["backgroundPng"].isString())throw QStringLiteral("지원하지 않는 EPUB 프로필 파일입니다.");
        const auto Label=name(Root["name"].toString());const auto Value=style(Root["profile"].toObject(),true);const auto Id=Value["backgroundAsset"].toString(),Encoded=Root["backgroundPng"].toString();
        const auto Existing=List(Settings);if(!Existing)throw Existing.error;bool Found=false;for(const auto& Row:Existing.profiles)Found|=Row.toObject()["name"]==Label;
        if(Found && !Overwrite)throw QStringLiteral("같은 이름의 프로필이 있습니다. 덮어쓰기를 명시적으로 선택하세요.");
        if(!Found && Existing.profiles.size()>=32)throw QStringLiteral("프로필은 32개까지 보관합니다.");
        if(Encoded.size()>11*1024*1024)throw QStringLiteral("프로필 배경 크기 제한을 초과했습니다.");
        const auto Decoded=QByteArray::fromBase64Encoding(Encoded.toLatin1(),QByteArray::AbortOnBase64DecodingErrors);
        if(!Decoded || (Id.isEmpty()!=Decoded.decoded.isEmpty()))throw QStringLiteral("프로필 배경 데이터가 올바르지 않습니다.");
        if(!Id.isEmpty()) {const auto Imported=ReadingAssets::Install(Decoded.decoded,Settings,Id);if(!Imported.error.isEmpty())throw Imported.error;}
        return Save(Settings,Label,Value,Overwrite);
    }catch(const QString& Error){return {{},{},{},Error};}
}
}
