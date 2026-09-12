#pragma once
#include "../storage/materializedContent.hpp"

namespace hc::plugins
{
    enum class MetadataType { Integer, Real, Boolean, Text, Date, Time, Timestamp, FullText };
    enum class MetadataStatus { Value, Empty, Delayed, OnDemand };
    struct MetadataField
    {
        int index_ = 0;
        std::string name_;
        std::vector<std::string> units_;
        MetadataType type_ = MetadataType::Text;
        bool descending_ = false;
    };
    struct MetadataRequest { int field_ = 0, unit_ = 0; bool deferSlow_ = true; };
    struct MetadataValue
    {
        MetadataStatus status_ = MetadataStatus::Empty;
        MetadataType type_ = MetadataType::Text;
        // Date/time use ISO text; timestamps retain UTC FILETIME ticks as an
        // integer. Neither native pointers nor SDK unions cross this boundary.
        std::variant<std::monostate, std::int64_t, double, bool, std::string> value_;
    };
    class IMetadataProvider
    {
    public:
        virtual ~IMetadataProvider() = default;
        virtual storage::Task<std::vector<MetadataField>> Fields(storage::OperationContext Context) = 0;
        virtual storage::Task<MetadataValue> Query(std::shared_ptr<storage::IContentSource> Source,
            MetadataRequest Request, storage::OperationContext Context) = 0;
    };
}
