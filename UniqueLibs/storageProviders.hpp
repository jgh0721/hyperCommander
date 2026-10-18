#pragma once
#include "commonLibs/storage/storage.hpp"

namespace hc::storage
{
    std::shared_ptr<IStorageProvider> MakeNativeReadProvider(LocatorKind Kind);
    Result<EntrySnapshot> BindNativeSnapshot(IStorageProvider& Provider, EntrySnapshot Entry);
    std::shared_ptr<IStorageProvider> MakeShellReadProvider(std::string RootParsingName);
    // The WCX owner copies its parsed tree before any worker request starts.
    using SnapshotDirectory = std::pair<Locator, std::vector<EntrySnapshot>>;
    std::shared_ptr<IStorageProvider> MakeArchiveReadProvider(Locator Root,
        std::vector<SnapshotDirectory> Directories);
}
