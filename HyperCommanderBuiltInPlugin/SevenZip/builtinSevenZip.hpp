#pragma once
#include "pluginHost/approvedWcxBackend.hpp"
namespace hc::plugins {
ApprovedWcxOptions BuiltinSevenZipOptions();
// Encode only a fresh private candidate with the bundled codec.
storage::Result<storage::Unit> CreateBuiltinSevenZipArchive(ArchiveUpdate Request, storage::OperationContext Context);
}
