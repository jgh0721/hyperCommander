#pragma once
#include "commandBus.hpp"

namespace hc::script { struct Services; }

namespace hc::automation {
using namespace storage;
using namespace commands;
enum class ExpressionKind { Literal, Variable, Not, And, Or, Equal, NotEqual, Less, LessEqual, Greater, GreaterEqual };
struct Expression {
    ExpressionKind kind_ = ExpressionKind::Literal;
    Value literal_ = false;
    std::string variable_;
    std::shared_ptr<const Expression> left_, right_;
};
using ExpressionPtr = std::shared_ptr<const Expression>;
enum class StepKind { Invoke, Assign, Branch, Jump };
struct Step {
    std::string command_;
    Parameters parameters_;
    std::size_t line_ = 1;
    StepKind kind_ = StepKind::Invoke;
    std::map<std::string, ExpressionPtr> arguments_;
    ExpressionPtr expression_;
    std::string binding_;
    std::size_t target_ = 0, pipeline_ = 0;
};
enum class FailurePolicy { Stop, Continue };
struct Program : std::vector<Step> {
    using std::vector<Step>::vector;
    std::uint32_t version_ = 1;
    FailurePolicy failurePolicy_ = FailurePolicy::Stop;
};
using SchemaLookup = std::function<Result<CommandDescriptor>(const std::string&)>;
Result<Value> Evaluate(const ExpressionPtr& Expression, const Parameters& Variables, std::size_t Depth = 0);

// Callbacks run on the caller's thread. Dispatch returns a nonblocking promise
// future; Poll reports actual completion, not merely command acceptance.
struct Host {
    std::function<Result<CommandContext>()> capture_;
    std::function<Task<CommandOutcome>(CommandInvocation)> dispatch_;
    std::function<Result<bool>(const CommandInvocation&, const CommandOutcome&)> poll_;
    std::function<void(const CommandInvocation&, const CommandOutcome&)> cancel_;
    std::function<Result<Parameters>(const CommandInvocation&, const CommandOutcome&)> output_;
    SchemaLookup schema_;
    std::shared_ptr<script::Services> script_;
};
enum class State { Running, Completed, CompletedWithErrors, Failed, Cancelled };
struct StepFailure { std::size_t line_; Error error_; };
class Execution {
public:
    Execution(Program Program, Host Host, std::set<std::string> Permissions, std::string RunId,
        OperationContext Context = {});
    ~Execution();
    void Tick();
    void Cancel();
    State Status() const { return state_; }
    std::size_t CompletedSteps() const { return completed_; }
    std::size_t Line() const { return offset_ < program_.size() ? program_[offset_].line_ : 0; }
    const std::optional<Error>& Failure() const { return failure_; }
    const std::vector<StepFailure>& Errors() const { return errors_; }
    const Parameters& Variables() const { return variables_; }
private:
    void fail(Error Failure);
    void commandFailed(Error Failure);
    void commandCompleted(CommandOutcome Outcome);
    void bindResult(Parameters Values);
    Program program_;
    Host host_;
    std::set<std::string> permissions_;
    std::string runId_;
    OperationContext context_;
    std::stop_source stop_;
    std::stop_token callerStop_;
    State state_ = State::Running;
    std::size_t offset_ = 0;
    std::size_t completed_ = 0, ticks_ = 0;
    CommandInvocation invocation_;
    std::optional<Task<CommandOutcome>> pending_;
    std::optional<CommandOutcome> accepted_;
    std::optional<Error> failure_;
    std::vector<StepFailure> errors_;
    Parameters variables_{{"result.ok", false}, {"result.code", std::string("not-run")}, {"result.jobId", std::string{}}};
};
}
