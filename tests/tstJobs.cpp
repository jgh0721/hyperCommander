#include "commonLibs/operations/jobScheduler.hpp"
#include <QtTest>
#include <semaphore>

using namespace hc::operations;
using namespace hc::storage;
using namespace std::chrono_literals;

namespace
{
    struct Probe
    {
        std::atomic<int> calls_{0};
        std::atomic<int> finalizations_{0};
        std::atomic<int> destructions_{0};
        std::atomic<JobState> finalizedAs_{JobState::Queued};
    };
    struct Gate
    {
        std::binary_semaphore entered_{0};
        std::binary_semaphore release_{0};
        void Enter()
        {
            entered_.release();
            if (!release_.try_acquire_for(3s)) throw std::runtime_error("test-gate-timeout");
        }
    };
    class ScriptTask final : public IJobTask
    {
    public:
        using Work = std::function<JobStep(JobStepContext)>;
        ScriptTask(std::shared_ptr<Probe> ProbeValue, Work Advance = {}, std::vector<Error> Cleanup = {})
            : probe_(std::move(ProbeValue)), advance_(std::move(Advance)), cleanup_(std::move(Cleanup)) {}
        ~ScriptTask() override { ++probe_->destructions_; }
        JobStep Advance(JobStepContext Context) override
        {
            ++probe_->calls_;
            return advance_ ? advance_(std::move(Context)) : JobStep{JobState::Completed};
        }
        std::vector<Error> Finalize(JobState State) override
        {
            probe_->finalizedAs_ = State;
            ++probe_->finalizations_;
            return cleanup_;
        }
    private:
        std::shared_ptr<Probe> probe_;
        Work advance_;
        std::vector<Error> cleanup_;
    };
    class InlineExecutor final : public hc::commands::IExecutor
    {
    public:
        bool Post(std::function<void()> Work) override { Work(); return true; }
    };
    JobRequest Lane(std::string Name)
    {
        JobRequest Request;
        Request.resourceLanes_ = {std::move(Name)};
        return Request;
    }
    JobStep Prompt()
    {
        JobStep Step{JobState::WaitingForUser};
        Step.interaction_ = InteractionRequest{"", InteractionKind::Overwrite, "destination-exists",
            {InteractionChoice::Replace, InteractionChoice::Skip, InteractionChoice::Cancel}};
        return Step;
    }
    bool Ready(const JobHandle& Handle) { return Handle.completion_.wait_for(3s) == std::future_status::ready; }
}

class JobTests : public QObject
{
    Q_OBJECT
private slots:
    void interactionReleasesWorkerButRetainsResourceLane()
    {
        auto A = std::make_shared<Probe>();
        auto B = std::make_shared<Probe>();
        auto C = std::make_shared<Probe>();
        JobScheduler Scheduler(std::make_shared<hc::commands::EventHub>(), 1);
        auto First = Scheduler.Submit(Lane("archive:one"), std::make_unique<ScriptTask>(A, [A](JobStepContext Context) {
            if (A->calls_ == 1) return Prompt();
            if (!Context.answer_ || Context.answer_->choice_ != InteractionChoice::Replace)
                throw std::runtime_error("missing-answer");
            return JobStep{JobState::Completed};
        }));
        QVERIFY(First.HasValue());
        const auto Handle = First.Value();
        QTRY_COMPARE(Scheduler.Snapshot(Handle.id_).Value().state_, JobState::WaitingForUser);
        auto Blocked = Scheduler.Submit(Lane("archive:one"), std::make_unique<ScriptTask>(B));
        auto Independent = Scheduler.Submit(Lane("volume:two"), std::make_unique<ScriptTask>(C));
        QVERIFY(Blocked.HasValue() && Independent.HasValue());
        QVERIFY(Ready(Independent.Value()));
        QCOMPARE(C->calls_.load(), 1);
        QCOMPARE(B->calls_.load(), 0);
        QCOMPARE(Scheduler.Snapshot(Blocked.Value().id_).Value().state_, JobState::Queued);
        const auto Request = *Scheduler.Snapshot(Handle.id_).Value().interaction_;
        QCOMPARE(Scheduler.Reply(Handle.id_, {"old", InteractionChoice::Replace}).Failure().code_, std::string("interaction-expired"));
        QVERIFY(!Scheduler.Reply(Handle.id_, {Request.id_, InteractionChoice::Elevate}).HasValue());
        QVERIFY(Scheduler.Reply(Handle.id_, {Request.id_, InteractionChoice::Replace}).HasValue());
        QVERIFY(Ready(Handle));
        QVERIFY(Ready(Blocked.Value()));
        QCOMPARE(Handle.completion_.get().snapshot_.state_, JobState::Completed);
        QCOMPARE(A->finalizations_.load(), 1);
        QCOMPARE(A->destructions_.load(), 1);
        QVERIFY(!Scheduler.Reply(Handle.id_, {Request.id_, InteractionChoice::Replace}).HasValue());
    }
    void cancelWaitingAndNeverStartedJobs()
    {
        auto A = std::make_shared<Probe>();
        auto B = std::make_shared<Probe>();
        JobScheduler Scheduler(std::make_shared<hc::commands::EventHub>(), 1);
        auto First = Scheduler.Submit(Lane("volume"), std::make_unique<ScriptTask>(A, [](auto) { return Prompt(); }));
        QVERIFY(First.HasValue());
        const auto Handle = First.Value();
        QTRY_COMPARE(Scheduler.Snapshot(Handle.id_).Value().state_, JobState::WaitingForUser);
        auto Second = Scheduler.Submit(Lane("volume"), std::make_unique<ScriptTask>(B));
        QVERIFY(Second.HasValue());
        QVERIFY(Scheduler.Cancel(Second.Value().id_).HasValue());
        QVERIFY(Ready(Second.Value()));
        QCOMPARE(B->calls_.load(), 0);
        QCOMPARE(B->finalizations_.load(), 1);
        QCOMPARE(Second.Value().completion_.get().snapshot_.state_, JobState::Cancelled);
        const auto Request = *Scheduler.Snapshot(Handle.id_).Value().interaction_;
        QVERIFY(Scheduler.Reply(Handle.id_, {Request.id_, InteractionChoice::Cancel}).HasValue());
        QVERIFY(Ready(Handle));
        QCOMPARE(A->calls_.load(), 1);
        QCOMPARE(Handle.completion_.get().snapshot_.state_, JobState::Cancelled);
    }
    void pauseBeforeCommitAndCancelAtSafePoint()
    {
        for (const bool CancelInstead : {false, true})
        {
            auto A = std::make_shared<Probe>();
            auto G = std::make_shared<Gate>();
            JobScheduler Scheduler(std::make_shared<hc::commands::EventHub>(), 1);
            auto Submitted = Scheduler.Submit({}, std::make_unique<ScriptTask>(A, [A, G](JobStepContext Context) {
                if (A->calls_ == 1) { G->Enter(); return JobStep{JobState::Committing}; }
                if (!Context.committing_) throw std::runtime_error("commit-phase-required");
                return JobStep{JobState::Completed};
            }));
            QVERIFY(Submitted.HasValue());
            const auto Handle = Submitted.Value();
            QVERIFY(G->entered_.try_acquire_for(2s));
            if (CancelInstead) QVERIFY(Scheduler.Cancel(Handle.id_).HasValue());
            else QVERIFY(Scheduler.Pause(Handle.id_, true).HasValue());
            G->release_.release();
            if (!CancelInstead)
            {
                QTRY_COMPARE(Scheduler.Snapshot(Handle.id_).Value().state_, JobState::Paused);
                QCOMPARE(A->calls_.load(), 1);
                QVERIFY(Scheduler.Pause(Handle.id_, false).HasValue());
            }
            QVERIFY(Ready(Handle));
            QCOMPARE(Handle.completion_.get().snapshot_.state_, CancelInstead ? JobState::Cancelled : JobState::Completed);
            QCOMPARE(A->calls_.load(), CancelInstead ? 1 : 2);
        }
    }
    void committedResultWinsOverLateCancellation()
    {
        auto A = std::make_shared<Probe>();
        auto G = std::make_shared<Gate>();
        JobScheduler Scheduler(std::make_shared<hc::commands::EventHub>(), 1);
        auto Submitted = Scheduler.Submit({}, std::make_unique<ScriptTask>(A, [A, G](JobStepContext Context) {
            if (A->calls_ == 1) return JobStep{JobState::Committing};
            G->Enter();
            if (!Context.committing_ || Context.operation_.stopToken_.stop_possible()
                || Context.operation_.deadline_ != std::chrono::steady_clock::time_point::max())
                throw std::runtime_error("commit-inherited-cancellation");
            JobStep Step{JobState::Completed};
            Step.item_ = ItemOutcome{"file", ItemOutcome::Status::Completed, {}, true, false};
            return Step;
        }));
        QVERIFY(Submitted.HasValue());
        const auto Handle = Submitted.Value();
        QVERIFY(G->entered_.try_acquire_for(2s));
        QVERIFY(Scheduler.Cancel(Handle.id_).HasValue());
        G->release_.release();
        QVERIFY(Ready(Handle));
        const auto& Result = Handle.completion_.get();
        QCOMPARE(Result.snapshot_.state_, JobState::Completed);
        QVERIFY(Result.snapshot_.cancelRequested_);
        QVERIFY(Result.items_.at(0).destinationCommitted_);
        QCOMPARE(A->finalizations_.load(), 1);
    }
    void cancellingPausedCommitCannotBeUndoneByResume()
    {
        auto A = std::make_shared<Probe>();
        auto G = std::make_shared<Gate>();
        JobScheduler Scheduler(std::make_shared<hc::commands::EventHub>(), 1);
        auto Submitted = Scheduler.Submit({}, std::make_unique<ScriptTask>(A, [A, G](auto) {
            if (A->calls_ == 1) { G->Enter(); return JobStep{JobState::Committing}; }
            return JobStep{JobState::Completed};
        }));
        QVERIFY(Submitted.HasValue());
        const auto Handle = Submitted.Value();
        QVERIFY(G->entered_.try_acquire_for(2s));
        QVERIFY(Scheduler.Pause(Handle.id_, true).HasValue());
        G->release_.release();
        QTRY_COMPARE(Scheduler.Snapshot(Handle.id_).Value().state_, JobState::Paused);
        QVERIFY(Scheduler.Cancel(Handle.id_).HasValue());
        Scheduler.Pause(Handle.id_, false); // May already be terminal; never revives commit.
        QVERIFY(Ready(Handle));
        QCOMPARE(Handle.completion_.get().snapshot_.state_, JobState::Cancelled);
        QCOMPARE(A->calls_.load(), 1);
    }
    void multipleLanesAreAcquiredAtomically()
    {
        auto A = std::make_shared<Probe>();
        auto B = std::make_shared<Probe>();
        auto C = std::make_shared<Probe>();
        JobScheduler Scheduler(std::make_shared<hc::commands::EventHub>(), 3);
        auto First = Scheduler.Submit(Lane("destination"), std::make_unique<ScriptTask>(A, [](auto) { return Prompt(); }));
        QVERIFY(First.HasValue());
        QTRY_COMPARE(Scheduler.Snapshot(First.Value().id_).Value().state_, JobState::WaitingForUser);
        JobRequest Request;
        Request.resourceLanes_ = {"source", "destination", "source"};
        auto Both = Scheduler.Submit(Request, std::make_unique<ScriptTask>(B));
        auto SourceOnly = Scheduler.Submit(Lane("source"), std::make_unique<ScriptTask>(C));
        QVERIFY(Both.HasValue() && SourceOnly.HasValue());
        QVERIFY(Ready(SourceOnly.Value()));
        QCOMPARE(B->calls_.load(), 0); // No partial acquisition blocks the source-only job.
        QVERIFY(Scheduler.SetLaneLimit("destination", 2).HasValue());
        QVERIFY(Ready(Both.Value()));
        QCOMPARE(B->calls_.load(), 1);
        QVERIFY(!Scheduler.SetLaneLimit("destination", 0).HasValue());
        QVERIFY(Scheduler.Cancel(First.Value().id_).HasValue());
        QVERIFY(Ready(First.Value()));
    }
    void retriesAreBoundedAndReleaseWorker()
    {
        auto A = std::make_shared<Probe>();
        auto B = std::make_shared<Probe>();
        JobScheduler Scheduler(std::make_shared<hc::commands::EventHub>(), 1);
        JobRequest Request;
        Request.maxRetries_ = 2;
        Request.initialRetryDelay_ = 100ms;
        Request.maximumRetryDelay_ = 200ms;
        auto Submitted = Scheduler.Submit(Request, std::make_unique<ScriptTask>(A, [A](JobStepContext Context) {
            if (Context.retryAttempt_ != static_cast<unsigned>(A->calls_ - 1)) throw std::runtime_error("retry-count");
            JobStep Step{JobState::Failed};
            Step.error_ = Error{ErrorCategory::Unavailable, "temporary", {}, {}, true};
            Step.retry_ = true;
            return Step;
        }));
        QVERIFY(Submitted.HasValue());
        auto Independent = Scheduler.Submit({}, std::make_unique<ScriptTask>(B));
        QVERIFY(Independent.HasValue());
        QVERIFY(Ready(Independent.Value()));
        QVERIFY(Ready(Submitted.Value()));
        QCOMPARE(A->calls_.load(), 3);
        QCOMPARE(Submitted.Value().completion_.get().snapshot_.state_, JobState::Failed);
        QCOMPARE(Submitted.Value().completion_.get().snapshot_.error_->code_, std::string("temporary"));
        auto C = std::make_shared<Probe>();
        auto Permanent = Scheduler.Submit(Request, std::make_unique<ScriptTask>(C, [](auto) {
            JobStep Step{JobState::Failed};
            Step.error_ = Error{ErrorCategory::AccessDenied, "denied"};
            Step.retry_ = true;
            return Step;
        }));
        QVERIFY(Permanent.HasValue() && Ready(Permanent.Value()));
        QCOMPARE(C->calls_.load(), 1);
    }
    void deadlineAndExternalStopWakeInteraction()
    {
        for (const bool ExternalStop : {false, true})
        {
            auto A = std::make_shared<Probe>();
            std::stop_source Stop;
            JobScheduler Scheduler(std::make_shared<hc::commands::EventHub>(), 1);
            JobRequest Request;
            Request.operation_.stopToken_ = Stop.get_token();
            if (!ExternalStop) Request.operation_.deadline_ = std::chrono::steady_clock::now() + 200ms;
            auto Submitted = Scheduler.Submit(Request, std::make_unique<ScriptTask>(A, [](auto) { return Prompt(); }));
            QVERIFY(Submitted.HasValue());
            QTRY_COMPARE(Scheduler.Snapshot(Submitted.Value().id_).Value().state_, JobState::WaitingForUser);
            if (ExternalStop) Stop.request_stop();
            QVERIFY(Ready(Submitted.Value()));
            QCOMPARE(Submitted.Value().completion_.get().snapshot_.state_, ExternalStop ? JobState::Cancelled : JobState::Failed);
            QCOMPARE(A->calls_.load(), 1);
            QCOMPARE(A->finalizations_.load(), 1);
        }
    }
    void partialOutcomesAndCleanupFailureRemainVisible()
    {
        auto A = std::make_shared<Probe>();
        JobScheduler Scheduler(std::make_shared<hc::commands::EventHub>(), 1);
        auto Submitted = Scheduler.Submit({}, std::make_unique<ScriptTask>(A, [A](auto) {
            JobStep Step{A->calls_ == 1 ? JobState::Running : JobState::Completed};
            Step.progress_ = JobProgress{100, 200, "file-two"};
            Step.item_ = A->calls_ == 1 ? ItemOutcome{"file-one", ItemOutcome::Status::Completed, {}, true, false}
                : ItemOutcome{"file-two", ItemOutcome::Status::Failed, Error{ErrorCategory::AccessDenied, "delete-source"}, true, false};
            return Step;
        }, std::vector<Error>{{ErrorCategory::AccessDenied, "stage-cleanup"}}));
        QVERIFY(Submitted.HasValue() && Ready(Submitted.Value()));
        const auto& Result = Submitted.Value().completion_.get();
        QCOMPARE(Result.snapshot_.state_, JobState::Failed);
        QCOMPARE(Result.snapshot_.completedItems_, 1u);
        QCOMPARE(Result.snapshot_.failedItems_, 1u);
        QCOMPARE(Result.snapshot_.progress_.bytesDone_, 100u);
        QCOMPARE(Result.items_.size(), 2u);
        QVERIFY(Result.items_[1].destinationCommitted_);
        QVERIFY(!Result.items_[1].sourceRemoved_);
        QCOMPARE(Result.items_[1].error_->code_, std::string("delete-source"));
        QCOMPARE(Result.cleanupErrors_.at(0).code_, std::string("stage-cleanup"));
        QCOMPARE(A->finalizedAs_.load(), JobState::Failed);
    }
    void invalidStepsExceptionsAndBudgets()
    {
        JobScheduler Scheduler(std::make_shared<hc::commands::EventHub>(), 1, 1);
        for (const int Mode : {0, 1, 2, 3, 4, 5})
        {
            auto A = std::make_shared<Probe>();
            JobRequest Request;
            Request.maxItemResults_ = 1;
            auto Submitted = Scheduler.Submit(Request, std::make_unique<ScriptTask>(A, [Mode, A](auto) -> JobStep {
                if (Mode == 0) throw std::runtime_error("provider-threw");
                if (Mode == 1) return JobStep{static_cast<JobState>(255)};
                if (Mode == 2) return JobStep{JobState::WaitingForUser};
                if (Mode == 5) return A->calls_ == 1 ? JobStep{JobState::Committing} : Prompt();
                JobStep Step{JobState::Running};
                Step.item_ = ItemOutcome{"file"};
                if (Mode == 4)
                {
                    Step.retry_ = true;
                    Step.error_ = Error{ErrorCategory::Unavailable, "transient", {}, {}, true};
                }
                return Step;
            }));
            QVERIFY(Submitted.HasValue() && Ready(Submitted.Value()));
            QCOMPARE(Submitted.Value().completion_.get().snapshot_.state_, JobState::Failed);
            QCOMPARE(A->finalizations_.load(), 1);
            auto Full = Scheduler.Submit({}, std::make_unique<ScriptTask>(std::make_shared<Probe>()));
            QCOMPARE(Full.Failure().category_, ErrorCategory::BudgetExceeded);
            QVERIFY(Scheduler.Forget(Submitted.Value().id_).HasValue());
            QVERIFY(!Scheduler.Snapshot(Submitted.Value().id_).HasValue());
        }
    }
    void reentrantObserversAndShutdown()
    {
        auto Hub = std::make_shared<hc::commands::EventHub>();
        auto Executor = std::make_shared<InlineExecutor>();
        auto A = std::make_shared<Probe>();
        std::atomic<int> Observations{0};
        std::atomic<bool> ReentrantSnapshot{true};
        JobScheduler Scheduler(Hub, 2);
        auto Subscription = Hub->Subscribe("", Executor, [&](const auto& Event) {
            ReentrantSnapshot = ReentrantSnapshot && Scheduler.Snapshot(Event.causationId_).HasValue();
            ++Observations;
        });
        QVERIFY(Subscription.HasValue());
        auto Submitted = Scheduler.Submit({}, std::make_unique<ScriptTask>(A, [](auto) { return Prompt(); }));
        QVERIFY(Submitted.HasValue());
        QTRY_COMPARE(Scheduler.Snapshot(Submitted.Value().id_).Value().state_, JobState::WaitingForUser);
        Scheduler.Shutdown();
        QVERIFY(Ready(Submitted.Value()));
        QCOMPARE(Submitted.Value().completion_.get().snapshot_.state_, JobState::Cancelled);
        QVERIFY(ReentrantSnapshot.load());
        QVERIFY(Observations >= 3);
        QCOMPARE(A->finalizations_.load(), 1);
        QVERIFY(!Scheduler.Submit({}, std::make_unique<ScriptTask>(std::make_shared<Probe>())).HasValue());
        Scheduler.Shutdown();
    }
};

QTEST_GUILESS_MAIN(JobTests)
#include "tstJobs.moc"
