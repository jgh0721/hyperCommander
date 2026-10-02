#pragma once
#include "commonLibs/storage/materializedContent.hpp"
#include <functional>

namespace hc::storage
{
    enum class ProducedSize { Exact, Bounded, Growing };
    class INativeContentStore
    {
    public:
        virtual ~INativeContentStore() = default;
        // Runs on the caller's worker. The producer must close its file before
        // returning, observe cancellation/budgets, and never retain the path.
        // Bounded reserves Entry.size_ as a ceiling until release and returns
        // the actual size in the lease description.
        // Growing charges observed live bytes on producer progress and at close;
        // Entry.size_ remains the caller's ceiling, not an up-front reservation.
        virtual Result<std::shared_ptr<IMaterializedFileLease>> Produce(EntrySnapshot Entry,
            std::vector<std::shared_ptr<IMaterializedFileLease>> Parents, std::optional<std::uint64_t> CompressedBytes,
            OperationContext Context, std::function<Result<Unit>(const std::string&, OperationContext)> Producer,
            ProducedSize Size = ProducedSize::Exact) = 0;
    };
    Result<std::shared_ptr<INativeContentStore>> MakeNativeContentStore(const std::string& BaseDirectory,
        MaterializationLimits Limits = {});
    // BaseDirectory must exist on a local volume. Creates a private managed child.
    // The scheduler has a dedicated owner. The service may be released on a
    // different caller worker; destruction waits for jobs and cleanup to drain.
    Result<std::shared_ptr<IMaterializer>> MakeNativeMaterializer(const std::string& BaseDirectory,
        MaterializationLimits Limits = {});
}
