#pragma once
#include "commonLibs/storage/writableStorage.hpp"

namespace hc::storage
{
    std::shared_ptr<IWritableStorage> MakeNativeWritableStorage(LocatorKind Kind,
        std::shared_ptr<IStorageProvider> Reader);
}
