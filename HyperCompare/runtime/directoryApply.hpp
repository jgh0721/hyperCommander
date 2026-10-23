#pragma once
#include "directoryPlan.hpp"
#include <functional>

namespace hc::compare
{
    void ApplyDirectoryPlan(const Request& Input, Result& Value, const Budget& Limits,
        const std::function<void(std::size_t)>& AfterCommit = {});
}
