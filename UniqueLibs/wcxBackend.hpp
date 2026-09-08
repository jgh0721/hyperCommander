#pragma once
#include "commonLibs/plugins/archiveBackend.hpp"
#include <mutex>

// Serializes legacy WCX DLL entry points within this process only.
std::recursive_timed_mutex& WcxCallMutex();

namespace hc::plugins
{
    // Legacy fallback requires background capability. The isolated host invokes
    // the same adapter on its sole plugin thread and can support foreground WCX.
    std::shared_ptr<IArchiveBackend> MakeInProcessWcxBackend(std::string PluginPath, bool RequireBackground = true);
}
