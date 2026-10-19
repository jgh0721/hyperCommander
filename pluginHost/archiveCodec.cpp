#include "archiveCodec.hpp"
#include <QJsonArray>
#include "commonLibs/plugins/pluginWire.hpp"

namespace hc::plugins
{
    using namespace storage;
    namespace
    {
        Error Invalid() { return {ErrorCategory::InvalidRequest, "host-archive-payload", {"plugin-host"}}; }
        QString Text(const std::string& Value) { return QString::fromStdString(Value); }
        bool String(const QJsonValue& Value, std::size_t Limit)
        { return Value.isString() && !Value.toString().contains(QChar::Null) && Value.toString().size() <= static_cast<qsizetype>(Limit); }
    }
    Result<std::uint64_t> WireUnsigned(const QJsonValue& Value)
    {
        if (!Value.isString()) return Invalid(); const auto Text = Value.toString();
        if (Text.isEmpty() || Text.size() > 20 || !std::ranges::all_of(Text, [](QChar Ch) { return Ch >= '0' && Ch <= '9'; })) return Invalid();
        bool Ok = false; const auto Number = Text.toULongLong(&Ok); if (!Ok) return Invalid(); return std::uint64_t(Number);
    }
    QJsonObject EncodeArchiveHeader(const ArchiveHeader& Header)
    {
        QJsonObject Out{{"name", Text(Header.name_)}, {"size", QString::number(Header.size_)}, {"packedSize", QString::number(Header.packedSize_)},
            {"attributes", QString::number(Header.attributes_)}, {"crc", QString::number(Header.crc_)}, {"modifiedUtcMs", QString::number(Header.modifiedUtcMs_)}, {"directory", Header.directory_}, {"encrypted", Header.encrypted_}, {"rebuildEncrypted", Header.rebuildEncrypted_}};
        if (Header.metadataRename_) Out["metadataRename"] = true;
        return Out;
    }
    Result<ArchiveHeader> DecodeArchiveHeader(const QJsonObject& Object)
    {
        if (Object.size() != 9 + Object.contains("metadataRename") || (Object.contains("metadataRename") && !Object["metadataRename"].isBool()) || !Object["rebuildEncrypted"].isBool() || !String(Object["name"], 1024) || Object["name"].toString().isEmpty() || !Object["directory"].isBool() || !Object["encrypted"].isBool() || !Object["modifiedUtcMs"].isString()) return Invalid();
        auto Size = WireUnsigned(Object["size"]), Packed = WireUnsigned(Object["packedSize"]), Attributes = WireUnsigned(Object["attributes"]), Crc = WireUnsigned(Object["crc"]);
        if (!Size.HasValue() || !Packed.HasValue() || !Attributes.HasValue() || !Crc.HasValue() || Attributes.Value() > UINT32_MAX || Crc.Value() > UINT32_MAX) return Invalid();
        bool Ok = false; auto Time = Object["modifiedUtcMs"].toString().toLongLong(&Ok); if (!Ok) return Invalid();
        return ArchiveHeader{Object["name"].toString().toStdString(), Size.Value(), Packed.Value(), static_cast<std::uint32_t>(Attributes.Value()), static_cast<std::uint32_t>(Crc.Value()), Time, Object["directory"].toBool(), Object["encrypted"].toBool(), Object["rebuildEncrypted"].toBool(), Object["metadataRename"].toBool()};
    }
    QJsonObject EncodeSevenZipSettings(const SevenZipCreateSettings& Settings)
    {
        const auto Optional = [](auto Value) { return Value ? QJsonValue(QString::number(*Value)) : QJsonValue(QJsonValue::Null); };
        return {{"version", 1}, {"level", int(Settings.level_)}, {"method", int(Settings.method_)},
            {"dictionary", Optional(Settings.dictionaryBytes_)}, {"word", Optional(Settings.wordSize_)}, {"threads", Optional(Settings.threads_)},
            {"solidMode", int(Settings.solid_)}, {"solidBytes", Optional(Settings.solidBlockBytes_)},
            {"encrypt", Settings.encrypt_}, {"header", Settings.encryptHeaders_}};
    }
    Result<SevenZipCreateSettings> DecodeSevenZipSettings(const QJsonObject& Object)
    {
        if (Object.size() != 10 || Object["version"] != 1 || !Object["encrypt"].isBool() || !Object["header"].isBool()) return Invalid();
        SevenZipCreateSettings Out;
        const auto Level = Object["level"].toInt(-1), Method = Object["method"].toInt(-1), Solid = Object["solidMode"].toInt(-1);
        if (Level < 0 || Level > 9 || Object["level"].toDouble(-1) != Level || Method < 0 || Method > 7
            || Object["method"].toDouble(-1) != Method || Solid < 0 || Solid > 2 || Object["solidMode"].toDouble(-1) != Solid) return Invalid();
        Out.level_ = unsigned(Level); Out.method_ = SevenZipMethod(Method); Out.solid_ = ArchiveSolidMode(Solid);
        Out.encrypt_ = Object["encrypt"].toBool(); Out.encryptHeaders_ = Object["header"].toBool();
        for (const auto& [Key, Target] : {std::pair{"dictionary", &Out.dictionaryBytes_}, {"solidBytes", &Out.solidBlockBytes_}}) {
            if (Object[Key].isNull()) continue; auto Value = WireUnsigned(Object[Key]);
            if (!Value.HasValue() || !Value.Value()) return Invalid(); *Target = Value.Value();
        }
        for (const auto& [Key, Target] : {std::pair{"word", &Out.wordSize_}, {"threads", &Out.threads_}}) {
            if (Object[Key].isNull()) continue; auto Value = WireUnsigned(Object[Key]);
            if (!Value.HasValue() || !Value.Value() || Value.Value() > UINT_MAX) return Invalid(); *Target = unsigned(Value.Value());
        }
        return Out;
    }
    QJsonObject EncodeArchiveUpdate(const ArchiveUpdate& Update)
    {
        QJsonArray Added, Removed; for (const auto& Name : Update.add_) Added.append(Text(Name)); for (const auto& Name : Update.remove_) Removed.append(Text(Name));
        QJsonObject Object{{"archive", Text(Update.archive_)}, {"inputDirectory", Text(Update.inputDirectory_)}, {"add", Added}, {"remove", Removed}, {"maxOutputBytes", QString::number(Update.maxOutputBytes_)}, {"encrypt", Update.encrypt_}};
        if (Update.creation_) Object["creation"] = QJsonObject{{"level", int(Update.creation_->level_)}, {"solid", Update.creation_->solid_}};
        if (Update.creation_ && Update.creation_->advanced_) {
            auto Creation = Object["creation"].toObject(); Creation["advanced"] = EncodeSevenZipSettings(*Update.creation_->advanced_); Object["creation"] = Creation;
        }
        if (!Update.metadataOrigins_.empty()) { QJsonArray Origins; for (const auto& Name : Update.metadataOrigins_) Origins.append(Text(Name)); Object["metadataOrigins"] = Origins; }
        if (!Update.reuseData_.empty()) { QJsonArray Flags; for (bool Flag : Update.reuseData_) Flags.append(Flag); Object["reuseData"] = Flags; }
        if (Update.createNew_) Object["createNew"] = true;
        return Object;
    }
    Result<ArchiveUpdate> DecodeArchiveUpdate(const QJsonObject& Object)
    {
        if (Object.size() != 6 + Object.contains("creation") + Object.contains("metadataOrigins") + Object.contains("reuseData") + Object.contains("createNew") || !Object["encrypt"].isBool() || !String(Object["archive"], 32768) || !String(Object["inputDirectory"], 32768) || !Object["add"].isArray() || !Object["remove"].isArray()) return Invalid();
        auto Limit = WireUnsigned(Object["maxOutputBytes"]); if (!Limit.HasValue() || !Limit.Value()) return Invalid();
        ArchiveUpdate Out; Out.archive_ = Object["archive"].toString().toStdString(); Out.inputDirectory_ = Object["inputDirectory"].toString().toStdString(); Out.maxOutputBytes_ = Limit.Value();
        Out.encrypt_ = Object["encrypt"].toBool();
        if (Object.contains("createNew")) { if (Object["createNew"] != QJsonValue(true)) return Invalid(); Out.createNew_ = true; }
        if (Object.contains("creation")) {
            const auto Options = Object["creation"].toObject(); const auto Level = Options["level"].toInt(-1);
            if (Options.size() != 2 + Options.contains("advanced") || !Options["solid"].isBool() || Level < 0 || Level > 9 || Options["level"].toDouble(-1) != Level) return Invalid();
            Out.creation_ = ArchiveCreationOptions{unsigned(Level), Options["solid"].toBool()};
            if (Options.contains("advanced")) {
                auto Advanced = DecodeSevenZipSettings(Options["advanced"].toObject()); if (!Advanced.HasValue()) return Advanced.Failure();
                if (Advanced.Value().level_ != unsigned(Level) || Advanced.Value().encrypt_ != Out.encrypt_
                    || (Advanced.Value().solid_ != ArchiveSolidMode::Off) != Out.creation_->solid_) return Invalid();
                Out.creation_->advanced_ = std::move(Advanced.Value());
            }
        }
        for (const auto& [Key, Target] : {std::pair{"add", &Out.add_}, {"remove", &Out.remove_}})
        {
            const auto Names = Object[Key].toArray();
            for (const auto Name : Names) { if (!String(Name, 4096)) return Invalid(); Target->push_back(Name.toString().toStdString()); }
        }
        if (Object.contains("metadataOrigins")) {
            if (!Object["metadataOrigins"].isArray() || Object["metadataOrigins"].toArray().size() != qsizetype(Out.add_.size())) return Invalid();
            for (const auto Name : Object["metadataOrigins"].toArray()) {
                if (!Name.isString() || (!Name.toString().isEmpty() && !String(Name, 4096))) return Invalid();
                Out.metadataOrigins_.push_back(Name.toString().toStdString());
            }
        }
        if (Object.contains("reuseData")) {
            if (!Object["reuseData"].isArray() || Object["reuseData"].toArray().size() != qsizetype(Out.add_.size())) return Invalid();
            for (const auto Flag : Object["reuseData"].toArray()) { if (!Flag.isBool()) return Invalid(); Out.reuseData_.push_back(Flag.toBool()); }
        }
        return Out;
    }
    QJsonObject EncodeContext(const OperationContext& Context, unsigned MaximumMilliseconds)
    {
        if (Context.passwordPrompt_) MaximumMilliseconds = std::max(MaximumMilliseconds, 180000u);
        const auto Maximum = static_cast<long long>(std::min(MaximumMilliseconds, 3600000u));
        const auto Milliseconds = Context.deadline_ == std::chrono::steady_clock::time_point::max() ? Maximum
            : std::clamp(static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(Context.deadline_ - std::chrono::steady_clock::now()).count()), 0LL, Maximum);
        // v1 hosts only understand this bounded legacy envelope. v2 carries
        // the independent policy and caller deadline outside this shim.
        return {{"maxEntries", QString::number(Context.EnumerationLimit())}, {"pageItems", QString::number(Context.maxPageItems_)}, {"milliseconds", QString::number(Milliseconds)}};
    }
    Result<OperationContext> DecodeContext(const QJsonObject& Object)
    {
        if (Object.size() != 3) return Invalid();
        auto Entries = WireUnsigned(Object["maxEntries"]), Page = WireUnsigned(Object["pageItems"]), Time = WireUnsigned(Object["milliseconds"]);
        if (!Entries.HasValue() || !Page.HasValue() || !Time.HasValue() || !Entries.Value() || Entries.Value() > SIZE_MAX || !Page.Value() || Page.Value() > 100000 || Time.Value() > 3600000) return Invalid();
        OperationContext Context; Context.maxEnumerationEntries_ = Entries.Value(); Context.maxPageItems_ = Page.Value();
        Context.deadline_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(Time.Value()); return Context;
    }
    QJsonObject EncodeContextV2(const OperationContext& Context)
    {
        QJsonObject Value{{"entries", QString::number(Context.maxEnumerationEntries_)}, {"page", QString::number(Context.maxPageItems_)},
            {"read", QString::number(std::min(Context.maxReadBytes_, std::size_t(WirePayloadLimit)))}, {"automatic", Context.workload_.IsAutomatic()},
            {"autoBytes", QString::number(Context.workload_.automaticFileBytes_)}, {"autoEntries", QString::number(Context.workload_.automaticEntries_)},
            {"autoDepth", QString::number(Context.workload_.automaticDepth_)}, {"autoRatio", QString::number(Context.workload_.automaticExpansionRatio_)}};
        Value["milliseconds"] = Context.deadline_ == std::chrono::steady_clock::time_point::max() ? QJsonValue(QJsonValue::Null)
            : QJsonValue(QString::number(std::max<std::int64_t>(0, std::chrono::duration_cast<std::chrono::milliseconds>(Context.deadline_ - std::chrono::steady_clock::now()).count())));
        return Value;
    }
    Result<OperationContext> DecodeContextV2(const QJsonObject& Object)
    {
        if (Object.size() != 9 + Object.contains("metadataRename") || (Object.contains("metadataRename") && !Object["metadataRename"].isBool()) || !Object["automatic"].isBool()) return Invalid();
        auto Entries = WireUnsigned(Object["entries"]), Page = WireUnsigned(Object["page"]), Read = WireUnsigned(Object["read"]),
            Bytes = WireUnsigned(Object["autoBytes"]), Count = WireUnsigned(Object["autoEntries"]), Depth = WireUnsigned(Object["autoDepth"]), Ratio = WireUnsigned(Object["autoRatio"]);
        if (!Entries.HasValue() || Entries.Value() > SIZE_MAX || !Page.HasValue() || !Page.Value() || Page.Value() > 100000
            || !Read.HasValue() || !Read.Value() || Read.Value() > WirePayloadLimit || !Bytes.HasValue() || !Count.HasValue()
            || !Depth.HasValue() || Depth.Value() > SIZE_MAX || !Ratio.HasValue()) return Invalid();
        OperationContext Context; Context.maxEnumerationEntries_ = Entries.Value(); Context.maxPageItems_ = Page.Value(); Context.maxReadBytes_ = Read.Value();
        Context.workload_ = {Object["automatic"].toBool() ? WorkloadIntent::Automatic : WorkloadIntent::Explicit, Bytes.Value(), Count.Value(), std::size_t(Depth.Value()), Ratio.Value()};
        if (!Object["milliseconds"].isNull())
        {
            auto Time = WireUnsigned(Object["milliseconds"]); if (!Time.HasValue()) return Invalid();
            const auto Now = std::chrono::steady_clock::now();
            const auto Remaining = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::time_point::max() - Now).count();
            if (Time.Value() > std::uint64_t(Remaining)) return Invalid();
            Context.deadline_ = Now + std::chrono::milliseconds(Time.Value());
        }
        return Context;
    }
}
