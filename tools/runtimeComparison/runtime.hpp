#pragma once
#include "wire.hpp"
namespace hc::script::comparison {
std::unique_ptr<IScriptRuntime> MakeRuntime();
}
