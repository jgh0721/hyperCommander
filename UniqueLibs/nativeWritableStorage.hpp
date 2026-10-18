#pragma once
#include "commonLibs/storage/writableStorage.hpp"

namespace hc::storage
{
    // Only restores a missing destination from an identity-verified backup.
    // Published destinations and ambiguous artifacts are never removed/replayed.
    Result<WriteReceipt> RecoverNativeWrite(const WriteRecoveryRecord& Record);
    std::shared_ptr<IWritableStorage> MakeNativeWritableStorage(LocatorKind Kind,
        std::shared_ptr<IStorageProvider> Reader);
}
