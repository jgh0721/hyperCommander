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

namespace hc::settings {
struct ConfigPaths {
    QString directory_, error_;
    static ConfigPaths Resolve(const QStringList& Arguments, const QString& ExecutableDirectory,
        const QString& UserDirectory = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation))
    {
        QString Explicit; bool Portable = false, Seen = false;
        for (int Index = 1; Index < Arguments.size(); ++Index) {
            const auto& Arg = Arguments[Index];
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
        const auto Path = Seen ? Explicit : Portable ? QDir(ExecutableDirectory).filePath("config") : UserDirectory;
        if (Path.isEmpty()) return {{}, "unavailable-config-dir"};
        return {QDir::cleanPath(QFileInfo(Path).absoluteFilePath()), {}};
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
inline QJsonObject Defaults()
{
    // Compiled JSON defaults are available before widgets and without a writable installation.
    return QJsonDocument::fromJson(R"({"schemaVersion":1,
        "general":{"sizeInHeader":"bytes","sizeInFooter":"bytes","useBuiltInShortcuts":true},
        "appearance":{"selected":"","schemes":{}},"shortcuts":{},
        "favorites":{"drivesOn":"none","items":[]},"fileSets":[],"editors":[],
        "plugins":{"viewerSelection":"manual","wlx":[],"wcx":[],"wfx":[],"wdx":[],"dopusVfs":[],"dopusViewer":[]},
        "columns":{},"automation":{"version":2,"scripts":[]},"options":{}})").object();
}
inline QJsonObject MergeDefaults(QJsonObject Value, const QJsonObject& Base, const QString& Path, QStringList& Issues)
{
    for (auto It = Base.begin(); It != Base.end(); ++It) {
        const auto Key = Path.isEmpty() ? It.key() : Path + '.' + It.key();
        if (!Value.contains(It.key())) Value[It.key()] = It.value();
        else if (Value.value(It.key()).type() != It.value().type()) {
            Issues.append(Key); Value[It.key()] = It.value();
        } else if (It.value().isObject()) Value[It.key()] = MergeDefaults(Value.value(It.key()).toObject(), It.value().toObject(), Key, Issues);
    }
    return Value;
}

class JsonStore : public QObject {
public:
    explicit JsonStore(QString FileName, QJsonObject Defaults = {{"schemaVersion", 1}}, QObject* Parent = nullptr)
        : QObject(Parent), path_(QFileInfo(FileName).absoluteFilePath()), defaults_(std::move(Defaults)) { Reload(); }
    const QString& FileName() const { return path_; }
    const QString& Error() const { return error_; }
    const QStringList& Issues() const { return issues_; }
    bool Writable() const { return writable_; }
    bool Exists() const { return exists_; }
    QJsonObject Object() const { return document_; }
    bool Editing() const { return editing_; }
    void SetErrorHandler(std::function<void(const QString&)> Handler) { errorHandler_ = std::move(Handler); }
    bool Reload()
    {
        editing_ = false; error_.clear(); issues_.clear(); writable_ = true; document_ = defaults_;
        if (!read(original_, exists_)) { writable_ = false; return fail("read-failed"); }
        if (!exists_) return true;
        QJsonParseError Parse;
        const auto Doc = QJsonDocument::fromJson(original_, &Parse);
        if (Parse.error != QJsonParseError::NoError || !Doc.isObject()) { writable_ = false; return fail("invalid-json"); }
        if (Doc.object().value("schemaVersion") != QJsonValue(1)) { writable_ = false; return fail("unsupported-schema-version"); }
        document_ = MergeDefaults(Doc.object(), defaults_, {}, issues_);
        normalize(document_, issues_);
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
        if (document_.value("schemaVersion") != QJsonValue(1)) return fail("invalid-schema-version");
        QStringList Problems;
        auto Validated = MergeDefaults(document_, defaults_, {}, Problems);
        if (!Problems.isEmpty()) return fail("invalid-fields: " + Problems.join(", "));
        normalize(Validated, Problems);
        if (!Problems.isEmpty() || Validated != document_) return fail("invalid-fields");
        if (!QDir().mkpath(QFileInfo(path_).absolutePath())) return fail("create-directory-failed");
        QLockFile Lock(path_ + ".lock"); Lock.setStaleLockTime(0);
        if (!Lock.tryLock(0)) return fail("file-locked");
        QByteArray Current; bool Exists = false;
        if (!read(Current, Exists)) return fail("read-before-save-failed");
        // Compare the bytes, including whitespace, under the same lock used by every writer.
        if (Exists != exists_ || QCryptographicHash::hash(Current, QCryptographicHash::Sha256)
            != QCryptographicHash::hash(original_, QCryptographicHash::Sha256)) return fail("changed-on-disk");
        auto Bytes = QJsonDocument(document_).toJson(QJsonDocument::Indented);
        Bytes.replace("\n", "\r\n");
        if (Bytes.size() > maxBytes_) return fail("document-too-large");
        QSaveFile File(path_); File.setDirectWriteFallback(false);
        if (!File.open(QIODevice::WriteOnly) || File.write(Bytes) != Bytes.size() || !File.commit()) return fail("atomic-save-failed");
        original_ = Bytes; exists_ = true; error_.clear(); issues_.clear(); return true;
    }
protected:
    virtual void normalize(QJsonObject&, QStringList&) {}
private:
    bool read(QByteArray& Bytes, bool& Exists) const
    {
        QFile File(path_); Exists = QFileInfo::exists(path_);
        if (!Exists) { Bytes.clear(); return true; }
        if (!File.open(QIODevice::ReadOnly) || File.size() > maxBytes_) return false;
        Bytes = File.read(maxBytes_ + 1); return File.error() == QFileDevice::NoError && Bytes.size() <= maxBytes_;
    }
    bool fail(const QString& Code)
    {
        error_ = path_ + ": " + Code;
        if (errorHandler_) errorHandler_(error_);
        return false;
    }
    static constexpr qint64 maxBytes_ = 16 * 1024 * 1024;
    QString path_, error_;
    QJsonObject defaults_, document_;
    QByteArray original_;
    QStringList issues_;
    bool exists_ = false, writable_ = true, editing_ = false;
    std::function<void(const QString&)> errorHandler_;
};

class Repository : public JsonStore {
public:
    explicit Repository(const QString& Directory) : JsonStore(QDir(Directory).filePath("settings.json"), Defaults()) { Reload(); }
    QJsonValue Read(Section Domain) const { return Object().value(SectionName(Domain)); }
    bool Put(Section Domain, const QJsonValue& Value)
    { auto Next = Object(); Next[SectionName(Domain)] = Value; return Replace(Next); }
    QJsonArray Plugins(const QString& Format) const { return Read(Section::Plugins).toObject().value(Format).toArray(); }
    bool SetPlugins(const QString& Format, const QJsonArray& Items)
    { auto Next = Read(Section::Plugins).toObject(); Next[Format] = Items; return Put(Section::Plugins, Next); }
    int SizeStyle(bool Header) const
    { return qMax(0, SizeFormats().indexOf(Read(Section::General).toObject().value(Header ? "sizeInHeader" : "sizeInFooter").toString())); }
protected:
    void normalize(QJsonObject& Document, QStringList& Issues) override
    {
        const auto Report = [&](const QString& Path) { if (!Issues.contains(Path)) Issues.append(Path); };
        auto General = Document["general"].toObject();
        for (const auto* Key : {"sizeInHeader", "sizeInFooter"})
            if (!SizeFormats().contains(General[Key].toString())) { General[Key] = "bytes"; Report(QString("general.") + Key); }
        Document["general"] = General;
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
inline Repository& AppSettings() { static Repository Instance(Paths().directory_); return Instance; }
inline JsonStore& WorkspaceSettings() { static JsonStore Instance(QDir(Paths().directory_).filePath("workspace.json")); return Instance; }
inline QStringList Strings(const QJsonValue& Value)
{ QStringList Result; for (const auto& Item : Value.toArray()) if (Item.isString()) Result.append(Item.toString()); return Result; }
inline QByteArray JsonBytes(const QJsonValue& Value) { return Value.isObject() ? QJsonDocument(Value.toObject()).toJson(QJsonDocument::Compact) : QByteArray{}; }
inline QJsonObject JsonObject(const QByteArray& Value) { return QJsonDocument::fromJson(Value).object(); }
}
