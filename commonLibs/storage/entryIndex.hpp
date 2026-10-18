#pragma once
#include "storage.hpp"

namespace hc::storage
{
    // Immutable, disk-backed snapshot. Offsets and counts are 64 bit; MaxItems
    // bounds a single request, never the total number of enumerated entries.
    class IEntryIndex
    {
    public:
        virtual ~IEntryIndex() = default;
        virtual std::uint64_t Count() const = 0;
        virtual Result<EntryPage> Read(std::uint64_t Offset, std::size_t MaxItems,
            const OperationContext& Context = {}) const = 0;
    };
    struct EntryRange { std::uint64_t offset_ = 0, count_ = 0; };
    // A manifest retains its source snapshot, including revisions. Changing the
    // current directory or sort order cannot change the selected resources.
    class ISelectionManifest : public IEntryIndex
    {
    public:
        // UI/provider-bound selections retain the originating session. Pure
        // metadata indexes may leave this empty and require an external lease.
        virtual std::shared_ptr<IStorageProvider> Provider() const { return {}; }
    };
}
