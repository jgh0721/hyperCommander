#pragma once
#include "commonLibs/operations/fileOperation.hpp"
#include "commonLibs/plugins/archiveBackend.hpp"

namespace hc::sevenzip {
struct CreateRequest {
    std::vector<hc::operations::OperationSource> sources_;
    hc::storage::Locator destination_;
    std::string format_ = "7z";
    hc::plugins::ArchiveCreationOptions options_;
    bool encrypt_ = false;
    std::string correlationId_;
};
hc::storage::Result<hc::operations::PreparedJob> PrepareCreate(CreateRequest Request,
    hc::storage::OperationContext Context = {});
}
