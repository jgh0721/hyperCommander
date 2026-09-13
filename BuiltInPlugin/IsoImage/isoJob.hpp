#pragma once
#include "commonLibs/operations/fileOperation.hpp"
#include <QString>

namespace hc::iso {
struct CreateRequest {
    std::vector<hc::operations::OperationSource> sources_;
    hc::storage::Locator destination_;
    unsigned profile_ = 2;
    QString volumeLabel_ = "HYPERCOMMANDER";
    std::string correlationId_;
};
hc::storage::Result<hc::operations::PreparedJob> PrepareCreate(CreateRequest Request,
    hc::storage::OperationContext Context = {});
}
