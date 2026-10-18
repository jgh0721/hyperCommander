#include "automation.hpp"

namespace hc::automation {
namespace {
Error ExpressionError(const std::string& Code) { return {ErrorCategory::InvalidRequest, "automation-expression:" + Code}; }
}
Result<Value> Evaluate(const ExpressionPtr& Expression, const Parameters& Variables, std::size_t Depth)
{
    if (!Expression || Depth > 32) return ExpressionError("depth-or-empty");
    const auto& E = *Expression;
    if (E.kind_ == ExpressionKind::Literal) return E.literal_;
    if (E.kind_ == ExpressionKind::Variable) {
        const auto Found = Variables.find(E.variable_);
        return Found == Variables.end() ? Result<Value>(ExpressionError("unknown-variable:" + E.variable_)) : Result<Value>(Found->second);
    }
    auto Left = Evaluate(E.left_, Variables, Depth + 1); if (!Left.HasValue()) return Left.Failure();
    if (E.kind_ == ExpressionKind::Not || E.kind_ == ExpressionKind::And || E.kind_ == ExpressionKind::Or) {
        if (!std::holds_alternative<bool>(Left.Value())) return ExpressionError("boolean-required");
        const auto Boolean = std::get<bool>(Left.Value());
        if (E.kind_ == ExpressionKind::Not) return Value(!Boolean);
        if ((E.kind_ == ExpressionKind::And && !Boolean) || (E.kind_ == ExpressionKind::Or && Boolean)) return Value(Boolean);
        auto Right = Evaluate(E.right_, Variables, Depth + 1); if (!Right.HasValue()) return Right.Failure();
        if (!std::holds_alternative<bool>(Right.Value())) return ExpressionError("boolean-required");
        return Right.Value();
    }
    auto Right = Evaluate(E.right_, Variables, Depth + 1); if (!Right.HasValue()) return Right.Failure();
    if (E.kind_ == ExpressionKind::Equal) return Value(Left.Value() == Right.Value());
    if (E.kind_ == ExpressionKind::NotEqual) return Value(Left.Value() != Right.Value());
    if (Left.Value().index() != Right.Value().index() || std::holds_alternative<bool>(Left.Value())) return ExpressionError("comparable-values-required");
    const bool Less = Left.Value() < Right.Value(), Equal = Left.Value() == Right.Value();
    switch (E.kind_) {
    case ExpressionKind::Less: return Value(Less);
    case ExpressionKind::LessEqual: return Value(Less || Equal);
    case ExpressionKind::Greater: return Value(!Less && !Equal);
    case ExpressionKind::GreaterEqual: return Value(!Less);
    default: return ExpressionError("operator");
    }
}
Execution::Execution(Program Program, Host Host, std::set<std::string> Permissions, std::string RunId, OperationContext Context)
    : program_(std::move(Program)), host_(std::move(Host)), permissions_(std::move(Permissions)),
      runId_(std::move(RunId)), context_(Context), callerStop_(Context.stopToken_)
{
    context_.stopToken_ = stop_.get_token();
    context_.deadline_ = std::min(context_.deadline_, std::chrono::steady_clock::now() + std::chrono::minutes(5));
    if (program_.empty() || program_.size() > 256 || program_.version_ != 1 || runId_.empty() || !host_.capture_ || !host_.dispatch_ || !host_.poll_ || !host_.cancel_)
        fail({ErrorCategory::InvalidRequest, "automation-program-or-host"});
    for (std::size_t I = 0; I < program_.size(); ++I)
        if ((program_[I].kind_ == StepKind::Branch || program_[I].kind_ == StepKind::Jump) && (program_[I].target_ <= I || program_[I].target_ > program_.size()))
            fail({ErrorCategory::InvalidRequest, "automation-forward-branch-required"});
}
Execution::~Execution() { Cancel(); }
void Execution::fail(Error Failure)
{
    stop_.request_stop();
    if (!invocation_.id_.empty()) { try { host_.cancel_(invocation_, accepted_.value_or(CommandOutcome{})); } catch (...) {} }
    failure_ = std::move(Failure);
    state_ = failure_->category_ == ErrorCategory::Cancelled ? State::Cancelled : State::Failed;
}
void Execution::Cancel() { if (state_ == State::Running) fail({ErrorCategory::Cancelled, "automation-cancelled"}); }
void Execution::bindResult(Parameters Values)
{
    const auto Bind = [&](const std::string& Prefix) {
        for (auto It = variables_.begin(); It != variables_.end();) {
            if (It->first.starts_with(Prefix + '.')) It = variables_.erase(It); else ++It;
        }
        for (const auto& [Name, Value] : Values) variables_[Prefix + '.' + Name] = Value;
    };
    Bind("result"); if (!program_[offset_].binding_.empty()) Bind(program_[offset_].binding_);
}
void Execution::commandFailed(Error Failure)
{
    // Continue is for operational failures. Permission, invalid handles, syntax,
    // cancellation and resource budgets always terminate the run.
    if (program_.failurePolicy_ != FailurePolicy::Continue || Failure.code_ == "deadline-exceeded"
        || (Failure.category_ != ErrorCategory::NotFound && Failure.category_ != ErrorCategory::Unavailable)) { fail(std::move(Failure)); return; }
    if (accepted_) host_.cancel_(invocation_, *accepted_);
    accepted_.reset(); errors_.push_back({Line(), Failure});
    bindResult({{"ok", false}, {"code", Failure.code_}, {"jobId", std::string{}}});
    const auto Group = program_[offset_].pipeline_; ++offset_;
    while (Group && offset_ < program_.size() && program_[offset_].pipeline_ == Group) ++offset_;
}
void Execution::commandCompleted(CommandOutcome Outcome)
{
    Parameters Values;
    if (host_.output_) {
        auto Output = host_.output_(invocation_, Outcome);
        if (!Output.HasValue()) { commandFailed(Output.Failure()); return; }
        Values = std::move(Output.Value());
    }
    Values["ok"] = true; Values["code"] = std::string{}; Values["jobId"] = Outcome.jobId_;
    if (Values.size() > 64) { fail({ErrorCategory::BudgetExceeded, "automation-output-fields"}); return; }
    bindResult(std::move(Values)); accepted_.reset(); ++offset_; ++completed_;
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
            if (!Result.HasValue()) commandFailed(Result.Failure());
            else if (Result.Value().status_ == CommandOutcome::Status::Accepted) {
                if (Result.Value().jobId_.empty()) { fail({ErrorCategory::Internal, "automation-missing-job"}); return; }
                accepted_ = Result.Value();
            } else commandCompleted(Result.Value());
            if (state_ != State::Running) return;
        }
        if (accepted_) {
            auto Finished = host_.poll_(invocation_, *accepted_);
            if (!Finished.HasValue()) commandFailed(Finished.Failure());
            else if (!Finished.Value()) return;
            else commandCompleted(*accepted_);
            if (state_ != State::Running) return;
        }
        for (int Batch = 0; Batch < 32; ++Batch) {
            if (offset_ == program_.size()) { state_ = errors_.empty() ? State::Completed : State::CompletedWithErrors; return; }
            if (++ticks_ > 1024 || variables_.size() > 1024) { fail({ErrorCategory::BudgetExceeded, "automation-step-or-variable-limit"}); return; }
            auto Context = host_.capture_(); if (!Context.HasValue()) { fail(Context.Failure()); return; }
            variables_["context.path"] = Context.Value().location_.root_;
            variables_["context.selectionCount"] = std::int64_t(Context.Value().selection_.size());
            const auto& Step = program_[offset_];
            if (Step.kind_ == StepKind::Jump) { offset_ = Step.target_; continue; }
            if (Step.kind_ == StepKind::Assign || Step.kind_ == StepKind::Branch) {
                auto Value = Evaluate(Step.expression_, variables_); if (!Value.HasValue()) { fail(Value.Failure()); return; }
                if (Step.kind_ == StepKind::Assign) { variables_[Step.binding_] = Value.Value(); ++offset_; }
                else {
                    if (!std::holds_alternative<bool>(Value.Value())) { fail(ExpressionError("boolean-required")); return; }
                    offset_ = std::get<bool>(Value.Value()) ? offset_ + 1 : Step.target_;
                }
                continue;
            }
            invocation_ = {}; invocation_.id_ = Step.command_; invocation_.parameters_ = Step.parameters_;
            for (const auto& [Name, Expression] : Step.arguments_) {
                auto Value = Evaluate(Expression, variables_); if (!Value.HasValue()) { fail(Value.Failure()); return; }
                invocation_.parameters_[Name] = Value.Value();
            }
            invocation_.context_ = std::move(Context.Value()); invocation_.permissions_ = permissions_;
            invocation_.source_ = "automation"; invocation_.correlationId_ = runId_ + ":" + std::to_string(offset_ + 1);
            invocation_.causationId_ = runId_; invocation_.operation_ = context_;
            pending_.emplace(host_.dispatch_(invocation_)); return;
        }
    } catch (...) { fail({ErrorCategory::Internal, "automation-host-exception"}); }
}
}
