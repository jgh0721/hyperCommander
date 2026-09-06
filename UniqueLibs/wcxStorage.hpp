#pragma once
#include "commonLibs/storage/storage.hpp"
#include "commonLibs/plugins/archiveBackend.hpp"
#include <mutex>

// Legacy and provider calls share this gate until M6 moves WCX to its host.
std::recursive_timed_mutex& WcxCallMutex();
namespace hc::storage
{
    // Construction does no archive I/O. The first enumeration mounts a snapshot
    // on a worker. A new provider is required to refresh a changed container.
    std::shared_ptr<IStorageProvider> MakeWcxReadProvider(std::string PluginPath, Locator Root,
        std::string TemporaryBase = {}, std::shared_ptr<hc::plugins::IArchiveBackend> Backend = {});
}
