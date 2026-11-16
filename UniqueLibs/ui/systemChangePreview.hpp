#pragma once
#include "UniqueLibs/registry/registryHistory.hpp"
class QWidget;
namespace hc::ui {
bool ConfirmSystemChanges(QWidget* Parent, const hc::registry::RegistryPlan& Plan, const QString& Title);
}
