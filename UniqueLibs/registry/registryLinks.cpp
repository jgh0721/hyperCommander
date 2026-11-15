#include "stdafx.h"
#include "registryLinks.hpp"
#include "registryLinkCache.hpp"

/// 모듈
/// UIs
/// 기타 헤더파일
/// 고유 라이브러리
/// 공통 라이브러리
#include "commonLibs/system/registryValue.hpp"
#include "commonLibs/system/registryEnvironment.hpp"
#include "commonLibs/system/registryWatch.hpp"
/// 외부 라이브러리
/// 선언 헤더파일들
/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QFutureWatcher>
#include <QtConcurrent>
#include <QRegularExpression>
#include <QPointer>
#include <QTimer>
#include <QSet>
#include <QDir>
#include <QFileInfo>
#include <QCryptographicHash>
#include <QLockFile>
#include <QScopeGuard>
#include "commonLibs/system/processInfo.hpp"
#include <Shlwapi.h>

DECLARE_CMNLIBSV2_NAMESPACE
Q_LOGGING_CATEGORY(lcRegistryLinks, "hc.registry.links")
namespace hc::registry {
namespace {
const QString FileExts = QStringLiteral("Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts");
const QRegularExpression Guid(QStringLiteral("\\{[0-9a-fA-F]{8}-(?:[0-9a-fA-F]{4}-){3}[0-9a-fA-F]{12}\\}"));
QString Text(const RegistryResult<RegistryValue>& Value)
{
    if (!Value || (Value.value.type != REG_SZ && Value.value.type != REG_EXPAND_SZ)) return {};
    const auto Decoded = RegistryValueCodec::DecodeText(Value.value); return Decoded.lossless ? Decoded.text : QString();
}
RegistryLocation Classes(const QString& Key, View Bits) { return {Hive::ClassesRoot, Key, Bits}; }
RegistryLink KeyLink(IRegistryBackend& Backend, const RegistryLocation& Source, const QString& Name,
    const RegistryLocation& Target, const QString& Label, bool Validate = true)
{
    RegistryLink Link; Link.source = Source; Link.sourceName = Name; Link.target = Target; Link.label = Label;
    if (!Validate) return Link;
    const auto Snapshot = Backend.Enumerate(Target); Link.error = Snapshot.error;
    if (Snapshot) for (const auto& Value : Snapshot.value.values) if (Value.name.isEmpty()) { Link.text = Text({Value}); break; }
    return Link;
}
}
RegistryResult<RegistryLocation> RegistryLinkResolver::PhysicalClasses(IRegistryBackend& Backend, Hive HiveId, View Bits)
{
    const RegistryLocation Root{HiveId, "Software\\Classes", Bits};
    const auto Value = Backend.Read(Root, "SymbolicLinkValue");
    if (!Value) {
        if (Value.error == ERROR_FILE_NOT_FOUND) return {Root};
        return {{}, Value.error};
    }
    if (Value.value.type != REG_LINK || Value.value.bytes.size() % 2) return {{}, ERROR_INVALID_DATA};
    auto Native = QString::fromUtf16(reinterpret_cast<const char16_t*>(Value.value.bytes.constData()), Value.value.bytes.size() / 2);
    while (Native.endsWith(QChar(0))) Native.chop(1);
    std::optional<RegistryLocation> Target;
    if (Native.startsWith("\\Registry\\User\\", Qt::CaseInsensitive)) Target = RegistryLocation::Parse("HKU\\" + Native.mid(15), Bits);
    else if (Native.startsWith("\\Registry\\Machine\\", Qt::CaseInsensitive)) Target = RegistryLocation::Parse("HKLM\\" + Native.mid(18), Bits);
    if (!Target) return {{}, ERROR_INVALID_DATA};
    return {*Target}; // The caller displays and uses this explicit physical path.
}
QString RegistryLinkResolver::CanonicalKey(const RegistryLocation& Location)
{
    if (Location.hive == Hive::ClassesRoot) return Location.key;
    if (Location.hive == Hive::Users && Location.key.section('\\', 0, 0).endsWith("_Classes", Qt::CaseInsensitive))
        return Location.key.section('\\', 1);
    const QString Prefix = QStringLiteral("Software\\Classes");
    if ((Location.hive == Hive::CurrentUser || Location.hive == Hive::LocalMachine)
        && (Location.key.compare(Prefix, Qt::CaseInsensitive) == 0 || Location.key.startsWith(Prefix + '\\', Qt::CaseInsensitive))) return Location.key.mid(Prefix.size() + 1);
    return {};
}
QString RegistryLinkResolver::FilePath(const QString& Input, const QProcessEnvironment& Environment)
{
    QString Value = Input.trimmed(); if (Value.startsWith('@')) Value.remove(0, 1);
    const auto Expanded = ExpandEnvironmentText(Value, Environment); if (Expanded.tooLarge || !Expanded.missing.isEmpty()) return {};
    Value = Expanded.text;
    if (Value.startsWith('"')) { const auto End = Value.indexOf('"', 1); return End > 1 ? Value.mid(1, End - 1) : QString(); }
    static const QRegularExpression End(QStringLiteral("\\.(?:exe|dll|sys|ocx|cpl|ico)(?=,|\\s|$)"), QRegularExpression::CaseInsensitiveOption);
    const auto Match = End.match(Value); if (!Match.hasMatch()) return {};
    Value = Value.left(Match.capturedEnd()).trimmed();
    // Never execute a command or load its image to discover a path.
    if (Value.startsWith("\\SystemRoot\\", Qt::CaseInsensitive)) Value = Environment.value("SystemRoot") + Value.mid(11);
    if (Value.size() >= 3 && Value[1] == ':' && (Value[2] == '\\' || Value[2] == '/')) return Value;
    if (Value.startsWith("\\\\")) return Value;
    if (!Value.contains('\\') && !Value.contains('/') && !Value.contains(' ')) {
        wchar_t System[32768]{}; const auto Length = GetSystemDirectoryW(System, DWORD(std::size(System)));
        if (Length && Length < std::size(System)) return QString::fromWCharArray(System, int(Length)) + '\\' + Value;
    }
    return {};
}
QList<RegistryLink> RegistryLinkResolver::ValueLinks(IRegistryBackend& Backend, const RegistryLocation& Location,
    const RegistryValue& Value, const QProcessEnvironment& Environment, bool InspectFiles, bool ValidateTargets)
{
    QList<RegistryLink> Result; const auto Key = CanonicalKey(Location); const auto Data = Text({Value});
    if (Value.type == REG_LINK && Value.bytes.size() % 2 == 0) {
        auto Native = QString::fromUtf16(reinterpret_cast<const char16_t*>(Value.bytes.constData()), Value.bytes.size() / 2);
        while (Native.endsWith(QChar(0))) Native.chop(1);
        std::optional<RegistryLocation> Target;
        if (Native.startsWith("\\Registry\\Machine\\", Qt::CaseInsensitive)) Target = RegistryLocation{Hive::LocalMachine, Native.mid(18), Location.view};
        else if (Native.startsWith("\\Registry\\User\\", Qt::CaseInsensitive)) Target = RegistryLocation{Hive::Users, Native.mid(15), Location.view};
        if (Target) Result.append(KeyLink(Backend, Location, Value.name, *Target, QObject::tr("심볼릭 링크"), ValidateTargets));
        return Result;
    }
    const auto Add = [&](const QString& Target, const QString& Label) { if (!Target.isEmpty()) Result.append(KeyLink(Backend, Location, Value.name, Classes(Target, Location.view), Label, ValidateTargets)); };
    if (Location.key.endsWith("\\OpenWithProgids", Qt::CaseInsensitive)) Add(Value.name, QObject::tr("ProgID"));
    if (Data.isEmpty()) return Result;
    if (Key.startsWith('.') && !Key.contains('\\')) {
        if (Value.name.isEmpty()) Add(Data, QObject::tr("ProgID"));
        if (Value.name.compare("PerceivedType", Qt::CaseInsensitive) == 0) Add("SystemFileAssociations\\" + Data, QObject::tr("인지 형식"));
    }
    if (Location.key.endsWith("\\UserChoice", Qt::CaseInsensitive) && Value.name.compare("ProgId", Qt::CaseInsensitive) == 0) Add(Data, QObject::tr("사용자 선택 ProgID · 해시 유효성 미확인"));
    if (Location.key.endsWith("\\OpenWithList", Qt::CaseInsensitive) && Value.name.compare("MRUList", Qt::CaseInsensitive) != 0) Add("Applications\\" + Data, QObject::tr("응용 프로그램"));
    const auto Leaf = Key.section('\\', -1);
    if (Value.name.isEmpty() && (Leaf.compare("CurVer", Qt::CaseInsensitive) == 0
        || Leaf.compare("ProgID", Qt::CaseInsensitive) == 0 || Leaf.compare("VersionIndependentProgID", Qt::CaseInsensitive) == 0))
        Add(Data, Leaf);
    if (const auto Path = RegistryLocation::Parse(Data, Location.view))
        Result.append(KeyLink(Backend, Location, Value.name, *Path, QObject::tr("레지스트리 경로"), ValidateTargets));
    if (!ValidateTargets) {
        auto Matches = Guid.globalMatch(Data); QSet<QString> Seen;
        while (Matches.hasNext()) {
            const auto Id = Matches.next().captured(); if (Seen.contains(Id.toCaseFolded())) continue; Seen.insert(Id.toCaseFolded());
            QString Category;
            for (const auto& Kind : {QString("CLSID"), QString("Interface"), QString("AppID"), QString("TypeLib")})
                if (Value.name.compare(Kind, Qt::CaseInsensitive) == 0 || Leaf.compare(Kind, Qt::CaseInsensitive) == 0) { Category = Kind; break; }
            if (Category.isEmpty() && (Leaf.compare("TreatAs", Qt::CaseInsensitive) == 0 || Leaf.compare("AutoConvertTo", Qt::CaseInsensitive) == 0 || Key.contains("\\shellex\\", Qt::CaseInsensitive))) Category = "CLSID";
            for (const auto& Kind : Category.isEmpty() ? QStringList{"CLSID", "Interface", "AppID", "TypeLib"} : QStringList{Category})
                Add(Kind + '\\' + Id, Kind);
        }
        return Result;
    }
    const auto GuidMatch = Guid.match(Data);
    if (GuidMatch.hasMatch()) {
        const auto Id = GuidMatch.captured(); bool Found = false;
        for (const auto& Category : {QString("CLSID"), QString("Interface"), QString("AppID"), QString("TypeLib")}) {
            auto Link = KeyLink(Backend, Location, Value.name, Classes(Category + '\\' + Id, Location.view), Category);
            if (Link.error != ERROR_FILE_NOT_FOUND && Link.error != ERROR_PATH_NOT_FOUND) { Result.append(Link); Found = true; break; }
        }
        if (!Found) { RegistryLink Link; Link.kind = RegistryLink::Kind::Guid; Link.source = Location; Link.sourceName = Value.name; Link.label = QObject::tr("GUID · 등록 없음"); Link.text = Id; Link.error = ERROR_FILE_NOT_FOUND; Result.append(Link); }
    }
    if (InspectFiles && Data.startsWith('@')) {
        // https://learn.microsoft.com/windows/win32/api/shlwapi/nf-shlwapi-shloadindirectstring
        wchar_t Buffer[32768]{}; const auto Error = SHLoadIndirectString(reinterpret_cast<LPCWSTR>(Data.utf16()), Buffer, DWORD(std::size(Buffer)), nullptr);
        RegistryLink Link; Link.kind = RegistryLink::Kind::Text; Link.source = Location; Link.sourceName = Value.name;
        Link.label = QObject::tr("간접 문자열"); Link.text = SUCCEEDED(Error) ? QString::fromWCharArray(Buffer) : Data; Link.error = SUCCEEDED(Error) ? 0 : quint32(Error); Result.append(Link);
    }
    if (InspectFiles) {
        auto File = FilePath(Data, Environment);
        // An in-process COM server in the 32-bit registry view is opened by a
        // 32-bit client. Inspect that client's DLL, never the native namesake.
        // https://learn.microsoft.com/windows/win32/winprog64/file-system-redirector
        if (Location.view == View::Wow32 && Key.endsWith("\\InprocServer32", Qt::CaseInsensitive)) {
            wchar_t Native[MAX_PATH]{}, Wow[MAX_PATH]{};
            const auto NativeLength = GetSystemDirectoryW(Native, DWORD(std::size(Native)));
            const auto WowLength = GetSystemWow64DirectoryW(Wow, DWORD(std::size(Wow)));
            if (NativeLength && NativeLength < std::size(Native) && WowLength && WowLength < std::size(Wow)) {
                const auto Prefix = QString::fromWCharArray(Native, int(NativeLength)) + '\\';
                const auto Normal = QDir::toNativeSeparators(QDir::cleanPath(File));
                const auto Relative = Normal.mid(Prefix.size());
                const QStringList Exempt{"catroot", "catroot2", "driverstore", "drivers\\etc", "logfiles", "spool"};
                const bool Shared = std::any_of(Exempt.begin(), Exempt.end(), [&](const QString& Name) {
                    return Relative.compare(Name, Qt::CaseInsensitive) == 0 || Relative.startsWith(Name + '\\', Qt::CaseInsensitive);
                });
                if (Normal.startsWith(Prefix, Qt::CaseInsensitive) && !Shared)
                    File = QString::fromWCharArray(Wow, int(WowLength)) + '\\' + Relative;
            }
        }
        if (!File.isEmpty()) {
            RegistryLink Link; Link.kind = RegistryLink::Kind::File; Link.source = Location; Link.sourceName = Value.name; Link.file = File;
            Link.label = Key.endsWith("\\DefaultIcon", Qt::CaseInsensitive) ? QObject::tr("아이콘 파일") : Data.startsWith('@') ? QObject::tr("리소스 파일") : QObject::tr("파일");
            const auto Attributes = GetFileAttributesW(reinterpret_cast<LPCWSTR>(File.utf16())); Link.error = Attributes == INVALID_FILE_ATTRIBUTES ? GetLastError() : 0;
            Result.append(Link);
        }
    }
    return Result;
}
RegistryLinkResult RegistryLinkResolver::Resolve(IRegistryBackend& Backend, const RegistryLocation& Location,
    const std::optional<RegistryValue>& Value, const QProcessEnvironment& Environment)
{
    RegistryLinkResult Result; const auto Snapshot = Backend.Enumerate(Location);
    if (!Snapshot) { Result.diagnostics.append(QObject::tr("키 읽기 실패 (%1)").arg(Snapshot.error)); return Result; }
    if (Location.hive == Hive::ClassesRoot) for (const auto HiveId : {Hive::CurrentUser, Hive::LocalMachine}) {
        const auto Physical = PhysicalClasses(Backend, HiveId, Location.view);
        if (Physical) Result.physicalClasses.append(Location.key.isEmpty() ? Physical.value : Physical.value.Child(Location.key));
        else Result.diagnostics.append(QObject::tr("실제 Classes 위치 확인 실패 (%1)").arg(Physical.error));
    }
    auto Values = Snapshot.value.values;
    if (Value) Values.removeIf([&](const RegistryValue& Current) { return Current.name.compare(Value->name, Qt::CaseInsensitive) != 0; });
    for (const auto& Item : Values) Result.forward.append(ValueLinks(Backend, Location, Item, Environment));
    const auto Key = CanonicalKey(Location); QString Extension;
    if (Key.startsWith('.')) Extension = Key.section('\\', 0, 0);
    else if (Location.hive == Hive::CurrentUser && Location.key.startsWith(FileExts + '\\', Qt::CaseInsensitive)) Extension = Location.key.mid(FileExts.size() + 1).section('\\', 0, 0);
    if (Extension.isEmpty() && !Key.isEmpty() && !Key.startsWith("CLSID\\", Qt::CaseInsensitive)) {
        const auto Root = Backend.Enumerate(Classes({}, Location.view));
        if (Root) for (const auto& Child : Root.value.keys) {
            if (!Child.name.startsWith('.')) continue;
            const auto Default = Text(Backend.Read(Classes(Child.name, Location.view), {}));
            const auto Choice = Text(Backend.Read({Hive::CurrentUser, FileExts + '\\' + Child.name + "\\UserChoice", Location.view}, "ProgId"));
            if ((!Default.isEmpty() && (Key.compare(Default, Qt::CaseInsensitive) == 0 || Key.startsWith(Default + '\\', Qt::CaseInsensitive)))
                || (!Choice.isEmpty() && (Key.compare(Choice, Qt::CaseInsensitive) == 0 || Key.startsWith(Choice + '\\', Qt::CaseInsensitive)))) { Extension = Child.name; break; }
        }
    }
    const auto AddServer = [&](const RegistryLocation& Server, const QString& Label, bool Fallback) {
        auto Link = KeyLink(Backend, Location, {}, Server, Label); Link.fallback = Fallback; Result.chain.append(Link);
        const auto Raw = Backend.Read(Server, {}); if (Raw) {
            Result.chain.last().text = Text(Raw);
            auto Files = ValueLinks(Backend, Server, Raw.value, Environment);
            for (auto& File : Files) { File.fallback = Fallback; Result.chain.append(File); }
        }
    };
    if (!Extension.isEmpty()) {
        const auto Ext = Classes(Extension, Location.view); RegistryLocation User{Hive::CurrentUser, FileExts + '\\' + Extension + "\\UserChoice", Location.view};
        const auto Choice = Text(Backend.Read(User, "ProgId")); const auto Default = Text(Backend.Read(Ext, {}));
        if (!Choice.isEmpty()) {
            auto Link = KeyLink(Backend, User, "ProgId", Classes(Choice, Location.view), QObject::tr("사용자 선택 · 해시 유효성 미확인")); Result.chain.append(Link);
            AddServer(Classes(Choice + "\\shell\\open\\command", Location.view), QObject::tr("열기 명령"), false);
        }
        if (!Default.isEmpty()) {
            auto Link = KeyLink(Backend, Ext, {}, Classes(Default, Location.view), QObject::tr("확장명 → ProgID")); Link.fallback = !Choice.isEmpty(); Result.chain.append(Link);
            AddServer(Classes(Default + "\\shell\\open\\command", Location.view), QObject::tr("확장명 기본 열기 명령"), !Choice.isEmpty());
            for (const auto& Extra : {QString("DefaultIcon")}) {
                const auto Target = Extra == "DefaultIcon" ? Classes(Default + '\\' + Extra, Location.view) : Ext.Child(Extra);
                const auto Item = Backend.Enumerate(Target); if (Item) { Result.chain.append(KeyLink(Backend, Ext, {}, Target, Extra)); for (const auto& Raw : Item.value.values) Result.chain.append(ValueLinks(Backend, Target, Raw, Environment)); }
            }
            const auto Friendly = Backend.Read(Classes(Default, Location.view), "FriendlyTypeName"); if (Friendly) Result.chain.append(ValueLinks(Backend, Classes(Default, Location.view), Friendly.value, Environment));
        }
    }
    if (!Extension.isEmpty()) {
        const auto Ext = Classes(Extension, Location.view);
        for (const auto& Name : {QString("Content Type"), QString("PerceivedType")}) {
            const auto Raw = Backend.Read(Ext, Name);
            if (Raw) { auto Link = KeyLink(Backend, Ext, Name, Ext, Name == "Content Type" ? QObject::tr("MIME 형식") : QObject::tr("인지 형식")); Link.text = Text(Raw); Result.chain.append(Link); }
        }
        for (const auto& Tail : {QString("ShellNew"), QString("PersistentHandler")}) {
            const auto Target = Ext.Child(Tail); const auto Contents = Backend.Enumerate(Target);
            if (Contents) {
                Result.chain.append(KeyLink(Backend, Ext, {}, Target, Tail == "ShellNew" ? QObject::tr("새로 만들기") : QObject::tr("검색 색인")));
                for (const auto& Raw : Contents.value.values) Result.chain.append(ValueLinks(Backend, Target, Raw, Environment));
            }
        }
        const auto Common = Classes("*\\shellex\\ContextMenuHandlers", Location.view);
        const auto Contents = Backend.Enumerate(Common);
        if (Contents) { auto Link = KeyLink(Backend, Ext, {}, Common, QObject::tr("모든 파일 공통 메뉴 처리기")); Link.text = QObject::tr("%1개").arg(Contents.value.keys.size()); Result.chain.append(Link); }
    }
    QString Clsid;
    if (Key.startsWith("CLSID\\", Qt::CaseInsensitive)) Clsid = Key.section('\\', 1, 1);
    else if (Key.contains("\\shellex\\", Qt::CaseInsensitive)) Clsid = Guid.match(Text(Backend.Read(Location, {}))).captured();
    if (!Clsid.isEmpty()) {
        const auto Com = Classes("CLSID\\" + Clsid, Location.view); Result.chain.append(KeyLink(Backend, Location, {}, Com, QObject::tr("COM 개체 (CLSID)")));
        for (const auto& Kind : {QString("InprocServer32"), QString("LocalServer32")}) if (Backend.Enumerate(Com.Child(Kind))) AddServer(Com.Child(Kind), Kind, false);
    }
    return Result;
}
RegistryLinkIndex RegistryLinkResolver::BuildIndex(IRegistryBackend& Backend, View Bits, const std::atomic_bool* Cancel, qsizetype MaxKeys, QList<RegistryLocation> Roots, std::atomic<qsizetype>* Progress)
{
    RegistryLinkIndex Result; QList<RegistryLocation> Pending = Roots.isEmpty() ? QList<RegistryLocation>{Classes({}, Bits), {Hive::CurrentUser, FileExts, Bits}} : std::move(Roots);
    QSet<QString> Seen; qsizetype Edges = 0; const QProcessEnvironment EmptyEnvironment;
    while (!Pending.isEmpty()) {
        if ((Cancel && Cancel->load()) || Result.keys >= MaxKeys || Edges >= 250000) { Result.complete = false; break; }
        const auto Key = Pending.takeLast(); if (Seen.contains(Key.Id())) continue; Seen.insert(Key.Id()); ++Result.keys; if (Progress) Progress->fetch_add(1);
        const auto Snapshot = Backend.Enumerate(Key, Cancel);
        if (!Snapshot) { Result.complete = false; if (Result.diagnostics.size() < 100) Result.diagnostics.append(Key.Path() + QString(" (%1)").arg(Snapshot.error)); continue; }
        for (const auto& Value : Snapshot.value.values) for (const auto& Link : ValueLinks(Backend, Key, Value, EmptyEnvironment, false))
            if (Link.target && Edges++ < 250000) Result.incoming[Link.target->Id()].append(Link);
        if (!Snapshot.value.info.link && Key.key.count('\\') < 128) for (const auto& Child : Snapshot.value.keys) Pending.append(Key.Child(Child.name));
    }
    if (!Pending.isEmpty()) Result.diagnostics.append(QObject::tr("색인이 중지되었거나 제한에 도달했습니다."));
    return Result;
}
struct RegistryLinks::Watches { std::vector<std::unique_ptr<RegistryWatcher>> items; QTimer* debounce = nullptr; QTimer* progress = nullptr; };
struct RegistryLinks::IndexWorker {
    explicit IndexWorker(View Bits, QString File, QString Sid) : cache(Bits), file(std::move(File)), sid(std::move(Sid)) {}
    RegistryLinkCache cache;
    QString file, sid;
    std::unique_ptr<QLockFile> lock;
    std::atomic_bool busy{false}, cancel{false}, refresh{false};
    std::atomic_int users{0};
    bool loaded = false, lastFast = false;
    qsizetype checkpointKeys = 0;
};
RegistryLinks::RegistryLinks(QObject* Parent) : QObject(Parent), watches_(std::make_unique<Watches>())
{
    watches_->debounce = new QTimer(this); watches_->debounce->setSingleShot(true); watches_->debounce->setInterval(750);
    connect(watches_->debounce, &QTimer::timeout, this, &RegistryLinks::Refresh);
    watches_->progress = new QTimer(this); watches_->progress->setInterval(1000);
    connect(watches_->progress, &QTimer::timeout, this, &RegistryLinks::ProgressChanged);
}
RegistryLinks::~RegistryLinks() { Suspend(); }
void RegistryLinks::Suspend()
{
    if (indexWorker_ && indexWorker_->users.fetch_sub(1) == 1) {
        const auto Worker = indexWorker_; Worker->cancel.store(true);
        if (!Worker->busy.exchange(true)) {
            // Closing between slices must checkpoint the latest queues too.
            (void)QtConcurrent::run([Worker] {
                const auto Release = qScopeGuard([&] { Worker->busy.store(false); });
                if (Worker->loaded && !Worker->file.isEmpty()) Worker->cache.Save(Worker->file, Worker->sid);
            });
        }
    }
    indexWorker_.reset();
    ++generation_; ++indexGeneration_; loading_ = indexLoading_ = indexJob_ = false;
    watches_->debounce->stop(); watches_->progress->stop(); watches_->items.clear();
    indexed_ = false;
}
void RegistryLinks::Select(const RegistryLocation& Location, std::optional<RegistryValue> Value)
{
    if (indexed_ && indexView_ != Location.view) Suspend();
    location_ = Location; value_ = std::move(Value); const auto Generation = ++generation_; loading_ = true; result_ = {};
    const auto SelectedValue = value_; const auto Environment = QProcessEnvironment::systemEnvironment();
    auto* Watcher = new QFutureWatcher<RegistryLinkResult>(this);
    connect(Watcher, &QFutureWatcherBase::finished, this, [this, Watcher, Generation] {
        auto Result = Watcher->result(); Watcher->deleteLater(); if (Generation != generation_) return;
        result_ = std::move(Result); loading_ = false; emit Changed();
    });
    Watcher->setFuture(QtConcurrent::run([Location, SelectedValue, Environment] { WinRegistryBackend Backend; return RegistryLinkResolver::Resolve(Backend, Location, SelectedValue, Environment); }));
    if (!indexed_) { indexView_ = Location.view; startIndex(); }
}
QList<RegistryLink> RegistryLinks::BackReferences() const
{
    if (!location_) return {}; const auto Key = RegistryLinkResolver::CanonicalKey(*location_);
    auto Result = index_.incoming.value(location_->Id());
    if (!Key.isEmpty() && Classes(Key, location_->view).Id() != location_->Id())
        Result.append(index_.incoming.value(Classes(Key, location_->view).Id()));
    std::sort(Result.begin(), Result.end(), [](const RegistryLink& A, const RegistryLink& B) {
        const auto Left = A.source.Id() + '\n' + A.sourceName.toCaseFolded();
        const auto Right = B.source.Id() + '\n' + B.sourceName.toCaseFolded(); return Left < Right;
    });
    Result.erase(std::unique(Result.begin(), Result.end(), [](const RegistryLink& A, const RegistryLink& B) {
        return A.source.Id() == B.source.Id() && A.sourceName.compare(B.sourceName, Qt::CaseInsensitive) == 0;
    }), Result.end());
    return Result;
}
void RegistryLinks::Refresh()
{
    if (!location_ || !indexed_) return;
    if (indexWorker_) indexWorker_->refresh.store(true);
    fastComplete_ = false; Select(*location_, value_); startIndex();
}
void RegistryLinks::startIndex()
{
    if (indexJob_ || !location_) return;
    if (!indexed_) {
        const auto Sid = QString::fromStdWString(hc::system::CurrentUserSid());
        const auto Name = QString::fromLatin1(QCryptographicHash::hash(Sid.toUtf8(), QCryptographicHash::Sha256).toHex()) + "-" + QString::number(int(indexView_)) + ".json";
        const auto File = cacheDirectory_.isEmpty() ? QString() : QDir(cacheDirectory_).filePath(Name);
        // GUI-thread acquisition; the worker owns all cache mutations. Views of
        // the same SID/bitness share one serialized traversal and checkpoint.
        static QHash<QString, std::weak_ptr<IndexWorker>> Workers;
        const auto Key = File.isEmpty() ? Sid + ":" + QString::number(int(indexView_)) : File;
        indexWorker_ = Workers.value(Key).lock();
        if (!indexWorker_) { indexWorker_ = std::make_shared<IndexWorker>(indexView_, File, Sid); Workers[Key] = indexWorker_; }
        indexWorker_->users.fetch_add(1); indexWorker_->cancel.store(false);
        indexed_ = true; index_ = {}; fastComplete_ = false;
        for (const auto& Root : QList<RegistryLocation>{{Hive::CurrentUser, "Software\\Classes", indexView_},
            {Hive::LocalMachine, "Software\\Classes", indexView_}, {Hive::Users, Sid + "_Classes", indexView_},
            {Hive::CurrentUser, FileExts, indexView_}}) {
            auto Watch = std::make_unique<RegistryWatcher>(); const QPointer<RegistryLinks> Guard(this);
            if (Watch->Start(Root, true, [Guard](quint32) {
                if (Guard) QMetaObject::invokeMethod(Guard, [Guard] { if (Guard && Guard->indexed_) Guard->watches_->debounce->start(); }, Qt::QueuedConnection);
            })) watches_->items.push_back(std::move(Watch));
        }
    }
    const auto Worker = indexWorker_;
    if (!Worker || Worker->busy.exchange(true)) {
        QTimer::singleShot(100, this, [this] { if (indexed_) startIndex(); }); return;
    }
    indexJob_ = indexLoading_ = true; watches_->progress->start();
    progress_ = std::make_shared<std::atomic<qsizetype>>(index_.keys); const auto Progress = progress_;
    const auto Generation = indexGeneration_;
    struct Batch { RegistryLinkIndex index; bool fast = false, done = false; };
    auto* Watcher = new QFutureWatcher<Batch>(this);
    connect(Watcher, &QFutureWatcherBase::finished, this, [this, Watcher, Generation] {
        auto Result = Watcher->result(); Watcher->deleteLater(); if (Generation != indexGeneration_) return;
        const auto Before = BackReferences(); const auto Diagnostics = index_.diagnostics;
        const bool StageChanged = fastComplete_ != Result.fast || indexLoading_ == Result.done;
        indexJob_ = false; index_ = std::move(Result.index); fastComplete_ = Result.fast;
        indexLoading_ = !Result.done; progress_.reset();
        // Preserve active link buttons while unrelated partitions are scanned.
        if (StageChanged || Before != BackReferences() || Diagnostics != index_.diagnostics) emit Changed();
        else emit ProgressChanged();
        if (Result.done) watches_->progress->stop();
        else QTimer::singleShot(50, this, [this] { if (indexed_) startIndex(); });
    });
    Watcher->setFuture(QtConcurrent::run([Worker, Progress] {
        const auto Release = qScopeGuard([&] { Worker->busy.store(false); });
        const bool Background = SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
        const auto Priority = qScopeGuard([&] { if (Background) SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_END); });
        Batch Result;
        if (!Worker->loaded) {
            if (!Worker->file.isEmpty()) {
                QDir().mkpath(QFileInfo(Worker->file).absolutePath());
                Worker->lock = std::make_unique<QLockFile>(Worker->file + ".lock");
                if (!Worker->lock->tryLock(0)) { Result.index.complete = false; Result.index.diagnostics.append(QObject::tr("다른 프로세스의 연결 색인 작업을 기다립니다.")); return Result; }
                Worker->cache.Load(Worker->file, Worker->sid);
                // A completed disk cache needs timestamp validation after a gap
                // without watches. Interrupted traversal retains its work queue.
                if (Worker->cache.Complete()) Worker->cache.Validate();
            }
            Worker->loaded = true;
        }
        if (Worker->refresh.exchange(false)) Worker->cache.Validate();
        WinRegistryBackend Backend; Worker->cache.Advance(Backend, &Worker->cancel, 100000, 750, Progress.get());
        Result.fast = Worker->cache.FastComplete(); Result.done = Worker->cache.Complete(); Result.index = Worker->cache.Snapshot();
        const bool Checkpoint = Worker->cancel.load() || Result.done || (!Worker->lastFast && Result.fast)
            || Worker->cache.Keys() >= Worker->checkpointKeys + 100000;
        if (Checkpoint && !Worker->file.isEmpty()) {
            if (!Worker->cache.Save(Worker->file, Worker->sid)) Result.index.diagnostics.append(QObject::tr("연결 색인 캐시 저장 실패 · 현재 결과는 계속 사용할 수 있습니다."));
            else Worker->checkpointKeys = Worker->cache.Keys();
        }
        Worker->lastFast = Result.fast;
        return Result;
    }));
}
} // namespace hc::registry