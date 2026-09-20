#pragma once
#include "commonLibs/storage/storage.hpp"
#include "commonLibs/plugins/archiveBackend.hpp"
#include <mutex>

// Legacy DLL calls share this gate. Hosted I/O does not hold it in the parent.
std::recursive_timed_mutex& WcxCallMutex();
namespace hc::storage
{
    using WcxBackends = std::vector<std::shared_ptr<hc::plugins::IArchiveBackend>>;
    struct WcxContainer
    {
        Locator root_;
        EntrySnapshot entry_;
        std::shared_ptr<IStorageProvider> reader_;
        std::shared_ptr<hc::plugins::IArchiveBackend> backend_;
    };
    Result<WcxContainer> ResolveWcxContainer(const std::shared_ptr<IStorageProvider>& Reader, OperationContext Context);
    // Construction does no archive I/O. The first enumeration mounts a snapshot
    // on a worker. A new provider is required to refresh a changed container.
    std::shared_ptr<IStorageProvider> MakeWcxReadProvider(std::string PluginPath, Locator Root,
        std::string TemporaryBase = {}, std::shared_ptr<hc::plugins::IArchiveBackend> Backend = {}, WcxBackends Ancestors = {});
}
