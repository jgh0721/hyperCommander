#pragma once
#include "commonLibs/storage/materializedContent.hpp"

namespace hc::storage
{
    // BaseDirectory must exist on a local volume. Creates a private managed child.
    // Like JobScheduler, create and destroy the service on its owner thread.
    Result<std::shared_ptr<IMaterializer>> MakeNativeMaterializer(const std::string& BaseDirectory,
        MaterializationLimits Limits = {});
}
