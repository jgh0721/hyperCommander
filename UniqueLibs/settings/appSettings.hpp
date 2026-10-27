#pragma once
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QStringList>
#include <functional>
#include "UniqueLibs/document/documentHandlerSettings.hpp"
#include "UniqueLibs/productDefaults.hpp"
#include "profileConfig.hpp"
#include "unifiedConfig.hpp"
#include "updateSettings.hpp"

namespace hc::settings {
struct ConfigPaths {
    QString directory_, error_;
    // 이전 기본 위치. 설정 파일이 없을 때 이행 원본으로만 사용한다.
    QStringList legacy_;
    static ConfigPaths Resolve(const QStringList& Arguments, const QString& ExecutableDirectory,
        const QString& UserDirectory = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation), bool Document = false)
    {
        QString Explicit; bool Portable = false, Seen = false;
        for (int Index = 1; Index < Arguments.size(); ++Index) {
            const auto& Arg = Arguments[Index];
            if (Arg == "--") break;
            if (Arg == "--portable") Portable = true;
            else if (Arg == "--config-dir" || Arg.startsWith("--config-dir=")) {
                if (Seen) return {{}, "duplicate-config-dir"};
                Seen = true;
                if (Arg == "--config-dir") {
                    if (++Index >= Arguments.size() || Arguments[Index].startsWith("--")) return {{}, "missing-config-dir"};
                    Explicit = Arguments[Index];
                } else Explicit = Arg.mid(13);
                if (Explicit.trimmed().isEmpty()) return {{}, "empty-config-dir"};
            }
        }
        // --config-dir가 없으면 실행 파일 폴더를 사용한다. --portable은 호환을 위해서만 받는다.
        const auto Path = Seen ? Explicit : ExecutableDirectory;
        if (Path.isEmpty()) return {{}, "unavailable-config-dir"};
        QStringList Legacy;
        if (!Seen && !Document) {
            const auto Adjacent = QDir(ExecutableDirectory).filePath("config");
            for (const auto& Candidate : Portable ? QStringList{Adjacent, UserDirectory} : QStringList{UserDirectory, Adjacent})
                if (!Candidate.isEmpty()) Legacy.append(QDir::cleanPath(QFileInfo(Candidate).absoluteFilePath()));
        }
        return {QDir::cleanPath(QFileInfo(Path).absoluteFilePath()), {}, Legacy};
    }
};

enum class Section { General, Appearance, Shortcuts, Favorites, FileSets, Editors, Plugins, Columns, Automation, Options };
inline QString SectionName(Section Value)
{
    switch (Value) {
    case Section::General: return "general";
    case Section::Appearance: return "appearance";
    case Section::Shortcuts: return "shortcuts";
    case Section::Favorites: return "favorites";
    case Section::FileSets: return "fileSets";
    case Section::Editors: return "editors";
    case Section::Plugins: return "plugins";
    case Section::Columns: return "columns";
    case Section::Automation: return "automation";
    case Section::Options: return "options";
    }
    return {};
}
inline QStringList SizeFormats()
{
    return {"bytes", "kbytes", "adaptive3", "adaptive2", "adaptive1", "adaptive0",
        "adaptiveT3", "adaptiveT2", "adaptiveT1", "adaptiveT0", "adaptiveM1", "adaptiveM0"};
}
// 이전 API는 유지하고 제품 정의는 한 헤더에서만 만든다.
inline QJsonObject BuiltInAppearance() { return hc::defaults::Appearance(); }
inline QJsonObject Defaults() { return hc::defaults::Settings(true); }
inline QJsonObject MergeDefaults(QJsonObject Value, const QJsonObject& Base, const QString& Path, QStringList& Issues)
{
    for (auto It = Base.begin(); It != Base.end(); ++It) {
        const auto Key = Path.isEmpty() ? It.key() : Path + '.' + It.key();
        if (!Value.contains(It.key())) Value[It.key()] = It.value();
        else if (Value.value(It.key()).type() != It.value().type()) {
            Issues.append(Key); Value[It.key()] = It.value();
        } else if (It.value().isObject()) {
            const auto Child = Value.value(It.key()).toObject(), ChildBase = It.value().toObject();
            // 사용자가 지운 구형 그룹 색상과 아직 지원하지 않는 버전의 문서는 보존한다.
            if (Key == "fileSets" || Key.endsWith(".fileSets") || (Child.contains("version") && ChildBase.contains("version") && Child["version"] != ChildBase["version"])) continue;
            Value[It.key()] = MergeDefaults(Child, ChildBase, Key, Issues);
        }
    }
    return Value;
}

class JsonStore : public QObject {
public:
    enum class Scope { Standalone, Product, Workspace };
    // Stores sharing one file own disjoint top-level keys. Each compares and replaces only its own keys.
    struct Partition {
        QStringList keys_;
        bool only_ = false;                          // true: keys_만 소유, false: keys_를 제외한 키를 소유
        std::function<bool(QString&)> prepare_;     // 읽기 전에 한 번 실행한다(이전 파일 이행 등).
        bool Shared() const { return !keys_.isEmpty(); }
        bool Owns(const QString& Key) const { return Key == "schemaVersion" || keys_.contains(Key) == only_; }
    };
    explicit JsonStore(QString FileName, QJsonObject Defaults = {{"schemaVersion", 1}}, QObject* Parent = nullptr, Scope Domain = Scope::Standalone,
        Partition Part = {})
        : QObject(Parent), path_(QFileInfo(FileName).absoluteFilePath()), defaults_(std::move(Defaults)), scope_(Domain), partition_(std::move(Part)) { Reload(); }
    const QString& FileName() const { return path_; }
    const QString& Error() const { return error_; }
    const QStringList& Issues() const { return issues_; }
    bool Writable() const { return writable_; }
    bool Exists() const { return exists_; }
    QJsonObject Object() const { return document_; }
    QJsonObject CommittedObject() const { return committedDocument_; }
    bool Editing() const { return editing_; }
    void SetErrorHandler(std::function<void(const QString&)> Handler) { errorHandler_ = std::move(Handler); }
    bool Reload()
    {
        editing_ = false; error_.clear(); issues_.clear(); writable_ = true; document_ = defaults_; committedDocument_ = defaults_;
        if (!read(original_, exists_)) { writable_ = false; return fail(readError_.isEmpty() ? "read-failed" : readError_); }
        if (!exists_) return true;
        QJsonParseError Parse;
        const auto Doc = QJsonDocument::fromJson(original_, &Parse);
        if (Parse.error != QJsonParseError::NoError || !Doc.isObject()) { writable_ = false; return fail("invalid-json"); }
        if (Doc.object().value("schemaVersion") != defaults_.value("schemaVersion")) { writable_ = false; return fail("unsupported-schema-version"); }
        auto Loaded = Doc.object(); migrate(Loaded);
        document_ = MergeDefaults(Loaded, defaults_, {}, issues_);
        normalize(document_, issues_);
        committedDocument_ = document_;
        return true;
    }
    void BeginEdit() { editing_ = true; }
    void DiscardEdit() { Reload(); }
    bool Replace(const QJsonObject& Value)
    {
        if (!writable_) return fail("read-only");
        document_ = Value;
        return editing_ || Commit();
    }
    bool Commit()
    {
        if (!writable_) return fail("read-only");
        if (document_.value("schemaVersion") != defaults_.value("schemaVersion")) return fail("invalid-schema-version");
        QStringList Problems;
        auto Validated = MergeDefaults(document_, defaults_, {}, Problems);
        if (!Problems.isEmpty()) return fail("invalid-fields: " + Problems.join(", "));
        normalize(Validated, Problems);
        if (!Problems.isEmpty() || Validated != document_) return fail("invalid-fields");
        if (scope_ != Scope::Standalone) {
            auto Base = QJsonDocument::fromJson(original_).object(); Base["schemaVersion"] = 2;
            auto Desired = exists_ ? UnifiedConfig::Changed(committedDocument_, document_, Base) : document_;
            Desired["schemaVersion"] = 2;
            QJsonObject Saved; QString Error;
            if (!UnifiedConfig::Save(path_, Base, Desired, Saved, Error)) return fail(Error);
            document_ = MergeDefaults(project(Saved), defaults_, {}, issues_);
            committedDocument_ = document_; original_ = QJsonDocument(project(Saved)).toJson();
            exists_ = true; error_.clear(); return true;
        }
        if (!QDir().mkpath(QFileInfo(path_).absolutePath())) return fail("create-directory-failed");
        QLockFile Lock(path_ + ".lock"); Lock.setStaleLockTime(0);
        if (!Lock.tryLock(0)) return fail("file-locked");
        QByteArray Current; bool Exists = false;
        if (!readFile(Current, Exists)) return fail("read-before-save-failed");
        auto Output = document_;
        if (partition_.Shared()) {
            // Keys owned by other stores keep their disk values; only changes to our keys conflict.
            QJsonObject Disk;
            if (Exists) {
                const auto Doc = QJsonDocument::fromJson(Current);
                if (!Doc.isObject() || Doc.object().value("schemaVersion") != defaults_.value("schemaVersion")) return fail("changed-on-disk");
                Disk = Doc.object();
            }
            const auto Mine = [this](const QJsonObject& Root) { auto Result = owned(Root); Result.remove("schemaVersion"); return Result; };
            if (Mine(Disk) != Mine(QJsonDocument::fromJson(original_).object())) return fail("changed-on-disk");
            for (const auto& Key : Disk.keys()) if (partition_.Owns(Key)) Disk.remove(Key);
            for (auto It = document_.begin(); It != document_.end(); ++It) if (partition_.Owns(It.key())) Disk[It.key()] = It.value();
            Output = Disk;
        }
        // Compare the bytes, including whitespace, under the same lock used by every writer.
        else if (Exists != exists_ || QCryptographicHash::hash(Current, QCryptographicHash::Sha256)
            != QCryptographicHash::hash(original_, QCryptographicHash::Sha256)) return fail("changed-on-disk");
        auto Bytes = QJsonDocument(Output).toJson(QJsonDocument::Indented);
        Bytes.replace("\n", "\r\n");
        if (Bytes.size() > maxBytes_) return fail("document-too-large");
        QSaveFile File(path_); File.setDirectWriteFallback(false);
        if (!File.open(QIODevice::WriteOnly) || File.write(Bytes) != Bytes.size() || !File.commit()) return fail("atomic-save-failed");
        committedDocument_ = document_; exists_ = true; error_.clear(); issues_.clear();
        original_ = partition_.Shared() ? QJsonDocument(owned(Output)).toJson(QJsonDocument::Compact) : Bytes;
        return true;
    }
protected:
    virtual void migrate(QJsonObject&) {}
    virtual void normalize(QJsonObject&, QStringList&) {}
    const QJsonObject& defaultDocument() const { return defaults_; }
private:
    bool read(QByteArray& Bytes, bool& Exists) const
    {
        readError_.clear();
        if (scope_ != Scope::Standalone) {
            QJsonObject Root; QString Error;
            if (!UnifiedConfig::Load(path_, Root, Error)) { readError_ = Error; return false; }
            Exists = QFileInfo::exists(path_); Bytes = QJsonDocument(project(Root)).toJson(); return true;
        }
        if (partition_.prepare_ && !partition_.prepare_(readError_)) return false;
        if (!readFile(Bytes, Exists)) return false;
        if (!partition_.Shared() || !Exists) return true;
        // 손상된 문서는 그대로 넘겨 Reload가 오류를 보고하게 한다.
        const auto Doc = QJsonDocument::fromJson(Bytes);
        if (Doc.isObject()) Bytes = QJsonDocument(owned(Doc.object())).toJson(QJsonDocument::Compact);
        return true;
    }
    bool readFile(QByteArray& Bytes, bool& Exists) const
    {
        QFile File(path_); Exists = QFileInfo::exists(path_);
        if (!Exists) { Bytes.clear(); return true; }
        if (!File.open(QIODevice::ReadOnly) || File.size() > maxBytes_) return false;
        Bytes = File.read(maxBytes_ + 1); return File.error() == QFileDevice::NoError && Bytes.size() <= maxBytes_;
    }
    QJsonObject project(QJsonObject Root) const
    {
        if (scope_ == Scope::Workspace) {
            QJsonObject Result{{"schemaVersion", 1}};
            if (Root.contains("workspace")) Result["workspace"] = Root["workspace"];
            return Result;
        }
        return Root;
    }
    QJsonObject owned(const QJsonObject& Root) const
    {
        QJsonObject Result;
        for (auto It = Root.begin(); It != Root.end(); ++It) if (partition_.Owns(It.key())) Result[It.key()] = It.value();
        return Result;
    }
    bool fail(const QString& Code)
    {
        error_ = path_ + ": " + Code;
        if (errorHandler_) errorHandler_(error_);
        return false;
    }
    static constexpr qint64 maxBytes_ = 16 * 1024 * 1024;
    QString path_, error_;
    mutable QString readError_;
    QJsonObject defaults_, document_, committedDocument_;
    QByteArray original_;
    QStringList issues_;
    bool exists_ = false, writable_ = true, editing_ = false;
    std::function<void(const QString&)> errorHandler_;
    Scope scope_;
    Partition partition_;
};

// In HyperCommander.json the settings store owns every key except "workspace"; the workspace store owns only "workspace".
inline JsonStore::Partition ProfilePartition(bool Workspace, const QString& Directory, const QStringList& Legacy)
{
    return {{"workspace"}, Workspace, [Directory, Legacy](QString& Error) { return ProfileConfig::Migrate(Directory, Legacy, Error); }};
}

class Repository : public JsonStore {
public:
    static QJsonObject DefaultsForDirectory(const QString& Directory, const QStringList& Legacy = {})
    {
        // 신규 설치는 제품 기본값, 기존 설치(이행할 이전 설정 포함)의 누락 키는 호환 프리셋을 사용한다.
        return hc::defaults::Settings(ProfileConfig::HasSettings(Directory, Legacy));
    }
    explicit Repository(const QString& Directory, const QStringList& Legacy = {})
        : JsonStore(ProfileConfig::FileName(Directory), DefaultsForDirectory(Directory, Legacy), nullptr, Scope::Standalone,
            ProfilePartition(false, Directory, Legacy)) { Reload(); }
    QJsonValue Read(Section Domain) const { return Object().value(SectionName(Domain)); }
    bool Put(Section Domain, const QJsonValue& Value)
    {
        auto Next = Object(); Next[SectionName(Domain)] = Value;
        if (Domain == Section::Editors) {
            auto Editors = Value.isArray() ? Read(Domain).toObject() : Value.toObject();
            if (Value.isArray()) Editors["items"] = Value;
            Next[SectionName(Domain)] = hc::document::MigrateEditors(Editors);
        }
        if (Next[SectionName(Domain)].isObject()) {
            // 일부 필드만 저장하는 기존 호출자도 누락된 제품 기본값을 함께 보존한다.
            // 잘못된 타입은 초안에 남겨 Commit의 검증에서 거부한다.
            QStringList Problems;
            const auto Merged = MergeDefaults(Next[SectionName(Domain)].toObject(),
                defaultDocument()[SectionName(Domain)].toObject(), SectionName(Domain), Problems);
            if (Problems.isEmpty()) Next[SectionName(Domain)] = Merged;
        }
        hc::defaults::MigrateCommandSettings( Next );
        return Replace(Next);
    }
    QJsonArray Plugins(const QString& Format) const { return Read(Section::Plugins).toObject().value(Format).toArray(); }
    bool SetPlugins(const QString& Format, const QJsonArray& Items)
    { auto Next = Read(Section::Plugins).toObject(); Next[Format] = Items; return Put(Section::Plugins, Next); }
    int SizeStyle(bool Header) const
    { return qMax(0, SizeFormats().indexOf(Read(Section::General).toObject().value(Header ? "sizeInHeader" : "sizeInFooter").toString())); }
protected:
    void migrate(QJsonObject& Document) override
    {
        Document["editors"] = hc::document::MigrateEditors(Document["editors"]);
        hc::defaults::MigrateCommandSettings( Document );
    }
    void normalize(QJsonObject& Document, QStringList& Issues) override
    {
        const auto Report = [&](const QString& Path) { if (!Issues.contains(Path)) Issues.append(Path); };
        auto General = Document["general"].toObject();
        if (!QStringList{"legacy", "prototype"}.contains(General["shortcutPreset"].toString())) { General["shortcutPreset"] = "legacy"; Report("general.shortcutPreset"); }
        for (const auto* Key : {"sizeInHeader", "sizeInFooter"})
            if (!SizeFormats().contains(General[Key].toString())) { General[Key] = "bytes"; Report(QString("general.") + Key); }
        if (!QStringList{"all", "associated", "standard", "none"}.contains(General["iconMode"].toString())) { General["iconMode"] = "standard"; Report("general.iconMode"); }
        for (const auto* Key : {"specialFolderIcons", "virtualFolderIcons", "explorerDefaultIcons", "iconOverlays"})
            if (General.contains(Key) && !General[Key].isBool()) { General[Key] = true; Report(QString("general.") + Key); }
        if (!QStringList{"plain", "brackets", "suffix"}.contains(General["directoryStyle"].toString())) { General["directoryStyle"] = "plain"; Report("general.directoryStyle"); }
        if (General.contains("singleInstance") && !General["singleInstance"].isBool()) { General["singleInstance"] = false; Report("general.singleInstance"); }
        if (General.contains("newTabPosition") && !QStringList{"last", "right", "left", "first"}.contains(General["newTabPosition"].toString())) { General["newTabPosition"] = "last"; Report("general.newTabPosition"); }
        if (General.contains("refresh")) {
            auto Refresh = General["refresh"].toObject();
            const auto Seconds = Refresh["fallbackSeconds"].toDouble(-1);
            if (!Refresh["fallbackSeconds"].isDouble() || Seconds < 1 || Seconds > 86400 || Seconds != double(qint64(Seconds))) {
                Refresh["fallbackSeconds"] = 15; Report("general.refresh.fallbackSeconds");
            }
            General["refresh"] = Refresh;
        }
        auto Update = General.value("update").toObject();
        if (Update.contains("checkIntervalDays") && Update["checkIntervalDays"] != QJsonValue(UpdateCheckIntervalDays(Update))) {
            Update["checkIntervalDays"] = UpdateCheckIntervalDays(Update);
            General["update"] = Update; Report("general.update.checkIntervalDays");
        }
        Document["general"] = General;
        auto Copy = Document["options"].toObject();
        for (const auto* Key : {"folderSizeOnSpace", "folderSizeBeforeOperations", "folderSizeUseEverything"})
            if (Copy.contains(Key) && !Copy[Key].isBool()) { Copy[Key] = true; Report(QString("options.") + Key); }
        if (Copy.contains("folderSizeMode") && !QStringList{"manual", "local", "all", "everything"}.contains(Copy["folderSizeMode"].toString())) {
            Copy["folderSizeMode"] = "everything"; Report("options.folderSizeMode");
        }
        for (const auto* Key : {"copyMetadata", "copyVerify", "copyAcl", "copyStreams", "saveJobProgress", "showJobDetails", "enableJobRecovery", "jobCloseOnSuccess", "delayedFileWrites", "renameInDialog", "renameSelectExtension", "renameOnSlowClick"}) if (Copy.contains(Key) && !Copy[Key].isBool()) {
            Copy[Key] = QString(Key) == "copyMetadata" || QString(Key) == "jobCloseOnSuccess" || QString(Key) == "delayedFileWrites" || QString(Key) == "renameInDialog" || QString(Key) == "renameOnSlowClick"; Report(QString("options.") + Key);
        }
        if (Copy.contains("copyPattern")) {
            const auto Pattern = Copy["copyPattern"].toString(); bool Valid = Copy["copyPattern"].isString() && !Pattern.trimmed().isEmpty() && Pattern.toUtf8().size() <= 4096;
            for (const auto Ch : Pattern) if (Ch.unicode() < 32 || QStringLiteral("/\\:").contains(Ch)) Valid = false;
            for (const auto& Part : Pattern.split(';')) if (Part.trimmed().isEmpty()) Valid = false;
            if (!Valid) { Copy["copyPattern"] = "*"; Report("options.copyPattern"); }
        }
        if (Copy.contains("copySpeedMiB")) {
            const auto Speed = Copy["copySpeedMiB"].toDouble(-1);
            if (!Copy["copySpeedMiB"].isDouble() || Speed < 0 || Speed > 1048576 || Speed != double(qint64(Speed))) { Copy["copySpeedMiB"] = 0; Report("options.copySpeedMiB"); }
        }
        if (Copy.contains("deleteToRecycle") && !Copy["deleteToRecycle"].isBool()) { Copy["deleteToRecycle"] = true; Report("options.deleteToRecycle"); }
        if (Copy.contains("jobNotification") && !QStringList{"none", "status", "attention", "sound"}.contains(Copy["jobNotification"].toString())) { Copy["jobNotification"] = "attention"; Report("options.jobNotification"); }
        if (Copy.contains("queueInsertion") && !QStringList{"first", "last"}.contains(Copy["queueInsertion"].toString())) { Copy["queueInsertion"] = "last"; Report("options.queueInsertion"); }
        if (Copy.contains("jobConcurrency") && (!Copy["jobConcurrency"].isDouble() || (Copy["jobConcurrency"].toDouble() != 1 && Copy["jobConcurrency"].toDouble() != 2))) { Copy["jobConcurrency"] = 2; Report("options.jobConcurrency"); }
        Document["options"] = Copy;
        auto Favorites = Document["favorites"].toObject();
        if (!QStringList{"none","top","bottom"}.contains(Favorites["drivesOn"].toString())) { Favorites["drivesOn"] = "none"; Report("favorites.drivesOn"); }
        Document["favorites"] = Favorites;
        auto Plugins = Document["plugins"].toObject();
        if (!QStringList{"manual", "automatic", "force"}.contains(Plugins["viewerSelection"].toString())) { Plugins["viewerSelection"] = "manual"; Report("plugins.viewerSelection"); }
        for (const auto* Format : {"wlx","wcx","wfx","wdx","dopusVfs","dopusViewer"}) {
            const auto Entries = Plugins[Format].toArray(); QJsonArray Valid;
            for (const auto& Entry : Entries) {
                auto Record = Entry.toObject();
                if (!Entry.isObject() || !Record["path"].isString() || Record["path"].toString().isEmpty() || Valid.size() >= 1000) { Report(QString("plugins.") + Format); continue; }
                // A malformed approval never becomes an in-process execution request.
                if (Record.contains("hostEnabled") && !Record["hostEnabled"].isBool()) { Record["hostEnabled"] = true; Record["hostReviewed"] = QJsonObject{}; Report(QString("plugins.") + Format + ".hostEnabled"); }
                if (Record.contains("hostReviewed") && !Record["hostReviewed"].isObject()) { Record["hostReviewed"] = QJsonObject{}; Report(QString("plugins.") + Format + ".hostReviewed"); }
                Valid.append(Record);
            }
            Plugins[Format] = Valid;
        }
        Document["plugins"] = Plugins;
        auto Appearance = Document["appearance"].toObject(); auto Schemes = Appearance["schemes"].toObject();
        const QRegularExpression Color("^#[0-9A-Fa-f]{6}$");
        for (auto It = Schemes.begin(); It != Schemes.end(); ++It) {
            auto Record = It.value().toObject(); const auto Prefix = "appearance.schemes." + It.key();
            if (!It.value().isObject()) { Report(Prefix); It.value() = QJsonObject{}; continue; }
            auto Fonts = Record.value("fonts").toObject();
            for (auto Font = Fonts.begin(); Font != Fonts.end(); ++Font) {
                auto Value = Font.value().toObject();
                if (!Value["family"].isString() || Value["family"].toString().isEmpty()) { Value["family"] = "Malgun Gothic"; Report(Prefix + ".fonts." + Font.key() + ".family"); }
                const auto Size = Value["pointSize"].toDouble(-1);
                if (Size < 6 || Size > 72 || Size != int(Size)) { Value["pointSize"] = Font.key() == "list" ? 14 : 9; Report(Prefix + ".fonts." + Font.key() + ".pointSize"); }
                Font.value() = Value;
            }
            if (Record.contains("fonts")) Record["fonts"] = Fonts;
            auto Colors = Record.value("colors").toObject();
            for (auto Entry = Colors.begin(); Entry != Colors.end(); ++Entry)
                if (!Entry.value().isString() || !Color.match(Entry.value().toString()).hasMatch()) {
                    Entry.value() = Entry.key().contains("Background") ? "#FFFFFF" : "#202020"; Report(Prefix + ".colors." + Entry.key());
                }
            if (Record.contains("colors")) Record["colors"] = Colors;
            It.value() = Record;
        }
        Appearance["schemes"] = Schemes; Document["appearance"] = Appearance;
    }
};
inline ConfigPaths Paths() { return ConfigPaths::Resolve(QCoreApplication::arguments(), QCoreApplication::applicationDirPath()); }
inline Repository& AppSettings() { static Repository Instance(Paths().directory_, Paths().legacy_); return Instance; }
// 작업공간은 설정과 같은 HyperCommander.json의 "workspace" 키에 저장한다.
inline JsonStore& WorkspaceSettings()
{
    static JsonStore Instance(ProfileConfig::FileName(Paths().directory_), {{"schemaVersion", 1}}, nullptr, JsonStore::Scope::Standalone,
        ProfilePartition(true, Paths().directory_, Paths().legacy_));
    return Instance;
}
// Document preferences use the same directory but never migrate the commander's legacy locations.
inline ConfigPaths DocumentPaths() { return ConfigPaths::Resolve(QCoreApplication::arguments(), QCoreApplication::applicationDirPath(), {}, true); }
inline QStringList Strings(const QJsonValue& Value)
{ QStringList Result; for (const auto& Item : Value.toArray()) if (Item.isString()) Result.append(Item.toString()); return Result; }
inline QByteArray JsonBytes(const QJsonValue& Value) { return Value.isObject() ? QJsonDocument(Value.toObject()).toJson(QJsonDocument::Compact) : QByteArray{}; }
inline QJsonObject JsonObject(const QByteArray& Value) { return QJsonDocument::fromJson(Value).object(); }
}
