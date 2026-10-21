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
        };
    public:
        explicit ProviderInteractionTask(std::unique_ptr<IJobTask> Inner, std::string Activity = {}) : inner_(std::move(Inner)), activity_(std::move(Activity)) {}
        ~ProviderInteractionTask() override { stop(); }
        JobStep Advance(JobStepContext Context) override
        {
            if (!pending_)
            {
                pending_ = std::make_shared<Pending>(); auto Shared = pending_;
                Context.operation_.interactionStart_ = [Shared, Committing = Context.committing_](const storage::ProviderInteractionRequest& Request, const storage::OperationContext&) {
                    if (Committing || Request.kind_ != storage::ProviderInteractionKind::Password)
                        return storage::Ready<storage::ProviderInteractionReply>(storage::Error{storage::ErrorCategory::Unsupported, "job-provider-interaction-unavailable"});
                    std::lock_guard Lock(Shared->mutex_);
                    if (Shared->answer_) return storage::Ready<storage::ProviderInteractionReply>(storage::Error{storage::ErrorCategory::InvalidRequest, "job-provider-interaction-busy"});
                    Shared->prompt_ = Request; Shared->answer_.emplace(); auto Future = Shared->answer_->get_future(); Shared->changed_.notify_all(); return Future;
                };
                worker_ = std::jthread([this, Shared, Context = std::move(Context)](std::stop_token Stop) mutable {
                    // Preserve both caller cancellation and finalization cancellation.
                    std::stop_source Combined;
                    std::stop_callback Caller(Context.operation_.stopToken_, [&] { Combined.request_stop(); });
                    std::stop_callback Closing(Stop, [&] { Combined.request_stop(); });
                    Context.operation_.stopToken_ = Combined.get_token();
                    JobStep Result{JobState::Failed};
                    try { Result = inner_->Advance(std::move(Context)); }
                    catch (...) { Result.error_ = storage::Error{storage::ErrorCategory::Internal, "provider-continuation-exception"}; }
                    { std::lock_guard Lock(Shared->mutex_); Shared->result_ = std::move(Result); }
                    Shared->changed_.notify_all();
                });
            }
            else if (Context.answer_)
            {
                std::lock_guard Lock(pending_->mutex_);
                if (pending_->answer_) {
                    pending_->answer_->set_value(storage::ProviderInteractionReply{Context.answer_->choice_ == InteractionChoice::Confirm, storage::Password(std::move(Context.answer_->value_))});
                    pending_->answer_.reset(); pending_->prompt_.reset();
                }
            }
            std::unique_lock Lock(pending_->mutex_);
            if (!pending_->changed_.wait_for(Lock, std::chrono::milliseconds(100), [&] { return pending_->result_.has_value() || pending_->prompt_.has_value(); })) {
                JobStep Step{Context.committing_ ? JobState::Committing : JobState::Running};
                if (!Context.committing_) Step.deferFor_=std::chrono::milliseconds(100);
                if (!activity_.empty()) { Step.progress_=lastProgress_.value_or(JobProgress{}); Step.progress_->currentItem_=activity_; Step.progress_->currentBytes_=0; Step.progress_->currentTotal_.reset(); }
                return Step;
            }
            if (pending_->result_) {
                auto Result = std::move(*pending_->result_); if (Result.progress_) lastProgress_=Result.progress_; Lock.unlock(); worker_.join(); pending_.reset(); return Result;
            }
            JobStep Step{JobState::WaitingForUser};
            Step.interaction_ = InteractionRequest{{}, InteractionKind::Password, "provider-password-required", {InteractionChoice::Confirm, InteractionChoice::Cancel}};
            Step.interaction_->source_ = pending_->prompt_->subject_;
            Step.interaction_->passwordAttempt_ = pending_->prompt_->attempt_;
            return Step;
        }
        std::vector<storage::Error> Finalize(JobState State) override
        { stop(); return inner_->Finalize(State); }
    private:
        void stop()
        {
            if (!worker_.joinable()) return;
            worker_.request_stop();
            if (pending_) {
                std::lock_guard Lock(pending_->mutex_);
                if (pending_->answer_) { pending_->answer_->set_value(storage::Error{storage::ErrorCategory::Cancelled, "cancelled"}); pending_->answer_.reset(); }
            }
            worker_.join(); pending_.reset();
        }
        std::unique_ptr<IJobTask> inner_;
        std::shared_ptr<Pending> pending_;
        std::jthread worker_;
        std::string activity_;
        std::optional<JobProgress> lastProgress_;
    };
}
