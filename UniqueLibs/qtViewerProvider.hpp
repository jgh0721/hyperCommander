#pragma once
#include "commonLibs/viewer/viewer.hpp"

namespace hc::viewer
{
    std::shared_ptr<ViewerRegistry> MakeBuiltinViewerRegistry();
    bool CanViewBuiltinMultimedia(const storage::EntrySnapshot& Source, std::span<const std::byte> Prefix);
}
