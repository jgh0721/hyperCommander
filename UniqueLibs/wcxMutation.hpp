#pragma once
#include "commonLibs/storage/archiveMutation.hpp"

namespace hc::storage
{
    // Reader is the selected archive's navigation session, so expected IDs are
    // checked against the same snapshots used by conflict/selection decisions.
    Result<std::unique_ptr<IArchiveMutationSession>> BeginWcxMutation(std::string PluginPath,
        Locator Root, std::shared_ptr<IStorageProvider> Reader, std::string TemporaryBase,
        ArchiveMutationLimits Limits = {}, OperationContext Context = {},
        std::shared_ptr<IWritableStorage> DestinationWriter = {});
}
