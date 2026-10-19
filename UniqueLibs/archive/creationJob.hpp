#pragma once
#include "creatorRegistry.hpp"

namespace hc::archive {
struct CreateJobRequest {
    plugins::ArchiveCreateRequest value_;
    CreatorRuntime creator_;
    std::vector<operations::OperationSource> sources_;
    std::shared_ptr<storage::ISelectionManifest> selection_;
    std::set<std::string> completedOutputs_;
};
storage::Result<operations::PreparedJob> PrepareCreation(CreateJobRequest Request, storage::OperationContext Context = {});
CreateJobRequest RetryCreation(CreateJobRequest Request, const operations::JobResult& Result);
}
