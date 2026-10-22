#pragma once
#include "jobScheduler.hpp"
#include <condition_variable>

namespace hc::operations
{
    // Suspend only the SDK continuation while its prompt is displayed. The
    // scheduler worker returns WaitingForUser and can run independent jobs.
    class ProviderInteractionTask final : public IJobTask
    {
        struct Pending
        {
            std::mutex mutex_;
            std::condition_variable changed_;
            std::optional<storage::ProviderInteractionRequest> prompt_;
            std::optional<std::promise<storage::Result<storage::ProviderInteractionReply>>> answer_;
            std::optional<JobStep> result_;
            std::optional<JobStepContext> work_;
            bool busy_ = false;
        };
    public:
        explicit ProviderInteractionTask(std::unique_ptr<IJobTask> Inner, std::string Activity = {}) : inner_(std::move(Inner)), activity_(std::move(Activity)) {}
        ~ProviderInteractionTask() override { stop(); }
        JobStep Advance(JobStepContext Context) override
        {
            if (!pending_) pending_ = std::make_shared<Pending>();
            if (!callerStop_ && !Context.committing_ && Context.operation_.stopToken_.stop_possible())
                callerStop_ = std::make_unique<std::stop_callback<std::function<void()>>>(Context.operation_.stopToken_,
                    [Closing = closing_]() mutable { Closing.request_stop(); });
            {
                std::lock_guard Lock(pending_->mutex_);
                if (Context.answer_ && promptShown_) {
                    if (pending_->answer_) {
                        pending_->answer_->set_value(storage::ProviderInteractionReply{Context.answer_->choice_ == InteractionChoice::Confirm,
                            storage::Password(std::move(Context.answer_->value_))});
                        pending_->answer_.reset(); pending_->prompt_.reset();
                    }
                    Context.answer_.reset(); promptShown_ = false;
                }
                // A background continuation can post after the worker that
                // started it has already returned. Keep one task-wide mailbox.
                if (pending_->prompt_ && !Context.answer_) return question();
            }
            std::unique_lock Lock(pending_->mutex_);
            if (!pending_->busy_ && !pending_->result_)
            {
                auto Shared = pending_;
                Context.operation_.interactionStart_ = [Shared, Stop = closing_.get_token(), Committing = Context.committing_](const storage::ProviderInteractionRequest& Request, const storage::OperationContext&) {
                    if (Stop.stop_requested()) return storage::Ready<storage::ProviderInteractionReply>(storage::Error{storage::ErrorCategory::Cancelled, "cancelled"});
                    if (Committing || Request.kind_ != storage::ProviderInteractionKind::Password)
                        return storage::Ready<storage::ProviderInteractionReply>(storage::Error{storage::ErrorCategory::Unsupported, "job-provider-interaction-unavailable"});
                    std::lock_guard Lock(Shared->mutex_);
                    if (Stop.stop_requested()) return storage::Ready<storage::ProviderInteractionReply>(storage::Error{storage::ErrorCategory::Cancelled, "cancelled"});
                    if (Shared->answer_) return storage::Ready<storage::ProviderInteractionReply>(storage::Error{storage::ErrorCategory::InvalidRequest, "job-provider-interaction-busy"});
                    Shared->prompt_ = Request; Shared->answer_.emplace(); auto Future = Shared->answer_->get_future(); Shared->changed_.notify_all(); return Future;
                };
                // Tokens outlive one Advance: decoding/copying futures may
                // still retain this context after the worker has returned.
                Context.operation_.stopToken_ = Context.committing_ ? std::stop_token{} : closing_.get_token();
                pending_->work_ = std::move(Context); pending_->busy_ = true;
                if (!worker_.joinable()) worker_ = std::jthread([this, Shared, Closing = closing_](std::stop_token Stop) mutable {
                    std::stop_callback Cancel(Stop, [Closing, Shared]() mutable { Closing.request_stop(); Shared->changed_.notify_all(); });
                    for (;;) {
                        std::optional<JobStepContext> Work;
                        {
                            std::unique_lock Guard(Shared->mutex_);
                            Shared->changed_.wait(Guard, [&] { return Stop.stop_requested() || Shared->work_.has_value(); });
                            // An admitted commit keeps its original cancellation
                            // semantics even if finalization races worker startup.
                            if (Stop.stop_requested() && (!Shared->work_ || !Shared->work_->committing_)) return;
                            Work = std::move(Shared->work_); Shared->work_.reset();
                        }
                        JobStep Result{JobState::Failed};
                        try { Result = inner_->Advance(std::move(*Work)); }
                        catch (...) { Result.error_ = storage::Error{storage::ErrorCategory::Internal, "provider-continuation-exception"}; }
                        { std::lock_guard Guard(Shared->mutex_); Shared->result_ = std::move(Result); Shared->busy_ = false; }
                        Shared->changed_.notify_all();
                    }
                });
                pending_->changed_.notify_all();
            }
            if (!pending_->changed_.wait_for(Lock, std::chrono::milliseconds(100), [&] { return pending_->result_.has_value() || pending_->prompt_.has_value(); })) {
                JobStep Step{Context.committing_ ? JobState::Committing : JobState::Running};
                if (!Context.committing_) Step.deferFor_=std::chrono::milliseconds(100);
                if (!activity_.empty()) { Step.progress_=lastProgress_.value_or(JobProgress{}); Step.progress_->currentItem_=activity_; Step.progress_->currentBytes_=0; Step.progress_->currentTotal_.reset(); }
                return Step;
            }
            if (pending_->result_) {
                auto Result = std::move(*pending_->result_); pending_->result_.reset(); if (Result.progress_) lastProgress_=Result.progress_; return Result;
            }
            return question();
        }
        std::vector<storage::Error> Finalize(JobState State) override
        { stop(); return inner_->Finalize(State); }
    private:
        JobStep question()
        {
            promptShown_ = true;
            JobStep Step{JobState::WaitingForUser};
            Step.interaction_ = InteractionRequest{{}, InteractionKind::Password, "provider-password-required", {InteractionChoice::Confirm, InteractionChoice::Cancel}};
            Step.interaction_->source_ = pending_->prompt_->subject_;
            Step.interaction_->passwordAttempt_ = pending_->prompt_->attempt_;
            return Step;
        }
        void stop()
        {
            closing_.request_stop();
            if (worker_.joinable()) worker_.request_stop();
            if (pending_) {
                std::lock_guard Lock(pending_->mutex_);
                if (pending_->answer_) { pending_->answer_->set_value(storage::Error{storage::ErrorCategory::Cancelled, "cancelled"}); pending_->answer_.reset(); }
            }
            if (worker_.joinable()) worker_.join();
            // A queued context owns the mailbox through interactionStart_.
            // Drop it even when thread creation or startup was interrupted.
            if (pending_) { std::lock_guard Lock(pending_->mutex_); pending_->work_.reset(); }
            pending_.reset(); callerStop_.reset();
        }
        std::unique_ptr<IJobTask> inner_;
        std::shared_ptr<Pending> pending_;
        std::jthread worker_;
        std::stop_source closing_;
        std::unique_ptr<std::stop_callback<std::function<void()>>> callerStop_;
        std::string activity_;
        std::optional<JobProgress> lastProgress_;
        bool promptShown_ = false;
    };
}
