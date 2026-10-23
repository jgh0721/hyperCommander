#pragma once
#include "directoryPlan.hpp"
#include <functional>

namespace hc::compare
{
    Result RenameDirectoryEntry(const Request& Input, const Result& Comparison, int Side, const QString& Path, const QString& Name, const Budget& Limits);
    void ApplyDirectoryPlan(const Request& Input, Result& Value, const Budget& Limits,
        const std::function<void(std::size_t)>& AfterCommit = {});
}
