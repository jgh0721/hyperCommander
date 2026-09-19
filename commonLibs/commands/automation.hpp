#pragma once
#include "commandBus.hpp"

namespace hc::automation {
using namespace storage;
using namespace commands;
struct Step {
    std::string command_;
    Parameters parameters_;
    std::size_t line_ = 1;
};
using Program = std::vector<Step>;

// Callbacks run on the caller's thread. Dispatch returns a nonblocking promise
// future; Poll reports actual completion, not merely command acceptance.
struct Host {
    std::function<Result<CommandContext>()> capture_;
    std::function<Task<CommandOutcome>(CommandInvocation)> dispatch_;
    std::function<Result<bool>(const CommandInvocation&, const CommandOutcome&)> poll_;
    std::function<void(const CommandInvocation&, const CommandOutcome&)> cancel_;
};
enum class State { Running, Completed, Failed, Cancelled };
class Execution {
public:
    Execution(Program Program, Host Host, std::set<std::string> Permissions, std::string RunId,
        OperationContext Context = {});
    ~Execution();
    void Tick();
    void Cancel();
    State Status() const { return state_; }
    std::size_t CompletedSteps() const { return offset_; }
    std::size_t Line() const { return offset_ < program_.size() ? program_[offset_].line_ : 0; }
    const std::optional<Error>& Failure() const { return failure_; }
private:
    void fail(Error Failure);
    Program program_;
    Host host_;
    std::set<std::string> permissions_;
    std::string runId_;
    OperationContext context_;
    std::stop_source stop_;
    std::stop_token callerStop_;
    State state_ = State::Running;
    std::size_t offset_ = 0;
    CommandInvocation invocation_;
    std::optional<Task<CommandOutcome>> pending_;
    std::optional<CommandOutcome> accepted_;
    std::optional<Error> failure_;
};
}
