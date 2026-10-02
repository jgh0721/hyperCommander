#include "jobScheduler.hpp"
#include <condition_variable>
#include <stdexcept>
#include <random>
#include "providerInteractionTask.hpp"

namespace hc::operations
{
    using storage::ErrorCategory;
    bool IsTerminal(JobState State) { return State == JobState::Completed || State == JobState::Failed || State == JobState::Cancelled; }
    const char* StateName(JobState State)
    {
        switch (State)
        {
        case JobState::Queued: return "queued";
        case JobState::Preparing: return "preparing";
        case JobState::Running: return "running";
        case JobState::WaitingForUser: return "waiting-for-user";
        case JobState::Paused: return "paused";
        case JobState::Committing: return "committing";
        case JobState::Completed: return "completed";
        case JobState::Failed: return "failed";
        case JobState::Cancelled: return "cancelled";
        }
        return "invalid";
    }
    namespace
    {
        struct Job
        {
            JobRequest request_;
            std::unique_ptr<IJobTask> task_;
            JobResult result_;
            std::stop_source stop_;
            std::unique_ptr<std::stop_callback<std::function<void()>>> externalStop_;
            std::promise<JobResult> promise_;
            std::shared_future<JobResult> future_ = promise_.get_future().share();
            std::optional<InteractionAnswer> answer_;
            JobState resume_ = JobState::Preparing;
            bool executing_ = false;
            bool lanesHeld_ = false;
            bool commitEntered_ = false;
            std::uint64_t interactionSequence_ = 0;
            std::chrono::steady_clock::time_point readyAt_{};
        };
        Error Missing() { return {ErrorCategory::NotFound, "job-not-found"}; }
        Error Closed() { return {ErrorCategory::InvalidRequest, "job-terminal"}; }
        std::int64_t Number(std::uint64_t Value) { return static_cast<std::int64_t>(std::min(Value, static_cast<std::uint64_t>(INT64_MAX))); }
    }
    struct JobScheduler::State
    {
        std::mutex mutex_;
        std::condition_variable changed_;
        std::map<std::string, std::shared_ptr<Job>> jobs_;
        std::map<std::string, std::size_t> laneLimits_;
        std::map<std::string, std::size_t> occupied_;
        std::shared_ptr<commands::EventHub> events_;
        std::shared_ptr<IJobJournal> journal_;
        std::deque<commands::EventEnvelope> notifications_;
        bool draining_ = false;
        bool stopping_ = false;
        std::size_t maxJobs_ = 1024;
        std::string lastPicked_;
        std::uint64_t dropped_ = 0;

        void notify(Job& Value, const std::string& EventId, bool Progress = false)
        {
            auto& Snapshot = Value.result_.snapshot_;
            Snapshot.updated_ = std::chrono::steady_clock::now();
            Snapshot.cancelRequested_ |= Value.stop_.stop_requested();
            const auto Elapsed = std::chrono::duration<double>(Snapshot.updated_ - Snapshot.submitted_).count();
            Snapshot.bytesPerSecond_ = Elapsed > 0 ? Snapshot.progress_.bytesDone_ / Elapsed : 0;
            commands::EventEnvelope Event;
            Event.id_ = EventId;
            Event.producer_ = "operation-scheduler";
            Event.correlationId_ = Snapshot.correlationId_;
            Event.causationId_ = Snapshot.id_;
            Event.progress_ = Progress;
            Event.payload_ = {{"job", Snapshot.id_}, {"state", std::string(StateName(Snapshot.state_))},
                {"bytes", Number(Snapshot.progress_.bytesDone_)}, {"completed", Number(Snapshot.completedItems_)},
                {"skipped", Number(Snapshot.skippedItems_)}, {"failed", Number(Snapshot.failedItems_)},
                {"code", Snapshot.error_ ? Snapshot.error_->code_ : std::string{}}};
            if (Snapshot.interaction_) Event.payload_["interaction"] = Snapshot.interaction_->id_;
            // Observations are bounded; snapshots and completion are authoritative.
            if (notifications_.size() == 4096) { notifications_.pop_front(); ++dropped_; }
            notifications_.push_back(std::move(Event));
        }
        void transition(Job& Value, JobState Next)
        {
            if (Value.result_.snapshot_.state_ == Next) return;
            Value.result_.snapshot_.state_ = Next;
            notify(Value, IsTerminal(Next) ? "job." + std::string(StateName(Next)) : "job.changed");
        }
        bool acquire(Job& Value)
        {
            if (Value.lanesHeld_) return true;
            for (const auto& Lane : Value.request_.resourceLanes_)
            {
                const auto Limit = laneLimits_.contains(Lane) ? laneLimits_[Lane] : 1;
                if (occupied_[Lane] >= Limit) return false;
            }
            for (const auto& Lane : Value.request_.resourceLanes_) ++occupied_[Lane];
            Value.lanesHeld_ = true;
            return true;
        }
        void release(Job& Value)
        {
            if (!Value.lanesHeld_) return;
            for (const auto& Lane : Value.request_.resourceLanes_)
                if (--occupied_[Lane] == 0) occupied_.erase(Lane);
            Value.lanesHeld_ = false;
        }
    };

    JobScheduler::JobScheduler(std::shared_ptr<commands::EventHub> Events, std::size_t WorkerCount, std::size_t MaxRetainedJobs, std::shared_ptr<IJobJournal> Journal)
        : state_(std::make_shared<State>())
    {
        if (!Events || !WorkerCount || WorkerCount > 64 || !MaxRetainedJobs || MaxRetainedJobs > 65536)
            throw std::invalid_argument("invalid-scheduler-options");
        state_->events_ = std::move(Events);
        state_->journal_ = std::move(Journal);
        state_->maxJobs_ = MaxRetainedJobs;
        try { for (std::size_t Index = 0; Index < WorkerCount; ++Index) workers_.emplace_back([Shared = state_] { run(Shared); }); }
        catch (...) { Shutdown(); throw; }
    }
    JobScheduler::~JobScheduler() { Shutdown(); }

    Result<JobHandle> JobScheduler::Submit(JobRequest Request, std::unique_ptr<IJobTask> Task)
    {
        if (!Task || !Request.maxItemResults_ || Request.maxItemResults_ > 1000000 || Request.maxRetries_ > 16
            || Request.initialRetryDelay_.count() < 1 || Request.maximumRetryDelay_ < Request.initialRetryDelay_
            || Request.maximumRetryDelay_ > std::chrono::minutes(1) || Request.resourceLanes_.size() > 64)
            return Error{ErrorCategory::InvalidRequest, "invalid-job-request"};
        for (const auto& Lane : Request.resourceLanes_)
            if (Lane.empty() || Lane.size() > 1024) return Error{ErrorCategory::InvalidRequest, "invalid-resource-lane"};
        std::sort(Request.resourceLanes_.begin(), Request.resourceLanes_.end());
        Request.resourceLanes_.erase(std::unique(Request.resourceLanes_.begin(), Request.resourceLanes_.end()), Request.resourceLanes_.end());
        auto Value = std::make_shared<Job>();
        if (Request.operation_.providerInteractions_ && !Request.operation_.interactionStart_)
            Task = std::make_unique<ProviderInteractionTask>(std::move(Task));
        Value->request_ = std::move(Request);
        Value->task_ = std::move(Task);
        static std::atomic<std::uint64_t> NextId{0};
        auto& Snapshot = Value->result_.snapshot_;
        static const std::string Session = [] { std::random_device Random; return std::to_string(Random()) + "-" + std::to_string(Random()); }();
        Snapshot.id_ = "hc-job-" + Session + "-" + std::to_string(++NextId);
        Snapshot.correlationId_ = Value->request_.correlationId_.empty() ? Snapshot.id_ : Value->request_.correlationId_;
        Snapshot.submitted_ = std::chrono::steady_clock::now();
        Value->externalStop_ = std::make_unique<std::stop_callback<std::function<void()>>>(Value->request_.operation_.stopToken_, [Stop = Value->stop_]() mutable { Stop.request_stop(); });
        {
            std::lock_guard Lock(state_->mutex_);
            if (state_->stopping_) return Error{ErrorCategory::Unavailable, "scheduler-closed"};
            if (state_->jobs_.size() >= state_->maxJobs_) return Error{ErrorCategory::BudgetExceeded, "job-history-full"};
            state_->jobs_.emplace(Snapshot.id_, Value);
            state_->notify(*Value, "job.changed");
        }
        state_->changed_.notify_all();
        flushEvents(state_);
        return JobHandle{Snapshot.id_, Value->future_};
    }
    Result<JobSnapshot> JobScheduler::Snapshot(const std::string& Id) const
    {
        std::lock_guard Lock(state_->mutex_);
        const auto Found = state_->jobs_.find(Id);
        return Found == state_->jobs_.end() ? Result<JobSnapshot>(Missing()) : Result<JobSnapshot>(Found->second->result_.snapshot_);
    }
    std::vector<JobSnapshot> JobScheduler::Snapshots() const
    {
        std::lock_guard Lock(state_->mutex_);
        std::vector<JobSnapshot> Values;
        for (const auto& [Id, Value] : state_->jobs_) Values.push_back(Value->result_.snapshot_);
        return Values;
    }
    Result<storage::Unit> JobScheduler::Cancel(const std::string& Id)
    {
        std::shared_ptr<Job> Value;
        {
            std::lock_guard Lock(state_->mutex_);
            const auto Found = state_->jobs_.find(Id);
            if (Found == state_->jobs_.end()) return Missing();
            Value = Found->second;
            if (IsTerminal(Value->result_.snapshot_.state_)) return Closed();
            Value->result_.snapshot_.cancelRequested_ = true;
        }
        Value->stop_.request_stop(); // Never invoke stop callbacks under the scheduler lock.
        state_->changed_.notify_all();
        return storage::Unit{};
    }
    Result<storage::Unit> JobScheduler::Pause(const std::string& Id, bool Paused)
    {
        {
            std::lock_guard Lock(state_->mutex_);
            const auto Found = state_->jobs_.find(Id);
            if (Found == state_->jobs_.end()) return Missing();
            auto& Value = *Found->second;
            auto& Snapshot = Value.result_.snapshot_;
            if (IsTerminal(Snapshot.state_)) return Closed();
            Snapshot.pauseRequested_ = Paused;
            if (!Paused && Snapshot.state_ == JobState::Paused) state_->transition(Value, Value.resume_);
            else state_->notify(Value, "job.changed");
        }
        state_->changed_.notify_all();
        flushEvents(state_);
        return storage::Unit{};
    }
    Result<storage::Unit> JobScheduler::Reply(const std::string& Id, InteractionAnswer Answer)
    {
        bool Cancelled = false;
        {
            std::lock_guard Lock(state_->mutex_);
            const auto Found = state_->jobs_.find(Id);
            if (Found == state_->jobs_.end()) return Missing();
            auto& Value = *Found->second;
            auto& Snapshot = Value.result_.snapshot_;
            const auto Prompt = Snapshot.interaction_;
            if (Snapshot.state_ != JobState::WaitingForUser || !Prompt || Prompt->id_ != Answer.requestId_
                || Value.executing_ || Snapshot.cancelRequested_ || Value.stop_.stop_requested()
                || std::chrono::steady_clock::now() >= Value.request_.operation_.deadline_)
                return Error{ErrorCategory::StaleRevision, "interaction-expired"};
            if (Answer.value_.size() > 65536 || std::find(Prompt->choices_.begin(), Prompt->choices_.end(), Answer.choice_) == Prompt->choices_.end())
                return Error{ErrorCategory::InvalidRequest, "interaction-answer"};
            Cancelled = Answer.choice_ == InteractionChoice::Cancel;
            if (Cancelled) Snapshot.cancelRequested_ = true;
            Value.answer_ = std::move(Answer);
            Snapshot.interaction_.reset();
            state_->transition(Value, Value.resume_);
        }
        if (Cancelled) Cancel(Id);
        state_->changed_.notify_all();
        flushEvents(state_);
        return storage::Unit{};
    }
    Result<storage::Unit> JobScheduler::Forget(const std::string& Id)
    {
        std::shared_ptr<Job> Removed;
        {
            std::lock_guard Lock(state_->mutex_);
            const auto Found = state_->jobs_.find(Id);
            if (Found == state_->jobs_.end()) return Missing();
            if (!IsTerminal(Found->second->result_.snapshot_.state_) || Found->second->executing_)
                return Error{ErrorCategory::InvalidRequest, "job-active"};
            Removed = std::move(Found->second);
            state_->jobs_.erase(Found);
        }
        return storage::Unit{};
    }
    Result<storage::Unit> JobScheduler::SetLaneLimit(std::string Lane, std::size_t Limit)
    {
        if (Lane.empty() || Lane.size() > 1024 || !Limit || Limit > 64) return Error{ErrorCategory::InvalidRequest, "invalid-lane-limit"};
        { std::lock_guard Lock(state_->mutex_); state_->laneLimits_[std::move(Lane)] = Limit; }
        state_->changed_.notify_all();
        return storage::Unit{};
    }
    std::uint64_t JobScheduler::DroppedNotifications() const { std::lock_guard Lock(state_->mutex_); return state_->dropped_; }

    void JobScheduler::flushEvents(const std::shared_ptr<State>& Shared)
    {
        std::unique_lock Lock(Shared->mutex_);
        if (Shared->draining_) return;
        Shared->draining_ = true;
        while (!Shared->notifications_.empty())
        {
            auto Event = std::move(Shared->notifications_.front());
            Shared->notifications_.pop_front();
            Lock.unlock();
            std::size_t Rejected = 0;
            try { Rejected = Shared->events_->Publish(std::move(Event)).rejected_; }
            catch (...) { Rejected = 1; }
            Lock.lock();
            Shared->dropped_ += Rejected;
        }
        Shared->draining_ = false;
    }

    void JobScheduler::run(const std::shared_ptr<State>& Shared)
    {
        for (;;)
        {
            std::shared_ptr<Job> Value;
            JobStepContext Context;
            std::optional<Error> Abort;
            {
                std::unique_lock Lock(Shared->mutex_);
                auto Position = Shared->jobs_.upper_bound(Shared->lastPicked_);
                bool Active = false;
                const auto Now = std::chrono::steady_clock::now();
                for (std::size_t Index = 0; Index < Shared->jobs_.size(); ++Index)
                {
                    if (Position == Shared->jobs_.end()) Position = Shared->jobs_.begin();
                    const auto Candidate = (Position++)->second;
                    auto& Snapshot = Candidate->result_.snapshot_;
                    if (IsTerminal(Snapshot.state_)) continue;
                    Active = true;
                    if (Candidate->executing_) continue;
                    const bool Committing = Snapshot.state_ == JobState::Committing && Candidate->commitEntered_;
                    if (!Committing)
                    {
                        if (Snapshot.cancelRequested_ || Candidate->stop_.stop_requested()) Abort = Error{ErrorCategory::Cancelled, "cancelled"};
                        else if (Now >= Candidate->request_.operation_.deadline_) Abort = Error{ErrorCategory::Unavailable, "deadline-exceeded"};
                    }
                    if (!Abort)
                    {
                        if (Snapshot.state_ == JobState::WaitingForUser) continue;
                        if (Snapshot.pauseRequested_ && !Committing)
                        {
                            if (Snapshot.state_ != JobState::Paused) { Candidate->resume_ = Snapshot.state_; Shared->transition(*Candidate, JobState::Paused); }
                            continue;
                        }
                        if (Candidate->readyAt_ > Now || !Shared->acquire(*Candidate)) continue;
                    }
                    Value = Candidate;
                    Value->executing_ = true;
                    Shared->lastPicked_ = Snapshot.id_;
                    if (Snapshot.state_ == JobState::Queued) Shared->transition(*Value, JobState::Preparing);
                    // Claim the commit boundary under the same lock as Cancel
                    // and Pause. A paused commit has not entered that boundary.
                    const bool EnteringCommit = Snapshot.state_ == JobState::Committing;
                    if (EnteringCommit && !Abort) Value->commitEntered_ = true;
                    Context.jobId_ = Snapshot.id_;
                    Context.operation_ = Value->request_.operation_;
                    Context.operation_.stopToken_ = EnteringCommit ? std::stop_token{} : Value->stop_.get_token();
                    if (EnteringCommit) Context.operation_.deadline_ = std::chrono::steady_clock::time_point::max();
                    Context.committing_ = EnteringCommit;
                    Context.retryAttempt_ = Snapshot.retryAttempt_;
                    Context.answer_ = std::move(Value->answer_);
                    Value->answer_.reset();
                    break;
                }
                if (!Value)
                {
                    if (Shared->stopping_ && !Active) return;
                    Lock.unlock(); flushEvents(Shared); Lock.lock();
                    // Poll external stop tokens and deadlines even while all jobs await UI.
                    Shared->changed_.wait_for(Lock, std::chrono::milliseconds(25));
                    continue;
                }
            }
            flushEvents(Shared);
            JobStep Step;
            if (!Abort && Shared->journal_)
            {
                JobResult Checkpoint;
                { std::lock_guard Lock(Shared->mutex_); Checkpoint = Value->result_; }
                try { auto Saved = Shared->journal_->Checkpoint(Checkpoint); if (!Saved.HasValue()) Abort = Saved.Failure(); }
                catch (...) { Abort = Error{ErrorCategory::Unavailable, "job-journal-exception"}; }
            }
            if (Abort) { Step.next_ = Abort->category_ == ErrorCategory::Cancelled ? JobState::Cancelled : JobState::Failed; Step.error_ = Abort; }
            else
            {
                try { Step = Value->task_->Advance(std::move(Context)); }
                catch (...) { Step.next_ = JobState::Failed; Step.error_ = Error{ErrorCategory::Internal, "job-task-exception"}; }
            }
            JobState Terminal = JobState::Running;
            {
                std::lock_guard Lock(Shared->mutex_);
                auto& Snapshot = Value->result_.snapshot_;
                // Retry describes an unfinished step, not another outcome for an
                // already accounted item or an outstanding user interaction.
                if (Step.retry_ && (Step.item_ || Step.interaction_ || Step.replaceItem_))
                {
                    Step = JobStep{JobState::Failed};
                    Step.error_ = Error{ErrorCategory::Internal, "invalid-retry-result"};
                }
                if (Step.progress_) { Snapshot.progress_ = std::move(*Step.progress_); Shared->notify(*Value, "job.progress", true); }
                if (Step.replaceItem_ && (!Step.item_ || *Step.replaceItem_ >= Value->result_.items_.size()
                    || Value->result_.items_[*Step.replaceItem_].item_ != Step.item_->item_
                    || (Value->result_.items_[*Step.replaceItem_].destinationCommitted_ && !Step.item_->destinationCommitted_)
                    || (Value->result_.items_[*Step.replaceItem_].sourceRemoved_ && !Step.item_->sourceRemoved_)))
                {
                    Step = JobStep{JobState::Failed};
                    Step.error_ = Error{ErrorCategory::Internal, "invalid-item-result-update"};
                }
                if (Step.item_)
                {
                    if (!Step.replaceItem_ && Value->result_.items_.size() == Value->request_.maxItemResults_)
                    { Step.next_ = JobState::Failed; Step.error_ = Error{ErrorCategory::BudgetExceeded, "item-result-budget"}; Step.retry_ = false; }
                    else
                    {
                        if (Step.replaceItem_)
                        {
                            switch (Value->result_.items_[*Step.replaceItem_].status_)
                            {
                            case ItemOutcome::Status::Completed: --Snapshot.completedItems_; break;
                            case ItemOutcome::Status::Skipped: --Snapshot.skippedItems_; break;
                            case ItemOutcome::Status::Failed: --Snapshot.failedItems_; break;
                            }
                        }
                        switch (Step.item_->status_)
                        {
                        case ItemOutcome::Status::Completed: ++Snapshot.completedItems_; break;
                        case ItemOutcome::Status::Skipped: ++Snapshot.skippedItems_; break;
                        case ItemOutcome::Status::Failed: ++Snapshot.failedItems_; break;
                        }
                        if (Step.replaceItem_) Value->result_.items_[*Step.replaceItem_] = std::move(*Step.item_);
                        else Value->result_.items_.push_back(std::move(*Step.item_));
                    }
                }
                if (Step.retry_)
                {
                    if (Snapshot.state_ != JobState::Committing && Step.error_ && Step.error_->retryable_ && Snapshot.retryAttempt_ < Value->request_.maxRetries_)
                    {
                        const auto Delay = std::min(Value->request_.maximumRetryDelay_, Value->request_.initialRetryDelay_ * (1 << Snapshot.retryAttempt_));
                        Value->readyAt_ = std::chrono::steady_clock::now() + Delay;
                        ++Snapshot.retryAttempt_;
                        Snapshot.error_ = Step.error_;
                        Value->executing_ = false;
                        Shared->notify(*Value, "job.retry");
                        Shared->changed_.notify_all();
                        continue;
                    }
                    Step.next_ = JobState::Failed;
                    if (!Step.error_) Step.error_ = Error{ErrorCategory::Internal, "invalid-retry"};
                }
                else Snapshot.retryAttempt_ = 0;
                Snapshot.error_ = Step.error_;
                // Entering commit is a safe point. Cancellation received while
                // preparing must win before a task gains the uncancellable phase.
                if (Step.next_ == JobState::Committing && Snapshot.state_ != JobState::Committing)
                {
                    if (Snapshot.cancelRequested_ || Value->stop_.stop_requested())
                    { Step.next_ = JobState::Cancelled; Snapshot.error_ = Error{ErrorCategory::Cancelled, "cancelled"}; }
                    else if (std::chrono::steady_clock::now() >= Value->request_.operation_.deadline_)
                    { Step.next_ = JobState::Failed; Snapshot.error_ = Error{ErrorCategory::Unavailable, "deadline-exceeded"}; }
                }
                if (Snapshot.state_ == JobState::Committing && Step.next_ == JobState::WaitingForUser)
                { Step.next_ = JobState::Failed; Snapshot.error_ = Error{ErrorCategory::Internal, "interaction-during-commit"}; }
                if (Step.next_ == JobState::WaitingForUser)
                {
                    if (!Step.interaction_ || Step.interaction_->choices_.empty() || Step.interaction_->choices_.size() > 16)
                    { Step.next_ = JobState::Failed; Snapshot.error_ = Error{ErrorCategory::Internal, "invalid-interaction"}; }
                    else
                    {
                        Step.interaction_->id_ = Snapshot.id_ + ":" + std::to_string(++Value->interactionSequence_);
                        Snapshot.interaction_ = std::move(Step.interaction_);
                        Value->resume_ = JobState::Running;
                    }
                }
                if (Step.next_ == JobState::Queued || Step.next_ == JobState::Paused || std::string_view(StateName(Step.next_)) == "invalid")
                { Step.next_ = JobState::Failed; Snapshot.error_ = Error{ErrorCategory::Internal, "invalid-task-transition"}; }
                if (Step.next_ == JobState::Completed && Snapshot.failedItems_)
                { Step.next_ = JobState::Failed; Snapshot.error_ = Error{ErrorCategory::Unavailable, "partial-failure"}; }
                if (Step.next_ == JobState::Failed && !Snapshot.error_) Snapshot.error_ = Error{ErrorCategory::Internal, "job-failed"};
                if (IsTerminal(Step.next_)) Terminal = Step.next_;
                else
                {
                    if (Step.next_ == JobState::Committing && Snapshot.state_ != JobState::Committing && Snapshot.pauseRequested_)
                    { Value->resume_ = Step.next_; Shared->transition(*Value, JobState::Paused); }
                    else Shared->transition(*Value, Step.next_);
                    if (Step.next_ != JobState::Committing) Value->commitEntered_ = false;
                    Value->executing_ = false;
                }
            }
            if (IsTerminal(Terminal))
            {
                std::vector<Error> Cleanup;
                try { Cleanup = Value->task_->Finalize(Terminal); }
                catch (...) { Cleanup.push_back({ErrorCategory::Internal, "cleanup-exception"}); }
                // History retains value results, never live provider/task handles.
                Value->task_.reset();
                if (Shared->journal_)
                {
                    JobResult Checkpoint;
                    { std::lock_guard Lock(Shared->mutex_); Checkpoint = Value->result_; }
                    Checkpoint.snapshot_.state_ = Cleanup.empty() ? Terminal : JobState::Failed;
                    Checkpoint.cleanupErrors_ = Cleanup;
                    if (!Cleanup.empty()) Checkpoint.snapshot_.error_ = Error{ErrorCategory::Unavailable, "cleanup-incomplete"};
                    try { auto Saved = Shared->journal_->Checkpoint(Checkpoint); if (!Saved.HasValue()) Cleanup.push_back(Saved.Failure()); }
                    catch (...) { Cleanup.push_back({ErrorCategory::Unavailable, "job-journal-exception"}); }
                }
                {
                    std::lock_guard Lock(Shared->mutex_);
                    Value->result_.cleanupErrors_ = std::move(Cleanup);
                    if (!Value->result_.cleanupErrors_.empty())
                    { Terminal = JobState::Failed; Value->result_.snapshot_.error_ = Error{ErrorCategory::Unavailable, "cleanup-incomplete"}; }
                    Value->result_.snapshot_.interaction_.reset();
                    Shared->release(*Value);
                    Shared->transition(*Value, Terminal);
                    Value->promise_.set_value(Value->result_);
                    Value->executing_ = false;
                }
            }
            Shared->changed_.notify_all();
            flushEvents(Shared);
        }
    }
    void JobScheduler::Shutdown()
    {
        // Joining from a worker (including an inline observer) can wait for the
        // caller's own unfinished task. Lifetime belongs to the creating thread.
        if (std::this_thread::get_id() != owner_) throw std::logic_error("scheduler-owner-thread-required");
        std::vector<std::shared_ptr<Job>> Pending;
        {
            std::lock_guard Lock(state_->mutex_);
            state_->stopping_ = true;
            for (const auto& [Id, Value] : state_->jobs_) if (!IsTerminal(Value->result_.snapshot_.state_))
            { Value->result_.snapshot_.cancelRequested_ = true; Pending.push_back(Value); }
        }
        for (const auto& Value : Pending) Value->stop_.request_stop();
        state_->changed_.notify_all();
        for (auto& Worker : workers_)
        {
            if (!Worker.joinable()) continue;
            Worker.join();
        }
        workers_.clear();
        flushEvents(state_);
    }
}
