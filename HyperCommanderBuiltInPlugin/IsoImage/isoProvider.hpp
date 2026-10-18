#pragma once
#include "commonLibs/storage/storage.hpp"
#include "include/hcIso/isoImage.h"

namespace hc::iso {
// A top-level local/SMB image. Locators retain the existing wcx hop so saved
// locations and the transitional WCX mutation service remain compatible.
std::shared_ptr<storage::IStorageProvider> MakeReadProvider(storage::Locator Root,
    uint32_t Namespace = HC_ISO_AUTO);
}
