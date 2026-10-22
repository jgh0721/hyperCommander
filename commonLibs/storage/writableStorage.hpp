#pragma once
#include "storage.hpp"
#include <span>

namespace hc::storage
{
    enum class Mutation { CreateContent, MakeDirectory, Remove, Rename, Recycle };
    struct DestinationExpectation
    {
        // Missing destination is an expectation too: never replace a file that
        // appeared after the user's conflict decision.
        bool exists_ = false;
        std::optional<ResourceRevision> revision_;
    };
    struct CopyOptions
    {
        bool metadata_ = true, verify_ = false, acl_ = false, streams_ = false;
    };
    struct CopyCapabilities
    {
        bool metadata_ = true, verify_ = false, acl_ = false, streams_ = false;
    };
    struct CopyExtraProgress { bool done_ = true; std::uint64_t bytes_ = 0; };
    struct FileMetadata
    {
        std::optional<std::int64_t> modifiedUtcMs_;
        std::optional<std::uint32_t> attributes_;
    };
    struct MetadataPatch
    {
        std::uint32_t setMask_ = 0, clearMask_ = 0;
        std::optional<std::int64_t> modifiedUtcMs_;
        bool Valid() const { return !(setMask_ & clearMask_) && !((setMask_ | clearMask_) & ~0x27u)
            && (!modifiedUtcMs_ || (*modifiedUtcMs_ >= -11644473599999LL && *modifiedUtcMs_ <= 910692730085477LL)); }
        bool Empty() const { return !setMask_ && !clearMask_ && !modifiedUtcMs_; }
    };
    struct WriteReceipt
    {
        Locator destination_;
        std::vector<Error> cleanupErrors_;
        std::vector<Locator> preservedArtifacts_;
        std::string transaction_, destinationIdentity_;
        bool destinationCommitted_ = true;
    };
    class IWriteSession
    {
    public:
        virtual ~IWriteSession() = default;
        // These bounded operations execute on the scheduler worker. A session
        // has one owner and one outstanding call, never concurrent Append/Abort.
        virtual Result<Unit> Append(std::span<const std::byte> Bytes, OperationContext Context) = 0;
        virtual bool CanProduceContent(OperationContext) const { return false; }
        // Exclusive producer worker; the coordinator joins before any other call.
        virtual Result<Unit> ProduceContent(ContentProducer, OperationContext)
        { return Error{ErrorCategory::Unsupported, "produced-content-unavailable"}; }
        virtual Result<Unit> SetMetadata(FileMetadata Metadata, OperationContext Context) = 0;
        // Source identity is immutable. Each extra/verification advance is bounded.
        virtual Result<Unit> ConfigureCopy(EntrySnapshot, CopyOptions Options, OperationContext)
        { return Options.acl_ || Options.streams_ ? Result<Unit>(Error{ErrorCategory::Unsupported, "copy-extras-unavailable"}) : Result<Unit>(Unit{}); }
        virtual Result<Unit> RecordSourceBytes(std::span<const std::byte>, OperationContext)
        { return Error{ErrorCategory::Unsupported, "copy-verification-unavailable"}; }
        virtual Result<CopyExtraProgress> AdvanceCopyExtras(std::uint64_t, OperationContext) { return CopyExtraProgress{}; }
        virtual Result<bool> VerifyContent(OperationContext) { return Error{ErrorCategory::Unsupported, "copy-verification-unavailable"}; }
        virtual Result<WriteReceipt> Commit(OperationContext Context) = 0;
        // Idempotent, uncancelled cleanup. Never delete committed destination or
        // a backup that cannot safely be restored; report its recovery locator.
        virtual Result<Unit> Abort() = 0;
    };
    class IWritableStorage
    {
    public:
        virtual ~IWritableStorage() = default;
        virtual ProviderId Id() const = 0;
        virtual Result<SupportDecision> Supports(Mutation Operation, Locator At, OperationContext Context) = 0;
        virtual Result<std::optional<EntrySnapshot>> Inspect(Locator At, OperationContext Context) = 0;
        // Existing object only: never opens a content-write session or replaces data.
        virtual Result<Unit> PatchMetadata(EntrySnapshot, MetadataPatch, OperationContext)
        { return Error{ErrorCategory::Unsupported, "metadata-patch-unavailable", Id()}; }
        virtual Result<std::unique_ptr<IWriteSession>> BeginWrite(Locator At, DestinationExpectation Expected, OperationContext Context) = 0;
        virtual CopyCapabilities CopySupport() const { return {}; }
        virtual bool SupportsParallelCopies() const { return false; }
        virtual std::size_t PreferredCopyWorkers(Locator, Locator) const { return 1; }
        virtual bool CanProduceContent(OperationContext) const { return false; }
        virtual Result<std::unique_ptr<IWriteSession>> BeginCopy(Locator At, DestinationExpectation Expected, CopyOptions Options, OperationContext Context)
        {
            if (Options.verify_ || Options.acl_ || Options.streams_) return Error{ErrorCategory::Unsupported, "copy-options-unavailable", Id()};
            return BeginWrite(std::move(At), std::move(Expected), Context);
        }
        virtual Result<Unit> MakeDirectory(Locator At, OperationContext Context) = 0;
        // Remove is one file or one empty directory; recursive traversal and
        // source-after-destination ordering belong to the coordinator.
        virtual Result<Unit> Remove(EntrySnapshot Entry, OperationContext Context) = 0;
        virtual Result<Unit> Rename(EntrySnapshot Entry, Locator To, OperationContext Context) = 0;
        // Providers must recycle or fail. Permanent removal is never a fallback.
        virtual Result<WriteReceipt> Recycle(EntrySnapshot, OperationContext) { return Error{ErrorCategory::Unsupported, "recycle-not-supported", Id()}; }
    };
}
