#include "stdafx.h"
#include "registryLinkCache.hpp"

/// 모듈
/// UIs
/// 기타 헤더파일
/// 고유 라이브러리
/// 공통 라이브러리
/// 외부 라이브러리
/// 선언 헤더파일들
/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QFile>
#include <QSaveFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QDir>
#include <QFileInfo>

DECLARE_CMNLIBSV2_NAMESPACE
Q_LOGGING_CATEGORY(lcRegistryLinkCache, "hc.registry.linkCache")
namespace hc::registry {
namespace {
const QString FileExts = QStringLiteral("Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts");
QJsonObject LocationJson(const RegistryLocation& Location)
{ return {{"path", Location.Path()}, {"view", int(Location.view)}}; }
std::optional<RegistryLocation> ReadLocation(const QJsonObject& Json, View Bits)
{
    if (Json["view"].toInt(-1) != int(Bits)) return {};
    return RegistryLocation::Parse(Json["path"].toString(), Bits);
}
QJsonArray LocationsJson(const QList<RegistryLocation>& Locations)
{ QJsonArray Result; for (const auto& Location : Locations) Result.append(LocationJson(Location)); return Result; }
}
RegistryLinkCache::RegistryLinkCache(View Bits) : bits_(Bits) { Validate(); }
void RegistryLinkCache::Validate()
{
    fastPending_ = {{Hive::ClassesRoot, {}, bits_}, {Hive::CurrentUser, FileExts, bits_}};
    fullPending_ = fastPending_; fastSeen_.clear(); fullSeen_.clear();
    fastDone_ = fullDone_ = false; reads_ = 0; fastMs_ = fullMs_ = 0;
}
RegistryLinkCache::Node RegistryLinkCache::read(IRegistryBackend& Backend, const RegistryLocation& Location, const std::atomic_bool* Cancel)
{
    const auto Old = nodes_.constFind(Location.Id());
    if (Old != nodes_.cend() && !Old->error) {
        const auto Info = Backend.QueryInfo(Location);
        if (Info && Info.value.lastWrite == Old->written && Info.value.subkeyCount == quint32(Old->children.size())) return *Old;
    }
    Node Result; Result.location = Location;
    const auto Snapshot = Backend.Enumerate(Location, Cancel); ++reads_; Result.error = Snapshot.error;
    if (!Snapshot) return Result;
    Result.written = Snapshot.value.info.lastWrite; Result.link = Snapshot.value.info.link;
    for (const auto& Child : Snapshot.value.keys) Result.children.append(Child.name);
    for (const auto& Value : Snapshot.value.values)
        Result.links.append(RegistryLinkResolver::ValueLinks(Backend, Location, Value, {}, false, false));
    return Result;
}
bool RegistryLinkCache::fastChild(const RegistryLocation& Parent, const QString& Child) const
{
    if (Parent.hive == Hive::CurrentUser) return true;
    const auto Key = Parent.key.toCaseFolded(), Name = Child.toCaseFolded();
    if (Key.isEmpty()) return true; // Discover every registered top-level class.
    if (Key == "applications" || Key.startsWith("applications\\")) return true;
    if (Key.contains("\\shellex") || Name == "shellex" || Name == "background") return true;
    if (Key.startsWith('.') && !Key.contains('\\')) return Name == "openwithprogids";
    if (Key == "clsid") return true;
    if (Key.startsWith("clsid\\") && Key.count('\\') == 1)
        return Name == "progid" || Name == "versionindependentprogid" || Name == "treatas"
            || Name == "autoconvertto" || Name == "inprocserver32" || Name == "localserver32";
    if (!Key.contains('\\')) return Name == "clsid" || Name == "curver";
    return false;
}
bool RegistryLinkCache::Advance(IRegistryBackend& Backend, const std::atomic_bool* Cancel, qsizetype Chunk,
    int Milliseconds, std::atomic<qsizetype>* Progress)
{
    if (fullDone_) return true;
    QElapsedTimer Slice; Slice.start(); qsizetype Count = 0; const bool Fast = !fastDone_;
    auto& Pending = Fast ? fastPending_ : fullPending_; auto& Seen = Fast ? fastSeen_ : fullSeen_;
    while (!Pending.isEmpty() && Count < Chunk && (Count == 0 || Slice.elapsed() < Milliseconds)) {
        if (Cancel && Cancel->load()) break;
        const auto Location = Pending.takeLast();
        if (Seen.contains(Location.Id())) continue;
        auto Node = read(Backend, Location, Cancel);
        if (Node.error == ERROR_CANCELLED) { Pending.append(Location); break; }
        Seen.insert(Location.Id()); ++Count;
        if (Node.error != ERROR_FILE_NOT_FOUND && Node.error != ERROR_PATH_NOT_FOUND) nodes_.insert(Location.Id(), Node);
        else nodes_.remove(Location.Id());
        if (!Node.error && !Node.link)
            for (const auto& Child : Node.children) if (!Fast || fastChild(Location, Child)) Pending.append(Location.Child(Child));
        if (Progress) Progress->store(nodes_.size());
    }
    if (Fast) { fastMs_ += Slice.elapsed(); fastDone_ = Pending.isEmpty(); }
    else {
        fullMs_ += Slice.elapsed(); fullDone_ = Pending.isEmpty();
        if (fullDone_) for (auto It = nodes_.begin(); It != nodes_.end();) {
            if (!fullSeen_.contains(It.key())) It = nodes_.erase(It); else ++It;
        }
    }
    return fullDone_;
}
RegistryLinkIndex RegistryLinkCache::Snapshot() const
{
    RegistryLinkIndex Result; Result.keys = nodes_.size(); Result.complete = fullDone_;
    for (const auto& Node : nodes_) {
        if (Node.error) {
            Result.complete = false;
            if (Result.diagnostics.size() < 100) Result.diagnostics.append(Node.location.Path() + QString(" (%1)").arg(Node.error));
        }
        for (const auto& Link : Node.links) if (Link.target) Result.incoming[Link.target->Id()].append(Link);
    }
    return Result;
}
bool RegistryLinkCache::Save(const QString& Path, const QString& Sid) const
{
    QJsonArray Nodes;
    for (const auto& Node : nodes_) {
        QJsonArray Links;
        for (const auto& Link : Node.links) if (Link.target)
            Links.append(QJsonObject{{"target", LocationJson(*Link.target)}, {"name", Link.sourceName}, {"label", Link.label}});
        Nodes.append(QJsonObject{{"location", LocationJson(Node.location)}, {"written", QString::number(Node.written)},
            {"children", QJsonArray::fromStringList(Node.children)}, {"links", Links}, {"error", int(Node.error)}, {"link", Node.link}});
    }
    QJsonObject Object{{"version", 1}, {"sid", Sid}, {"view", int(bits_)}, {"nodes", Nodes},
        {"fastPending", LocationsJson(fastPending_)}, {"fullPending", LocationsJson(fullPending_)},
        {"fastSeen", QJsonArray::fromStringList(fastSeen_.values())}, {"fullSeen", QJsonArray::fromStringList(fullSeen_.values())},
        {"fastDone", fastDone_}, {"fullDone", fullDone_}, {"fastMs", double(fastMs_)}, {"fullMs", double(fullMs_)}};
    QDir().mkpath(QFileInfo(Path).absolutePath());
    QSaveFile File(Path); if (!File.open(QIODevice::WriteOnly)) return false;
    const auto Bytes = QJsonDocument(Object).toJson(QJsonDocument::Compact);
    return File.write(Bytes) == Bytes.size() && File.commit();
}
bool RegistryLinkCache::Load(const QString& Path, const QString& Sid)
{
    QFile File(Path);
    if (!File.open(QIODevice::ReadOnly) || File.size() > 256 * 1024 * 1024) return false;
    QJsonParseError Error; const auto Document = QJsonDocument::fromJson(File.readAll(), &Error);
    if (Error.error != QJsonParseError::NoError || !Document.isObject()) return false;
    const auto Object = Document.object();
    if (Object["version"].toInt() != 1 || Object["sid"].toString() != Sid || Object["view"].toInt(-1) != int(bits_)) return false;
    RegistryLinkCache Loaded(bits_);
    for (const auto& Item : Object["nodes"].toArray()) {
        const auto Json = Item.toObject(); const auto Location = ReadLocation(Json["location"].toObject(), bits_);
        if (!Location) return false;
        Node Node; Node.location = *Location; Node.written = Json["written"].toString().toULongLong();
        Node.error = quint32(Json["error"].toInt()); Node.link = Json["link"].toBool();
        for (const auto& Child : Json["children"].toArray()) {
            const auto Name = Child.toString(); if (!WinRegistryBackend::ValidName(Name, true)) return false; Node.children.append(Name);
        }
        for (const auto& Entry : Json["links"].toArray()) {
            const auto LinkJson = Entry.toObject(); const auto Target = ReadLocation(LinkJson["target"].toObject(), bits_); if (!Target) return false;
            RegistryLink Link; Link.source = *Location; Link.sourceName = LinkJson["name"].toString(); Link.label = LinkJson["label"].toString(); Link.target = *Target; Node.links.append(Link);
        }
        Loaded.nodes_.insert(Location->Id(), std::move(Node));
    }
    const auto Queue = [&](const QString& Name, QList<RegistryLocation>& Result) {
        Result.clear(); for (const auto& Value : Object[Name].toArray()) {
            const auto Location = ReadLocation(Value.toObject(), bits_); if (!Location) return false; Result.append(*Location);
        }
        return true;
    };
    if (!Queue("fastPending", Loaded.fastPending_) || !Queue("fullPending", Loaded.fullPending_)) return false;
    for (const auto& Id : Object["fastSeen"].toArray()) Loaded.fastSeen_.insert(Id.toString());
    for (const auto& Id : Object["fullSeen"].toArray()) Loaded.fullSeen_.insert(Id.toString());
    Loaded.fastDone_ = Object["fastDone"].toBool(); Loaded.fullDone_ = Object["fullDone"].toBool();
    Loaded.fastMs_ = qint64(Object["fastMs"].toDouble()); Loaded.fullMs_ = qint64(Object["fullMs"].toDouble());
    if (!Loaded.fullDone_) {
        // Keep the unfinished queue, but validate cached nodes changed while no
        // watcher was alive. Timestamp hits reuse links without reading values.
        Loaded.fullSeen_.clear();
        for (const auto& Node : Loaded.nodes_) Loaded.fullPending_.append(Node.location);
        Loaded.fastPending_ = {{Hive::ClassesRoot, {}, bits_}, {Hive::CurrentUser, FileExts, bits_}};
        Loaded.fastSeen_.clear(); Loaded.fastDone_ = false;
    }
    *this = std::move(Loaded); return true;
}
} // namespace hc::registry