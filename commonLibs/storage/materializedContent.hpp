#pragma once
#include "storage.hpp"
#include <mutex>

namespace hc::storage
{
    class ProviderContentSource final : public IContentSource
    {
    public:
        ProviderContentSource(std::shared_ptr<IStorageProvider> Provider, EntrySnapshot Entry)
            : provider_(std::move(Provider)), entry_(std::move(Entry)) {}
        // Optional archive-close transaction captured by the owning navigation session.
        // The argument is a saved draft, never a path supplied by the helper.
        using ArchiveWriteback = std::function<Result<Unit>(const std::string&, OperationContext)>;
        void SetArchiveWriteback(ArchiveWriteback Writeback) { archiveWriteback_ = std::move(Writeback); }
        const ArchiveWriteback& GetArchiveWriteback() const { return archiveWriteback_; }
        void RememberPrefix(std::vector<std::byte> Bytes, std::shared_ptr<IReadStream> Tail = {}) {
            std::lock_guard Lock(prefixMutex_);
            if (Bytes.size() <= 65536) {
                prefix_ = std::move(Bytes);
                // Preserve the decoder cursor so an archive probe is not extracted twice.
                if (entry_.locator_.kind_ == LocatorKind::Archive) prefixTail_ = std::move(Tail);
            }
        }
        std::vector<std::byte> Prefix() const { std::lock_guard Lock(prefixMutex_); return prefix_; }
        EntrySnapshot Describe() const override { return entry_; }
        Task<std::shared_ptr<IReadStream>> OpenRead(OperationContext Context) override
        {
            if (!provider_) return Ready<std::shared_ptr<IReadStream>>(Error{ErrorCategory::InvalidRequest, "content-provider-required"});
            std::shared_ptr<IReadStream> Tail; std::vector<std::byte> Prefix;
            { std::lock_guard Lock(prefixMutex_); Tail = std::exchange(prefixTail_, {}); if (Tail) Prefix = prefix_; }
            if (Tail) {
                auto Verified = Verify(Context).get(); if (!Verified.HasValue()) return Ready<std::shared_ptr<IReadStream>>(Verified.Failure());
                return Ready<std::shared_ptr<IReadStream>>(std::shared_ptr<IReadStream>(std::make_shared<PrefixStream>(std::move(Prefix), std::move(Tail), entry_.size_)));
            }
            return provider_->OpenRead(entry_.id_, std::move(Context));
        }
        Task<Unit> Verify(OperationContext Context) override
        {
            if (!provider_) return Ready<Unit>(Error{ErrorCategory::InvalidRequest, "content-provider-required"});
            auto Fresh = provider_->Stat(entry_.id_, std::move(Context)).get();
            if (!Fresh.HasValue()) return Ready<Unit>(Fresh.Failure());
            // A successful Stat has checked the captured revision. Its result
            // can strengthen a metadata-only token without changing the object.
            if (Fresh.Value().id_.providerId_ != entry_.id_.providerId_
                || Fresh.Value().id_.key_ != entry_.id_.key_ || Fresh.Value().id_.scope_ != entry_.id_.scope_
                || Fresh.Value().locator_ != entry_.locator_
                || Fresh.Value().size_ != entry_.size_ || Fresh.Value().directory_ != entry_.directory_)
                return Ready<Unit>(Error{ErrorCategory::StaleRevision, "content-changed"});
            return Ready<Unit>(Unit{});
        }
    private:
        class PrefixStream final : public IReadStream {
        public:
            PrefixStream(std::vector<std::byte> Bytes, std::shared_ptr<IReadStream> Tail, std::uint64_t Size)
                : bytes_(std::move(Bytes)), tail_(std::move(Tail)), size_(Size) {}
            bool CanSeek() const override { return false; }
            ReadAccess Access() const override { return ReadAccess::Sequential; }
            Task<Unit> Seek(std::uint64_t, OperationContext) override { return Ready<Unit>(Error{ErrorCategory::Unsupported, "prefix-stream-seek"}); }
            Task<ReadChunk> Read(std::size_t Count, OperationContext Context) override {
                if (auto Stop = Context.Check({})) return Ready<ReadChunk>(*Stop);
                if (!Count || !Context.maxReadBytes_) return Ready<ReadChunk>(Error{ErrorCategory::InvalidRequest, "zero-read-size"});
                if (position_ < bytes_.size()) {
                    const auto Size = (std::min)({Count, Context.maxReadBytes_, bytes_.size() - position_});
                    ReadChunk Chunk{{bytes_.begin() + position_, bytes_.begin() + position_ + Size}, position_ + Size == size_};
                    position_ += Size; return Ready<ReadChunk>(std::move(Chunk));
                }
                return tail_->Read(Count, std::move(Context));
            }
        private:
            std::vector<std::byte> bytes_; std::shared_ptr<IReadStream> tail_;
            std::uint64_t size_; std::size_t position_ = 0;
        };
        std::shared_ptr<IStorageProvider> provider_;
        EntrySnapshot entry_;
        ArchiveWriteback archiveWriteback_;
        mutable std::mutex prefixMutex_;
        std::vector<std::byte> prefix_;
        std::shared_ptr<IReadStream> prefixTail_;
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
    struct MaterializationAuxiliary
    {
        // Exact suffix of the generated private content basename, never a path.
        // These are disposable caches, not a writable document transaction.
        std::string suffix_;
        std::uint64_t maxBytes_ = 0;
    };
    struct MaterializationRequest
    {
        std::shared_ptr<IContentSource> source_;
        // Retain the complete container lifetime until this child lease closes.
        std::vector<std::shared_ptr<IMaterializedFileLease>> parents_;
        std::optional<std::uint64_t> compressedBytes_;
        std::vector<MaterializationAuxiliary> auxiliaries_;
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
    // Optional private-copy capability. Existing materializer and lease vtables
    // stay unchanged; ordinary/direct sources cannot silently adopt auxiliaries.
    class IPrivateAuxiliaryMaterializer
    {
    public:
        virtual ~IPrivateAuxiliaryMaterializer() = default;
        virtual Task<std::shared_ptr<IMaterializedFileLease>> MaterializeWithAuxiliaries(
            MaterializationRequest Request, OperationContext Context) = 0;
    };
    inline Task<std::shared_ptr<IMaterializedFileLease>> MaterializePrivateContent(
        IMaterializer& Materializer, MaterializationRequest Request, OperationContext Context)
    {
        if (Request.auxiliaries_.empty()) return Materializer.Materialize(std::move(Request), std::move(Context));
        const auto Owner = dynamic_cast<IPrivateAuxiliaryMaterializer*>(&Materializer);
        if (!Owner) return Ready<std::shared_ptr<IMaterializedFileLease>>(Error{ErrorCategory::Unsupported, "materialization-auxiliaries-unsupported"});
        return Owner->MaterializeWithAuxiliaries(std::move(Request), std::move(Context));
    }
}
