#pragma once
#include "commonLibs/columns/columnQueryEngine.hpp"
namespace hc::columns { FieldValues ReadNativeFields(const QString& Path, const QString& Family, std::stop_token Stop); }
