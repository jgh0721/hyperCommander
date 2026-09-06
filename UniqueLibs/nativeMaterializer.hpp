#pragma once
#include "commonLibs/storage/materializedContent.hpp"
#include <functional>

namespace hc::storage
{
    class INativeContentStore
    {
    public:
        virtual ~INativeContentStore() = default;
        // Runs on the caller's worker. The producer must close its file before
        // returning, observe cancellation/budgets, and never retain the path.
        virtual Result<std::shared_ptr<IMaterializedFileLease>> Produce(EntrySnapshot Entry,
            std::vector<std::shared_ptr<IMaterializedFileLease>> Parents, std::optional<std::uint64_t> CompressedBytes,
            OperationContext Context, std::function<Result<Unit>(const std::string&, OperationContext)> Producer) = 0;
    };
    Result<std::shared_ptr<INativeContentStore>> MakeNativeContentStore(const std::string& BaseDirectory,
        MaterializationLimits Limits = {});
    // BaseDirectory must exist on a local volume. Creates a private managed child.
    // Like JobScheduler, create and destroy the service on its owner thread.
    Result<std::shared_ptr<IMaterializer>> MakeNativeMaterializer(const std::string& BaseDirectory,
        MaterializationLimits Limits = {});
}
