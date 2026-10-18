#include "manifestFile.hpp"
#include "commonLibs/layout/productLayout.hpp"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QSaveFile>
#include <cmath>

namespace hc::plugins
{
    using namespace storage;
    namespace
    {
        Error Invalid(const char* Code) { return {ErrorCategory::InvalidRequest, Code, {"plugin"}}; }
        const std::map<QString, PluginFormat> Formats{{"unegg", PluginFormat::UnEgg}, {"7zip", PluginFormat::SevenZip}, {"wcx", PluginFormat::Wcx}, {"wlx", PluginFormat::Wlx}, {"wfx", PluginFormat::Wfx}, {"wdx", PluginFormat::Wdx}, {"dopus-vfs", PluginFormat::DopusVfs}, {"dopus-viewer", PluginFormat::DopusViewer}};
        const std::map<QString, Architecture> Architectures{{"x64", Architecture::X64}, {"x86", Architecture::X86}, {"arm64", Architecture::Arm64}};
        const std::map<QString, ExecutionMode> Executions{{"isolated", ExecutionMode::Isolated}, {"trusted-in-process", ExecutionMode::TrustedInProcess}};
        const std::map<QString, Extension> Extensions{{"archive.read", Extension::ArchiveRead}, {"archive.write", Extension::ArchiveWrite},
            {"storage.read", Extension::StorageRead}, {"storage.write", Extension::StorageWrite}, {"viewer", Extension::Viewer}, {"metadata", Extension::Metadata}, {"content.convert", Extension::ContentConvert}};
        const std::map<QString, Permission> Permissions{{"content.read", Permission::ReadContent}, {"content.write", Permission::WriteContent}, {"network", Permission::Network}, {"ui", Permission::UserInterface}, {"process.child", Permission::ChildProcess}};
        bool ExactKeys(const QJsonObject& Object, const QStringList& Keys)
        { return Object.size() == Keys.size() && std::ranges::all_of(Keys, [&](const auto& Key) { return Object.contains(Key); }); }
        bool Integer(const QJsonValue& Value, std::uint32_t& Out)
        {
            if (!Value.isDouble()) return false;
            const auto Number = Value.toDouble();
            if (!std::isfinite(Number) || Number < 0 || Number > UINT32_MAX || Number != std::floor(Number)) return false;
            Out = static_cast<std::uint32_t>(Number); return true;
        }
        template<class T> bool EnumValue(const QJsonValue& Value, const std::map<QString, T>& Values, T& Out)
        {
            if (!Value.isString()) return false;
            const auto At = Values.find(Value.toString()); if (At == Values.end()) return false;
            Out = At->second; return true;
        }
        template<class T> bool EnumSet(const QJsonValue& Value, const std::map<QString, T>& Values, std::set<T>& Out)
        {
            if (!Value.isArray() || Value.toArray().size() > static_cast<qsizetype>(Values.size())) return false;
            for (const auto Item : Value.toArray()) { T Parsed{}; if (!EnumValue(Item, Values, Parsed) || !Out.insert(Parsed).second) return false; }
            return true;
        }
        template<class T> QString Name(T Value, const std::map<QString, T>& Values)
        { for (const auto& [Key, Item] : Values) if (Item == Value) return Key; return {}; }
        template<class T> QJsonArray Names(const std::set<T>& Items, const std::map<QString, T>& Values)
        { QJsonArray Out; for (auto Item : Items) Out.append(Name(Item, Values)); return Out; }
        bool UniqueKeys(const QByteArray& Json)
        {
            // QJsonDocument accepts repeated keys. Inspect lexical key tokens in
            // already-valid JSON so escaped spellings cannot shadow a field.
            std::vector<QSet<QString>> Objects;
            for (qsizetype At = 0; At < Json.size(); ++At)
            {
                if (Json[At] == '{') { if (Objects.size() >= 16) return false; Objects.emplace_back(); }
                else if (Json[At] == '}') { if (Objects.empty()) return false; Objects.pop_back(); }
                else if (Json[At] == '"')
                {
                    const auto Start = At;
                    while (++At < Json.size()) { if (Json[At] == '\\') ++At; else if (Json[At] == '"') break; }
                    auto Next = At + 1; while (Next < Json.size() && QByteArray(" \r\n\t").contains(Json[Next])) ++Next;
                    if (Next < Json.size() && Json[Next] == ':')
                    {
                        if (Objects.empty()) return false;
                        const auto Key = QJsonDocument::fromJson("[" + Json.mid(Start, At - Start + 1) + "]").array().at(0).toString();
                        if (Objects.back().contains(Key)) return false;
                        Objects.back().insert(Key);
                    }
                }
            }
            return Objects.empty();
        }
    }
    Result<PluginManifest> ParseManifest(const QByteArray& Json)
    {
        if (Json.size() > 65536) return Error{ErrorCategory::BudgetExceeded, "plugin-manifest-budget", {"plugin"}};
        QJsonParseError Parse; const auto Document = QJsonDocument::fromJson(Json, &Parse);
        if (Parse.error != QJsonParseError::NoError || !Document.isObject()) return Invalid("plugin-manifest-json");
        if (!UniqueKeys(Json)) return Invalid("plugin-manifest-duplicate-key");
        auto Object = Document.object();
        const auto Dependencies = Object.take("dependencies");
        const auto Helpers = Object.take("helpers");
        const auto Compatibility = Object.take("compatibility");
        const auto PackageRoot = Object.take("packageRoot");
        if (!ExactKeys(Object, {"schemaVersion", "id", "version", "binary", "sha256", "format", "architecture", "execution", "protocol", "extensions", "permissions"})) return Invalid("plugin-manifest-fields");
        PluginManifest Out;
        if (!PackageRoot.isUndefined()) {
            if (!PackageRoot.isString() || PackageRoot.toString().contains(QChar::Null)
                || PackageRoot.toString().size() > 32768 || !QDir::isAbsolutePath(PackageRoot.toString())) return Invalid("plugin-package-root");
            Out.packageRoot_ = PackageRoot.toString().toStdString();
        }
        if (!Dependencies.isUndefined())
        {
            if (!Dependencies.isObject() || Dependencies.toObject().size() > 64) return Invalid("plugin-dependencies");
            const auto Items = Dependencies.toObject();
            for (auto At = Items.begin(); At != Items.end(); ++At)
            { if (!At.value().isString()) return Invalid("plugin-dependencies"); Out.dependencies_.emplace(At.key().toStdString(), At.value().toString().toStdString()); }
        }
        if (!Helpers.isUndefined())
        {
            if (!Helpers.isObject() || Helpers.toObject().size() > 1) return Invalid("plugin-helpers");
            const auto Items = Helpers.toObject();
            for (auto At = Items.begin(); At != Items.end(); ++At)
            { if (!At.value().isString()) return Invalid("plugin-helpers"); Out.helpers_.emplace(At.key().toStdString(), At.value().toString().toStdString()); }
        }
        if (!Compatibility.isUndefined())
        { if (!Compatibility.isString()) return Invalid("plugin-compatibility"); Out.compatibility_ = Compatibility.toString().toStdString(); }
        if (!Integer(Object["schemaVersion"], Out.schemaVersion_)) return Invalid("plugin-manifest-version");
        for (const auto& Pair : {std::pair{"id", &Out.id_}, {"version", &Out.version_}, {"binary", &Out.binary_}, {"sha256", &Out.sha256_}})
        { if (!Object[Pair.first].isString()) return Invalid("plugin-manifest-string"); *Pair.second = Object[Pair.first].toString().toStdString(); }
        if (!EnumValue(Object["format"], Formats, Out.format_) || !EnumValue(Object["architecture"], Architectures, Out.architecture_)
            || !EnumValue(Object["execution"], Executions, Out.execution_) || !EnumSet(Object["extensions"], Extensions, Out.extensions_)
            || !EnumSet(Object["permissions"], Permissions, Out.permissions_)) return Invalid("plugin-manifest-enum");
        if (!Object["protocol"].isObject()) return Invalid("plugin-protocol-range");
        const auto Protocol = Object["protocol"].toObject();
        if (!ExactKeys(Protocol, {"major", "minimumMinor", "maximumMinor"}) || !Integer(Protocol["major"], Out.protocol_.major_)
            || !Integer(Protocol["minimumMinor"], Out.protocol_.minimumMinor_) || !Integer(Protocol["maximumMinor"], Out.protocol_.maximumMinor_)) return Invalid("plugin-protocol-range");
        auto Valid = Validate(Out); if (!Valid.HasValue()) return Valid.Failure(); return Out;
    }
    QByteArray EncodeManifest(const PluginManifest& Manifest)
    {
        const auto S = [](const std::string& Value) { return QString::fromStdString(Value); };
        QJsonObject Object{{"schemaVersion", static_cast<qint64>(Manifest.schemaVersion_)}, {"id", S(Manifest.id_)}, {"version", S(Manifest.version_)},
            {"binary", S(Manifest.binary_)}, {"sha256", S(Manifest.sha256_)}, {"format", Name(Manifest.format_, Formats)}, {"architecture", Name(Manifest.architecture_, Architectures)},
            {"execution", Name(Manifest.execution_, Executions)}, {"extensions", Names(Manifest.extensions_, Extensions)}, {"permissions", Names(Manifest.permissions_, Permissions)},
            {"protocol", QJsonObject{{"major", static_cast<qint64>(Manifest.protocol_.major_)}, {"minimumMinor", static_cast<qint64>(Manifest.protocol_.minimumMinor_)}, {"maximumMinor", static_cast<qint64>(Manifest.protocol_.maximumMinor_)}}}};
        if (!Manifest.dependencies_.empty())
        { QJsonObject Dependencies; for (const auto& [Path, Hash] : Manifest.dependencies_) Dependencies.insert(S(Path), S(Hash)); Object.insert("dependencies", Dependencies); }
        if (!Manifest.helpers_.empty())
        { QJsonObject Helpers; for (const auto& [Path, Hash] : Manifest.helpers_) Helpers.insert(S(Path), S(Hash)); Object.insert("helpers", Helpers); }
        if (!Manifest.compatibility_.empty()) Object.insert("compatibility", S(Manifest.compatibility_));
        if (!Manifest.packageRoot_.empty()) Object.insert("packageRoot", S(Manifest.packageRoot_));
        return QJsonDocument(Object).toJson();
    }
    static Result<InspectedPlugin> InspectDeclared(const PluginManifest& Manifest, const QString& Directory, OperationContext Context)
    {
        if (const auto Stop = Context.Check({"plugin"})) return *Stop;
        auto Parsed = ParseManifest(EncodeManifest(Manifest)); if (!Parsed.HasValue()) return Parsed.Failure();
        const auto Root = QFileInfo(Directory).canonicalFilePath();
        auto InspectBinary = [&](const std::string& Relative, const std::string& ExpectedHash, bool Executable = false) -> Result<QString>
        {
        const auto Binary = QFileInfo(QDir(Root).filePath(QString::fromStdString(Relative))).canonicalFilePath();
        const auto Prefix = Root.endsWith('/') ? Root : Root + '/';
        if (Root.isEmpty() || Binary.isEmpty() || !Binary.startsWith(Prefix, Qt::CaseInsensitive)) return Invalid("plugin-binary-outside-root");
        QFile File(Binary); if (!File.open(QIODevice::ReadOnly)) return Error{ErrorCategory::Unavailable, "plugin-binary-open", {"plugin"}};
        constexpr qint64 Limit = 512LL * 1024 * 1024;
        if (File.size() > Limit) return Error{ErrorCategory::BudgetExceeded, "plugin-binary-budget", {"plugin"}};
        const auto Header = File.read(1024 * 1024);
        auto Arch = InspectPe(std::span(reinterpret_cast<const std::byte*>(Header.constData()), Header.size()), Executable);
        if (!Arch.HasValue()) return Arch.Failure();
        if (Arch.Value() != Parsed.Value().architecture_) return Invalid("plugin-binary-architecture-mismatch");
        if (!File.seek(0)) return Error{ErrorCategory::Unavailable, "plugin-binary-read", {"plugin"}};
        QCryptographicHash Hash(QCryptographicHash::Sha256); qint64 Bytes = 0;
        while (!File.atEnd())
        {
            if (const auto Stop = Context.Check({"plugin"})) return *Stop;
            const auto Chunk = File.read(65536);
            if (Chunk.isEmpty() || File.error() != QFileDevice::NoError) return Error{ErrorCategory::Unavailable, "plugin-binary-read", {"plugin"}};
            Bytes += Chunk.size(); if (Bytes > Limit) return Error{ErrorCategory::BudgetExceeded, "plugin-binary-budget", {"plugin"}};
            Hash.addData(Chunk);
        }
        if (Hash.result().toHex().toStdString() != ExpectedHash) return Invalid("plugin-binary-hash-mismatch");
        if (const auto Stop = Context.Check({"plugin"})) return *Stop;
        return Binary;
        };
        auto Binary = InspectBinary(Parsed.Value().binary_, Parsed.Value().sha256_); if (!Binary.HasValue()) return Binary.Failure();
        QStringList Dependencies;
        QSet<QString> Allowed{Binary.Value().toCaseFolded()};
        for (const auto& [Relative, Hash] : Parsed.Value().dependencies_)
        {
            auto Dependency = InspectBinary(Relative, Hash); if (!Dependency.HasValue()) return Dependency.Failure();
            if (Allowed.contains(Dependency.Value().toCaseFolded())) return Invalid("plugin-dependency-alias");
            Allowed.insert(Dependency.Value().toCaseFolded()); Dependencies.push_back(Dependency.Value());
        }
        QStringList Helpers;
        for (const auto& [Relative, Hash] : Parsed.Value().helpers_)
        {
            auto Helper = InspectBinary(Relative, Hash, true); if (!Helper.HasValue()) return Helper.Failure();
            if (Allowed.contains(Helper.Value().toCaseFolded())) return Invalid("plugin-helper-alias");
            Allowed.insert(Helper.Value().toCaseFolded()); Helpers.push_back(Helper.Value());
            // The built-in viewer's helper sits in the product ROOT, the directory the host itself
            // loads from; every DLL the helper imports from there is pinned as a dependency.
            if (Parsed.Value().compatibility_ == "hyperviewer-v1"
                && QFileInfo(Helper.Value()).absolutePath().compare(Root, Qt::CaseInsensitive) == 0) continue;
            // The helper has its own loader: also reject unreviewed adjacent DLLs.
            for (const auto& File : QDir(QFileInfo(Helper.Value()).absolutePath()).entryInfoList({"*.dll"}, QDir::Files | QDir::Hidden | QDir::System))
                if (!Allowed.contains(File.canonicalFilePath().toCaseFolded())) return Invalid("plugin-unreviewed-helper-dll");
        }
        // Reject package drift, including a new adjacent DLL after approval.
        // The host additionally avoids package-directory loader searches.
        for (const auto& File : QDir(QFileInfo(Binary.Value()).absolutePath()).entryInfoList({"*.dll"}, QDir::Files | QDir::Hidden | QDir::System))
            if (!Allowed.contains(File.canonicalFilePath().toCaseFolded())) return Invalid("plugin-unreviewed-adjacent-dll");
        return InspectedPlugin{std::move(Parsed.Value()), Binary.Value(), Dependencies, Helpers};
    }
    Result<InspectedPlugin> InspectManifestFile(const QString& Path, OperationContext Context)
    {
        if (const auto Stop = Context.Check({"plugin"})) return *Stop;
        QFile File(Path); if (!File.open(QIODevice::ReadOnly)) return Error{ErrorCategory::Unavailable, "plugin-manifest-open", {"plugin"}};
        auto Parsed = ParseManifest(File.read(65537)); if (!Parsed.HasValue()) return Parsed.Failure();
        auto Root = Parsed.Value().packageRoot_.empty() ? QFileInfo(Path).canonicalPath() : QString::fromStdString(Parsed.Value().packageRoot_);
        // The built-in viewer manifest is valid only in ROOT/<HyperViewerPackage>; its package root is ROOT.
        if (Parsed.Value().compatibility_ == "hyperviewer-v1" && Parsed.Value().packageRoot_.empty()) {
            const auto Package = QStringLiteral("/") + hc::layout::HyperViewerPackage;
            if (!Root.endsWith(Package, Qt::CaseInsensitive)) return Invalid("plugin-package-root");
            Root.chop(Package.size());
        }
        return InspectDeclared(Parsed.Value(), Root, Context);
    }
    bool PluginPackageChanged(const std::string& Code)
    {
        return Code == "plugin-binary-hash-mismatch" || Code == "plugin-unreviewed-adjacent-dll"
            || Code == "plugin-unreviewed-helper-dll" || Code == "plugin-review-required"
            || Code == "plugin-binary-open" || Code == "plugin-binary-outside-root"
            || Code == "plugin-manifest-open" || Code == "plugin-configured-binary-mismatch"
            || Code == "plugin-binary-architecture-mismatch";
    }
    Result<InspectedPlugin> InspectPluginBinary(const QString& Binary, PluginFormat Format,
        std::optional<PluginManifest> Previous, OperationContext Context)
    {
        const auto Path = QFileInfo(Binary).canonicalFilePath();
        if (Path.isEmpty()) return Error{ErrorCategory::Unavailable, "plugin-binary-open", {"plugin"}};
        PluginManifest Manifest;
        if (Previous && Previous->format_ == Format) Manifest = *Previous;
        else {
            Manifest.id_ = "user." + QCryptographicHash::hash(Path.toCaseFolded().toUtf8(), QCryptographicHash::Sha256).toHex().left(24).toStdString();
            Manifest.format_ = Format; Manifest.permissions_ = {Permission::ReadContent};
            switch (Format) {
            case PluginFormat::Wlx: case PluginFormat::DopusViewer: Manifest.extensions_ = {Extension::Viewer}; Manifest.permissions_.insert(Permission::UserInterface); break;
            case PluginFormat::Wcx: Manifest.extensions_ = {Extension::ArchiveRead, Extension::ArchiveWrite}; Manifest.permissions_.insert(Permission::WriteContent); break;
            case PluginFormat::Wfx: case PluginFormat::DopusVfs: Manifest.extensions_ = {Extension::StorageRead, Extension::StorageWrite}; Manifest.permissions_.insert(Permission::WriteContent); Manifest.permissions_.insert(Permission::UserInterface); break;
            case PluginFormat::Wdx: Manifest.extensions_ = {Extension::Metadata}; break;
            default: return Invalid("plugin-format");
            }
        }
        QString Root = QFileInfo(Path).absolutePath();
        if (Previous && Path.endsWith('/' + QString::fromStdString(Previous->binary_), Qt::CaseInsensitive))
            Root = QDir::cleanPath(Path.left(Path.size() - QString::fromStdString(Previous->binary_).size()));
        Manifest.packageRoot_ = Root.toStdString(); Manifest.binary_ = QDir(Root).relativeFilePath(Path).toStdString();
        if (Format == PluginFormat::Wcx && Manifest.extensions_.contains(Extension::ArchiveWrite)
            && QFileInfo(Path).fileName().compare("Total7zip.wcx64", Qt::CaseInsensitive) == 0) {
            Manifest.compatibility_ = "total7zip"; Manifest.permissions_.insert(Permission::UserInterface); Manifest.permissions_.insert(Permission::ChildProcess);
            Manifest.helpers_["64/7zG.exe"] = {}; Manifest.dependencies_["64/7z.dll"] = {};
        }
        // An update may remove a formerly adjacent dependency. Pin the files
        // that are actually in the new package rather than an obsolete list.
        std::erase_if(Manifest.dependencies_, [&](const auto& Item) { return !QFileInfo::exists(QDir(Root).filePath(QString::fromStdString(Item.first))); });
        const auto HashFile = [&](const QString& At, bool Executable = false) -> Result<std::string> {
            if (const auto Stop = Context.Check({"plugin"})) return *Stop;
            const auto Canonical = QFileInfo(At).canonicalFilePath();
            if (!Canonical.startsWith(Root.endsWith('/') ? Root : Root + '/', Qt::CaseInsensitive)) return Invalid("plugin-binary-outside-root");
            QFile File(Canonical); if (!File.open(QIODevice::ReadOnly)) return Error{ErrorCategory::Unavailable, "plugin-binary-open", {"plugin"}};
            if (File.size() > 512LL * 1024 * 1024) return Error{ErrorCategory::BudgetExceeded, "plugin-binary-budget", {"plugin"}};
            const auto Header = File.peek(1024 * 1024);
            auto Arch = InspectPe(std::span(reinterpret_cast<const std::byte*>(Header.constData()), Header.size()), Executable);
            if (!Arch.HasValue()) return Arch.Failure();
            if (Arch.Value() != Architecture::X64) return Invalid("plugin-binary-architecture-mismatch");
            QCryptographicHash Hash(QCryptographicHash::Sha256);
            while (!File.atEnd()) {
                if (const auto Stop = Context.Check({"plugin"})) return *Stop;
                const auto Bytes = File.read(65536); if (Bytes.isEmpty() || File.error() != QFileDevice::NoError) return Invalid("plugin-binary-read");
                Hash.addData(Bytes);
            }
            return Hash.result().toHex().toStdString();
        };
        auto Hash = HashFile(Path); if (!Hash.HasValue()) return Hash.Failure();
        Manifest.sha256_ = Hash.Value(); Manifest.version_ = "sha256-" + Hash.Value().substr(0, 16); Manifest.architecture_ = Architecture::X64;
        const auto AddDlls = [&](const QString& At) {
            for (const auto& File : QDir(At).entryInfoList({"*.dll"}, QDir::Files | QDir::Hidden | QDir::System)) {
                const auto Relative = QDir(Root).relativeFilePath(File.absoluteFilePath()).toStdString();
                if (QString::fromStdString(Relative).compare(QString::fromStdString(Manifest.binary_), Qt::CaseInsensitive) != 0
                    && std::ranges::none_of(Manifest.dependencies_, [&](const auto& Item) { return QString::fromStdString(Item.first).compare(QString::fromStdString(Relative), Qt::CaseInsensitive) == 0; }))
                    Manifest.dependencies_.try_emplace(Relative, std::string{});
            }
        };
        AddDlls(QFileInfo(Path).absolutePath());
        for (const auto& [Name, Hash] : Manifest.helpers_) AddDlls(QFileInfo(QDir(Root).filePath(QString::fromStdString(Name))).absolutePath());
        if (Manifest.dependencies_.size() > 64) return Invalid("plugin-dependency-budget");
        for (auto& [Name, Hash] : Manifest.dependencies_) { auto Fresh = HashFile(QDir(Root).filePath(QString::fromStdString(Name))); if (!Fresh.HasValue()) return Fresh.Failure(); Hash = Fresh.Value(); }
        for (auto& [Name, Hash] : Manifest.helpers_) { auto Fresh = HashFile(QDir(Root).filePath(QString::fromStdString(Name)), true); if (!Fresh.HasValue()) return Fresh.Failure(); Hash = Fresh.Value(); }
        return InspectDeclared(Manifest, Root, Context);
    }
    Result<QString> SavePluginManifest(const InspectedPlugin& Plugin, const QString& Directory, OperationContext Context)
    {
        auto Checked = InspectDeclared(Plugin.manifest_, QString::fromStdString(Plugin.manifest_.packageRoot_), Context);
        if (!Checked.HasValue()) return Checked.Failure();
        const auto Bytes = EncodeManifest(Plugin.manifest_);
        if (!QDir().mkpath(Directory)) return Invalid("plugin-manifest-directory");
        // Content-addressed files keep a cancelled settings draft from replacing
        // the declaration referenced by the last committed approval.
        const auto Path = QDir(Directory).filePath(QString::fromLatin1(QCryptographicHash::hash(Bytes, QCryptographicHash::Sha256).toHex()) + ".json");
        QSaveFile File(Path);
        if (!File.open(QIODevice::WriteOnly) || File.write(Bytes) != Bytes.size() || !File.commit()) return Invalid("plugin-manifest-save");
        return Path;
    }
}
