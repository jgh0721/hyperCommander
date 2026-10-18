#pragma once
#include "jobScheduler.hpp"
#include "../storage/writableStorage.hpp"

namespace hc::operations
{
    enum class FileElevationAction { Copy, Remove, Recycle, Rename, MakeDirectory, CreateContent, SecurityChange, SecurityRestore, Metadata };
    struct FileElevationRequest
    {
        std::string jobId_;
        FileElevationAction action_ = FileElevationAction::Copy;
        std::optional<storage::EntrySnapshot> source_;
        std::shared_ptr<storage::IStorageProvider> sourceReader_;
        storage::Locator destination_;
        storage::DestinationExpectation expected_;
        storage::CopyOptions copyOptions_;
        storage::MetadataPatch metadataPatch_;
        std::optional<storage::EntrySnapshot> publishedDestination_;
        std::vector<std::byte> content_;
        std::uint64_t maxBytes_ = std::numeric_limits<std::uint64_t>::max(), bytesPerSecondLimit_ = 0;
        bool allowReadOnlyRemoval_ = false, requiresPublishedDestination_ = false;
        // Security changes are independent, explicit jobs, never a side effect
        // of selecting Elevate for a copy, move, delete or create operation.
        bool grantAccess_ = false;
        std::string backupPath_, backupIdentity_, securityDigest_;
    };
    struct FileElevationStatus
    {
        std::uint64_t bytesDone_ = 0;
        bool readyToCommit_ = false, finished_ = false;
        bool destinationCommitted_ = false, sourceRemoved_ = false;
        std::optional<storage::EntrySnapshot> destination_;
        std::optional<storage::Error> error_;
        std::vector<storage::Error> cleanupErrors_;
        std::string detail_;
    };
    class IFileElevationSession
    {
    public:
        virtual ~IFileElevationSession() = default;
        virtual storage::Result<FileElevationStatus> Poll() = 0;
        // A helper cannot publish/delete/change security before the scheduler
        // durably enters its commit phase and explicitly opens this gate.
        virtual storage::Result<storage::Unit> Commit() = 0;
        virtual void Cancel() = 0;
        virtual std::vector<storage::Error> Close() = 0;
    };
    class IFileElevationService
    {
    public:
        virtual ~IFileElevationService() = default;
        virtual bool CanExecute(const FileElevationRequest& Request) const = 0;
        virtual storage::Result<std::unique_ptr<IFileElevationSession>> Begin(FileElevationRequest Request, storage::OperationContext Context) = 0;
    };
    class FileElevationApproval
    {
    public:
        using Clock = std::function<std::chrono::steady_clock::time_point()>;
        explicit FileElevationApproval(Clock Now = [] { return std::chrono::steady_clock::now(); }) : now_(std::move(Now)) {}
        std::optional<InteractionChoice> Decision(const std::string& JobId, const std::string& Scope) const
        {
            std::lock_guard Lock(mutex_);
            if (JobId != jobId_ || Scope != scope_ || now_() >= expires_) return {};
            return decision_;
        }
        void Choose(const std::string& JobId, const std::string& Scope, InteractionChoice Decision)
        {
            std::lock_guard Lock(mutex_); jobId_ = JobId; scope_ = Scope; decision_ = Decision;
            expires_ = now_() + std::chrono::minutes(2);
        }
    private:
        Clock now_;
        mutable std::mutex mutex_;
        std::string jobId_, scope_;
        InteractionChoice decision_ = InteractionChoice::Elevate;
        std::chrono::steady_clock::time_point expires_{};
    };

    // Used for explicitly requested security changes. Failed ordinary file jobs
    // own their own per-item continuation instead of restarting a whole request.
    class FileElevationJob final : public IJobTask
    {
    public:
        FileElevationJob(FileElevationRequest Request, std::shared_ptr<IFileElevationService> Service)
            : request_(std::move(Request)), service_(std::move(Service)) {}
        JobStep Advance(JobStepContext Context) override
        {
            const auto Fail = [](storage::Error Error) { JobStep Step{JobState::Failed}; Step.error_ = std::move(Error); return Step; };
            if (!session_)
            {
                request_.jobId_ = Context.jobId_;
                if (!service_ || !service_->CanExecute(request_)) return Fail({storage::ErrorCategory::Unsupported, "elevation-unavailable"});
                auto Started = service_->Begin(request_, Context.operation_); if (!Started.HasValue()) return Fail(Started.Failure());
                session_ = std::move(Started.Value());
            }
            auto Status = session_->Poll(); if (!Status.HasValue()) return Fail(Status.Failure());
            JobStep Step{commitSent_ ? JobState::Committing : JobState::Running};
            Step.progress_ = JobProgress{Status.Value().bytesDone_, {}, request_.source_ ? request_.source_->name_ : request_.destination_.root_};
            if (Status.Value().readyToCommit_ && !commitSent_)
            {
                if (Context.committing_)
                {
                    auto Committed = session_->Commit(); if (!Committed.HasValue()) return Fail(Committed.Failure()); commitSent_ = true;
                }
                Step.next_ = JobState::Committing;
            }
            if (!Status.Value().finished_) { if (Step.next_ == JobState::Running) Step.deferFor_ = std::chrono::milliseconds(50); return Step; }
            cleanup_ = Status.Value().cleanupErrors_;
            Step.error_ = Status.Value().error_;
            Step.next_ = Step.error_ ? Step.error_->category_ == storage::ErrorCategory::Cancelled ? JobState::Cancelled : JobState::Failed : JobState::Completed;
            Step.item_ = ItemOutcome{request_.source_ ? storage::Serialize(request_.source_->locator_) : storage::Serialize(request_.destination_),
                Step.error_ ? ItemOutcome::Status::Failed : ItemOutcome::Status::Completed, Step.error_,
                Status.Value().destinationCommitted_, Status.Value().sourceRemoved_, request_.source_};
            if (!Status.Value().detail_.empty() && Step.item_->source_) Step.item_->source_->hints_["securityBackup"] = Status.Value().detail_;
            return Step;
        }
        std::vector<storage::Error> Finalize(JobState State) override
        {
            if (session_)
            {
                if (State != JobState::Completed) session_->Cancel();
                auto Errors = session_->Close(); cleanup_.insert(cleanup_.end(), Errors.begin(), Errors.end()); session_.reset();
            }
            return cleanup_;
        }
    private:
        FileElevationRequest request_;
        std::shared_ptr<IFileElevationService> service_;
        std::unique_ptr<IFileElevationSession> session_;
        std::vector<storage::Error> cleanup_;
        bool commitSent_ = false;
    };
}
