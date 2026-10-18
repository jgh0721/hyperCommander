#pragma once
#include "scriptApi.hpp"

namespace hc::script {
std::unique_ptr<IScriptRuntime> MakeLuaRuntime();
// A compiled, worker-owned calculation. No command/file API is installed.
class IColumnRuntime {
public:
    virtual ~IColumnRuntime() = default;
    virtual Result<Unit> Prepare(std::string_view Source, RuntimeLimits Limits) = 0;
    virtual Result<ScriptValue> Invoke(const ScriptValue& Input) = 0;
    virtual void RequestInterrupt() = 0;
};
std::unique_ptr<IColumnRuntime> MakeLuaColumnRuntime();
// Parse only; never executes user code or exposes the host.
Result<Unit> CheckLuaSyntax(std::string_view Source);
}
