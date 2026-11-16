#pragma once
#include "commonLibs/system/environmentAnalysis.hpp"
#include <QString>
#include <memory>
#include "commonLibs/system/processEnvironment.hpp"
#include <functional>
class QWidget;
namespace hc::environment {
struct NamedSearchContext { QString name_; SearchContext context_; };
QString FindEnvironmentCommand(QWidget* Parent, const std::vector<NamedSearchContext>& Contexts, std::shared_ptr<IPathProbe> Probe);
std::optional<hc::system::ProcessKey> FindStaleEnvironmentProcess(QWidget* Parent, ValueMap Expected, bool Native,
    std::function<ProcessEnvironment(const hc::system::ProcessKey&)> Read);
}
