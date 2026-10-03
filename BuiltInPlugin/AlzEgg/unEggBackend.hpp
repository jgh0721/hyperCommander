#pragma once
#include "commonLibs/plugins/archiveBackend.hpp"
#include <Windows.h>
namespace hc::plugins {
// The caller owns the isolated host module and outlives every listing.
std::shared_ptr<IArchiveBackend> MakeUnEggBackend(HMODULE Module);
}
