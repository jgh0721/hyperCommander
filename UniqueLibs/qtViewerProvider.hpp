#pragma once
#include "commonLibs/viewer/viewer.hpp"

namespace hc::viewer
{
    std::shared_ptr<ViewerRegistry> MakeBuiltinViewerRegistry();
}
