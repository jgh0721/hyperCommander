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
        std::uint64_t maxFileBytes_ = 256ULL * 1024 * 1024;
        std::uint64_t maxTotalBytes_ = 1024ULL * 1024 * 1024;
        std::size_t maxActiveFiles_ = 64;
        std::size_t maxDepth_ = 8;
        std::uint64_t maxExpansionRatio_ = 1000;
        bool operator==(const MaterializationLimits&) const = default;
    };
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
