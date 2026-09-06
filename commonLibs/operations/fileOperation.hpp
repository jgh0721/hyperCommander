#pragma once
#include "jobScheduler.hpp"
#include "../storage/writableStorage.hpp"

namespace hc::operations
{
    enum class FileOperation { Copy, Move, Delete, MakeDirectory, Rename };
    enum class ConflictPolicy { Ask, Replace, Skip };
    struct OperationSource
    {
        storage::EntrySnapshot entry_;
        std::shared_ptr<storage::IStorageProvider> reader_;
        std::shared_ptr<storage::IWritableStorage> writer_;
    };
    struct FileOperationRequest
    {
        FileOperation operation_ = FileOperation::Copy;
        std::vector<OperationSource> sources_;
        // Copy/move: destination directory. Mkdir/rename: complete destination.
        storage::Locator destination_;
        std::shared_ptr<storage::IWritableStorage> destinationWriter_;
        ConflictPolicy conflict_ = ConflictPolicy::Ask;
        bool confirmDelete_ = true;
        std::size_t maxEntries_ = 100000;
        std::size_t maxDepth_ = 32;
        std::uint64_t maxBytes_ = 64ULL * 1024 * 1024 * 1024;
        std::string correlationId_;
    };
    // Pure construction/validation, no I/O; allows coordinators to compose tasks
    // while retaining one scheduler job and one set of resource lanes.
    Result<PreparedJob> PrepareFileOperation(FileOperationRequest Request, storage::OperationContext Context = {});
    Result<JobHandle> SubmitFileOperation(JobScheduler& Scheduler, FileOperationRequest Request,
        storage::OperationContext Context = {});
}
