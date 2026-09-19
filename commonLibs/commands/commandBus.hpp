#pragma once
#include "../storage/storage.hpp"
#include <deque>
#include <atomic>
#include <functional>
#include <set>

namespace hc::commands
{
    using storage::Error;
    using storage::ErrorCategory;
    using storage::Result;
    using storage::Unit;
    using Value = std::variant<bool, std::int64_t, std::string>;
    using Parameters = std::map<std::string, Value>;

    class IExecutor
    {
    public:
        virtual ~IExecutor() = default;
        // Post never executes work after returning false. Accepted work runs once
        // or is destroyed on shutdown; implementations must not retain it forever.
        virtual bool Post(std::function<void()> Work) = 0;
    };
    struct EventEnvelope
    {
        std::string id_;
        std::uint32_t schemaVersion_ = 1;
        std::chrono::system_clock::time_point timestamp_;
        std::string producer_;
        std::string correlationId_;
        std::string causationId_;
        std::uint64_t sequence_ = 0;
        Parameters payload_;
        bool progress_ = false;
    };
    struct PublishResult
    {
        std::size_t accepted_ = 0;
        std::size_t coalesced_ = 0;
        std::size_t rejected_ = 0;
    };
    namespace detail
    {
        struct Subscriber : std::enable_shared_from_this<Subscriber>
        {
            std::mutex mutex_;
            bool active_ = true;
            bool scheduled_ = false;
            std::size_t capacity_ = 128;
            std::string filter_;
            std::deque<EventEnvelope> queue_;
            std::shared_ptr<IExecutor> executor_;
            std::function<void(const EventEnvelope&)> callback_;
            void Close()
            {
                std::lock_guard Lock(mutex_);
                active_ = false;
                queue_.clear();
            }
            bool Schedule()
            {
                const auto Self = shared_from_this();
                try { if (executor_->Post([Self] { Self->Drain(); })) return true; }
                catch (...) {}
                Close();
                return false;
            }
            void Drain()
            {
                // Yield between bounded batches so UI work is not starved.
                for (int Index = 0; Index < 16; ++Index)
                {
                    EventEnvelope Event;
                    {
                        std::lock_guard Lock(mutex_);
                        if (!active_ || queue_.empty()) { scheduled_ = false; return; }
                        Event = std::move(queue_.front());
                        queue_.pop_front();
                    }
                    try { callback_(Event); }
                    catch (...) { Close(); return; }
                }
                Schedule();
            }
        };
    }
    class EventSubscription
    {
    public:
        EventSubscription() = default;
        explicit EventSubscription(std::shared_ptr<detail::Subscriber> State) : state_(std::move(State)) {}
        EventSubscription(const EventSubscription&) = delete;
        EventSubscription& operator=(const EventSubscription&) = delete;
        EventSubscription(EventSubscription&&) = default;
        EventSubscription& operator=(EventSubscription&& Other) noexcept
        {
            if (this != &Other) { Reset(); state_ = std::move(Other.state_); }
            return *this;
        }
        ~EventSubscription() { Reset(); }
        void Reset() { if (state_) state_->Close(); state_.reset(); }
    private:
        std::shared_ptr<detail::Subscriber> state_;
    };
    class EventHub
    {
    public:
        Result<EventSubscription> Subscribe(std::string Filter, std::shared_ptr<IExecutor> Executor,
            std::function<void(const EventEnvelope&)> Callback, std::size_t Capacity = 128)
        {
            if (!Executor || !Callback || !Capacity || Capacity > 65536)
                return Error{ErrorCategory::InvalidRequest, "invalid-subscription"};
            auto State = std::make_shared<detail::Subscriber>();
            State->filter_ = std::move(Filter);
            State->executor_ = std::move(Executor);
            State->callback_ = std::move(Callback);
            State->capacity_ = Capacity;
            std::lock_guard Lock(mutex_);
            subscribers_.push_back(State);
            return EventSubscription(std::move(State));
        }
        PublishResult Publish(EventEnvelope Event)
        {
            if (Event.id_.empty() || Event.schemaVersion_ != 1) return {0, 0, 1};
            PublishResult Result;
            std::vector<std::shared_ptr<detail::Subscriber>> Schedule;
            {
                // Numbering and enqueueing share one lock, even with concurrent producers.
                std::lock_guard Lock(mutex_);
                Event.sequence_ = ++sequence_;
                Event.timestamp_ = std::chrono::system_clock::now();
                for (auto It = subscribers_.begin(); It != subscribers_.end();)
                {
                    const auto State = It->lock();
                    if (!State) { It = subscribers_.erase(It); continue; }
                    ++It;
                    std::lock_guard StateLock(State->mutex_);
                    if (!State->active_ || (!State->filter_.empty() && State->filter_ != Event.id_)) continue;
                    if (Event.progress_)
                    {
                        const auto Old = std::find_if(State->queue_.begin(), State->queue_.end(), [&](const auto& Pending) {
                            return Pending.progress_ && Pending.id_ == Event.id_ && Pending.correlationId_ == Event.correlationId_;
                        });
                        if (Old != State->queue_.end()) { State->queue_.erase(Old); ++Result.coalesced_; }
                    }
                    if (State->queue_.size() >= State->capacity_) { ++Result.rejected_; continue; }
                    State->queue_.push_back(Event);
                    ++Result.accepted_;
                    if (!State->scheduled_) { State->scheduled_ = true; Schedule.push_back(State); }
                }
            }
            for (const auto& State : Schedule)
                if (!State->Schedule()) { --Result.accepted_; ++Result.rejected_; }
            return Result;
        }
    private:
        std::mutex mutex_;
        std::uint64_t sequence_ = 0;
        std::vector<std::weak_ptr<detail::Subscriber>> subscribers_;
    };

    enum class ParameterType { Boolean, Integer, String };
    struct ParameterSpec { ParameterType type_; bool required_ = true; };
    struct CommandDescriptor
    {
        std::string id_;
        std::uint32_t schemaVersion_ = 1;
        std::map<std::string, ParameterSpec> parameters_;
        std::string permission_ = "navigation";
    };
    struct CommandInvocation
    {
        std::string id_;
        std::uint32_t schemaVersion_ = 1;
        Parameters parameters_;
        storage::CommandContext context_;
        std::set<std::string> permissions_;
        std::string source_;
        std::string correlationId_;
        std::string causationId_;
        storage::OperationContext operation_;
    };
    struct CommandOutcome
    {
        enum class Status { Completed, Accepted };
        Status status_ = Status::Completed;
        std::string jobId_;
    };
    inline Result<Unit> ValidateInvocation(const CommandDescriptor& Descriptor, const CommandInvocation& Invocation)
    {
        if (Invocation.id_ != Descriptor.id_ || Invocation.schemaVersion_ != Descriptor.schemaVersion_)
            return Error{ErrorCategory::InvalidRequest, "schema-version"};
        if (!Descriptor.permission_.empty() && !Invocation.permissions_.contains(Descriptor.permission_))
            return Error{ErrorCategory::AccessDenied, "permission-required"};
        for (const auto& [Name, Spec] : Descriptor.parameters_)
            if (Spec.required_ && !Invocation.parameters_.contains(Name)) return Error{ErrorCategory::InvalidRequest, "missing-parameter:" + Name};
        for (const auto& [Name, Value] : Invocation.parameters_)
        {
            const auto Spec = Descriptor.parameters_.find(Name);
            if (Spec == Descriptor.parameters_.end() || Value.index() != static_cast<std::size_t>(Spec->second.type_))
                return Error{ErrorCategory::InvalidRequest, "parameter-type:" + Name};
        }
        return Unit{};
    }
    struct CommandHandler
    {
        std::function<storage::SupportDecision(const CommandInvocation&)> canExecute_;
        // Execute runs on the registered executor and returns promptly. Long work
        // returns Accepted with an opaque handle; M4 owns the operation scheduler.
        std::function<Result<CommandOutcome>(const CommandInvocation&)> execute_;
    };
    struct RegisteredCommand
    {
        CommandDescriptor descriptor_;
        CommandHandler handler_;
        std::shared_ptr<IExecutor> executor_;
    };
    class CommandRegistry
    {
    public:
        Result<Unit> Register(CommandDescriptor Descriptor, CommandHandler Handler, std::shared_ptr<IExecutor> Executor)
        {
            if (Descriptor.id_.empty() || !Descriptor.schemaVersion_ || !Handler.execute_ || !Executor)
                return Error{ErrorCategory::InvalidRequest, "invalid-command"};
            auto Registration = std::make_shared<RegisteredCommand>(RegisteredCommand{std::move(Descriptor), std::move(Handler), std::move(Executor)});
            std::lock_guard Lock(mutex_);
            if (!commands_.emplace(Registration->descriptor_.id_, Registration).second)
                return Error{ErrorCategory::InvalidRequest, "duplicate-command"};
            return Unit{};
        }
        Result<std::shared_ptr<const RegisteredCommand>> Resolve(const std::string& Id) const
        {
            std::lock_guard Lock(mutex_);
            const auto It = commands_.find(Id);
            if (It == commands_.end()) return Error{ErrorCategory::NotFound, "unknown-command"};
            return std::shared_ptr<const RegisteredCommand>(It->second);
        }
        void Unregister(const std::string& Id) { std::lock_guard Lock(mutex_); commands_.erase(Id); }
    private:
        mutable std::mutex mutex_;
        std::map<std::string, std::shared_ptr<RegisteredCommand>> commands_;
    };
    namespace detail
    {
        struct Completion
        {
            std::promise<Result<CommandOutcome>> promise_;
            bool finished_ = false;
            ~Completion() { if (!finished_) promise_.set_value(Error{ErrorCategory::Cancelled, "executor-closed"}); }
            void Finish(Result<CommandOutcome> Value) { finished_ = true; promise_.set_value(std::move(Value)); }
        };
    }
    class CommandDispatcher
    {
    public:
        CommandDispatcher(std::shared_ptr<CommandRegistry> Registry, std::shared_ptr<EventHub> Events)
            : registry_(std::move(Registry)), events_(std::move(Events)) {}
        storage::Task<CommandOutcome> Dispatch(CommandInvocation Invocation)
        {
            const auto Found = registry_->Resolve(Invocation.id_);
            if (!Found.HasValue()) return storage::Ready<CommandOutcome>(Found.Failure());
            const auto Command = Found.Value();
            const auto& Descriptor = Command->descriptor_;
            const auto Valid = ValidateInvocation(Descriptor, Invocation);
            if (!Valid.HasValue()) return storage::Ready<CommandOutcome>(Valid.Failure());
            if (Invocation.correlationId_.empty()) Invocation.correlationId_ = "command-" + std::to_string(++nextId_);
            auto Completion = std::make_shared<detail::Completion>();
            auto Future = Completion->promise_.get_future();
            const auto Posted = Command->executor_->Post([Command, Invocation = std::move(Invocation), Completion, Events = events_]() {
                auto Publish = [&](const std::string& Id, const std::string& Code) {
                    EventEnvelope Event;
                    Event.id_ = Id;
                    Event.producer_ = "command-dispatcher";
                    Event.correlationId_ = Invocation.correlationId_;
                    Event.causationId_ = Invocation.causationId_;
                    Event.payload_ = {{"command", Invocation.id_}, {"source", Invocation.source_}, {"code", Code}};
                    Events->Publish(std::move(Event));
                };
                Publish("command.started", "");
                Result<CommandOutcome> Outcome = Error{ErrorCategory::Internal, "handler-exception"};
                try
                {
                    if (const auto Failure = Invocation.operation_.Check({})) Outcome = *Failure;
                    else if (Command->handler_.canExecute_)
                    {
                        const auto Decision = Command->handler_.canExecute_(Invocation);
                        Outcome = Decision.availability_ == storage::Availability::Supported
                            ? Command->handler_.execute_(Invocation)
                            : Result<CommandOutcome>(Error{ErrorCategory::Unsupported, Decision.reason_});
                    }
                    else Outcome = Command->handler_.execute_(Invocation);
                }
                catch (...) {}
                Publish(!Outcome.HasValue() ? "command.failed" : Outcome.Value().status_ == CommandOutcome::Status::Accepted
                    ? "command.accepted" : "command.completed", Outcome.HasValue() ? "" : Outcome.Failure().code_);
                Completion->Finish(std::move(Outcome));
            });
            if (!Posted) Completion->Finish(Error{ErrorCategory::Cancelled, "executor-closed"});
            return Future;
        }
    private:
        std::shared_ptr<CommandRegistry> registry_;
        std::shared_ptr<EventHub> events_;
        std::atomic<std::uint64_t> nextId_{0};
    };
}
