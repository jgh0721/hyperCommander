#pragma once
#include "archiveOperation.hpp"

namespace hc::operations
{
    struct ArchiveMoveEndpoint
    {
        storage::Locator root_;
        std::shared_ptr<storage::IStorageProvider> reader_;
        std::function<Result<std::unique_ptr<storage::IArchiveMutationSession>>(storage::OperationContext)> begin_;
        storage::ArchiveMutationLimits limits_;
    };
    struct MoveOperationRequest
    {
        std::vector<storage::EntrySnapshot> sources_;
        std::shared_ptr<storage::IStorageProvider> sourceReader_;
        std::shared_ptr<storage::IWritableStorage> sourceWriter_;
        std::optional<ArchiveMoveEndpoint> sourceArchive_;
        storage::Locator destination_;
        std::shared_ptr<storage::IWritableStorage> destinationWriter_;
        std::optional<ArchiveMoveEndpoint> destinationArchive_;
        ConflictPolicy conflict_ = ConflictPolicy::Ask;
        std::size_t maxEntries_ = 100000;
        std::size_t maxDepth_ = 32;
        std::uint64_t maxBytes_ = 1024ULL * 1024 * 1024;
        std::string correlationId_;
    };
    // Copy all accepted items before removing any source. Cross-container moves
    // are not distributed transactions: a failed removal retains copied output.
    Result<JobHandle> SubmitMoveOperation(JobScheduler& Scheduler, MoveOperationRequest Request,
        storage::OperationContext Context = {});
}
