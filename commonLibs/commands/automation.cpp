#include "automation.hpp"

namespace hc::automation {
Execution::Execution(Program Program, Host Host, std::set<std::string> Permissions, std::string RunId, OperationContext Context)
    : program_(std::move(Program)), host_(std::move(Host)), permissions_(std::move(Permissions)),
      runId_(std::move(RunId)), context_(Context), callerStop_(Context.stopToken_)
{
    context_.stopToken_ = stop_.get_token();
    context_.deadline_ = std::min(context_.deadline_, std::chrono::steady_clock::now() + std::chrono::minutes(5));
    if (program_.empty() || program_.size() > 256 || runId_.empty() || !host_.capture_ || !host_.dispatch_ || !host_.poll_ || !host_.cancel_)
        fail({ErrorCategory::InvalidRequest, "automation-program-or-host"});
}
Execution::~Execution() { Cancel(); }
void Execution::fail(Error Failure)
{
    stop_.request_stop();
    if (!invocation_.id_.empty()) { try { host_.cancel_(invocation_, accepted_.value_or(CommandOutcome{})); } catch (...) {} }
    failure_ = std::move(Failure);
    state_ = failure_->category_ == ErrorCategory::Cancelled ? State::Cancelled : State::Failed;
}
void Execution::Cancel()
{
    if (state_ == State::Running) fail({ErrorCategory::Cancelled, "automation-cancelled"});
}
void Execution::Tick()
{
    if (state_ != State::Running) return;
    if (callerStop_.stop_requested()) { Cancel(); return; }
    if (const auto Failure = context_.Check({"automation"})) { fail(*Failure); return; }
    try {
        if (pending_) {
            if (pending_->wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
            auto Result = pending_->get(); pending_.reset();
            if (!Result.HasValue()) { fail(Result.Failure()); return; }
            if (Result.Value().status_ == CommandOutcome::Status::Accepted) {
                if (Result.Value().jobId_.empty()) { fail({ErrorCategory::Internal, "automation-missing-job"}); return; }
                accepted_ = Result.Value();
            } else ++offset_;
        }
        if (accepted_) {
            auto Finished = host_.poll_(invocation_, *accepted_);
            if (!Finished.HasValue()) { fail(Finished.Failure()); return; }
            if (!Finished.Value()) return;
            accepted_.reset(); ++offset_;
        }
        if (offset_ == program_.size()) { state_ = State::Completed; return; }
        auto Context = host_.capture_();
        if (!Context.HasValue()) { fail(Context.Failure()); return; }
        invocation_ = {}; invocation_.id_ = program_[offset_].command_; invocation_.parameters_ = program_[offset_].parameters_;
        invocation_.context_ = std::move(Context.Value()); invocation_.permissions_ = permissions_;
        invocation_.source_ = "automation"; invocation_.correlationId_ = runId_ + ":" + std::to_string(offset_ + 1);
        invocation_.causationId_ = runId_; invocation_.operation_ = context_;
        pending_.emplace(host_.dispatch_(invocation_));
    } catch (...) { fail({ErrorCategory::Internal, "automation-host-exception"}); }
}
}
