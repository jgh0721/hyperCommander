#pragma once
#include "scriptApi.hpp"

namespace hc::script {
std::unique_ptr<IScriptRuntime> MakeLuaRuntime();
// Parse only; never executes user code or exposes the host.
Result<Unit> CheckLuaSyntax(std::string_view Source);
}
