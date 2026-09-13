#include "archiveCodec.hpp"
#include <QJsonArray>

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
        return {{"name", Text(Header.name_)}, {"size", QString::number(Header.size_)}, {"packedSize", QString::number(Header.packedSize_)},
            {"attributes", QString::number(Header.attributes_)}, {"crc", QString::number(Header.crc_)}, {"modifiedUtcMs", QString::number(Header.modifiedUtcMs_)}, {"directory", Header.directory_}};
    }
    Result<ArchiveHeader> DecodeArchiveHeader(const QJsonObject& Object)
    {
        if (Object.size() != 7 || !String(Object["name"], 1024) || Object["name"].toString().isEmpty() || !Object["directory"].isBool() || !Object["modifiedUtcMs"].isString()) return Invalid();
        auto Size = WireUnsigned(Object["size"]), Packed = WireUnsigned(Object["packedSize"]), Attributes = WireUnsigned(Object["attributes"]), Crc = WireUnsigned(Object["crc"]);
        if (!Size.HasValue() || !Packed.HasValue() || !Attributes.HasValue() || !Crc.HasValue() || Attributes.Value() > UINT32_MAX || Crc.Value() > UINT32_MAX) return Invalid();
        bool Ok = false; auto Time = Object["modifiedUtcMs"].toString().toLongLong(&Ok); if (!Ok) return Invalid();
        return ArchiveHeader{Object["name"].toString().toStdString(), Size.Value(), Packed.Value(), static_cast<std::uint32_t>(Attributes.Value()), static_cast<std::uint32_t>(Crc.Value()), Time, Object["directory"].toBool()};
    }
    QJsonObject EncodeArchiveUpdate(const ArchiveUpdate& Update)
    {
        QJsonArray Added, Removed; for (const auto& Name : Update.add_) Added.append(Text(Name)); for (const auto& Name : Update.remove_) Removed.append(Text(Name));
        return {{"archive", Text(Update.archive_)}, {"inputDirectory", Text(Update.inputDirectory_)}, {"add", Added}, {"remove", Removed}, {"maxOutputBytes", QString::number(Update.maxOutputBytes_)}};
    }
    Result<ArchiveUpdate> DecodeArchiveUpdate(const QJsonObject& Object)
    {
        if (Object.size() != 5 || !String(Object["archive"], 32768) || !String(Object["inputDirectory"], 32768) || !Object["add"].isArray() || !Object["remove"].isArray()) return Invalid();
        auto Limit = WireUnsigned(Object["maxOutputBytes"]); if (!Limit.HasValue() || !Limit.Value() || Limit.Value() > 256ULL * 1024 * 1024) return Invalid();
        ArchiveUpdate Out; Out.archive_ = Object["archive"].toString().toStdString(); Out.inputDirectory_ = Object["inputDirectory"].toString().toStdString(); Out.maxOutputBytes_ = Limit.Value();
        for (const auto& [Key, Target] : {std::pair{"add", &Out.add_}, {"remove", &Out.remove_}})
        {
            const auto Names = Object[Key].toArray(); if (Names.size() > 100000) return Invalid();
            for (const auto Name : Names) { if (!String(Name, 4096)) return Invalid(); Target->push_back(Name.toString().toStdString()); }
        }
        return Out;
    }
    QJsonObject EncodeContext(const OperationContext& Context, unsigned MaximumMilliseconds)
    {
        const auto Maximum = static_cast<long long>(std::min(MaximumMilliseconds, 3600000u));
        const auto Milliseconds = Context.deadline_ == std::chrono::steady_clock::time_point::max() ? Maximum
            : std::clamp(static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(Context.deadline_ - std::chrono::steady_clock::now()).count()), 0LL, Maximum);
        return {{"maxEntries", QString::number(Context.maxEnumerationEntries_)}, {"pageItems", QString::number(Context.maxPageItems_)}, {"milliseconds", QString::number(Milliseconds)}};
    }
    Result<OperationContext> DecodeContext(const QJsonObject& Object)
    {
        if (Object.size() != 3) return Invalid();
        auto Entries = WireUnsigned(Object["maxEntries"]), Page = WireUnsigned(Object["pageItems"]), Time = WireUnsigned(Object["milliseconds"]);
        if (!Entries.HasValue() || !Page.HasValue() || !Time.HasValue() || !Entries.Value() || Entries.Value() > 100000 || !Page.Value() || Page.Value() > 100000 || Time.Value() > 3600000) return Invalid();
        OperationContext Context; Context.maxEnumerationEntries_ = Entries.Value(); Context.maxPageItems_ = Page.Value();
        Context.deadline_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(Time.Value()); return Context;
    }
}
