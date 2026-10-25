#include "stdafx.h"
#include "epubDraftStore.hpp"
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QRegularExpression>
#include <QSaveFile>
#include <QtEndian>
#include <algorithm>
#include <climits>
#include <utility>
namespace hc::document::epub {
namespace {
constexpr qint64 OriginalLimit=64LL*1024*1024,OverlayLimit=32LL*1024*1024,FileLimit=100LL*1024*1024;
const QByteArray Magic("HCEDRF01",8);
bool text(const QString& Value,int Limit=32768){return Value.size()<=Limit && !Value.contains(QChar::Null) && QString::fromUtf8(Value.toUtf8())==Value;}
bool path(const QString& Root,const QString& Path) {
    const QFileInfo Dir(Root),File(Path);
    return Dir.isAbsolute() && !Dir.isSymLink() && Dir.isDir() && !File.isSymLink()
        && QDir::cleanPath(File.absolutePath()).compare(QDir::cleanPath(Dir.absoluteFilePath()),Qt::CaseInsensitive)==0
        && QRegularExpression("^[a-f0-9]{32}\\.hcedraft$").match(File.fileName()).hasMatch();
}
qint64 size(const QJsonValue& Value,qint64 Limit) {
    const auto Number=Value.toInteger(-1);
    return Value.isDouble() && Value.toDouble()!=double(Number)?-1:(Number>=0 && Number<=Limit?Number:-1);
}
QJsonObject header(QFile& File,QByteArray& Prefix,QString& Error) {
    Prefix=File.read(12);
    if(File.size()>FileLimit || Prefix.size()!=12 || Prefix.first(8)!=Magic){Error="epub-draft-header";return {};}
    const auto Length=qFromBigEndian<quint32>(Prefix.constData()+8);
    if(!Length || Length>64*1024){Error="epub-draft-header-budget";return {};}
    const auto Json=File.read(Length);Prefix+=Json;QJsonParseError Parse;
    const auto Document=QJsonDocument::fromJson(Json,&Parse);const auto Object=Document.object();
    if(Json.size()!=Length || Parse.error!=QJsonParseError::NoError || !Document.isObject() || Object["schemaVersion"]!=QJsonValue(1)
        || !Object["sourcePath"].isString() || !text(Object["sourcePath"].toString()) || !QFileInfo(Object["sourcePath"].toString()).isAbsolute()
        || QFileInfo(Object["sourcePath"].toString()).suffix().compare("epub",Qt::CaseInsensitive)!=0
        || !QRegularExpression("^[a-f0-9]{64}$").match(Object["sourceSha256"].toString()).hasMatch()
        || size(Object["originalBytes"],OriginalLimit)<=0 || !Object["entries"].isArray() || Object["entries"].toArray().size()>1024
        || !Object["hasCurrentDraft"].isBool() || !Object["currentResource"].isString() || !text(Object["currentResource"].toString(),2048)
        || size(Object["currentBytes"],2*1024*1024)<0 || (!Object["hasCurrentDraft"].toBool() && Object["currentBytes"].toInteger()!=0)
        || !Object["locator"].isString() || !text(Object["locator"].toString(),8192)
        || size(Object["cursor"],2*1024*1024)<0 || size(Object["scroll"],INT_MAX)<0 || Object["utcMs"].toInteger()<=0) {
        Error="epub-draft-metadata";return {};
    }
    return Object;
}
}
DraftWrite DraftStore::Write(const QString& Root,const QString& Session,const DraftSnapshot& Snapshot,const std::function<bool()>& Cancelled) {
    const auto Target=QDir(Root).filePath(Session+".hcedraft");
    if(!QDir().mkpath(Root) || !path(Root,Target) || Snapshot.original_.isEmpty() || Snapshot.original_.size()>OriginalLimit
        || Snapshot.changes_.size()>1024 || !text(Snapshot.sourcePath_) || !QFileInfo(Snapshot.sourcePath_).isAbsolute()
        || QFileInfo(Snapshot.sourcePath_).suffix().compare("epub",Qt::CaseInsensitive)!=0 || !text(Snapshot.currentResource_,2048)
        || !text(Snapshot.locator_,8192) || Snapshot.currentDraft_.size()>2*1024*1024 || (!Snapshot.hasCurrentDraft_ && !Snapshot.currentDraft_.isEmpty()) || Snapshot.cursor_<0 || Snapshot.cursor_>2*1024*1024 || Snapshot.scroll_<0)
        return {{},"epub-draft-admission"};
    QJsonArray Entries;qint64 Overlay=0;
    for(auto I=Snapshot.changes_.begin();I!=Snapshot.changes_.end();++I) {
        if(!text(I.key(),2048) || I.key().isEmpty() || I.value().size()>8*1024*1024 || (Overlay+=I.value().size())>OverlayLimit)return {{},"epub-draft-overlay-budget"};
        Entries.append(QJsonObject{{"path",I.key()},{"bytes",I.value().size()}});
    }
    const auto Json=QJsonDocument(QJsonObject{{"schemaVersion",1},{"sourcePath",Snapshot.sourcePath_},{"sourceSha256",QString::fromLatin1(QCryptographicHash::hash(Snapshot.original_,QCryptographicHash::Sha256).toHex())},
        {"originalBytes",Snapshot.original_.size()},{"entries",Entries},{"currentResource",Snapshot.currentResource_},{"hasCurrentDraft",Snapshot.hasCurrentDraft_},{"currentBytes",Snapshot.currentDraft_.size()},
        {"cursor",Snapshot.cursor_},{"scroll",Snapshot.scroll_},{"locator",Snapshot.locator_},{"utcMs",QDateTime::currentMSecsSinceEpoch()}}).toJson(QJsonDocument::Compact);
    if(Json.size()>64*1024)return {{},"epub-draft-header-budget"};
    QLockFile Lock(Target+".lock");if(!Lock.tryLock(0))return {{},"epub-draft-locked"};
    QSaveFile File(Target);File.setDirectWriteFallback(false);if(!File.open(QIODevice::WriteOnly))return {{},"epub-draft-write"};
    QCryptographicHash Hash(QCryptographicHash::Sha256);
    const auto Write=[&](const QByteArray& Bytes){if(Cancelled && Cancelled())return false;Hash.addData(Bytes);return File.write(Bytes)==Bytes.size();};
    QByteArray Prefix=Magic;const auto Length=qToBigEndian(quint32(Json.size()));Prefix.append(reinterpret_cast<const char*>(&Length),4);
    bool Ok=Write(Prefix) && Write(Json) && Write(Snapshot.original_);
    for(auto I=Snapshot.changes_.begin();Ok && I!=Snapshot.changes_.end();++I)Ok=Write(I.value());
    Ok=Ok && Write(Snapshot.currentDraft_);
    if(!Ok || (Cancelled && Cancelled())){File.cancelWriting();return {{},Cancelled && Cancelled()?"epub-draft-cancelled":"epub-draft-write"};}
    const auto Digest=Hash.result();if(File.write(Digest)!=Digest.size() || !File.commit())return {{},"epub-draft-write"};
    return {Target,{}};
}
DraftRead DraftStore::Read(const QString& Root,const QString& Path) {
    if(!path(Root,Path))return {{},"epub-draft-path"};
    QFile File(Path);if(!File.open(QIODevice::ReadOnly))return {{},"epub-draft-open"};
    QByteArray Prefix;QString Error;const auto Header=header(File,Prefix,Error);if(!Error.isEmpty())return {{},Error};
    QCryptographicHash Hash(QCryptographicHash::Sha256);Hash.addData(Prefix);DraftSnapshot Snapshot;
    const auto Read=[&](qint64 Length){const auto Bytes=File.read(Length);Hash.addData(Bytes);return Bytes;};
    const auto OriginalBytes=size(Header["originalBytes"],OriginalLimit);Snapshot.original_=Read(OriginalBytes);
    if(Snapshot.original_.size()!=OriginalBytes || QCryptographicHash::hash(Snapshot.original_,QCryptographicHash::Sha256).toHex()!=Header["sourceSha256"].toString().toLatin1())return {{},"epub-draft-original-hash"};
    qint64 Total=0;
    for(const auto& Value:Header["entries"].toArray()) {
        const auto Entry=Value.toObject();const auto Name=Entry["path"].toString();const auto Bytes=size(Entry["bytes"],8*1024*1024);
        if(!Value.isObject() || !Entry["path"].isString() || Name.isEmpty() || !text(Name,2048) || Snapshot.changes_.contains(Name) || Bytes<0 || (Total+=Bytes)>OverlayLimit)return {{},"epub-draft-entry"};
        auto Data=Read(Bytes);if(Data.size()!=Bytes)return {{},"epub-draft-truncated"};Snapshot.changes_[Name]=std::move(Data);
    }
    const auto CurrentBytes=size(Header["currentBytes"],2*1024*1024);Snapshot.currentDraft_=Read(CurrentBytes);
    if(Snapshot.currentDraft_.size()!=CurrentBytes)return {{},"epub-draft-truncated"};
    const auto Digest=Hash.result();if(File.read(32)!=Digest || !File.atEnd())return {{},"epub-draft-checksum"};
    Snapshot.sourcePath_=Header["sourcePath"].toString();Snapshot.currentResource_=Header["currentResource"].toString();Snapshot.locator_=Header["locator"].toString();
    Snapshot.hasCurrentDraft_=Header["hasCurrentDraft"].toBool();Snapshot.cursor_=Header["cursor"].toInt();Snapshot.scroll_=Header["scroll"].toInt();
    if(Snapshot.hasCurrentDraft_ && Snapshot.currentResource_.isEmpty())return {{},"epub-draft-selection"};
    return {std::move(Snapshot),{}};
}
QList<DraftInfo> DraftStore::List(const QString& Root,const QString& SourcePath,const QByteArray& SourceHash) {
    QList<DraftInfo> Result;if(!QFileInfo(Root).isDir() || QFileInfo(Root).isSymLink())return Result;
    const auto Names=QDir(Root).entryList({"*.hcedraft"},QDir::Files,QDir::Time);
    int Scanned=0;
    for(const auto& Name:Names) {
        if(Result.size()>=128 || ++Scanned>2048)break;const auto Path=QDir(Root).filePath(Name);if(!path(Root,Path))continue;
        QFile File(Path);if(!File.open(QIODevice::ReadOnly))continue;QByteArray Prefix;QString Error;const auto Header=header(File,Prefix,Error);
        const auto StoredPath=Header["sourcePath"].toString();const auto Hash=Header["sourceSha256"].toString().toLatin1();
        if(!Error.isEmpty() || SourcePath.isEmpty() && SourceHash.isEmpty() || Hash==SourceHash || QDir::cleanPath(StoredPath).compare(QDir::cleanPath(SourcePath),Qt::CaseInsensitive)==0)
            Result.append({Path,StoredPath,Error,Hash,Header["utcMs"].toInteger()});
    }
    return Result;
}
bool DraftStore::Remove(const QString& Root,const QString& Path) {
    if(!path(Root,Path))return false;QLockFile Lock(Path+".lock");return Lock.tryLock(0) && (!QFileInfo::exists(Path) || QFile::remove(Path));
}
}
