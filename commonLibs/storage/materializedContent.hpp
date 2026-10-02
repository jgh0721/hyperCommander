#pragma once
#include "storage.hpp"

namespace hc::storage
{
    class ProviderContentSource final : public IContentSource
    {
    public:
        ProviderContentSource(std::shared_ptr<IStorageProvider> Provider, EntrySnapshot Entry)
            : provider_(std::move(Provider)), entry_(std::move(Entry)) {}
        EntrySnapshot Describe() const override { return entry_; }
        Task<std::shared_ptr<IReadStream>> OpenRead(OperationContext Context) override
        {
            if (!provider_) return Ready<std::shared_ptr<IReadStream>>(Error{ErrorCategory::InvalidRequest, "content-provider-required"});
            return provider_->OpenRead(entry_.id_, std::move(Context));
        }
        Task<Unit> Verify(OperationContext Context) override
        {
            if (!provider_) return Ready<Unit>(Error{ErrorCategory::InvalidRequest, "content-provider-required"});
            auto Fresh = provider_->Stat(entry_.id_, std::move(Context)).get();
            if (!Fresh.HasValue()) return Ready<Unit>(Fresh.Failure());
            if (Fresh.Value().id_ != entry_.id_ || Fresh.Value().locator_ != entry_.locator_
                || Fresh.Value().size_ != entry_.size_ || Fresh.Value().directory_ != entry_.directory_)
                return Ready<Unit>(Error{ErrorCategory::StaleRevision, "content-changed"});
            return Ready<Unit>(Unit{});
        }
    private:
        std::shared_ptr<IStorageProvider> provider_;
        EntrySnapshot entry_;
    };
    struct MaterializationLimits
    {
        // Optional administrator/store constraints. Explicit work has no
        // product file/total-byte ceiling; automatic policy is in its context.
        std::uint64_t maxFileBytes_ = std::numeric_limits<std::uint64_t>::max();
        std::uint64_t maxTotalBytes_ = std::numeric_limits<std::uint64_t>::max();
        std::size_t maxActiveFiles_ = 64;
        std::size_t maxDepth_ = std::numeric_limits<std::size_t>::max();
        std::uint64_t maxExpansionRatio_ = 1000;
        bool operator==(const MaterializationLimits&) const = default;
    };
    inline std::optional<Error> CheckMaterialization(const EntrySnapshot& Entry, std::size_t Depth,
        std::optional<std::uint64_t> Compressed, const MaterializationLimits& Limits, const OperationContext& Context)
    {
        if (Entry.size_ > Limits.maxFileBytes_ || Depth > Limits.maxDepth_)
            return Error{ErrorCategory::BudgetExceeded, "materialization-file-budget"};
        if (!Context.workload_.IsAutomatic()) return {};
        if (Entry.size_ > Context.workload_.automaticFileBytes_ || Depth > Context.workload_.automaticDepth_)
            return Error{ErrorCategory::BudgetExceeded, "automatic-materialization-budget", {}, {}, true, "open-explicitly"};
        if (Entry.locator_.kind_ == LocatorKind::Archive && !Compressed)
            return Error{ErrorCategory::InvalidRequest, "compressed-size-required"};
        const auto Ratio = std::min(Limits.maxExpansionRatio_, Context.workload_.automaticExpansionRatio_);
        if (Compressed && Entry.size_ && (!*Compressed || !Ratio || (Entry.size_ - 1) / *Compressed >= Ratio))
            return Error{ErrorCategory::BudgetExceeded, "materialization-expansion-budget", {}, {}, true, "open-explicitly"};
        return {};
    }
    class IMaterializedFileLease
    {
    public:
        virtual ~IMaterializedFileLease() = default;
        virtual std::string LocalPath() const = 0;
        virtual EntrySnapshot Describe() const = 0;
        virtual std::size_t Depth() const = 0;
    };
    struct MaterializationRequest
    {
        std::shared_ptr<IContentSource> source_;
        // Retain the complete container lifetime until this child lease closes.
        std::vector<std::shared_ptr<IMaterializedFileLease>> parents_;
        std::optional<std::uint64_t> compressedBytes_;
    };
    struct MaterializationCleanup
    {
        std::size_t removed_ = 0;
        std::size_t busy_ = 0;
        std::vector<Error> errors_;
    };
    class IMaterializer
    {
    public:
        virtual ~IMaterializer() = default;
        virtual Task<std::shared_ptr<IMaterializedFileLease>> Materialize(MaterializationRequest Request, OperationContext Context) = 0;
        // Lifecycle/maintenance calls run off the UI thread. Live leases survive
        // recovery; cleanup retries never inherit a cancelled operation token.
        virtual MaterializationCleanup Recover() = 0;
        virtual MaterializationCleanup DrainCleanup() = 0;
    };
}
