#pragma once
#include "commonLibs/operations/fileOperation.hpp"
#include <functional>

namespace hc::operations {
struct ExtractRequest {
    std::vector<OperationSource> archives_;
    std::shared_ptr<storage::ISelectionManifest> selection_;
    storage::Locator sourceRoot_, destination_;
    std::shared_ptr<storage::IWritableStorage> writer_;
    // Factory captures the configured plugin priority and approvals on the UI
    // thread. Each returned provider performs mounting only on the job worker.
    std::function<std::vector<std::shared_ptr<storage::IStorageProvider>>(const storage::EntrySnapshot&)> readers_;
    bool separateFolders_ = true;
    ConflictPolicy conflict_ = ConflictPolicy::Ask;
    std::shared_ptr<IFileElevationService> elevation_;
};
storage::Result<PreparedJob> PrepareExtract(ExtractRequest Request, storage::OperationContext Context = {});
}
