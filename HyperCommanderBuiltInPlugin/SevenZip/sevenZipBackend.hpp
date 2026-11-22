#pragma once
#include "commonLibs/plugins/archiveBackend.hpp"
#include <Windows.h>
namespace hc::plugins {
// The creation job owns the private output directory and cleans it on every failure.
storage::Result<storage::Unit> EncodePrivateSevenZipCandidate(HMODULE Module, ArchiveUpdate Request, storage::OperationContext Context);
// The isolated host owns the module and outlives every cursor.
std::shared_ptr<IArchiveBackend> MakeSevenZipBackend(HMODULE Module);
}
