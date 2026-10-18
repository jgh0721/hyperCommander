#pragma once
#include "jobScheduler.hpp"

namespace hc::operations
{
    struct ItemOutcomePage { std::vector<ItemOutcome> items_; bool end_ = false; };
    // Results may outlive the displayed page. Replace addresses an emitted item
    // without incrementing totals (destination commit followed by source removal).
    // Recovery persistence and scheduler integration are separate from this API.
    class IItemOutcomeStore
    {
    public:
        virtual ~IItemOutcomeStore() = default;
        virtual std::uint64_t Count() const = 0;
        virtual Result<std::uint64_t> Append(const ItemOutcome& Item) = 0;
        virtual Result<storage::Unit> Replace(std::uint64_t Row, const ItemOutcome& Item) = 0;
        virtual Result<ItemOutcomePage> Read(std::uint64_t Offset, std::size_t MaxItems,
            const storage::OperationContext& Context = {}) const = 0;
    };
}
