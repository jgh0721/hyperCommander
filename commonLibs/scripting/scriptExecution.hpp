#pragma once
#include "luaRuntime.hpp"

namespace hc::script {
// Owner calls Tick/Cancel; only copied values cross the worker mailbox.
// Destruction revokes host access before joining the worker, without a UI event loop.
class ScriptExecution {
public:
    ScriptExecution(std::shared_ptr<IScriptHostApi> Api, std::string Source, RuntimeLimits Limits = {});
    ~ScriptExecution();
    void Tick();
    void Cancel();
    bool IsRunning() const;
    std::optional<Result<ScriptValue>> Outcome() const;
private:
    struct Mailbox;
    std::shared_ptr<Mailbox> mailbox_;
    std::shared_ptr<IScriptHostApi> api_;
    std::unique_ptr<IScriptRuntime> runtime_;
    std::jthread worker_;
    std::chrono::steady_clock::time_point deadline_;
    bool closed_ = false;
};
}
