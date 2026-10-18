#pragma once
#include "pluginProcess.hpp"
#include "commonLibs/plugins/archiveBackend.hpp"

namespace hc::plugins
{
    // The caller retains operation/lease ownership and final archive commit.
    std::shared_ptr<IArchiveBackend> MakeHostedArchiveBackend(std::shared_ptr<PluginProcess> Process);
}
