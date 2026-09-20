#include "scriptExecution.hpp"
#include <condition_variable>

namespace hc::script {
struct ScriptExecution::Mailbox final : IScriptHostApi {
    std::mutex mutex_;
    std::condition_variable changed_;
    std::string method_;
    std::optional<ScriptValue> arguments_;
    std::optional<Result<ScriptValue>> reply_, outcome_;
    bool closed_ = false;
    Result<ScriptValue> Call(std::string_view Method, const ScriptValue& Arguments) override {
        std::unique_lock Lock(mutex_);
        if (closed_) return Error{ErrorCategory::Cancelled, "script-session-closed"};
        method_ = Method; arguments_ = Arguments; reply_.reset();
        changed_.wait(Lock, [this] { return closed_ || reply_.has_value(); });
        if (closed_) return Error{ErrorCategory::Cancelled, "script-session-closed"};
        auto Reply = std::move(*reply_); reply_.reset(); return Reply;
    }
    void Close() override {
        { std::lock_guard Lock(mutex_); closed_ = true; arguments_.reset(); }
        changed_.notify_all();
    }
};
ScriptExecution::ScriptExecution(std::shared_ptr<IScriptHostApi> Api, std::string Source, RuntimeLimits Limits)
    : mailbox_(std::make_shared<Mailbox>()), api_(std::move(Api)), runtime_(MakeLuaRuntime()),
      deadline_(std::chrono::steady_clock::now() + Limits.wallTime_) {
    worker_ = std::jthread([this, Source = std::move(Source), Limits] {
        Result<ScriptValue> Outcome = Error{ErrorCategory::Internal, "runtime-worker"};
        try {
            auto Loaded = runtime_->Load(mailbox_, Limits);
            { std::lock_guard Lock(mailbox_->mutex_); if (mailbox_->closed_) runtime_->RequestInterrupt(); }
            Outcome = Loaded.HasValue() ? runtime_->Evaluate(Source) : Result<ScriptValue>(Loaded.Failure());
        } catch (...) { Outcome = Error{ErrorCategory::Internal, "runtime-worker"}; }
        runtime_->Unload();
        std::lock_guard Lock(mailbox_->mutex_); mailbox_->outcome_ = std::move(Outcome);
    });
}
ScriptExecution::~ScriptExecution() {
    Cancel();
    if (worker_.joinable()) worker_.join();
}
bool ScriptExecution::IsRunning() const { std::lock_guard Lock(mailbox_->mutex_); return !mailbox_->outcome_; }
std::optional<Result<ScriptValue>> ScriptExecution::Outcome() const { std::lock_guard Lock(mailbox_->mutex_); return mailbox_->outcome_; }
void ScriptExecution::Cancel() {
    runtime_->RequestInterrupt(); mailbox_->Close();
    if (!closed_) { closed_ = true; if (api_) api_->Close(); }
}
void ScriptExecution::Tick() {
    if (!IsRunning()) { if (!closed_) { closed_ = true; if (api_) api_->Close(); } return; }
    if (closed_) return;
    if (std::chrono::steady_clock::now() >= deadline_) { Cancel(); return; }
    std::optional<ScriptValue> Arguments; std::string Method;
    {
        std::lock_guard Lock(mailbox_->mutex_);
        if (!mailbox_->arguments_) return;
        Arguments = std::move(mailbox_->arguments_); mailbox_->arguments_.reset(); Method = mailbox_->method_;
    }
    Result<ScriptValue> Reply = Error{ErrorCategory::Cancelled, "script-host-unavailable"};
    try { if (api_) Reply = api_->Call(Method, *Arguments); }
    catch (...) { Reply = Error{ErrorCategory::Internal, "script-host-exception"}; }
    {
        std::lock_guard Lock(mailbox_->mutex_);
        if (!mailbox_->closed_) mailbox_->reply_ = std::move(Reply);
    }
    mailbox_->changed_.notify_all();
}
}
