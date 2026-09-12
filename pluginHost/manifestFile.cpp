#include "manifestFile.hpp"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <cmath>

namespace hc::plugins
{
    using namespace storage;
    namespace
    {
        Error Invalid(const char* Code) { return {ErrorCategory::InvalidRequest, Code, {"plugin"}}; }
        const std::map<QString, PluginFormat> Formats{{"wcx", PluginFormat::Wcx}, {"wlx", PluginFormat::Wlx}, {"wfx", PluginFormat::Wfx}, {"wdx", PluginFormat::Wdx}, {"dopus-vfs", PluginFormat::DopusVfs}};
        const std::map<QString, Architecture> Architectures{{"x64", Architecture::X64}, {"x86", Architecture::X86}, {"arm64", Architecture::Arm64}};
        const std::map<QString, ExecutionMode> Executions{{"isolated", ExecutionMode::Isolated}, {"trusted-in-process", ExecutionMode::TrustedInProcess}};
        const std::map<QString, Extension> Extensions{{"archive.read", Extension::ArchiveRead}, {"archive.write", Extension::ArchiveWrite},
            {"storage.read", Extension::StorageRead}, {"storage.write", Extension::StorageWrite}, {"viewer", Extension::Viewer}, {"metadata", Extension::Metadata}};
        const std::map<QString, Permission> Permissions{{"content.read", Permission::ReadContent}, {"content.write", Permission::WriteContent}, {"network", Permission::Network}, {"ui", Permission::UserInterface}};
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
        if (!ExactKeys(Object, {"schemaVersion", "id", "version", "binary", "sha256", "format", "architecture", "execution", "protocol", "extensions", "permissions"})) return Invalid("plugin-manifest-fields");
        PluginManifest Out;
        if (!Dependencies.isUndefined())
        {
            if (!Dependencies.isObject() || Dependencies.toObject().size() > 64) return Invalid("plugin-dependencies");
            const auto Items = Dependencies.toObject();
            for (auto At = Items.begin(); At != Items.end(); ++At)
            { if (!At.value().isString()) return Invalid("plugin-dependencies"); Out.dependencies_.emplace(At.key().toStdString(), At.value().toString().toStdString()); }
        }
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
        return QJsonDocument(Object).toJson();
    }
    Result<InspectedPlugin> InspectManifestFile(const QString& Path, OperationContext Context)
    {
        if (const auto Stop = Context.Check({"plugin"})) return *Stop;
        QFile ManifestFile(Path); if (!ManifestFile.open(QIODevice::ReadOnly)) return Error{ErrorCategory::Unavailable, "plugin-manifest-open", {"plugin"}};
        auto Parsed = ParseManifest(ManifestFile.read(65537)); if (!Parsed.HasValue()) return Parsed.Failure();
        const auto Root = QFileInfo(Path).canonicalPath();
        auto InspectBinary = [&](const std::string& Relative, const std::string& ExpectedHash) -> Result<QString>
        {
        const auto Binary = QFileInfo(QDir(Root).filePath(QString::fromStdString(Relative))).canonicalFilePath();
        const auto Prefix = Root.endsWith('/') ? Root : Root + '/';
        if (Root.isEmpty() || Binary.isEmpty() || !Binary.startsWith(Prefix, Qt::CaseInsensitive)) return Invalid("plugin-binary-outside-root");
        QFile File(Binary); if (!File.open(QIODevice::ReadOnly)) return Error{ErrorCategory::Unavailable, "plugin-binary-open", {"plugin"}};
        constexpr qint64 Limit = 512LL * 1024 * 1024;
        if (File.size() > Limit) return Error{ErrorCategory::BudgetExceeded, "plugin-binary-budget", {"plugin"}};
        const auto Header = File.read(1024 * 1024);
        auto Arch = InspectPe(std::span(reinterpret_cast<const std::byte*>(Header.constData()), Header.size()));
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
        // Reject package drift, including a new adjacent DLL after approval.
        // The host additionally avoids package-directory loader searches.
        for (const auto& File : QDir(QFileInfo(Binary.Value()).absolutePath()).entryInfoList({"*.dll"}, QDir::Files | QDir::Hidden | QDir::System))
            if (!Allowed.contains(File.canonicalFilePath().toCaseFolded())) return Invalid("plugin-unreviewed-adjacent-dll");
        return InspectedPlugin{std::move(Parsed.Value()), Binary.Value(), Dependencies};
    }
}
