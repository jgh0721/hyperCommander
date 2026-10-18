#pragma once
#include "updateProtocol.hpp"
#include <QString>

namespace hc::update {
void StageArchive(const QString& Zip, const QString& Stage, const QString& Runtime, const Manifest& Release, std::stop_token Stop = {});
}
