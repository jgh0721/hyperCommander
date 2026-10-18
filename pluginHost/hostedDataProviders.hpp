#pragma once
#include "pluginProcess.hpp"
#include "commonLibs/plugins/metadataProvider.hpp"
#include "commonLibs/plugins/contentConverter.hpp"
#include "commonLibs/storage/writableStorage.hpp"
#include "UniqueLibs/nativeMaterializer.hpp"

namespace hc::plugins
{
    struct HostedStorage
    {
        std::shared_ptr<storage::IStorageProvider> reader_;
        std::shared_ptr<storage::IWritableStorage> writer_;
        storage::Locator root_;
    };
    // Factory/probe and all adapter calls execute on workers, as do the native
    // storage adapters. The process and materialized leases outlive cursors.
    storage::Result<HostedStorage> MakeHostedWfxProvider(std::shared_ptr<PluginProcess> Process,
        storage::ProviderId Id, std::shared_ptr<storage::INativeContentStore> Store, bool Writable);
    storage::Result<std::shared_ptr<IMetadataProvider>> MakeHostedWdxProvider(std::shared_ptr<PluginProcess> Process,
        std::shared_ptr<storage::IMaterializer> Materializer);
    storage::Result<HostedStorage> MakeHostedDopusVfsProvider(std::shared_ptr<PluginProcess> Process,
        storage::ProviderId Id, std::shared_ptr<storage::INativeContentStore> Store, bool Writable, const QString& NativeRoot);
    storage::Result<std::shared_ptr<IMetadataProvider>> MakeHostedDopusMetadataProvider(std::shared_ptr<PluginProcess> Process,
        std::shared_ptr<storage::IMaterializer> Materializer);
    storage::Result<std::shared_ptr<IContentConverter>> MakeHostedDopusConverter(std::shared_ptr<PluginProcess> Process,
        std::shared_ptr<storage::IMaterializer> Materializer);
}
