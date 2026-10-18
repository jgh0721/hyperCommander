#pragma once
#include "commonLibs/storage/writableStorage.hpp"

namespace hc::document
{
    // Worker-side, one transaction per invocation. No Qt/Win32/UI dependency.
    storage::Result<storage::WriteReceipt> SaveDocumentBytes(storage::IWritableStorage& Writer,
        storage::Locator Destination, storage::DestinationExpectation Expected,
        std::span<const std::byte> Bytes, storage::OperationContext Context = {});
}
