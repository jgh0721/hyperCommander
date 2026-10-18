#pragma once
#include "include/hcIso/isoImage.h"
#include "commonLibs/storage/storage.hpp"
#include <Windows.h>

namespace hc::iso {
// The replaceable LGPL core remains a DLL. No engine implementation is linked
// into the application; only the bundled, build-pinned C ABI is loaded here.
struct Runtime final {
    HMODULE module_ = nullptr;
    std::vector<HANDLE> pins_;
    decltype(&hcIsoOpenInput) openInput_ = nullptr;
    decltype(&hcIsoClose) close_ = nullptr;
    decltype(&hcIsoGetInfo) info_ = nullptr;
    decltype(&hcIsoGetEntry) entry_ = nullptr;
    decltype(&hcIsoFindEntry) find_ = nullptr;
    decltype(&hcIsoGetChild) child_ = nullptr;
    decltype(&hcIsoRead) read_ = nullptr;
    decltype(&hcIsoBuild) build_ = nullptr;
    ~Runtime();
    Runtime() = default;
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;
};
storage::Result<std::shared_ptr<Runtime>> LoadRuntime(const storage::OperationContext& Context);
storage::Error EngineError(int Status);
}
