#pragma once
#include "qtPagedStorage.hpp"
#include "commonLibs/operations/itemOutcomeStore.hpp"

namespace hc::operations
{
    class PagedItemOutcomeStore final : public IItemOutcomeStore
    {
    public:
        static Result<std::shared_ptr<IItemOutcomeStore>> Create(const QString& Parent = {});
        std::uint64_t Count() const override;
        Result<std::uint64_t> Append(const ItemOutcome& Item) override;
        Result<storage::Unit> Replace(std::uint64_t Row, const ItemOutcome& Item) override;
        Result<ItemOutcomePage> Read(std::uint64_t Offset, std::size_t MaxItems,
            const storage::OperationContext& Context = {}) const override;
    private:
        explicit PagedItemOutcomeStore(std::shared_ptr<storage::DiskRecords> Records) : records_(std::move(Records)) {}
        std::shared_ptr<storage::DiskRecords> records_;
        mutable std::mutex mutex_;
    };
}
