#pragma once
#include "storage.hpp"
#include <span>

namespace hc::storage
{
    enum class Mutation { CreateContent, MakeDirectory, Remove, Rename };
    struct DestinationExpectation
    {
        // Missing destination is an expectation too: never replace a file that
        // appeared after the user's conflict decision.
        bool exists_ = false;
        std::optional<ResourceRevision> revision_;
    };
    struct FileMetadata
    {
        std::optional<std::int64_t> modifiedUtcMs_;
        std::optional<std::uint32_t> attributes_;
    };
    struct WriteReceipt
    {
        Locator destination_;
        std::vector<Error> cleanupErrors_;
        std::vector<Locator> preservedArtifacts_;
    };
    class IWriteSession
    {
    public:
        virtual ~IWriteSession() = default;
        // These bounded operations execute on the scheduler worker. A session
        // has one owner and one outstanding call, never concurrent Append/Abort.
        virtual Result<Unit> Append(std::span<const std::byte> Bytes, OperationContext Context) = 0;
        virtual Result<Unit> SetMetadata(FileMetadata Metadata, OperationContext Context) = 0;
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
        virtual Result<std::unique_ptr<IWriteSession>> BeginWrite(Locator At, DestinationExpectation Expected, OperationContext Context) = 0;
        virtual Result<Unit> MakeDirectory(Locator At, OperationContext Context) = 0;
        // Remove is one file or one empty directory; recursive traversal and
        // source-after-destination ordering belong to the coordinator.
        virtual Result<Unit> Remove(EntrySnapshot Entry, OperationContext Context) = 0;
        virtual Result<Unit> Rename(EntrySnapshot Entry, Locator To, OperationContext Context) = 0;
    };
}
