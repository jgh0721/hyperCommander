#pragma once
#include "commonLibs/plugins/archiveBackend.hpp"
#include <Windows.h>
namespace hc::plugins {
// The isolated host owns the module and outlives every cursor.
std::shared_ptr<IArchiveBackend> MakeSevenZipBackend(HMODULE Module);
}
