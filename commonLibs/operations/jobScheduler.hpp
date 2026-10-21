#pragma once
#include "../commands/commandBus.hpp"
#include <thread>

namespace hc::operations
{
    class IItemOutcomeStore;
    using storage::Error;
    using storage::Result;
    enum class JobState { Queued, Preparing, Running, WaitingForUser, Paused, Committing, Completed, Failed, Cancelled };
    bool IsTerminal(JobState State);
    const char* StateName(JobState State);
    enum class InteractionKind { Overwrite, Rename, Elevation, Password, DeleteConfirmation, RetryFailure, ReadOnlyRemoval };
    enum class InteractionChoice { Replace, Skip, Rename, Retry, Elevate, Confirm, Cancel, ConfirmAll, ElevateAll, SkipAll, CopyOnly };
    struct InteractionRequest
    {
        std::string id_;
        InteractionKind kind_ = InteractionKind::Overwrite;
        std::string reason_;
        std::vector<InteractionChoice> choices_;
        std::optional<storage::ResourceId> resource_;
        std::string source_, destination_;
        unsigned passwordAttempt_ = 1;
    };
    struct InteractionAnswer
    {
        std::string requestId_;
        InteractionChoice choice_ = InteractionChoice::Cancel;
        // Password replies stay in the task channel, never in event payloads.
        std::string value_;
    };
    struct JobProgress
    {
        std::uint64_t bytesDone_ = 0;
        std::optional<std::uint64_t> bytesTotal_;
        std::string currentItem_;
        std::uint64_t currentBytes_ = 0;
        std::optional<std::uint64_t> currentTotal_, plannedItems_;
        // Confirmed discovery is a lower bound until plannedItems_ is present.
        // Deleted files exclude skipped items and directory-removal operations.
        std::optional<std::uint64_t> discoveredItems_, discoveredFiles_, deletedFiles_;
    };
    struct ItemOutcome
    {
        enum class Status { Completed, Skipped, Failed };
        std::string item_;
        Status status_ = Status::Completed;
        std::optional<Error> error_;
        bool destinationCommitted_ = false;
        bool sourceRemoved_ = false;
        // Immutable identity actually copied; a later move cleanup must never
        // resolve a changed path and delete its replacement.
        std::optional<storage::EntrySnapshot> source_;
    };
    struct JobMetadata
    {
        // Display-only policy summary. Never serialize credentials or callbacks.
        std::string label_, source_, destination_, options_, retryOf_;
    };
    enum class JobControl { Start, Pause, Resume, Cancel };
    struct JobSnapshot
    {
        std::string id_;
        std::string correlationId_;
        JobState state_ = JobState::Queued;
        JobProgress progress_;
        std::uint64_t completedItems_ = 0;
        std::uint64_t skippedItems_ = 0;
        std::uint64_t failedItems_ = 0;
        std::uint32_t retryAttempt_ = 0;
        bool pauseRequested_ = false;
        bool cancelRequested_ = false;
        std::optional<InteractionRequest> interaction_;
        std::optional<Error> error_;
        std::chrono::steady_clock::time_point submitted_;
        std::chrono::steady_clock::time_point updated_;
        double bytesPerSecond_ = 0;
        bool deferred_ = false;
        std::uint64_t queueOrder_ = 0;
        std::int64_t submittedUtcMs_ = 0;
        JobMetadata metadata_;
        std::string retriedAs_;
    };
    struct JobResult
    {
        JobSnapshot snapshot_;
        std::vector<ItemOutcome> items_;
        std::vector<Error> cleanupErrors_;
        // items_ is a bounded compatibility preview when this store is present.
        std::shared_ptr<IItemOutcomeStore> outcomes_;
        std::shared_ptr<IItemOutcomeStore> writes_;
    };
    struct JobHandle
    {
        std::string id_;
        std::shared_future<JobResult> completion_;
    };
    struct JobRequest
    {
        std::string correlationId_;
        bool deferred_ = false;
        JobMetadata metadata_;
        // Acquire all lanes together and retain them across pause/interaction.
        std::vector<std::string> resourceLanes_;
        storage::OperationContext operation_;
        std::size_t maxItemResults_ = std::numeric_limits<std::size_t>::max();
        std::uint32_t maxRetries_ = 3;
        std::chrono::milliseconds initialRetryDelay_{50};
        std::chrono::milliseconds maximumRetryDelay_{2000};
    };
    struct JobStepContext
    {
        std::string jobId_;
        storage::OperationContext operation_;
        std::optional<InteractionAnswer> answer_;
        std::uint32_t retryAttempt_ = 0;
        bool committing_ = false;
    };
    struct JobStep
    {
        JobState next_ = JobState::Running;
        std::optional<JobProgress> progress_;
        std::optional<ItemOutcome> item_;
        std::optional<InteractionRequest> interaction_;
        std::optional<Error> error_;
        bool retry_ = false;
        // Cooperative pacing: retain lanes, release the worker, honour cancel/pause.
        std::chrono::milliseconds deferFor_{0};
        // Replace a previously emitted outcome without counting the item twice.
        // Used when destination commit and source removal are separate phases.
        std::optional<std::size_t> replaceItem_;
    };
    class IJobTask
    {
    public:
        virtual ~IJobTask() = default;
        // One bounded I/O unit per call; keep continuation state in this object.
        // Return WaitingForUser to release the worker instead of waiting in it.
        virtual JobStep Advance(JobStepContext Context) = 0;
        // Called once, including for never-started and failed tasks, outside locks.
        // Cleanup must not inherit user cancellation; report every failed cleanup.
        virtual std::vector<Error> Finalize(JobState State) = 0;
    };
    struct PreparedJob
    {
        JobRequest request_;
        std::unique_ptr<IJobTask> task_;
    };
    class IJobJournal
    {
    public:
        virtual ~IJobJournal() = default;
        // Synchronous worker checkpoint, before task side effects and before
        // publishing completion. No passwords or interaction answers are passed.
        virtual Result<storage::Unit> Checkpoint(const JobResult& Result) = 0;
        virtual Result<std::vector<JobResult>> Recover() = 0;
        virtual Result<storage::Unit> Forget(const std::string& Id) = 0;
        virtual Result<std::shared_ptr<IItemOutcomeStore>> OpenItems(const std::string&)
        { return std::shared_ptr<IItemOutcomeStore>{}; }
        virtual Result<std::shared_ptr<IItemOutcomeStore>> OpenWrites(const std::string&)
        { return std::shared_ptr<IItemOutcomeStore>{}; }
    };
    class JobScheduler
    {
    public:
        explicit JobScheduler(std::shared_ptr<commands::EventHub> Events, std::size_t WorkerCount = 2, std::size_t MaxRetainedJobs = 1024,
            std::shared_ptr<IJobJournal> Journal = {});
        ~JobScheduler();
        JobScheduler(const JobScheduler&) = delete;
        JobScheduler& operator=(const JobScheduler&) = delete;
        Result<JobHandle> Submit(JobRequest Request, std::unique_ptr<IJobTask> Task);
        Result<JobSnapshot> Snapshot(const std::string& Id) const;
        std::vector<JobSnapshot> Snapshots() const;
        Result<storage::Unit> Cancel(const std::string& Id);
        Result<storage::Unit> Pause(const std::string& Id, bool Paused);
        Result<storage::Unit> Reply(const std::string& Id, InteractionAnswer Answer);
        Result<storage::Unit> Forget(const std::string& Id);
        // Validate the entire batch under one lock before changing any member.
        Result<storage::Unit> Control(const std::vector<std::string>& Ids, JobControl Control);
        Result<storage::Unit> ReorderQueued(const std::vector<std::string>& Ids);
        Result<storage::Unit> SetConcurrencyLimit(std::size_t Limit);
        std::size_t ConcurrencyLimit() const;
        Result<storage::Unit> SetLaneLimit(std::string Lane, std::size_t Limit);
        std::uint64_t DroppedNotifications() const;
        // Owner-thread lifetime operation; do not race destruction with API calls.
        void Shutdown();
    private:
        struct State;
        static void run(const std::shared_ptr<State>& Shared);
        static void flushEvents(const std::shared_ptr<State>& Shared);
        std::shared_ptr<State> state_;
        std::vector<std::jthread> workers_;
        std::thread::id owner_ = std::this_thread::get_id();
    };
}
