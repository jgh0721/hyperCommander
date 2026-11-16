#pragma once
#include "UniqueLibs/system/environmentProfiles.hpp"
class QWidget;
namespace hc::environment {
std::optional<EnvironmentProfile> EditEnvironmentProfile(QWidget* Parent, EnvironmentProfile Profile, const QString& InitialName = {});
}
