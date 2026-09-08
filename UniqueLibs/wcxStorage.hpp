#pragma once
#include "commonLibs/storage/storage.hpp"
#include "commonLibs/plugins/archiveBackend.hpp"
#include <mutex>

// Legacy DLL calls share this gate. Hosted I/O does not hold it in the parent.
std::recursive_timed_mutex& WcxCallMutex();
namespace hc::storage
{
    // Construction does no archive I/O. The first enumeration mounts a snapshot
    // on a worker. A new provider is required to refresh a changed container.
    std::shared_ptr<IStorageProvider> MakeWcxReadProvider(std::string PluginPath, Locator Root,
        std::string TemporaryBase = {}, std::shared_ptr<hc::plugins::IArchiveBackend> Backend = {});
}
