#pragma once
#include "commonLibs/system/environmentTypes.hpp"
class QWidget;
namespace hc::environment {
std::optional<Value> AskEnvironmentValue(QWidget* Parent, Scope Source);
}
