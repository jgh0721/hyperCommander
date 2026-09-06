#pragma once
#include "fileOperation.hpp"
#include "../storage/archiveMutation.hpp"

namespace hc::operations
{
    struct ArchiveOperationRequest
    {
        // Move is currently an in-container move, committed with the same batch.
        FileOperation operation_ = FileOperation::Copy;
        storage::Locator archive_;
        // Copy: directory; rename/mkdir: complete internal destination.
        storage::Locator destination_;
        std::shared_ptr<storage::IStorageProvider> reader_;
        std::vector<OperationSource> sources_;
        std::function<Result<std::unique_ptr<storage::IArchiveMutationSession>>(storage::OperationContext)> begin_;
        ConflictPolicy conflict_ = ConflictPolicy::Ask;
        bool confirmDelete_ = true;
        storage::ArchiveMutationLimits limits_;
        std::string correlationId_;
    };
    Result<JobHandle> SubmitArchiveOperation(JobScheduler& Scheduler, ArchiveOperationRequest Request,
        storage::OperationContext Context = {});
}
