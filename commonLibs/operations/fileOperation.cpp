#include "stdafx.h"
#include "fileOperation.hpp"
#include <set>
#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include "directoryPreparation.hpp"

namespace hc::operations
{
    using namespace storage;
    namespace
    {
        bool FileInUse(const std::string& Code)
        {
            return Code == "win32:32" || Code == "win32:33" || Code == "win32:1224";
        }
        std::u32string Scalars(const std::string& Text)
        {
            std::u32string Result;
            for (std::size_t I = 0; I < Text.size();) {
                const auto First = static_cast<unsigned char>(Text[I++]); char32_t Value = First;
                int More = First < 0x80 ? 0 : First < 0xe0 ? 1 : First < 0xf0 ? 2 : 3;
                if (More) Value &= (1u << (6 - More)) - 1;
                while (More-- && I < Text.size()) Value = (Value << 6) | (static_cast<unsigned char>(Text[I++]) & 63);
                if (Value >= U'A' && Value <= U'Z') Value += U'a' - U'A';
                Result += Value;
            }
            return Result;
        }
        bool Matches(const std::string& Name, const std::string& Patterns)
        {
            const auto Text = Scalars(Name);
            for (std::size_t Begin = 0; Begin < Patterns.size();) {
                const auto End = Patterns.find(';', Begin);
                auto Part = Patterns.substr(Begin, End == std::string::npos ? End : End - Begin);
                const auto First = Part.find_first_not_of(' '), Last = Part.find_last_not_of(' ');
                Part = First == std::string::npos ? "" : Part.substr(First, Last - First + 1);
                const auto Mask = Scalars(Part == "*.*" ? "*" : Part);
                std::size_t N = 0, M = 0, Star = std::u32string::npos, Retry = 0;
                while (N < Text.size()) {
                    if (M < Mask.size() && (Mask[M] == U'?' || Mask[M] == Text[N])) { ++M; ++N; }
                    else if (M < Mask.size() && Mask[M] == U'*') { Star = M++; Retry = N; }
                    else if (Star != std::u32string::npos) { M = Star + 1; N = ++Retry; }
                    else break;
                }
                while (M < Mask.size() && Mask[M] == U'*') ++M;
                if (N == Text.size() && M == Mask.size()) return true;
                if (End == std::string::npos) break;
                Begin = End + 1;
            }
            return false;
        }
        bool SafeName(const std::string& Name)
        {
            return !Name.empty() && Name != "." && Name != ".." && Name.find_first_of("/\\:") == std::string::npos
                && Name.find('\0') == std::string::npos;
        }
        void AddListedFileBytes(std::optional<std::uint64_t>& Total, const EntrySnapshot& Entry, const std::string& Pattern)
        {
            if (!Total) return;
            if (Entry.directory_ || (Entry.attributes_ & 0x400)) { Total.reset(); return; }
            if (!Matches(Entry.name_, Pattern)) return;
            if (Entry.size_ > std::numeric_limits<std::uint64_t>::max() - *Total) { Total.reset(); return; }
            *Total += Entry.size_;
        }
        std::string& EntryPath(Locator& At) { return At.kind_ == LocatorKind::Archive && !At.nesting_.empty() ? At.nesting_.back().entry_ : At.root_; }
        Locator Child(Locator Parent, const std::string& Name)
        {
            auto& Path = EntryPath(Parent);
            if (!Path.ends_with('/') && !Path.ends_with('\\')) Path += Parent.kind_ == LocatorKind::Archive ? '/' : '\\';
            Path += Name;
            return Parent;
        }
        Locator Parent(Locator At)
        {
            auto& Path = EntryPath(At);
            const auto End = Path.find_last_of("/\\");
            Path = Path.substr(0, End == 0 ? 1 : End);
            if (Path.size() == 2 && Path[1] == ':') Path += '\\';
            if (At.kind_ == LocatorKind::Plugin && Path.ends_with(':')) Path += '\\';
            return At;
        }
        std::string NativeKey(std::string Path)
        {
            std::replace(Path.begin(), Path.end(), '/', '\\');
            if (Path.starts_with("\\\\?\\UNC\\")) Path = "\\\\" + Path.substr(8);
            else if (Path.starts_with("\\\\?\\")) Path.erase(0, 4);
            while (!Path.empty() && Path.back() == '\\') Path.pop_back();
            for (auto& Letter : Path) if (Letter >= 'A' && Letter <= 'Z') Letter += 'a' - 'A';
            return Path;
        }
        bool SameOrWithin(const Locator& To, const Locator& From)
        {
            if (To.kind_ != From.kind_ || (To.kind_ != LocatorKind::Local && To.kind_ != LocatorKind::Smb && To.kind_ != LocatorKind::Plugin)) return false;
            const auto A = NativeKey(To.root_), B = NativeKey(From.root_);
            return A == B || A.starts_with(B + "\\");
        }
        std::string LaneFor(const Locator& At) { return FileOperationLane(At); }
        struct PlannedItem
        {
            OperationSource source_;
            Locator destination_;
            std::size_t depth_ = 0;
            bool removeDirectory_ = false, freshMetadata_ = false;
            std::optional<Error> removalError_;
            bool renameMove_ = false;
            bool readOnly_ = false, excluded_ = false, recursiveRemoval_ = false;
            // Returned by the bulk tree removal: list it through the provider.
            bool classicTraversal_ = false;
            std::size_t root_ = 0;
            std::shared_ptr<const Error> preparationError_;
        };
        // A slot is retained through the serial commit: <=8 files/job and
        // <=32/process, each with a <=1 MiB payload read window.
        std::atomic<std::size_t> CopySlots{0};
        // 32 small-copy slots * 2 MiB + 12 pipelines * 16 MiB <=256 MiB.
        std::atomic<std::size_t> ReadAheadSlots{0};
        class CopyExecutor final
        {
        public:
            ~CopyExecutor()
            {
                { std::lock_guard Lock(mutex_); closing_ = true; }
                changed_.notify_all(); workers_.clear();
            }
            template<class F> std::future<JobStep> Submit(F&& Work)
            {
                std::packaged_task<JobStep()> Task(std::forward<F>(Work));
                auto Future = Task.get_future();
                std::lock_guard Lock(mutex_);
                // Reserve a worker before publishing the task. Allocation or
                // thread creation failure must never orphan a queued callback.
                if (workers_.size() < std::min<std::size_t>(32, active_ + pending_.size() + 1))
                    workers_.emplace_back([this] { run(); });
                pending_.push_back(std::move(Task)); changed_.notify_one();
                return Future;
            }
        private:
            void run()
            {
                for (;;) {
                    std::packaged_task<JobStep()> Work;
                    {
                        std::unique_lock Lock(mutex_);
                        changed_.wait(Lock, [this] { return closing_ || !pending_.empty(); });
                        if (pending_.empty()) return;
                        Work = std::move(pending_.front()); pending_.pop_front(); ++active_;
                    }
                    Work();
                    { std::lock_guard Lock(mutex_); --active_; }
                }
            }
            std::mutex mutex_;
            std::condition_variable changed_;
            std::deque<std::packaged_task<JobStep()>> pending_;
            std::vector<std::jthread> workers_;
            std::size_t active_ = 0;
            bool closing_ = false;
        };
        CopyExecutor& CopyWorkers() { static CopyExecutor Workers; return Workers; }
        class FileTask final : public IJobTask
        {
        public:
            explicit FileTask(FileOperationRequest Request) : request_(std::move(Request))
            {
                if (request_.destinationWriter_) mutationScope_ = request_.destinationWriter_->CreateMutationScope();
                if (!mutationScope_) for (const auto& Source : request_.sources_) {
                    if (Source.writer_) mutationScope_ = Source.writer_->CreateMutationScope();
                    if (mutationScope_) break;
                }
                // Disabling recursive discovery must not discard file sizes
                // already captured by the selection, including RDP listings.
                if (request_.operation_ == FileOperation::Copy || request_.operation_ == FileOperation::Move) {
                    listedBytes_ = 0;
                    for (const auto& Source : request_.sources_) AddListedFileBytes(listedBytes_, Source.entry_, request_.includePattern_);
                }
                const bool Native = std::ranges::all_of(request_.sources_, [](const auto& Source) {
                        return (Source.entry_.locator_.kind_ == LocatorKind::Local || Source.entry_.locator_.kind_ == LocatorKind::Smb)
                            && !Source.reader_->UsesOrderedReads();
                    });
                incrementalDelete_ = request_.operation_ == FileOperation::Delete && request_.deleteWhileListing_ && Native;
                streaming_ = incrementalDelete_ || (!request_.calculateSizeBefore_ && Native
                    && (request_.operation_ == FileOperation::Copy || request_.operation_ == FileOperation::Move));
                rootFiles_.resize(request_.sources_.size());
                renameRoots_.resize(request_.sources_.size());
                for (std::size_t Root = 0; auto& Source : request_.sources_) {
                    PlannedItem Item{Source, request_.operation_ == FileOperation::Rename ? request_.destination_ : Child(request_.destination_, request_.destinationName_.empty() ? Source.entry_.name_ : request_.destinationName_)};
                    Item.root_ = Root++; pending_.push_back(std::move(Item));
                }
                if (incrementalDelete_ && request_.confirmDelete_) phase_ = Phase::Confirm;
            }
            ~FileTask() override
            {
                transferStop_.request_stop();
                stopCopies();
                if (readAhead_.valid()) readAhead_.wait();
                if (readAheadSlot_) --ReadAheadSlots;
                if (production_.valid()) production_.wait();
            }
            JobStep Advance(JobStepContext Context) override
            {
                if (Context.operation_.allowReadOnlyRemoval_) { request_.readOnly_ = ReadOnlyPolicy::Allow; allowAllReadOnly_ = true; }
                // Advance already belongs to a coordinator/continuation worker.
                // Local metadata and file opens need no nested PPL dispatch.
                Context.operation_.workerRead_ = true;
                Context.operation_.mutationScope_ = mutationScope_;
                if (Context.operation_.stopToken_.stop_possible()) mutationStop_ = Context.operation_.stopToken_;
                if (!deleteWorkers_) {
                    deleteWorkers_ = 1;
                    // Durable recovery retains its original per-file commit /
                    // outcome barrier. Never widen its crash window to a batch.
                    if (request_.operation_ == FileOperation::Delete && request_.deleteMode_ == DeleteMode::Permanent
                        && !Context.operation_.writeCheckpoint_ && !request_.sources_.empty() && request_.sources_.front().writer_
                        && request_.sources_.front().writer_->SupportsParallelRemovals())
                        deleteWorkers_ = std::clamp<std::size_t>(Context.operation_.fileDeleteWorkers_
                            ? Context.operation_.fileDeleteWorkers_
                            : request_.sources_.front().writer_->PreferredDeleteWorkers(request_.sources_.front().entry_.locator_), 1, 8);
                }
                jobId_ = Context.jobId_;
                if (const auto Error = Context.operation_.Check({})) return failed(*Error);
                if (phase_ == Phase::PreparationPrompt) return preparationQuestion(Context);
                if (phase_ == Phase::LockPrompt) return lockQuestion(Context);
                if (phase_ == Phase::ElevationPrompt) return permission(Context);
                if (phase_ == Phase::ElevationRun) return elevated(Context);
                if (phase_ == Phase::Preparing) {
                    auto Step = prepare(Context.operation_);
                    for (std::size_t I = 1; deleteWorkers_ > 1 && I < 64 && phase_ == Phase::Preparing && !tree_
                        && Step.next_ == JobState::Preparing && !Step.error_ && !Step.item_ && Step.items_.empty() && !Step.interaction_; ++I) {
                        if (const auto Error = Context.operation_.Check({})) return failed(*Error);
                        Step = prepare(Context.operation_);
                    }
                    return Step;
                }
                if (phase_ == Phase::RetryRemove)
                {
                    // A failed deletion has no side effects. Re-enter its commit
                    // boundary only after the cancellable, cooperative delay.
                    phase_ = Phase::Remove;
                    auto Step = progress(); Step.next_ = JobState::Committing; return Step;
                }
                if (phase_ == Phase::Confirm)
                {
                    if (!Context.answer_)
                    {
                        releasePreparedMutations(); auto Step = progress(); Step.next_ = JobState::WaitingForUser;
                        Step.interaction_ = InteractionRequest{"", InteractionKind::DeleteConfirmation, request_.deleteMode_ == DeleteMode::Recycle ? "confirm-recycle-delete" : "confirm-permanent-delete",
                            {InteractionChoice::Confirm, InteractionChoice::Cancel}};
                        return Step;
                    }
                    if (Context.answer_->choice_ != InteractionChoice::Confirm) return failed({ErrorCategory::InvalidRequest, "delete-confirmation-required"});
                    deleteConfirmed_ = true;
                    if (!planningFinished_) { phase_ = Phase::Preparing; return progress(); }
                    phase_ = Phase::Begin;
                }
                if (phase_ == Phase::ReadOnly) {
                    if (!Context.answer_) return readOnlyQuestion();
                    if (Context.answer_->choice_ == InteractionChoice::Skip) return done(ItemOutcome::Status::Skipped, destinationCommitted_);
                    if (Context.answer_->choice_ != InteractionChoice::Confirm && Context.answer_->choice_ != InteractionChoice::ConfirmAll) return failed({ErrorCategory::InvalidRequest, "readonly-answer-required"});
                    if (Context.answer_->choice_ == InteractionChoice::ConfirmAll) allowAllReadOnly_ = true;
                    readOnlyAnswered_ = true; phase_ = Phase::Begin; return progress();
                }
                if (phase_ == Phase::Conflict) return conflict(Context);
                if (phase_ == Phase::AutomaticName) return automaticName(Context.operation_);
                if (phase_ == Phase::Begin) {
                    if (!copies_.empty() || canStartCopies(Context.operation_)) return advanceCopies(std::move(Context));
                    return begin(Context.operation_);
                }
                if (phase_ == Phase::OpenRead)
                {
                    auto ReadContext = Context.operation_;
                    ReadContext.denyConcurrentMutation_ = true;
                    ReadContext.orderedBatchRead_=current().source_.reader_->UsesOrderedReads();
                    ReadContext.workerRead_ = true;
                    ReadContext.expectedWriteBytes_ = current().source_.entry_.size_;
                    if (Context.operation_.directContent_ && !request_.copyOptions_.verify_ && !request_.bytesPerSecondLimit_
                        && request_.destinationWriter_->CanProduceContent(Context.operation_)) {
                        auto Prepared = current().source_.reader_->PrepareContent(current().source_.entry_.id_, ReadContext).get();
                        if (!Prepared.HasValue()) {
                            if (Prepared.Failure().retryable_ && Context.retryAttempt_ < 3)
                            { JobStep Step{JobState::Running}; Step.error_ = Prepared.Failure(); Step.retry_ = true; return Step; }
                            return failed(Prepared.Failure());
                        }
                        producer_ = std::move(Prepared.Value());
                        // Unordered/legacy archive providers can also acquire a
                        // batch lease while producing directly. Always finish it.
                        if (producer_ && std::none_of(batchReaders_.begin(), batchReaders_.end(),
                            [&](const auto& Reader) { return Reader.get() == current().source_.reader_.get(); }))
                            batchReaders_.push_back(current().source_.reader_);
                    }
                    if (!producer_)
                    {
                        auto Opened = current().source_.reader_->OpenRead(current().source_.entry_.id_, ReadContext).get();
                        if (!Opened.HasValue()) {
                            if (Opened.Failure().retryable_ && Context.retryAttempt_ < 3)
                            { JobStep Step{JobState::Running}; Step.error_ = Opened.Failure(); Step.retry_ = true; return Step; }
                            return failed(Opened.Failure());
                        }
                        stream_ = std::move(Opened.Value());
                    }
                    auto WriteContext = Context.operation_; WriteContext.expectedWriteBytes_ = current().source_.entry_.size_;
                    WriteContext.producingContent_ = producer_.has_value();
                    auto Created = request_.destinationWriter_->BeginCopy(current().destination_, expected_, effectiveCopyOptions_, WriteContext);
                    if (!Created.HasValue()) return failed(Created.Failure());
                    session_ = std::move(Created.Value());
                    auto Configured = session_->ConfigureCopy(current().source_.entry_, effectiveCopyOptions_, WriteContext);
                    if (!Configured.HasValue()) return failed(Configured.Failure());
                    phase_ = producer_ ? Phase::Produce : Phase::Read;
                    return progress();
                }
                if (phase_ == Phase::Produce) {
                    const auto Size = current().source_.entry_.size_;
                    if (Size > request_.maxBytes_ - bytesDone_) return failed({ErrorCategory::BudgetExceeded, "copy-byte-budget"});
                    Result<Unit> Produced = Unit{};
                    const auto* Reader = current().source_.reader_.get();
                    const bool Background = Size > 1024 * 1024 || (current().source_.entry_.locator_.kind_ == LocatorKind::Archive
                        && !producedReaders_.contains(Reader));
                    if (Background && !production_.valid()) {
                        productionProgress_ = std::make_shared<std::atomic<std::uint64_t>>(0);
                        auto WorkContext = transferContext(Context.operation_);
                        WorkContext.progress_ = [Progress = productionProgress_, Observer = WorkContext.progress_](std::uint64_t Bytes) {
                            Progress->store(Bytes); if (Observer) Observer(Bytes);
                        };
                        production_ = std::async(std::launch::async, [Session = session_.get(), Producer = *producer_, WorkContext]() mutable {
                            return Session->ProduceContent(std::move(Producer), WorkContext);
                        });
                    }
                    if (production_.valid()) {
                        // Wait inside this bounded step and continue as soon as the
                        // producer finishes. A 10 ms deferral resumed only on the
                        // scheduler's next tick: 25 ms, or 15.6 ms timer resolution,
                        // added to every file over 1 MiB.
                        if (production_.wait_for(std::chrono::milliseconds(20)) != std::future_status::ready) {
                            const auto Producing = std::min(Size, productionProgress_->load());
                            auto Step = progress(); Step.progress_->bytesDone_ += Producing; Step.progress_->currentBytes_ += Producing;
                            return Step;
                        }
                        Produced = production_.get(); productionProgress_.reset();
                    } else Produced = session_->ProduceContent(*producer_, Context.operation_);
                    if (!Produced.HasValue()) return failed(Produced.Failure());
                    producedReaders_.insert(Reader);
                    itemBytes_ = Size; bytesDone_ += Size;
                    chunk_ = {}; chunk_.eof_ = true; phase_ = Phase::Write;
                    return progress();
                }
                if (phase_ == Phase::Read)
                {
                    auto ReadContext = Context.operation_; ReadContext.workerRead_ = true;
                    const bool NativeWindow = (ReadContext.nativeReadAhead_ || ReadContext.nativeCopyBufferBytes_ > 1024 * 1024) && !request_.bytesPerSecondLimit_
                        && current().source_.entry_.locator_.kind_ == LocatorKind::Local
                        && current().source_.entry_.size_ > 1024 * 1024 && current().source_.reader_->SupportsParallelReads();
                    const bool ArchiveWindow = ReadContext.archiveReadAhead_ && !request_.bytesPerSecondLimit_
                        && current().source_.entry_.locator_.kind_ == LocatorKind::Archive
                        && current().destination_.kind_ == LocatorKind::Smb && current().source_.entry_.size_ > 1024 * 1024;
                    if (ArchiveWindow) ReadContext.maxReadBytes_ = std::min<std::size_t>(ReadContext.maxReadBytes_, 1024 * 1024);
                    if ((NativeWindow || ArchiveWindow) && !readAheadSlot_) {
                        auto Slots = ReadAheadSlots.load();
                        while (Slots < 12 && !ReadAheadSlots.compare_exchange_weak(Slots, Slots + 1)) {}
                        readAheadSlot_ = Slots < 12;
                    }
                    ReadContext.nativeReadAhead_ &= NativeWindow && readAheadSlot_;
                    ReadContext.nativeCopyBufferBytes_ = NativeWindow && readAheadSlot_
                        ? std::clamp<std::size_t>(ReadContext.nativeCopyBufferBytes_, 1, 8 * 1024 * 1024) : 1024 * 1024;
                    if (current().source_.entry_.locator_.kind_ == LocatorKind::Local)
                        ReadContext.maxReadBytes_ = std::min(ReadContext.maxReadBytes_, ReadContext.nativeCopyBufferBytes_);
                    const auto Remaining = std::max<std::uint64_t>(1, current().source_.entry_.size_ - itemBytes_);
                    // Archive sessions use read capacity as decoder credit too.
                    // Keep the established window across small members and EOF.
                    const auto Capacity = current().source_.entry_.locator_.kind_ == LocatorKind::Archive
                        ? ReadContext.maxReadBytes_ : std::min<std::uint64_t>(ReadContext.maxReadBytes_, Remaining);
                    auto Chunk = readAhead_.valid() ? readAhead_.get() : stream_->Read(request_.bytesPerSecondLimit_ ? std::min<std::uint64_t>(Capacity, std::max<std::uint64_t>(1, request_.bytesPerSecondLimit_ / 10)) : Capacity, ReadContext).get();
                    if (!Chunk.HasValue()) return failed(Chunk.Failure());
                    if (Chunk.Value().bytes_.empty() && !Chunk.Value().eof_) return failed({ErrorCategory::Internal, "stream-no-progress"});
                    if (Chunk.Value().bytes_.size() > request_.maxBytes_ - bytesDone_)
                        return failed({ErrorCategory::BudgetExceeded, "copy-byte-budget"});
                    chunk_ = std::move(Chunk.Value());
                    if (ArchiveWindow && readAheadSlot_ && !chunk_.eof_) {
                        auto AheadContext = transferContext(ReadContext);
                        // Archive reads can wait on decoder/host IPC. Keep just
                        // one read outstanding while Append owns the other buffer.
                        readAhead_ = std::async(std::launch::async, [Stream = stream_, Capacity, AheadContext]() mutable {
                            return Stream->Read(Capacity, AheadContext).get();
                        });
                    }
                    else if (ReadContext.nativeReadAhead_ && !chunk_.eof_ && !request_.bytesPerSecondLimit_
                        && current().source_.entry_.locator_.kind_ == LocatorKind::Local
                        && current().source_.entry_.size_ > 1024 * 1024 && current().source_.reader_->SupportsParallelReads()) {
                        auto AheadContext = transferContext(ReadContext); AheadContext.workerRead_ = false;
                        const auto NextRemaining = Remaining > chunk_.bytes_.size() ? Remaining - chunk_.bytes_.size() : 1;
                        readAhead_ = stream_->Read(std::min<std::uint64_t>(Capacity, NextRemaining), AheadContext);
                    }
                    phase_ = Phase::Write;
                    return progress();
                }
                if (phase_ == Phase::Write)
                {
                    if (!chunk_.bytes_.empty())
                    {
                        if (request_.copyOptions_.verify_) {
                            auto Recorded = session_->RecordSourceBytes(chunk_.bytes_, Context.operation_);
                            if (!Recorded.HasValue()) return failed(Recorded.Failure());
                        }
                        auto Written = session_->Append(chunk_.bytes_, Context.operation_);
                        if (!Written.HasValue()) return failed(Written.Failure());
                        bytesDone_ += chunk_.bytes_.size();
                        itemBytes_ += chunk_.bytes_.size();
                    }
                    if (!chunk_.eof_) { phase_ = Phase::Read; return paced(chunk_.bytes_.size()); }
                    if (itemBytes_ != current().source_.entry_.size_) return failed({ErrorCategory::StaleRevision, "source-size-changed"});
                    auto Verified = current().source_.reader_->Stat(current().source_.entry_.id_, Context.operation_).get();
                    if (!Verified.HasValue()) return failed(Verified.Failure());
                    if (request_.copyOptions_.metadata_) {
                        auto Metadata = session_->SetMetadata({current().source_.entry_.modifiedUtcMs_, current().source_.entry_.attributes_}, Context.operation_);
                        if (!Metadata.HasValue()) return failed(Metadata.Failure());
                    }
                    stream_.reset();
                    producer_.reset();
                    phase_ = Phase::Extras;
                    return paced(chunk_.bytes_.size());
                }
                if (phase_ == Phase::Extras) {
                    auto ExtraContext = Context.operation_;
                    if (request_.bytesPerSecondLimit_) ExtraContext.maxReadBytes_ = std::min<std::size_t>(ExtraContext.maxReadBytes_, std::max<std::uint64_t>(1, request_.bytesPerSecondLimit_ / 10));
                    auto Extra = session_->AdvanceCopyExtras(request_.maxBytes_ - bytesDone_, ExtraContext);
                    if (!Extra.HasValue()) return failed(Extra.Failure());
                    bytesDone_ += Extra.Value().bytes_; extraBytes_ += Extra.Value().bytes_;
                    if (Extra.Value().done_) phase_ = request_.copyOptions_.verify_ ? Phase::Verify : Phase::ReadyCommit;
                    return paced(Extra.Value().bytes_);
                }
                if (phase_ == Phase::Verify) {
                    auto Verified = session_->VerifyContent(Context.operation_);
                    if (!Verified.HasValue()) return failed(Verified.Failure());
                    if (Verified.Value()) phase_ = Phase::ReadyCommit;
                    return progress();
                }
                if (phase_ == Phase::ReadyCommit) { phase_ = Phase::Commit; auto Step = progress(); Step.next_ = JobState::Committing; return Step; }
                if (!Context.committing_) return failed({ErrorCategory::Internal, "commit-boundary-required"});
                return commit(Context.operation_);
            }
            CompletedEffects TakeCompletedEffects() override
            {
                // Tree removal and transfer run ahead of the steps. A cancelled,
                // paused or failed job still records every completed file.
                if (!tree_ && !transfer_ && transferLeftover_.empty()) return {};
                CompletedEffects Effects;
                for (auto& Outcome : transferLeftover_) Effects.items_.push_back(std::move(Outcome));
                transferLeftover_.clear();
                if (tree_) {
                    tree_->Release();
                    if (auto Batch = tree_->Collect(std::numeric_limits<std::size_t>::max(), std::chrono::milliseconds(0)); Batch.HasValue())
                        Effects.items_ = acceptRemoved(std::move(Batch.Value().removed_));
                    tree_.reset();
                }
                if (transfer_) {
                    // Release removes the files it left incomplete first.
                    transfer_->Release();
                    if (auto Batch = transfer_->Collect(std::numeric_limits<std::size_t>::max(), std::chrono::milliseconds(0)); Batch.HasValue()) {
                        auto Transferred = acceptTransferred(std::move(Batch.Value().done_));
                        Effects.items_.insert(Effects.items_.end(), std::make_move_iterator(Transferred.begin()), std::make_move_iterator(Transferred.end()));
                    }
                    transfer_.reset();
                }
                Effects.progress_ = progress().progress_;
                return Effects;
            }
            std::vector<Error> Finalize(JobState State) override
            {
                if (tree_) { tree_->Release(); tree_.reset(); }
                if (transfer_) { transfer_->Release(); transfer_.reset(); }
                transferStop_.request_stop();
                stopCopies();
                for (auto& Copy : copies_) {
                    auto Errors = Copy->task_->Finalize(State);
                    cleanup_.insert(cleanup_.end(), Errors.begin(), Errors.end());
                }
                copies_.clear();
                if (readAhead_.valid()) {
                    try { readAhead_.get(); }
                    catch (...) { cleanup_.push_back({ErrorCategory::Internal, "read-ahead-finalize"}); }
                }
                // The producer is the session's only owner until it has stopped.
                // Cancellation reaches its copied operation context before Abort.
                if (production_.valid()) {
                    try { production_.get(); }
                    catch (...) { cleanup_.push_back({ErrorCategory::Internal, "content-producer-finalize"}); }
                }
                enumerations_.clear(); cursor_.reset();
                stream_.reset();
                producer_.reset();
                if (elevatedSession_) {
                    elevatedSession_->Cancel(); auto Errors = elevatedSession_->Close();
                    cleanup_.insert(cleanup_.end(), Errors.begin(), Errors.end()); elevatedSession_.reset();
                }
                if (session_)
                {
                    const auto Cleaned = session_->Abort();
                    if (!Cleaned.HasValue()) cleanup_.push_back(Cleaned.Failure());
                    session_.reset();
                }
                for (const auto& Reader:batchReaders_) {
                    auto Closed=Reader->FinishReadBatch({}).get(); if (!Closed.HasValue()) cleanup_.push_back(Closed.Failure());
                }
                batchReaders_.clear();
                if (ownsMutationScope_ && mutationScope_) mutationScope_->Clear();
                if (State != JobState::Completed || preservedFailure_)
                    for (const auto& At : preservedDestinations_)
                        cleanup_.push_back({ErrorCategory::Unavailable, "conflict-destination-preserved", {}, {}, false, {}, Serialize(At)});
                return cleanup_;
            }
        private:
            OperationContext transferContext(OperationContext Context)
            {
                if (!transferStop_.stop_possible()) transferStop_ = std::stop_source{};
                if (!transferCancellation_ && Context.stopToken_.stop_possible())
                    transferCancellation_ = std::make_unique<std::stop_callback<std::function<void()>>>(Context.stopToken_,
                        [Stop = transferStop_]() mutable { Stop.request_stop(); });
                Context.stopToken_ = transferStop_.get_token();
                return Context;
            }
            struct PreparedCopy
            {
                std::unique_ptr<FileTask> task_;
                std::future<JobStep> work_;
                std::optional<JobStep> barrier_;
                std::atomic<std::uint64_t> bytes_{0};
                bool reserved_ = false;
                bool recheck_ = false;
                ~PreparedCopy() { if (work_.valid()) work_.wait(); if (reserved_) --CopySlots; }
            };
            bool copyCandidate(std::size_t Index) const
            {
                if (Index >= items_.size()) return false;
                const auto& Item = items_[Index];
                return !Item.excluded_ && !Item.preparationError_ && !Item.source_.entry_.directory_ && !Item.removeDirectory_
                    && Item.source_.entry_.locator_.kind_ == LocatorKind::Local && Item.destination_.kind_ == LocatorKind::Local
                    && Item.source_.entry_.size_ <= 1024 * 1024 && !(Item.source_.entry_.attributes_ & (0x400 | 0x200 | 0x800 | 0x4000))
                    && Item.source_.reader_->SupportsParallelReads();
            }
            bool canStartCopies(const OperationContext& Context)
            {
                if (copyWorkers_ == 1 || Context.fileCopyWorkers_ == 1) return false;
                if (request_.operation_ != FileOperation::Copy || request_.bytesPerSecondLimit_ || request_.copyOptions_.streams_
                    || Context.writeCheckpoint_ || Context.workload_.IsAutomatic()
                    || !request_.destinationWriter_->SupportsParallelCopies() || !copyCandidate(index_)) return false;
                if (!copyWorkers_) {
                    copyWorkers_ = std::min<std::size_t>(8, Context.fileCopyWorkers_ ? Context.fileCopyWorkers_
                        : items_.size() >= 32 ? request_.destinationWriter_->PreferredCopyWorkers(current().source_.entry_.locator_, current().destination_) : 1);
                }
                return copyWorkers_ > 1 && (copyCandidate(index_ + 1) || !copies_.empty());
            }
            void stopCopies()
            {
                copyStop_.request_stop();
                for (auto& Copy : copies_) if (Copy->work_.valid()) {
                    try { Copy->work_.get(); }
                    catch (...) { cleanup_.push_back({ErrorCategory::Internal, "copy-worker-finalize"}); }
                }
            }
            void prepareCopy(PreparedCopy& Copy, const PlannedItem& Item)
            {
                // Never clone the complete selection into every worker.
                FileOperationRequest Request;
                Request.sources_ = {Item.source_}; Request.destination_ = Parent(Item.destination_);
                Request.destinationWriter_ = request_.destinationWriter_;
                Request.conflict_ = request_.conflict_; Request.copyOptions_ = request_.copyOptions_;
                Request.conflictDecision_ = request_.conflictDecision_;
                Request.approvedDestination_ = request_.approvedDestination_; Request.maxBytes_ = Item.source_.entry_.size_;
                auto Task = std::make_unique<FileTask>(std::move(Request));
                // The parent already checked this immutable source plan.
                Task->pending_.clear(); Task->items_.push_back(Item); Task->items_.back().root_ = 0;
                Task->bytesTotal_ = Item.source_.entry_.size_; Task->planningFinished_ = true;
                Task->phase_ = Phase::Begin; Task->copyWorkers_ = 1;
                // All file workers belong to one job. Reuse its parent pins and
                // capability lifetime rather than recreating/clearing per file.
                Task->mutationScope_ = mutationScope_; Task->ownsMutationScope_ = false;
                Copy.task_ = std::move(Task); Copy.barrier_.reset(); Copy.recheck_ = false;
            }
            static void copyBytes(PreparedCopy& Copy, std::uint64_t Bytes)
            {
                // Rechecking a destination can discard a prepared stage. Keep
                // logical progress monotonic while that same file is prepared again.
                auto Previous = Copy.bytes_.load();
                while (Bytes > Previous && !Copy.bytes_.compare_exchange_weak(Previous, Bytes)) {}
            }
            void runCopy(PreparedCopy& Copy, JobStepContext Context)
            {
                Context.committing_ = false; Context.operation_.fileCopyWorkers_ = 1;
                Copy.work_ = CopyWorkers().Submit([this, &Copy, Context = std::move(Context)]() mutable {
                    std::stop_callback Cancel(Context.operation_.stopToken_, [this] { copyStop_.request_stop(); });
                    Context.operation_.stopToken_ = copyStop_.get_token();
                    try {
                        for (;;) {
                            while (Context.operation_.pauseRequested_ && Context.operation_.pauseRequested_()) {
                                if (const auto Error = Context.operation_.Check({})) {
                                    JobStep Step{JobState::Failed}; Step.error_ = *Error; return Step;
                                }
                                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                            }
                            auto Step = Copy.task_->Advance(Context); Context.answer_.reset();
                            if (Step.progress_) copyBytes(Copy, Step.progress_->bytesDone_);
                            if (Step.next_ == JobState::Committing || Step.next_ == JobState::WaitingForUser
                                || IsTerminal(Step.next_) || Step.item_ || Step.retry_ || Step.deferFor_.count()) return Step;
                        }
                    } catch (...) { JobStep Step{JobState::Failed}; Step.error_ = Error{ErrorCategory::Internal, "copy-worker-exception"}; return Step; }
                });
            }
            JobStep copyProgress()
            {
                auto Step = progress();
                for (const auto& Copy : copies_) Step.progress_->bytesDone_ += Copy->bytes_.load();
                Step.progress_->activeFiles_ = static_cast<uint32_t>(copies_.size());
                if (!copies_.empty()) Step.progress_->currentBytes_ = copies_.front()->bytes_.load();
                return Step;
            }
            JobStep advanceCopies(JobStepContext Context)
            {
                // The coordinator alone consumes answers and enters commit.
                if (!Context.committing_ && !Context.answer_) {
                    while (copies_.size() < copyWorkers_ && copyCandidate(index_ + copies_.size())) {
                        const auto& Item = items_[index_ + copies_.size()];
                        if (std::any_of(copies_.begin(), copies_.end(), [&](const auto& Pending) {
                            return NativeKey(Pending->task_->items_.front().destination_.root_) == NativeKey(Item.destination_.root_);
                        })) break;
                        auto Copy = std::make_unique<PreparedCopy>();
                        auto Slots = CopySlots.load();
                        while (Slots < 32 && !CopySlots.compare_exchange_weak(Slots, Slots + 1)) {}
                        if (Slots >= 32) break;
                        Copy->reserved_ = true;
                        prepareCopy(*Copy, Item);
                        auto WorkContext = Context; WorkContext.answer_.reset();
                        WorkContext.operation_.maxReadBytes_ = std::min<std::size_t>(1024 * 1024, WorkContext.operation_.maxReadBytes_);
                        copies_.push_back(std::move(Copy)); runCopy(*copies_.back(), std::move(WorkContext));
                    }
                }
                if (copies_.empty()) return begin(Context.operation_);
                auto& Copy = *copies_.front();
                if (Copy.work_.valid()) {
                    // New workers were started above; wait for the oldest within
                    // this step instead of a 1 ms deferral that the scheduler's
                    // tick stretched to 15-25 ms.
                    if (Copy.work_.wait_for(std::chrono::milliseconds(20)) != std::future_status::ready) return copyProgress();
                    Copy.barrier_ = Copy.work_.get();
                }
                if (Copy.recheck_ && !Context.committing_) {
                    auto Errors = Copy.task_->Finalize(JobState::Cancelled);
                    cleanup_.insert(cleanup_.end(), Errors.begin(), Errors.end());
                    if (Errors.empty()) {
                        prepareCopy(Copy, current()); runCopy(Copy, std::move(Context)); return copyProgress();
                    }
                    Copy.recheck_ = false; Copy.barrier_ = JobStep{JobState::Failed}; Copy.barrier_->error_ = Errors.front();
                }
                if (!Copy.barrier_) { runCopy(Copy, std::move(Context)); return copyProgress(); }
                auto& Barrier = *Copy.barrier_;
                if ((Barrier.next_ == JobState::Committing && Context.committing_)
                    || (Barrier.next_ == JobState::WaitingForUser && Context.answer_)) {
                    Barrier = Copy.task_->Advance(Context);
                    if (Barrier.progress_) copyBytes(Copy, Barrier.progress_->bytesDone_);
                    // A user's rename/replace can change the meaning of another
                    // queued path, including aliases. Inspect those paths again
                    // after this file completes, before showing their questions.
                    if (Context.answer_) for (auto& Pending : copies_) if (Pending.get() != &Copy) Pending->recheck_ = true;
                }
                if (IsTerminal(Barrier.next_) && Barrier.next_ != JobState::Completed) {
                    const auto Error = Barrier.error_.value_or(storage::Error{ErrorCategory::Internal, "copy-worker-failed"});
                    expected_ = Copy.task_->expected_;
                    destinationCommitted_ = Copy.task_->destinationCommitted_;
                    current().destination_ = Copy.task_->current().destination_;
                    if (request_.continueSelectedFiles_ && !(Error.category_ == ErrorCategory::AccessDenied && request_.elevation_)) {
                        itemBytes_ = Copy.bytes_.load(); bytesDone_ += itemBytes_;
                        auto Errors = Copy.task_->Finalize(JobState::Failed);
                        cleanup_.insert(cleanup_.end(), Errors.begin(), Errors.end()); copies_.pop_front();
                        return failed(Error);
                    }
                    stopCopies();
                    for (auto& Pending : copies_) {
                        auto Errors = Pending->task_->Finalize(JobState::Failed);
                        cleanup_.insert(cleanup_.end(), Errors.begin(), Errors.end());
                    }
                    copies_.clear(); copyStop_ = std::stop_source{};
                    return failed(Error);
                }
                if (Barrier.item_) {
                    const auto Item = *Barrier.item_; bytesDone_ += Copy.bytes_.load();
                    auto Errors = Copy.task_->Finalize(Barrier.next_);
                    cleanup_.insert(cleanup_.end(), Errors.begin(), Errors.end()); copies_.pop_front();
                    auto Step = done(Item.status_, Item.destinationCommitted_, Item.sourceRemoved_);
                    for (const auto& Pending : copies_) Step.progress_->bytesDone_ += Pending->bytes_.load();
                    if (!cleanup_.empty()) { Step.next_ = JobState::Failed; Step.error_ = cleanup_.front(); }
                    return Step;
                }
                auto Step = copyProgress();
                Step.next_ = Barrier.next_ == JobState::Preparing ? JobState::Running : Barrier.next_;
                Step.interaction_ = Barrier.interaction_; Step.error_ = Barrier.error_;
                Step.retry_ = Barrier.retry_; Step.deferFor_ = Barrier.deferFor_;
                if (Step.next_ != JobState::Committing && Step.next_ != JobState::WaitingForUser) Copy.barrier_.reset();
                return Step;
            }
            struct EnumerationFrame {
                PlannedItem parent_;
                std::unique_ptr<IEntryCursor> cursor_;
                std::vector<EntrySnapshot> entries_;
                std::size_t next_ = 0;
                bool ended_ = false;
                // Declared after cursor_: destruction joins the outstanding
                // page before destroying the cursor captured by its provider.
                Task<EntryPage> readAhead_;
            };
            enum class Phase { Preparing, PreparationPrompt, Confirm, ReadOnly, Begin, Conflict, AutomaticName, PreserveDestination, OpenRead, Produce, Read, Write, Extras, Verify, ReadyCommit, Commit, MakeDirectory, Remove, RemoveBatch, RetryRemove, Rename, ElevationPrompt, ElevationRun, LockPrompt };
            PlannedItem& current() { return items_[index_]; }
            std::string itemLocator()
            { return Serialize(current().source_.entry_.locator_.root_.empty() ? current().destination_ : current().source_.entry_.locator_); }
            JobStep progress()
            {
                JobStep Step{phase_ == Phase::Preparing ? JobState::Preparing : JobState::Running};
                // Alternate stream sizes are unknown until each file is copied.
                // They join the total as they join the done count, so enabling
                // stream copy never hides the planned total.
                Step.progress_ = JobProgress{bytesDone_, bytesTotal_ + extraBytes_, index_ < items_.size() ? current().source_.entry_.name_ : std::string{}};
                if (phase_ == Phase::Preparing || !request_.calculateSizeBefore_) Step.progress_->bytesTotal_.reset();
                if (listedBytes_) {
                    Step.progress_->bytesTotal_ = (planningFinished_ ? bytesTotal_ : *listedBytes_) + extraBytes_;
                    Step.progress_->plannedItems_ = request_.sources_.size();
                }
                if (planningFinished_ || request_.operation_ == FileOperation::MakeDirectory) Step.progress_->plannedItems_ = request_.operation_ == FileOperation::Move
                    ? movePlannedItems() + treeItems_ : items_.size() + trimmedItems_ + treeItems_;
                if (request_.operation_ == FileOperation::Delete) {
                    Step.progress_->discoveredItems_ = verifiedItems_;
                    Step.progress_->discoveredFiles_ = discoveredFiles_;
                    Step.progress_->deletedFiles_ = deletedFiles_;
                    // No partial tree size/count may be published as the total.
                    if (!planningFinished_) { Step.progress_->bytesTotal_.reset(); Step.progress_->plannedItems_.reset(); }
                }
                Step.progress_->currentBytes_ = itemBytes_;
                if (index_ < items_.size() && !current().source_.entry_.directory_) Step.progress_->currentTotal_ = current().source_.entry_.size_;
                if (index_ < items_.size()) {
                    if (!current().source_.entry_.locator_.root_.empty()) Step.progress_->source_ = Serialize(current().source_.entry_.locator_);
                    if (!current().destination_.root_.empty()) Step.progress_->destination_ = Serialize(current().destination_);
                }
                Step.progress_->phase_ = phase_ == Phase::Preparing ? JobPhase::Preparing
                    : phase_ == Phase::Rename ? JobPhase::ApplyingNames : phase_ == Phase::MakeDirectory ? JobPhase::Creating
                    : phase_ == Phase::Verify ? JobPhase::Verifying
                    : (phase_ == Phase::Remove || phase_ == Phase::RetryRemove) && request_.operation_ == FileOperation::Move ? JobPhase::RemovingSource
                    : request_.operation_ == FileOperation::Delete ? JobPhase::Deleting
                    : phase_ == Phase::Commit ? JobPhase::Committing : JobPhase::Copying;
                return Step;
            }
            std::uint64_t movePlannedItems()
            {
                // Every step reports progress. Counting the whole plan each time
                // made large moves quadratic: 270,000 files moved at 77/s.
                // The plan only changes size after planning, so cache by size.
                if (movePlannedSize_ != items_.size()) {
                    movePlannedSize_ = items_.size();
                    movePlanned_ = std::ranges::count_if(items_, [](const auto& Item) { return !Item.source_.entry_.directory_ || Item.removeDirectory_ || Item.renameMove_; });
                }
                return movePlanned_;
            }
            JobStep paced(std::uint64_t Bytes)
            {
                auto Step = progress();
                if (request_.bytesPerSecondLimit_ && Bytes) Step.deferFor_ = std::chrono::milliseconds((Bytes * 1000 + request_.bytesPerSecondLimit_ - 1) / request_.bytesPerSecondLimit_);
                return Step;
            }
            JobStep failed(Error Error)
            {
                if (!preservedDestinations_.empty()) preservedFailure_ = true;
                // An automatic-name lookup or preservation failure must never
                // turn into an elevated replacement of the original target.
                if (phase_ == Phase::AutomaticName || phase_ == Phase::PreserveDestination || Error.code_.starts_with("automatic-name-failed:")) {
                    if (!Error.code_.starts_with("automatic-name-failed:")) Error.code_ = "automatic-name-failed:" + Error.code_;
                    if (Error.nativePath_.empty() && index_ < items_.size()) Error.nativePath_ = current().destination_.root_;
                    return terminalFailure(std::move(Error));
                }
                // A job-wide skip decision also applies before offering elevation.
                if (request_.operation_ == FileOperation::Delete && skipDeleteErrors_ && index_ < items_.size()
                    && (phase_ == Phase::Begin || phase_ == Phase::Remove)
                    && Error.category_ != ErrorCategory::Cancelled && Error.category_ != ErrorCategory::Internal
                    && Error.category_ != ErrorCategory::InvalidRequest && Error.category_ != ErrorCategory::BudgetExceeded)
                    return done(ItemOutcome::Status::Skipped);
                if (request_.fileLockInteractions_ && index_ < items_.size()
                    && (phase_ == Phase::Rename || phase_ == Phase::Remove)
                    && current().source_.entry_.locator_.kind_ == LocatorKind::Local
                    && FileInUse(Error.code_)) {
                    if (request_.operation_ == FileOperation::Delete && skipDeleteErrors_) return done(ItemOutcome::Status::Skipped);
                    lockError_ = Error; resumePhase_ = phase_; phase_ = Phase::LockPrompt;
                    return progress();
                }
                if (Error.category_ != ErrorCategory::Cancelled && request_.elevation_ && index_ < items_.size()
                    && phase_ != Phase::Preparing && phase_ != Phase::ElevationPrompt && phase_ != Phase::ElevationRun)
                {
                    auto Request = elevationRequest();
                    canElevate_ = request_.elevation_->CanExecute(Request);
                    if (canElevate_ || (destinationCommitted_ && request_.operation_==FileOperation::Move)) {
                        stream_.reset();
                        if (session_) {
                            auto Aborted = session_->Abort(); session_.reset();
                            if (!Aborted.HasValue()) { cleanup_.push_back(Aborted.Failure()); return terminalFailure(Error); }
                        }
                        permissionError_ = Error; elevatedRequest_ = std::move(Request); phase_ = Phase::ElevationPrompt;
                        // Leave the commit phase before asking a new question.
                        // Report an already-published move outcome immediately.
                        auto Step = progress();
                        if (destinationCommitted_ && !partialOutcome_) {
                            Step.item_ = ItemOutcome{itemLocator(), ItemOutcome::Status::Failed, Error, true, false, current().source_.entry_};
                            partialOutcome_ = outcomeIndex_++;
                        }
                        return Step;
                    }
                }
                if (request_.operation_ == FileOperation::Delete && request_.fileLockInteractions_
                    && index_ < items_.size() && (phase_ == Phase::Begin || phase_ == Phase::Remove)
                    && Error.category_ != ErrorCategory::Cancelled && Error.category_ != ErrorCategory::Internal
                    && Error.category_ != ErrorCategory::InvalidRequest && Error.category_ != ErrorCategory::BudgetExceeded) {
                    if (skipDeleteErrors_) return done(ItemOutcome::Status::Skipped);
                    lockError_ = Error; resumePhase_ = phase_; phase_ = Phase::LockPrompt; return progress();
                }
                return terminalFailure(std::move(Error));
            }
            JobStep lockQuestion(const JobStepContext& Context)
            {
                if (!Context.answer_) {
                    releasePreparedMutations(); auto Step = progress(); Step.next_ = JobState::WaitingForUser;
                    Step.interaction_ = InteractionRequest{"", InteractionKind::RetryFailure, (request_.operation_ == FileOperation::Delete && !FileInUse(lockError_.code_) ? "delete-failure:" : "file-in-use:") + lockError_.code_,
                        {InteractionChoice::Retry, InteractionChoice::Skip, InteractionChoice::Cancel}, current().source_.entry_.id_,
                        lockError_.nativePath_.empty() ? current().source_.entry_.locator_.root_ : lockError_.nativePath_, current().destination_.root_};
                    if (request_.operation_ == FileOperation::Delete) Step.interaction_->choices_.insert(Step.interaction_->choices_.end() - 1, InteractionChoice::SkipAll);
                    if (request_.elevation_ && request_.elevation_->CanExecute(elevationRequest()))
                        Step.interaction_->choices_.insert(Step.interaction_->choices_.end() - 1, InteractionChoice::Elevate);
                    return Step;
                }
                if (Context.answer_->choice_ == InteractionChoice::SkipAll && request_.operation_ == FileOperation::Delete) skipDeleteErrors_ = true;
                if (Context.answer_->choice_ == InteractionChoice::Skip || skipDeleteErrors_) return done(ItemOutcome::Status::Skipped, destinationCommitted_);
                if (Context.answer_->choice_ == InteractionChoice::Cancel) { auto Step = terminalFailure({ErrorCategory::Cancelled,"file-in-use-cancelled"}); Step.next_ = JobState::Cancelled; return Step; }
                if (Context.answer_->choice_ == InteractionChoice::Elevate && request_.elevation_) {
                    elevatedRequest_ = elevationRequest(); canElevate_ = request_.elevation_->CanExecute(elevatedRequest_);
                    permissionError_ = lockError_; phase_ = Phase::ElevationPrompt; return permission(Context);
                }
                if (Context.answer_->choice_ != InteractionChoice::Retry) return terminalFailure({ErrorCategory::InvalidRequest,"file-in-use-answer"});
                if (destinationCommitted_ && request_.operation_ == FileOperation::Move && !current().removeDirectory_) {
                    if (!publishedDestination_) return terminalFailure({ErrorCategory::StaleRevision,"move-destination-unverified"});
                    auto Fresh = request_.destinationWriter_->Inspect(current().destination_, Context.operation_);
                    if (!Fresh.HasValue() || !Fresh.Value() || Fresh.Value()->id_ != publishedDestination_->id_)
                        return terminalFailure({ErrorCategory::StaleRevision,"move-destination-changed"});
                }
                if (resumePhase_ == Phase::ElevationRun) return beginElevation(Context);
                phase_ = resumePhase_;
                auto Step = progress(); Step.next_ = JobState::Committing; return Step;
            }
            JobStep terminalFailure(Error Error)
            {
                if (Error.nativePath_.empty() && index_ < items_.size()
                    && (phase_ == Phase::Remove || phase_ == Phase::Rename)
                    && (Error.code_ == "win32:5" || Error.code_ == "win32:32" || Error.code_ == "win32:33" || Error.code_ == "win32:1224"))
                    Error.nativePath_ = current().source_.entry_.locator_.root_;
                if (request_.continueSelectedFiles_ && Error.category_ != ErrorCategory::Cancelled && Error.code_ != "deadline-exceeded"
                    && phase_ != Phase::Preparing && index_ < items_.size()) {
                    if (session_) {
                        auto Aborted = session_->Abort(); if (!Aborted.HasValue()) cleanup_.push_back(Aborted.Failure());
                    }
                    auto Step = done(ItemOutcome::Status::Failed, destinationCommitted_);
                    for (const auto& Pending : copies_) Step.progress_->bytesDone_ += Pending->bytes_.load();
                    Step.error_ = Error; Step.item_->error_ = Error;
                    return Step;
                }
                JobStep Step{JobState::Failed}; Step.error_ = Error;
                if (phase_ != Phase::Preparing && index_ < items_.size())
                    Step.item_ = ItemOutcome{itemLocator(), ItemOutcome::Status::Failed, Error, destinationCommitted_, false, current().source_.entry_};
                if (Step.item_ && partialOutcome_) Step.replaceItem_ = partialOutcome_;
                return Step;
            }
            JobStep done(ItemOutcome::Status Status = ItemOutcome::Status::Completed, bool Destination = false, bool Removed = false)
            {
                if (request_.operation_ == FileOperation::Delete && Status == ItemOutcome::Status::Completed && Removed)
                    deletedFiles_ += current().source_.entry_.directory_ ? (request_.deleteMode_ == DeleteMode::Recycle ? rootFiles_[current().root_] : 0) : 1;
                auto Step = progress();
                if (Status == ItemOutcome::Status::Skipped && (request_.operation_ == FileOperation::Move || request_.operation_ == FileOperation::Delete))
                    skippedSources_.push_back(current().source_.entry_.locator_);
                // Move directories produce one outcome when source removal runs.
                if (!(request_.operation_ == FileOperation::Move && current().source_.entry_.directory_ && !current().removeDirectory_ && !current().renameMove_))
                    Step.item_ = ItemOutcome{itemLocator(), Status, {}, Destination, Removed, current().source_.entry_};
                if (Step.item_) { if (partialOutcome_) Step.replaceItem_ = partialOutcome_; else ++outcomeIndex_; }
                partialOutcome_.reset(); publishedDestination_.reset();
                session_.reset(); stream_.reset(); producer_.reset(); chunk_ = {}; itemBytes_ = 0; destinationCommitted_ = false;
                ++index_; readOnlyAnswered_ = false; sourceRemoveRetries_ = 0;
                copyOptionsResolved_ = false;
                phase_ = Phase::Begin;
                if (index_ == items_.size()) {
                    if (streaming_ && !planningFinished_) { phase_ = Phase::Preparing; Step.next_ = JobState::Preparing; }
                    else Step.next_ = JobState::Completed;
                }
                trimProcessed();
                return Step;
            }
            void trimProcessed()
            {
                // Deletion completes plans in order. Release finished ones so a
                // large selection keeps memory proportional to unfinished work.
                if (request_.operation_ != FileOperation::Delete || index_ < 4096 || partialOutcome_) return;
                items_.erase(items_.begin(), items_.begin() + std::ptrdiff_t(index_));
                trimmedItems_ += index_; index_ = 0;
            }
            FileElevationRequest elevationRequest() const
            {
                FileElevationRequest Request; Request.jobId_ = jobId_;
                const auto& Item = items_[index_];
                if (!Item.source_.entry_.locator_.root_.empty()) { Request.source_ = Item.source_.entry_; Request.sourceReader_ = Item.source_.reader_; }
                Request.destination_ = Item.destination_; Request.expected_ = expected_;
                Request.copyOptions_ = copyOptionsResolved_ ? effectiveCopyOptions_ : request_.copyOptions_;
                Request.maxBytes_ = request_.maxBytes_ - (bytesDone_ - itemBytes_); Request.bytesPerSecondLimit_ = request_.bytesPerSecondLimit_;
                Request.allowReadOnlyRemoval_ = request_.readOnly_ == ReadOnlyPolicy::Allow || allowAllReadOnly_ || readOnlyAnswered_;
                Request.recursiveRemoval_ = Item.recursiveRemoval_;
                Request.maxEntries_ = request_.maxEntries_ - seen_.size() + 1;
                Request.maxDepth_ = request_.maxDepth_ - Item.depth_;
                if (destinationCommitted_ && request_.operation_ == FileOperation::Move && !Item.removeDirectory_) {
                    Request.action_ = FileElevationAction::Remove; Request.requiresPublishedDestination_ = true; Request.publishedDestination_ = publishedDestination_;
                }
                else if (request_.operation_ == FileOperation::Delete || Item.removeDirectory_)
                    Request.action_ = request_.operation_ == FileOperation::Delete && request_.deleteMode_ == DeleteMode::Recycle ? FileElevationAction::Recycle : FileElevationAction::Remove;
                else if (request_.operation_ == FileOperation::Rename || Item.renameMove_) Request.action_ = FileElevationAction::Rename;
                else if (request_.operation_ == FileOperation::MakeDirectory || Item.source_.entry_.directory_) Request.action_ = FileElevationAction::MakeDirectory;
                else Request.action_ = FileElevationAction::Copy;
                if(request_.operation_==FileOperation::Delete||Item.removeDirectory_)Request.destination_=Item.source_.entry_.locator_;
                if(Request.action_==FileElevationAction::MakeDirectory)Request.source_.reset();
                if (request_.operation_ == FileOperation::Delete && Request.source_ && !Request.source_->id_.revision_) {
                    const auto Listing = Request.source_->hints_.find("nativeListingRevision");
                    if (Listing != Request.source_->hints_.end()) Request.source_->id_.revision_ = ResourceRevision{Listing->second};
                }
                return Request;
            }
            std::string approvalScope() const
            {
                return std::to_string(int(request_.operation_)) + ":" + Serialize(request_.destination_);
            }
            JobStep permission(const JobStepContext& Context)
            {
                auto Cached = request_.elevationApproval_->Decision(jobId_, approvalScope());
                if(!canElevate_&&Cached==InteractionChoice::Elevate)Cached.reset();
                if (!Context.answer_ && !Cached) {
                    releasePreparedMutations(); auto Step = progress(); Step.next_ = JobState::WaitingForUser;
                    Step.interaction_ = InteractionRequest{"", InteractionKind::Elevation, permissionError_->code_,
                        {InteractionChoice::Elevate, InteractionChoice::ElevateAll, InteractionChoice::Retry, InteractionChoice::Skip, InteractionChoice::SkipAll, InteractionChoice::Cancel},
                        elevatedRequest_.source_ ? std::optional(elevatedRequest_.source_->id_) : std::nullopt};
                    if(!canElevate_) {
                        auto& Choices=Step.interaction_->choices_;
                        Choices.erase(std::remove_if(Choices.begin(),Choices.end(),[](InteractionChoice C){return C==InteractionChoice::Elevate||C==InteractionChoice::ElevateAll;}),Choices.end());
                    }
                    if (destinationCommitted_ && request_.operation_ == FileOperation::Move) Step.interaction_->choices_.push_back(InteractionChoice::CopyOnly);
                    Step.interaction_->source_ = elevatedRequest_.source_ ? elevatedRequest_.source_->locator_.root_ : "";
                    Step.interaction_->destination_ = elevatedRequest_.destination_.root_;
                    return Step;
                }
                const auto Choice = Context.answer_ ? Context.answer_->choice_ : *Cached;
                if (Choice == InteractionChoice::Skip || Choice == InteractionChoice::SkipAll) {
                    if (Choice == InteractionChoice::SkipAll) request_.elevationApproval_->Choose(jobId_, approvalScope(), InteractionChoice::Skip);
                    return done(ItemOutcome::Status::Skipped, destinationCommitted_);
                }
                if (Choice == InteractionChoice::CopyOnly && destinationCommitted_ && request_.operation_ == FileOperation::Move) {
                    skippedSources_.push_back(current().source_.entry_.locator_); return done(ItemOutcome::Status::Completed, true, false);
                }
                if (Choice == InteractionChoice::Retry) {
                    if (current().recursiveRemoval_) {
                        // Permission changes may also unblock errors captured for
                        // later roots. Revalidate those plans instead of asking
                        // again from their cached access-denied results.
                        std::deque<PlannedItem> Retried;
                        const auto Retry = [&](PlannedItem& Item) {
                            if (!Item.recursiveRemoval_) return false;
                            seen_.erase(Serialize(Item.source_.entry_.locator_));
                            Item.preparationError_.reset(); Item.recursiveRemoval_ = false;
                            Retried.push_back(std::move(Item)); return true;
                        };
                        items_.erase(std::remove_if(items_.begin() + index_, items_.end(), Retry), items_.end());
                        // Reopened subtrees must finish before any queued parent
                        // cleanup. Restore discovery order for the final reverse pass.
                        for (auto At = items_.size(); At > index_;) {
                            --At;
                            if (!items_[At].removeDirectory_) continue;
                            directories_.push_back(std::move(items_[At])); items_.erase(items_.begin() + At);
                        }
                        for (auto It = Retried.rbegin(); It != Retried.rend(); ++It) pending_.push_front(std::move(*It));
                        planningFinished_ = false; phase_ = Phase::Preparing;
                        auto Step = progress(); Step.next_ = JobState::Preparing; return Step;
                    }
                    phase_ = destinationCommitted_ ? Phase::Remove : Phase::Begin;
                    auto Step = progress(); if (destinationCommitted_) Step.next_ = JobState::Committing; return Step;
                }
                if (Choice != InteractionChoice::Elevate && Choice != InteractionChoice::ElevateAll) return terminalFailure({ErrorCategory::InvalidRequest, "invalid-elevation-answer"});
                if (Choice == InteractionChoice::ElevateAll) request_.elevationApproval_->Choose(jobId_, approvalScope(), InteractionChoice::Elevate);
                return beginElevation(Context);
            }
            JobStep beginElevation(const JobStepContext& Context)
            {
                // The helper opens its own verified handles. Release prepared
                // children and parent pins before handing over a partial tree.
                releasePreparedMutations();
                auto Begun = request_.elevation_->Begin(elevatedRequest_, Context.operation_);
                if (!Begun.HasValue()) return terminalFailure(Begun.Failure());
                elevatedSession_ = std::move(Begun.Value()); elevatedBase_ = bytesDone_ - itemBytes_; elevationCommitSent_ = false;
                phase_ = Phase::ElevationRun; return progress();
            }
            JobStep elevated(const JobStepContext& Context)
            {
                auto Status = elevatedSession_->Poll(); if (!Status.HasValue()) return terminalFailure(Status.Failure());
                if (elevatedRequest_.action_ == FileElevationAction::Copy) {
                    itemBytes_ = std::max(itemBytes_, Status.Value().bytesDone_); bytesDone_ = elevatedBase_ + itemBytes_;
                }
                auto Step = progress(); if (elevationCommitSent_) Step.next_ = JobState::Committing;
                if (Status.Value().readyToCommit_ && !elevationCommitSent_) {
                    if (Context.committing_) {
                        auto Gate = elevatedSession_->Commit(); if (!Gate.HasValue()) return terminalFailure(Gate.Failure()); elevationCommitSent_ = true;
                    }
                    Step.next_ = JobState::Committing;
                }
                if (!Status.Value().finished_) { if (Step.next_ == JobState::Running) Step.deferFor_ = std::chrono::milliseconds(50); return Step; }
                cleanup_.insert(cleanup_.end(), Status.Value().cleanupErrors_.begin(), Status.Value().cleanupErrors_.end());
                destinationCommitted_ |= Status.Value().destinationCommitted_;
                auto Closed = elevatedSession_->Close(); elevatedSession_.reset(); cleanup_.insert(cleanup_.end(), Closed.begin(), Closed.end());
                if (Status.Value().error_) {
                    // A mapped file can reject disposition with access denied even
                    // after DELETE access was granted. Elevation cannot release it.
                    // Keep the original error and offer inspection/retry; retries
                    // use the same verified elevated request and commit gate.
                    if (request_.fileLockInteractions_ && (FileInUse(Status.Value().error_->code_)
                            || (request_.operation_ == FileOperation::Delete && Status.Value().error_->code_ == "win32:5"))
                        && (elevatedRequest_.action_ == FileElevationAction::Remove || elevatedRequest_.action_ == FileElevationAction::Recycle || elevatedRequest_.action_ == FileElevationAction::Rename)) {
                        lockError_ = *Status.Value().error_; resumePhase_ = Phase::ElevationRun; phase_ = Phase::LockPrompt;
                        return progress();
                    }
                    auto Failed = terminalFailure(*Status.Value().error_);
                    if (Status.Value().error_->category_ == ErrorCategory::Cancelled) Failed.next_ = JobState::Cancelled;
                    return Failed;
                }
                if (elevatedRequest_.action_ == FileElevationAction::Copy && request_.operation_ == FileOperation::Move) {
                    publishedDestination_ = Status.Value().destination_;
                    phase_ = Phase::Remove; auto Next = progress(); Next.next_ = JobState::Committing; return Next;
                }
                if (current().preparationError_ && Status.Value().sourceRemoved_) ++verifiedItems_;
                return done(ItemOutcome::Status::Completed, destinationCommitted_, Status.Value().sourceRemoved_);
            }
            JobStep prepare(OperationContext Context)
            {
                Context.freshEnumerationMetadata_ = true;
                Context.fileDeleteWorkers_ = deleteWorkers_;
                if (request_.operation_ == FileOperation::Delete && request_.deleteMode_ == DeleteMode::Permanent)
                    Context.removalPreparation_ = std::dynamic_pointer_cast<IRemovalPreparation>(mutationScope_);
                Context.incrementalEnumeration_ = request_.operation_ == FileOperation::Delete;
                if (request_.operation_ == FileOperation::Delete) Context.allowUnavailableChildMetadata_ = true;
                if (!transferLeftover_.empty()) {
                    auto Step = progress(); Step.next_ = JobState::Running;
                    while (!transferLeftover_.empty() && Step.items_.size() < 128) {
                        Step.items_.push_back(std::move(transferLeftover_.front())); transferLeftover_.pop_front();
                    }
                    return Step;
                }
                if (tree_) return collectTree();
                if (transfer_) return collectTransfer();
                if (incrementalDelete_ && !enumerations_.empty()) return prepareIncremental(Context);
                if (incrementalDelete_ && request_.deleteMode_ == DeleteMode::Recycle && index_ < items_.size()) {
                    // Recycle a selected directory as one unit, after its own
                    // readonly/link checks, while later selected roots remain unscanned.
                    for (const auto& At : readOnlySources_) if (SameOrWithin(At, current().source_.entry_.locator_)) current().readOnly_ = true;
                    phase_ = Phase::Begin; return progress();
                }
                if (request_.operation_ == FileOperation::MakeDirectory)
                {
                    OperationSource Label;
                    auto Destination = request_.destination_;
                    Label.entry_.name_ = EntryPath(Destination);
                    items_.push_back({std::move(Label), request_.destination_});
                    phase_ = Phase::Begin;
                    return progress();
                }
                if (cursor_)
                {
                    auto Page = cursor_->Next(Context.maxPageItems_, Context).get();
                    if (!Page.HasValue()) return enumerationFailure(Page.Failure());
                    if (Page.Value().entries_.empty() && !Page.Value().end_) return failed({ErrorCategory::Internal, "cursor-no-progress"});
                    if (pending_.size() + seen_.size() + Page.Value().entries_.size() > request_.maxEntries_)
                        return failed({ErrorCategory::BudgetExceeded, "operation-entry-budget"});
                    for (auto& Entry : Page.Value().entries_)
                    {
                        if (!SafeName(Entry.name_)) return failed({ErrorCategory::InvalidRequest, "unsafe-entry-name"});
                        PlannedItem Item{{Entry, enumerating_.source_.reader_, enumerating_.source_.writer_},
                            Child(request_.preservePaths_ ? enumerating_.destination_ : request_.destination_, Entry.name_), enumerating_.depth_ + 1};
                        Item.freshMetadata_ = Context.freshEnumerationMetadata_ && Item.source_.reader_->SupportsFreshEnumerationMetadata();
                        Item.root_ = enumerating_.root_; pending_.push_back(std::move(Item));
                    }
                    if (Page.Value().end_) cursor_.reset();
                    return progress();
                }
                if (pending_.empty())
                {
                    planningFinished_ = true;
                    for (auto It = directories_.rbegin(); It != directories_.rend(); ++It) items_.push_back(*It);
                    // Permission Retry can resume discovery; transfer cleanup entries once.
                    directories_.clear();
                    if (request_.deleteMode_ == DeleteMode::Recycle && request_.operation_ == FileOperation::Delete)
                        for (auto& Item : items_) for (const auto& At : readOnlySources_) if (SameOrWithin(At, Item.source_.entry_.locator_)) Item.readOnly_ = true;
                    if (request_.operation_==FileOperation::Copy || request_.operation_==FileOperation::Move) {
                        // Prepare selected archive members once in decoder order.
                        // Keep directory creation before contents and retain the
                        // normal per-file conflict, commit and recovery boundaries.
                        std::map<IStorageProvider*,std::vector<EntrySnapshot>> Groups;
                        std::map<IStorageProvider*,std::size_t> GroupOrder;
                        bool Ordered=false;
                        for (const auto& Item:items_) if (!Item.source_.entry_.directory_) {
                            const auto Reader=Item.source_.reader_.get();
                            if (!GroupOrder.contains(Reader)) GroupOrder[Reader]=GroupOrder.size();
                            if (!Item.excluded_) Groups[Reader].push_back(Item.source_.entry_); Ordered|=Reader->UsesOrderedReads();
                        }
                        if (Ordered) std::stable_sort(items_.begin(),items_.end(),[&](const PlannedItem& A,const PlannedItem& B) {
                            if (A.source_.entry_.directory_!=B.source_.entry_.directory_) return A.source_.entry_.directory_;
                            if (A.source_.entry_.directory_) return false;
                            auto* Left=A.source_.reader_.get(); auto* Right=B.source_.reader_.get();
                            if (Left!=Right) return GroupOrder[Left]<GroupOrder[Right];
                            if (!Left->UsesOrderedReads()) return false;
                            const auto& Ah=A.source_.entry_.hints_; const auto& Bh=B.source_.entry_.hints_;
                            return Ah.contains("ordinal") && Bh.contains("ordinal") && std::stoull(Ah.at("ordinal"))<std::stoull(Bh.at("ordinal"));
                        });
                        const bool DirectBatch = Context.directContent_ && !request_.copyOptions_.verify_ && !request_.bytesPerSecondLimit_
                            && request_.destinationWriter_->CanProduceContent(Context);
                        for (auto& [Reader,Entries]:Groups) if (Reader->UsesOrderedReads()
                            || (DirectBatch && !Entries.empty() && Entries.front().locator_.kind_ == LocatorKind::Archive)) {
                            auto ReadContext = Context;
                            ReadContext.directContent_ = DirectBatch;
                            ReadContext.producingContent_ = ReadContext.directContent_;
                            auto Prepared=Reader->PrepareReadBatch(std::move(Entries),ReadContext).get(); if (!Prepared.HasValue()) return failed(Prepared.Failure());
                            const auto Owner=std::find_if(items_.begin(),items_.end(),[&](const PlannedItem& Item){return Item.source_.reader_.get()==Reader;});
                            batchReaders_.push_back(Owner->source_.reader_);
                        }
                    }
                    phase_ = request_.operation_ == FileOperation::Delete && request_.confirmDelete_ && !deleteConfirmed_ ? Phase::Confirm : Phase::Begin;
                    return progress();
                }
                auto Item = std::move(pending_.front()); pending_.pop_front();
                return prepareItem(std::move(Item), Context);
            }
            bool prefetchAvailable() const
            {
                // Completed ancestor pages do not occupy the single I/O slot.
                // Retain at most one running prefetch across the DFS stack.
                return std::ranges::none_of(enumerations_, [](const auto& Frame) {
                    return Frame.readAhead_.valid() && Frame.readAhead_.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready;
                });
            }
            void releasePreparedMutations()
            {
                // User interaction may last indefinitely. Finish the bounded
                // discovery page before closing its prepared handles; resume
                // through normal revision verification rather than holding locks.
                for (auto& Frame : enumerations_) if (Frame.readAhead_.valid()) Frame.readAhead_.wait();
                if (mutationScope_) mutationScope_->Clear();
            }
            JobStep prepareIncremental(OperationContext Context)
            {
                auto& Frame = enumerations_.back();
                if (Frame.next_ < Frame.entries_.size()) {
                    auto Entry = std::move(Frame.entries_[Frame.next_++]);
                    PlannedItem Item{{std::move(Entry), Frame.parent_.source_.reader_, Frame.parent_.source_.writer_}, {}, Frame.parent_.depth_ + 1};
                    Item.destination_ = Child(request_.preservePaths_ ? Frame.parent_.destination_ : request_.destination_, Item.source_.entry_.name_); Item.root_ = Frame.parent_.root_;
                    Item.freshMetadata_ = Item.source_.reader_->SupportsFreshEnumerationMetadata();
                    return prepareItem(std::move(Item), Context);
                }
                if (Frame.ended_) { enumerations_.pop_back(); return progress(); }
                const bool Prefetched = Frame.readAhead_.valid();
                auto Page = Prefetched ? Frame.readAhead_.get() : Frame.cursor_->Next(Context.maxPageItems_, Context).get();
                if (!Page.HasValue()) return enumerationFailure(Page.Failure());
                if (Page.Value().entries_.empty() && !Page.Value().end_) return failed({ErrorCategory::Internal, "cursor-no-progress"});
                if (Page.Value().entries_.size() > request_.maxEntries_ - seen_.size()) return failed({ErrorCategory::BudgetExceeded, "operation-entry-budget"});
                for (const auto& Entry : Page.Value().entries_) {
                    if (!SafeName(Entry.name_)) return failed({ErrorCategory::InvalidRequest, "unsafe-entry-name"});
                    const auto Expected = Child(Frame.parent_.source_.entry_.locator_, Entry.name_);
                    if (Entry.locator_.kind_ != Expected.kind_ || Entry.locator_.nesting_ != Expected.nesting_ || NativeKey(Entry.locator_.root_) != NativeKey(Expected.root_))
                        return failed({ErrorCategory::InvalidRequest, "enumerated-child-outside-source"});
                }
                Frame.entries_ = std::move(Page.Value().entries_); Frame.next_ = 0; Frame.ended_ = Page.Value().end_;
                if (!Frame.ended_ && deleteWorkers_ > 1 && prefetchAvailable()) {
                    Frame.readAhead_ = Frame.cursor_->Next(Context.maxPageItems_, Context);
                }
                return progress();
            }
            JobStep enumerationFailure(Error Error)
            {
                if (incrementalDelete_ && !enumerations_.empty()) {
                    auto Item = enumerations_.back().parent_; enumerations_.pop_back();
                    return preparationFailure(std::move(Item), std::move(Error), false);
                }
                if (cursor_ && request_.operation_ == FileOperation::Delete) {
                    cursor_.reset(); return preparationFailure(enumerating_, std::move(Error), false);
                }
                return failed(std::move(Error));
            }
            JobStep skipPreparation()
            {
                skippedSources_.push_back(preparationItem_.source_.entry_.locator_);
                phase_ = Phase::Preparing;
                auto Step = progress();
                Step.item_ = ItemOutcome{Serialize(preparationItem_.source_.entry_.locator_), ItemOutcome::Status::Skipped,
                    lockError_, false, false, preparationItem_.source_.entry_};
                ++outcomeIndex_; return Step;
            }
            JobStep preparationQuestion(const JobStepContext& Context)
            {
                if (!Context.answer_) {
                    releasePreparedMutations(); auto Step = progress(); Step.next_ = JobState::WaitingForUser;
                    Step.interaction_ = InteractionRequest{"", InteractionKind::RetryFailure, "delete-failure:" + lockError_.code_,
                        {InteractionChoice::Skip, InteractionChoice::SkipAll, InteractionChoice::Cancel}, preparationItem_.source_.entry_.id_,
                        lockError_.nativePath_.empty() ? preparationItem_.source_.entry_.locator_.root_ : lockError_.nativePath_};
                    if (preparationRetry_) Step.interaction_->choices_.insert(Step.interaction_->choices_.begin(), InteractionChoice::Retry);
                    return Step;
                }
                const auto Choice = Context.answer_->choice_;
                if (Choice == InteractionChoice::SkipAll) skipDeleteErrors_ = true;
                if (Choice == InteractionChoice::Skip || Choice == InteractionChoice::SkipAll) return skipPreparation();
                if (Choice == InteractionChoice::Retry && preparationRetry_) {
                    seen_.erase(Serialize(preparationItem_.source_.entry_.locator_));
                    pending_.push_front(std::move(preparationItem_));
                    phase_ = Phase::Preparing; return progress();
                }
                JobStep Step{JobState::Cancelled}; Step.error_ = lockError_; return Step;
            }
            JobStep preparationFailure(PlannedItem Item, Error Error, bool Retry = true)
            {
                // Protected entries can fail Stat or enumeration before Remove.
                // Keep the captured revision for independent helper verification;
                // an unscanned directory requires a bounded recursive delete.
                if (request_.operation_ == FileOperation::Delete && request_.elevation_ && !skipDeleteErrors_
                    && Error.category_ != ErrorCategory::Cancelled && Error.category_ != ErrorCategory::Internal
                    && Error.category_ != ErrorCategory::InvalidRequest && Error.category_ != ErrorCategory::BudgetExceeded
                    && Item.source_.entry_.locator_.kind_ == LocatorKind::Local && !(Item.source_.entry_.attributes_ & 0x400)
                    && std::ranges::none_of(skippedSources_, [&](const auto& At) { return SameOrWithin(At, Item.source_.entry_.locator_); })) {
                    if (Item.source_.entry_.directory_) {
                        Item.recursiveRemoval_ = true;
                        // A cursor can fail after queuing children. Transfer only
                        // the remaining subtree and release its discovery IDs.
                        const auto Within = [&](const PlannedItem& Pending) {
                            const bool Match = SameOrWithin(Pending.source_.entry_.locator_, Item.source_.entry_.locator_);
                            if (Match) seen_.erase(Serialize(Pending.source_.entry_.locator_));
                            return Match;
                        };
                        std::erase_if(pending_, Within);
                        std::erase_if(directories_, Within);
                        items_.erase(std::remove_if(items_.begin() + index_, items_.end(), Within), items_.end());
                        seen_.insert(Serialize(Item.source_.entry_.locator_));
                    }
                    Item.preparationError_ = std::make_shared<const storage::Error>(std::move(Error));
                    Item.readOnly_ = (Item.source_.entry_.attributes_ & 1) != 0;
                    if (!Item.source_.entry_.directory_) {
                        ++discoveredFiles_; ++rootFiles_[Item.root_];
                        if (Item.source_.entry_.size_ > request_.maxBytes_ - bytesTotal_) return failed({ErrorCategory::BudgetExceeded, "operation-byte-budget"});
                        bytesTotal_ += Item.source_.entry_.size_;
                    }
                    items_.push_back(std::move(Item));
                    if (streaming_) phase_ = Phase::Begin;
                    return progress();
                }
                if (request_.operation_ == FileOperation::Delete && request_.fileLockInteractions_
                    && Error.category_ != ErrorCategory::Cancelled && Error.category_ != ErrorCategory::Internal
                    && Error.category_ != ErrorCategory::InvalidRequest && Error.category_ != ErrorCategory::BudgetExceeded) {
                    preparationItem_ = std::move(Item); lockError_ = std::move(Error); preparationRetry_ = Retry;
                    phase_ = Phase::PreparationPrompt;
                    if (skipDeleteErrors_) return skipPreparation();
                    return progress();
                }
                if (!request_.continueSelectedFiles_) return failed(std::move(Error));
                Item.preparationError_ = std::make_shared<const storage::Error>(std::move(Error));
                items_.push_back(std::move(Item)); return progress();
            }
            JobStep prepareItem(PlannedItem Item, OperationContext Context)
            {
                if (Item.depth_ > request_.maxDepth_ || seen_.size() >= request_.maxEntries_)
                    return failed({ErrorCategory::BudgetExceeded, "operation-tree-budget"});
                if (!seen_.insert(Serialize(Item.source_.entry_.locator_)).second) return failed({ErrorCategory::InvalidRequest, "overlapping-selection"});
                // Native mutation enumeration already opened this child and
                // captured a handle revision. Do not reopen it immediately in
                // Stat. Selected roots, unavailable metadata and retries retain
                // the original query; OpenRead/Remove/Rename verify again.
                const bool FreshMetadata = std::exchange(Item.freshMetadata_, false) && Item.source_.entry_.id_.revision_;
                if (!FreshMetadata) {
                    auto Fresh = Item.source_.reader_->Stat(Item.source_.entry_.id_, Context).get();
                    if (!Fresh.HasValue()) return preparationFailure(std::move(Item), Fresh.Failure());
                    if (Fresh.Value().locator_ != Item.source_.entry_.locator_ || Fresh.Value().id_.providerId_ != Item.source_.reader_->Id()
                        || Fresh.Value().id_.key_ != Item.source_.entry_.id_.key_ || !SafeName(Fresh.Value().name_))
                        return preparationFailure(std::move(Item), {ErrorCategory::Internal, "provider-returned-different-entry"});
                    Item.source_.entry_ = std::move(Fresh.Value());
                }
                if ((Item.source_.entry_.attributes_ & 0x400) && request_.operation_ != FileOperation::Delete)
                    return preparationFailure(std::move(Item), {ErrorCategory::Unsupported, "reparse-entry-denied"});
                ++verifiedItems_;
                if (!Item.source_.entry_.directory_)
                {
                    ++discoveredFiles_; ++rootFiles_[Item.root_];
                    Item.excluded_ = (request_.operation_ == FileOperation::Copy || request_.operation_ == FileOperation::Move) && !Matches(Item.source_.entry_.name_, request_.includePattern_);
                    if (!Item.excluded_) {
                        if (Item.source_.entry_.size_ > request_.maxBytes_ - bytesTotal_) return failed({ErrorCategory::BudgetExceeded, "operation-byte-budget"});
                        bytesTotal_ += Item.source_.entry_.size_;
                    }
                }
                Item.readOnly_ = (Item.source_.entry_.attributes_ & 1) != 0;
                const bool Recycling = request_.operation_ == FileOperation::Delete && request_.deleteMode_ == DeleteMode::Recycle;
                if (Item.readOnly_) readOnlySources_.push_back(Item.source_.entry_.locator_);
                if (Item.source_.entry_.attributes_ & 0x400) {
                    // Delete only the selected link/object, never enumerate its target.
                    Item.removeDirectory_ = Item.source_.entry_.directory_;
                    items_.push_back(std::move(Item));
                    if (streaming_) phase_ = Phase::Begin;
                    return progress();
                }
                // A whole-tree rename avoids enumerating or copying any descendants.
                // Only an absent destination qualifies; existing folders still merge.
                if (request_.operation_ == FileOperation::Move && Item.source_.writer_
                    && Item.source_.entry_.locator_.kind_ == LocatorKind::Local && Item.destination_.kind_ == LocatorKind::Local
                    && (request_.includePattern_ == "*" || request_.includePattern_ == "*.*")
                    && request_.maxBytes_ == std::numeric_limits<std::uint64_t>::max()
                    && request_.copyOptions_.metadata_ && !request_.copyOptions_.verify_ && !request_.bytesPerSecondLimit_) {
                    // Descendants share their root's volumes: links and mount
                    // points are denied above, so query the volumes once per root.
                    auto& Renamable = renameRoots_[Item.root_];
                    if (!Renamable) {
                        const auto CanRename = Item.source_.writer_->CanRenameTo(Item.source_.entry_.locator_, Item.destination_, Context);
                        if (!CanRename.HasValue()) return failed(CanRename.Failure());
                        Renamable = CanRename.Value();
                    }
                    if (*Renamable) {
                        const auto Existing = request_.destinationWriter_->Inspect(Item.destination_, Context);
                        if (!Existing.HasValue()) return failed(Existing.Failure());
                        if (!Existing.Value()) {
                            Item.renameMove_ = true; items_.push_back(std::move(Item));
                            if (streaming_) phase_ = Phase::Begin;
                            return progress();
                        }
                        // Into an existing folder on the same volume: rename the
                        // children folder by folder instead of one planned item each.
                        if (Existing.Value()->directory_ && transferAllowed(Item, Context) && startTransfer(Item, Context, TreeTransferMode::Merge))
                            return progress();
                    }
                }
                // A whole folder copied, or moved to another volume, into a new
                // destination folder. The engine owns it until it reports back.
                if (transferAllowed(Item, Context) && (request_.operation_ == FileOperation::Copy || renameRoots_[Item.root_] == false)) {
                    const auto Existing = request_.destinationWriter_->Inspect(Item.destination_, Context);
                    if (!Existing.HasValue()) return failed(Existing.Failure());
                    if (!Existing.Value() && startTransfer(Item, Context, request_.operation_ == FileOperation::Copy ? TreeTransferMode::Copy : TreeTransferMode::Move))
                        return progress();
                }
                const auto Planned = [&] {
                    if (streaming_ && index_ < items_.size() && !(incrementalDelete_ && Recycling)
                        && !(incrementalDelete_ && deleteWorkers_ > 1 && items_.size() - index_ < std::size_t{64})) phase_ = Phase::Begin;
                    return progress();
                };
                if ((Recycling ? Item.depth_ == 0 : !Item.source_.entry_.directory_ || request_.operation_ != FileOperation::Delete)
                    && (request_.preservePaths_ || !Item.source_.entry_.directory_)) {
                    if (!Item.source_.entry_.directory_) { items_.push_back(std::move(Item)); return Planned(); }
                    items_.push_back(Item);
                }
                if (Item.source_.entry_.directory_ && request_.operation_ != FileOperation::Rename)
                {
                    // The tree engine owns this directory until it reports back;
                    // planned files wait so no prompt can hold its handles.
                    if (request_.operation_ == FileOperation::Delete && !Recycling && startTree(Item, Context)) return progress();
                    if (request_.operation_ == FileOperation::Move || (request_.operation_ == FileOperation::Delete && !Recycling))
                    { auto Removed = Item; Removed.removeDirectory_ = true; directories_.push_back(std::move(Removed)); }
                    auto Cursor = Item.source_.reader_->Enumerate(Item.source_.entry_.locator_, Context).get();
                    if (!Cursor.HasValue()) return request_.operation_ == FileOperation::Delete
                        ? preparationFailure(std::move(Item), Cursor.Failure(), false) : failed(Cursor.Failure());
                    if (incrementalDelete_) {
                        enumerations_.push_back({std::move(Item), std::move(Cursor.Value())});
                        // Capture the next directory while deleting the previous
                        // directory's remaining files. Small folders otherwise
                        // serialize discovery and removal despite file workers.
                        if (deleteWorkers_ > 1 && index_ < items_.size() && prefetchAvailable()) {
                            auto& Frame = enumerations_.back();
                            Frame.readAhead_ = Frame.cursor_->Next(Context.maxPageItems_, Context);
                            phase_ = Phase::Begin;
                        }
                    }
                    else { cursor_ = std::move(Cursor.Value()); enumerating_ = std::move(Item); }
                }
                return Planned();
            }
            bool startTree(const PlannedItem& Item, const OperationContext& Context)
            {
                // Permanent local deletion of a directory whose identity was
                // just verified. Descendants are removed as listed (the same
                // trust as Explorer-style tools) but never through links; durable
                // recovery keeps its per-file barriers on the item path.
                if (!incrementalDelete_ || request_.deleteMode_ != DeleteMode::Permanent || Context.writeCheckpoint_
                    || Item.classicTraversal_ || Item.preparationError_ || !Item.source_.writer_ || !Item.source_.reader_
                    || !Item.source_.entry_.id_.revision_
                    || !Item.source_.writer_->SupportsTreeRemoval(*Item.source_.reader_, Item.source_.entry_.locator_)) return false;
                TreeRemovalOptions Options;
                Options.workers_ = deleteWorkers_;
                Options.maxDepth_ = request_.maxDepth_ > Item.depth_ ? request_.maxDepth_ - Item.depth_ : 0;
                Options.allowReadOnly_ = request_.readOnly_ == ReadOnlyPolicy::Allow || allowAllReadOnly_;
                auto Started = Item.source_.writer_->BeginTreeRemoval(Item.source_.entry_, Options, Context);
                // The item path reopens the directory and reports a real error.
                if (!Started.HasValue()) return false;
                tree_ = std::move(Started.Value());
                treeRoot_ = Item; treeRoot_.removeDirectory_ = true;
                return true;
            }
            std::vector<ItemOutcome> acceptRemoved(std::vector<EntrySnapshot> Removed)
            {
                std::vector<ItemOutcome> Outcomes; Outcomes.reserve(Removed.size());
                for (auto& Entry : Removed) {
                    // The root was counted when it was planned and verified.
                    if (Entry.locator_ != treeRoot_.source_.entry_.locator_) ++verifiedItems_;
                    if (!Entry.directory_) {
                        ++discoveredFiles_; ++deletedFiles_; ++rootFiles_[treeRoot_.root_]; bytesTotal_ += Entry.size_;
                    }
                    ++treeItems_; ++outcomeIndex_;
                    treeCurrent_ = Entry.name_;
                    Outcomes.push_back(ItemOutcome{Serialize(Entry.locator_), ItemOutcome::Status::Completed, {}, false, true, std::move(Entry)});
                }
                if (!Outcomes.empty()) treeSource_ = Outcomes.back().item_;
                return Outcomes;
            }
            JobStep collectTree()
            {
                auto Collected = tree_->Collect(128, std::chrono::milliseconds(20));
                if (!Collected.HasValue()) { tree_->Release(); tree_.reset(); return failed(Collected.Failure()); }
                auto& Batch = Collected.Value();
                auto Outcomes = acceptRemoved(std::move(Batch.removed_));
                if (Batch.finished_) finishTree(Batch);
                auto Step = progress();
                if (tree_ || !Outcomes.empty()) {
                    Step.next_ = JobState::Running; Step.progress_->phase_ = JobPhase::Deleting;
                    if (!treeCurrent_.empty()) { Step.progress_->currentItem_ = treeCurrent_; Step.progress_->source_ = treeSource_; }
                }
                if (seen_.size() + treeItems_ > request_.maxEntries_ || bytesTotal_ > request_.maxBytes_) {
                    if (tree_) { tree_->Release(); tree_.reset(); }
                    Step = failed({ErrorCategory::BudgetExceeded, seen_.size() + treeItems_ > request_.maxEntries_ ? "operation-entry-budget" : "operation-byte-budget"});
                }
                Step.items_ = std::move(Outcomes);
                return Step;
            }
            void finishTree(TreeRemovalBatch& Batch)
            {
                tree_->Release(); tree_.reset(); treeCurrent_.clear(); treeSource_.clear();
                const auto& Root = treeRoot_.source_.entry_.locator_;
                for (auto& Kept : Batch.remaining_) {
                    // Kept because a descendant remains. Ancestors come first, so
                    // the final reverse pass removes them after those entries.
                    auto Item = treeRoot_;
                    if (Kept.entry_.locator_ != Root) {
                        Item = PlannedItem{{std::move(Kept.entry_), treeRoot_.source_.reader_, treeRoot_.source_.writer_}, {}, treeRoot_.depth_ + Kept.depth_};
                        Item.root_ = treeRoot_.root_; ++verifiedItems_;
                    }
                    Item.removeDirectory_ = true; Item.readOnly_ = (Item.source_.entry_.attributes_ & 1) != 0;
                    directories_.push_back(std::move(Item));
                }
                for (auto It = Batch.deferred_.rbegin(); It != Batch.deferred_.rend(); ++It) {
                    // Decisions, links and errors take the verified per-item path.
                    PlannedItem Item{{std::move(It->entry_), treeRoot_.source_.reader_, treeRoot_.source_.writer_}, {}, treeRoot_.depth_ + It->depth_};
                    Item.root_ = treeRoot_.root_;
                    if (Item.source_.entry_.locator_ == Root) {
                        // The engine could not list the root: plan it again.
                        Item = treeRoot_; Item.removeDirectory_ = false;
                        seen_.erase(Serialize(Root)); --verifiedItems_;
                    }
                    Item.classicTraversal_ = Item.source_.entry_.directory_;
                    pending_.push_front(std::move(Item));
                }
            }
            bool transferAllowed(const PlannedItem& Item, const OperationContext& Context) const
            {
                // Whole local folders with the default copy semantics. Durable
                // recovery, filters, limits, verification and copy extras keep the
                // per-item path and its per-file commit boundaries; so do folders
                // the engine already returned.
                return Context.fileTransferWorkers_ != 1 && !Context.writeCheckpoint_ && !Context.workload_.IsAutomatic()
                    && (request_.operation_ == FileOperation::Copy || request_.operation_ == FileOperation::Move)
                    && Item.source_.entry_.directory_ && !Item.preparationError_ && !Item.classicTraversal_
                    && Item.source_.writer_ && Item.source_.reader_ && Item.source_.entry_.id_.revision_ && request_.preservePaths_
                    && (request_.includePattern_ == "*" || request_.includePattern_ == "*.*")
                    && request_.maxBytes_ == std::numeric_limits<std::uint64_t>::max() && !request_.bytesPerSecondLimit_
                    && request_.copyOptions_.metadata_ && !request_.copyOptions_.verify_ && !request_.copyOptions_.acl_ && !request_.copyOptions_.streams_
                    && !request_.approvedDestination_
                    && Item.source_.writer_->SupportsTreeTransfer(*Item.source_.reader_, Item.source_.entry_.locator_, *request_.destinationWriter_, Item.destination_);
            }
            bool startTransfer(const PlannedItem& Item, const OperationContext& Context, TreeTransferMode Mode)
            {
                TreeTransferOptions Options;
                Options.mode_ = Mode;
                // Renames within one volume contend in the filesystem: two workers
                // renamed 4,700 entries/s against 12,000/s for one (qt-repo, ReFS).
                Options.workers_ = Mode == TreeTransferMode::Merge ? 1 : std::clamp<std::size_t>(Context.fileTransferWorkers_ ? Context.fileTransferWorkers_
                    : Item.source_.writer_->PreferredTreeTransferWorkers(Item.source_.entry_.locator_, Item.destination_), 1, 16);
                Options.maxDepth_ = request_.maxDepth_ > Item.depth_ ? request_.maxDepth_ - Item.depth_ : 0;
                Options.allowReadOnly_ = request_.readOnly_ == ReadOnlyPolicy::Allow || allowAllReadOnly_;
                Options.countFirst_ = request_.calculateSizeBefore_;
                auto Started = Item.source_.writer_->BeginTreeTransfer(Item.source_.entry_, Item.destination_, Options, Context);
                // The item path reopens the folder and reports a real error.
                if (!Started.HasValue()) return false;
                transfer_ = std::move(Started.Value()); transferRoot_ = Item; transferMode_ = Mode;
                transferAccepted_ = transferBytes_ = 0; transferItemsBase_ = treeItems_;
                transferCountedBytes_.reset(); transferCountedItems_.reset();
                return true;
            }
            std::vector<ItemOutcome> acceptTransferred(std::vector<TreeTransferEntry> Done)
            {
                std::vector<ItemOutcome> Outcomes; Outcomes.reserve(Done.size());
                if (!Done.empty()) {
                    transferCurrent_ = Done.back().entry_.name_;
                    transferSource_ = Serialize(Done.back().entry_.locator_); transferTarget_ = Serialize(Done.back().destination_);
                }
                for (auto& Item : Done) {
                    // The root was counted when it was planned and verified.
                    if (Item.entry_.locator_ != transferRoot_.source_.entry_.locator_) ++verifiedItems_;
                    if (!Item.entry_.directory_) {
                        ++discoveredFiles_; bytesTotal_ += Item.entry_.size_; bytesDone_ += Item.entry_.size_; transferAccepted_ += Item.entry_.size_;
                    }
                    ++treeItems_; ++outcomeIndex_;
                    Outcomes.push_back(ItemOutcome{Serialize(Item.entry_.locator_), ItemOutcome::Status::Completed, {}, true,
                        transferMode_ != TreeTransferMode::Copy, std::move(Item.entry_)});
                }
                return Outcomes;
            }
            JobStep collectTransfer()
            {
                auto Collected = transfer_->Collect(128, std::chrono::milliseconds(20));
                if (!Collected.HasValue()) { transfer_->Release(); transfer_.reset(); return failed(Collected.Failure()); }
                auto& Batch = Collected.Value();
                auto Outcomes = acceptTransferred(std::move(Batch.done_));
                transferBytes_ = Batch.bytes_;
                if (Batch.countedBytes_) { transferCountedBytes_ = Batch.countedBytes_; transferCountedItems_ = Batch.countedItems_; }
                if (Batch.finished_) {
                    // Results reported while the workers stopped still belong here.
                    transfer_->Release();
                    if (auto Rest = transfer_->Collect(std::numeric_limits<std::size_t>::max(), std::chrono::milliseconds(0)); Rest.HasValue()) {
                        auto More = acceptTransferred(std::move(Rest.Value().done_));
                        Outcomes.insert(Outcomes.end(), std::make_move_iterator(More.begin()), std::make_move_iterator(More.end()));
                    }
                    finishTransfer(Batch);
                }
                // A step carries at most 128 results; later steps send the rest.
                while (Outcomes.size() > 128) { transferLeftover_.push_front(std::move(Outcomes.back())); Outcomes.pop_back(); }
                auto Step = progress();
                if (transfer_ || !Outcomes.empty()) {
                    Step.next_ = JobState::Running; Step.progress_->phase_ = JobPhase::Copying;
                    if (!transferCurrent_.empty()) {
                        Step.progress_->currentItem_ = transferCurrent_;
                        Step.progress_->source_ = transferSource_; Step.progress_->destination_ = transferTarget_;
                    }
                }
                if (transfer_) {
                    // Bytes of files still being copied, and the listed totals.
                    Step.progress_->bytesDone_ += transferBytes_ > transferAccepted_ ? transferBytes_ - transferAccepted_ : 0;
                    if (request_.calculateSizeBefore_ && transferCountedBytes_)
                        Step.progress_->bytesTotal_ = bytesTotal_ - transferAccepted_ + *transferCountedBytes_ + extraBytes_;
                    if (transferCountedItems_)
                        Step.progress_->plannedItems_ = (request_.operation_ == FileOperation::Move ? movePlannedItems() : items_.size() + trimmedItems_)
                            + transferItemsBase_ + *transferCountedItems_;
                }
                if (seen_.size() + treeItems_ > request_.maxEntries_) {
                    if (transfer_) { transfer_->Release(); transfer_.reset(); }
                    Step = failed({ErrorCategory::BudgetExceeded, "operation-entry-budget"});
                }
                Step.items_ = std::move(Outcomes);
                return Step;
            }
            void finishTransfer(TreeTransferBatch& Batch)
            {
                transfer_->Release(); transfer_.reset();
                transferCurrent_.clear(); transferSource_.clear(); transferTarget_.clear();
                const auto& Root = transferRoot_.source_.entry_.locator_;
                for (auto& Kept : Batch.remaining_) {
                    // Moved folders kept because an entry below them remains. The
                    // final reverse pass removes them after those entries.
                    auto Item = transferRoot_;
                    if (Kept.entry_.locator_ != Root) {
                        Item = PlannedItem{{std::move(Kept.entry_), transferRoot_.source_.reader_, transferRoot_.source_.writer_},
                            std::move(Kept.destination_), transferRoot_.depth_ + Kept.depth_};
                        Item.root_ = transferRoot_.root_; ++verifiedItems_;
                    }
                    Item.removeDirectory_ = true; Item.readOnly_ = (Item.source_.entry_.attributes_ & 1) != 0;
                    directories_.push_back(std::move(Item));
                }
                for (auto It = Batch.deferred_.rbegin(); It != Batch.deferred_.rend(); ++It) {
                    // Decisions, links and errors take the verified per-item path.
                    PlannedItem Item{{std::move(It->entry_), transferRoot_.source_.reader_, transferRoot_.source_.writer_},
                        std::move(It->destination_), transferRoot_.depth_ + It->depth_};
                    Item.root_ = transferRoot_.root_;
                    if (Item.source_.entry_.locator_ == Root) {
                        // The engine could not list the root: plan it again.
                        Item = transferRoot_;
                        seen_.erase(Serialize(Root)); --verifiedItems_;
                    }
                    Item.classicTraversal_ = Item.source_.entry_.directory_;
                    pending_.push_front(std::move(Item));
                }
            }
            JobStep readOnlyQuestion() {
                releasePreparedMutations(); JobStep Step{JobState::WaitingForUser}; Step.interaction_ = InteractionRequest{"", InteractionKind::ReadOnlyRemoval, "read-only-removal",
                    {InteractionChoice::Confirm, InteractionChoice::ConfirmAll, InteractionChoice::Skip, InteractionChoice::Cancel}, current().source_.entry_.id_,
                    Serialize(current().source_.entry_.locator_)}; return Step;
            }
            JobStep begin(OperationContext Context)
            {
                if (index_ == items_.size()) return JobStep{JobState::Completed};
                if (current().preparationError_ && (request_.operation_ != FileOperation::Delete || !request_.elevation_))
                    return terminalFailure(*current().preparationError_);
                if (current().excluded_) return done(ItemOutcome::Status::Skipped);
                if (current().removalError_) {
                    auto Error = std::move(*current().removalError_); current().removalError_.reset();
                    phase_ = Phase::Remove; return failed(std::move(Error));
                }
                expected_ = {};
                destinationCommitted_ = request_.operation_ == FileOperation::Move && current().removeDirectory_;
                const bool Removing = request_.operation_ == FileOperation::Delete || current().removeDirectory_;
                const bool Renaming = request_.operation_ == FileOperation::Rename || current().renameMove_;
                auto Writer = Removing || Renaming ? current().source_.writer_ : request_.destinationWriter_;
                if (Removing && request_.operation_ == FileOperation::Delete) {
                    for (const auto& At : skippedSources_) if (SameOrWithin(At, current().source_.entry_.locator_)) return done(ItemOutcome::Status::Skipped);
                }
                if (current().removeDirectory_ && request_.operation_ == FileOperation::Move)
                    for (const auto& Skipped : skippedSources_)
                        if (SameOrWithin(Skipped, current().source_.entry_.locator_)) return done(ItemOutcome::Status::Skipped, true);
                if (Removing || (request_.operation_ == FileOperation::Move && (!current().source_.entry_.directory_ || current().renameMove_))) {
                    if (current().readOnly_ && request_.readOnly_ != ReadOnlyPolicy::Allow && !allowAllReadOnly_ && !readOnlyAnswered_) {
                        if (request_.readOnly_ == ReadOnlyPolicy::Skip) return done(ItemOutcome::Status::Skipped, destinationCommitted_);
                        phase_ = Phase::ReadOnly; return readOnlyQuestion();
                    }
                }
                if (current().preparationError_) {
                    if (current().recursiveRemoval_) return failed(*current().preparationError_);
                    // Retry queries again instead of replaying a cached error.
                    auto Resource = current().source_.entry_.id_;
                    if (!Resource.revision_) {
                        const auto Listing = current().source_.entry_.hints_.find("nativeListingRevision");
                        if (Listing != current().source_.entry_.hints_.end()) Resource.revision_ = ResourceRevision{Listing->second};
                    }
                    auto Fresh = current().source_.reader_->Stat(Resource, Context).get();
                    if (!Fresh.HasValue()) return failed(Fresh.Failure());
                    if (Fresh.Value().locator_ != current().source_.entry_.locator_ || Fresh.Value().id_.providerId_ != current().source_.reader_->Id()
                        || Fresh.Value().id_.key_ != current().source_.entry_.id_.key_ || !SafeName(Fresh.Value().name_))
                        return failed({ErrorCategory::Internal, "provider-returned-different-entry"});
                    if ((Fresh.Value().attributes_ & 0x400) && request_.operation_ != FileOperation::Delete)
                        return failed({ErrorCategory::Unsupported, "reparse-entry-denied"});
                    current().source_.entry_ = std::move(Fresh.Value()); current().readOnly_ = (current().source_.entry_.attributes_ & 1) != 0;
                    current().preparationError_.reset(); ++verifiedItems_;
                    return progress();
                }
                if ((request_.operation_ == FileOperation::Copy || request_.operation_ == FileOperation::Move)
                    && !Removing && !current().source_.entry_.directory_ && !copyOptionsResolved_) {
                    effectiveCopyOptions_ = request_.copyOptions_;
                    if (effectiveCopyOptions_.acl_ || effectiveCopyOptions_.streams_) {
                        auto Input = current().source_.reader_->CopyExtrasAt(current().source_.entry_.locator_, Context).get();
                        if (!Input.HasValue()) return failed(Input.Failure());
                        auto Output = request_.destinationWriter_->CopySupportAt(current().destination_, Context);
                        if (!Output.HasValue()) return failed(Output.Failure());
                        effectiveCopyOptions_.acl_ = effectiveCopyOptions_.acl_ && Input.Value().acl_ && Output.Value().acl_;
                        effectiveCopyOptions_.streams_ = effectiveCopyOptions_.streams_ && Input.Value().streams_ && Output.Value().streams_;
                    }
                    copyOptionsResolved_ = true;
                }
                const auto Kind = Removing ? request_.operation_ == FileOperation::Delete && request_.deleteMode_ == DeleteMode::Recycle ? Mutation::Recycle : Mutation::Remove : Renaming ? Mutation::Rename
                    : request_.operation_ == FileOperation::MakeDirectory || current().source_.entry_.directory_ ? Mutation::MakeDirectory : Mutation::CreateContent;
                const auto Supported = Writer->Supports(Kind, Removing ? current().source_.entry_.locator_ : current().destination_, Context);
                if (!Supported.HasValue()) return failed(Supported.Failure());
                if (Supported.Value().availability_ == Availability::Unsupported)
                    return failed({ErrorCategory::Unsupported, Supported.Value().reason_, Writer->Id()});
                if (request_.operation_ == FileOperation::MakeDirectory) {
                    const auto Existing = Writer->Inspect(current().destination_, Context); if (!Existing.HasValue()) return failed(Existing.Failure());
                    if (Existing.Value()) {
                        if (request_.fileLockInteractions_) {
                            phase_ = Phase::Conflict; replaceAllowed_ = false;
                            releasePreparedMutations(); auto Step = progress(); Step.next_ = JobState::WaitingForUser;
                            Step.interaction_ = InteractionRequest{"", InteractionKind::Rename, "destination-exists",
                                {InteractionChoice::Rename, InteractionChoice::Cancel}, Existing.Value()->id_, {}, Serialize(current().destination_)};
                            return Step;
                        }
                        if (!Existing.Value()->directory_) return failed({ErrorCategory::InvalidRequest, "destination-is-not-directory", Writer->Id()}); return done(ItemOutcome::Status::Completed, true);
                    }
                    phase_ = Phase::MakeDirectory;
                }
                else if (request_.operation_ == FileOperation::Delete || current().removeDirectory_) {
                    phase_ = deleteWorkers_ > 1 && deleteCandidate(index_) && deleteCandidate(index_ + 1)
                        ? Phase::RemoveBatch : Phase::Remove;
                }
                else if (Renaming) {
                    if (current().renameMove_) {
                        const auto Existing = Writer->Inspect(current().destination_, Context);
                        if (!Existing.HasValue()) return failed(Existing.Failure());
                        if (Existing.Value()) return failed({ErrorCategory::StaleRevision, "move-destination-appeared"});
                    }
                    if (request_.fileLockInteractions_) {
                        const auto Existing = Writer->Inspect(current().destination_, Context); if (!Existing.HasValue()) return failed(Existing.Failure());
                        if (Existing.Value() && Existing.Value()->id_ != current().source_.entry_.id_) {
                            phase_ = Phase::Conflict; replaceAllowed_ = false;
                            releasePreparedMutations(); auto Step = progress(); Step.next_ = JobState::WaitingForUser;
                            Step.interaction_ = InteractionRequest{"", InteractionKind::Rename, "destination-exists",
                                {InteractionChoice::Rename, InteractionChoice::Cancel}, Existing.Value()->id_, Serialize(current().source_.entry_.locator_), Serialize(current().destination_)};
                            return Step;
                        }
                    }
                    phase_ = Phase::Rename;
                }
                else
                {
                    const auto Existing = request_.destinationWriter_->Inspect(current().destination_, Context);
                    if (!Existing.HasValue()) return failed(Existing.Failure());
                    if (request_.approvedDestination_) {
                        const auto& Approved = *request_.approvedDestination_;
                        if (Approved.exists_ != Existing.Value().has_value()
                            || Approved.revision_ != (Existing.Value() ? Existing.Value()->id_.revision_ : std::nullopt))
                            return failed({ErrorCategory::StaleRevision, "elevation-destination-changed", request_.destinationWriter_->Id()});
                    }
                    if (Existing.Value())
                    {
                        if (Existing.Value()->directory_ && current().source_.entry_.directory_) return done(ItemOutcome::Status::Completed, true);
                        expected_ = {true, Existing.Value()->id_.revision_};
                        const auto Policy = request_.conflictDecision_ ? request_.conflictDecision_->load() : request_.conflict_;
                        if (Policy == ConflictPolicy::Skip && !current().source_.entry_.directory_) return done(ItemOutcome::Status::Skipped);
                        if (Policy != ConflictPolicy::Replace || Existing.Value()->directory_ || current().source_.entry_.directory_)
                        {
                            replaceAllowed_ = !Existing.Value()->directory_ && !current().source_.entry_.directory_;
                            phase_ = Phase::Conflict;
                            releasePreparedMutations(); JobStep Step{JobState::WaitingForUser};
                            Step.interaction_ = InteractionRequest{"", InteractionKind::Overwrite, "destination-exists",
                                {InteractionChoice::Rename, InteractionChoice::Cancel}, Existing.Value()->id_,
                                Serialize(current().source_.entry_.locator_), Serialize(current().destination_)};
                            if (!current().source_.entry_.directory_) {
                                Step.interaction_->choices_.push_back(InteractionChoice::Skip);
                                Step.interaction_->choices_.push_back(InteractionChoice::SkipAll);
                            }
                            if (replaceAllowed_) {
                                Step.interaction_->choices_.push_back(InteractionChoice::Replace);
                                Step.interaction_->choices_.push_back(InteractionChoice::ReplaceAll);
                                if (request_.operation_ == FileOperation::Copy) {
                                    Step.interaction_->choices_.push_back(InteractionChoice::RenameIncoming);
                                    const auto RenameSupport = request_.destinationWriter_->Supports(Mutation::Rename, current().destination_, Context);
                                    if (RenameSupport.HasValue() && RenameSupport.Value().availability_ != Availability::Unsupported
                                        && Existing.Value()->id_ != current().source_.entry_.id_) {
                                        conflictEntry_ = *Existing.Value();
                                        Step.interaction_->choices_.push_back(InteractionChoice::RenameExisting);
                                    }
                                }
                            }
                            return Step;
                        }
                    }
                    phase_ = current().source_.entry_.directory_ ? Phase::MakeDirectory : Phase::OpenRead;
                }
                auto Step = progress();
                if (phase_ != Phase::OpenRead) Step.next_ = JobState::Committing;
                return Step;
            }
            JobStep conflict(const JobStepContext& Context)
            {
                if (!Context.answer_) return failed({ErrorCategory::Internal, "conflict-answer-required"});
                const auto Choice = Context.answer_->choice_;
                if (Choice == InteractionChoice::Skip || Choice == InteractionChoice::SkipAll) {
                    if (Choice == InteractionChoice::SkipAll && request_.conflictDecision_) request_.conflictDecision_->store(ConflictPolicy::Skip);
                    return done(ItemOutcome::Status::Skipped);
                }
                if (Choice == InteractionChoice::RenameIncoming || Choice == InteractionChoice::RenameExisting) {
                    if (request_.operation_ != FileOperation::Copy || !replaceAllowed_
                        || (Choice == InteractionChoice::RenameExisting && !conflictEntry_))
                        return failed({ErrorCategory::InvalidRequest, "invalid-automatic-rename"});
                    automaticExisting_ = Choice == InteractionChoice::RenameExisting;
                    automaticIndex_ = 2; automaticOriginal_ = current().destination_;
                    phase_ = Phase::AutomaticName; return progress();
                }
                if (Context.answer_->choice_ == InteractionChoice::Rename)
                {
                    if (!SafeName(Context.answer_->value_)) return failed({ErrorCategory::InvalidRequest, "unsafe-destination-name"});
                    const auto Old = current().destination_;
                    const auto New = Child(Parent(Old), Context.answer_->value_);
                    if (current().source_.entry_.directory_)
                    {
                        for (auto& Item : items_)
                        {
                            auto At = Item.destination_, Prefix = Old, Target = New;
                            const auto OldPath = EntryPath(Prefix);
                            auto& Path = EntryPath(At);
                            if (At.kind_ == Old.kind_ && At.root_ == Old.root_ && At.nesting_ == Old.nesting_) Item.destination_ = New;
                            else if (SameOrWithin(At, Old)) { Path = EntryPath(Target) + Path.substr(OldPath.size()); Item.destination_ = std::move(At); }
                        }
                    }
                    current().destination_ = New; phase_ = Phase::Begin; return progress();
                }
                if ((Choice != InteractionChoice::Replace && Choice != InteractionChoice::ReplaceAll) || !replaceAllowed_) return failed({ErrorCategory::InvalidRequest, "invalid-conflict-choice"});
                if (Choice == InteractionChoice::ReplaceAll && request_.conflictDecision_) request_.conflictDecision_->store(ConflictPolicy::Replace);
                phase_ = Phase::OpenRead;
                return progress();
            }
            JobStep automaticName(OperationContext Context)
            {
                // One provider lookup per advance keeps large collision sets cancellable.
                if (automaticIndex_ > 100000) return failed({ErrorCategory::BudgetExceeded, "automatic-name-budget"});
                auto Original = automaticOriginal_;
                const auto Path = EntryPath(Original);
                const auto Name = Path.substr(Path.find_last_of("/\\") + 1);
                const auto Dot = Name.find_last_of('.');
                const auto Split = Dot == std::string::npos || Dot == 0 ? Name.size() : Dot;
                const auto Candidate = Name.substr(0, Split) + " (" + std::to_string(automaticIndex_++) + ")" + Name.substr(Split);
                automaticTarget_ = Child(Parent(Original), Candidate);
                const auto Existing = request_.destinationWriter_->Inspect(automaticTarget_, Context);
                if (!Existing.HasValue()) return failed(Existing.Failure());
                if (Existing.Value()) return progress();
                if (!automaticExisting_) {
                    current().destination_ = automaticTarget_; expected_ = {};
                    phase_ = Phase::Begin; return progress();
                }
                phase_ = Phase::PreserveDestination;
                auto Step = progress(); Step.next_ = JobState::Committing; return Step;
            }
            JobStep commit(OperationContext Context)
            {
                if (phase_ == Phase::PreserveDestination) {
                    // Rename must enforce both immutable source identity and no replacement.
                    const auto Preserved = request_.destinationWriter_->Rename(*conflictEntry_, automaticTarget_, Context);
                    if (!Preserved.HasValue()) return failed(Preserved.Failure());
                    preservedDestinations_.push_back(automaticTarget_);
                    conflictEntry_.reset(); expected_ = {}; phase_ = Phase::Begin;
                    return progress();
                }
                if (phase_ == Phase::MakeDirectory)
                {
                    const auto Made = request_.destinationWriter_->MakeDirectory(current().destination_, Context);
                    if (!Made.HasValue()) return failed(Made.Failure());
                    return done(ItemOutcome::Status::Completed, true);
                }
                if (phase_ == Phase::Rename)
                {
                    const auto Renamed = current().source_.writer_->Rename(current().source_.entry_, current().destination_, Context);
                    if (!Renamed.HasValue()) return failed(Renamed.Failure());
                    if (current().renameMove_ && !current().source_.entry_.directory_) { bytesDone_ += current().source_.entry_.size_; itemBytes_ = current().source_.entry_.size_; }
                    return done(ItemOutcome::Status::Completed, true, true);
                }
                if (phase_ == Phase::RemoveBatch) return removeBatch(Context);
                if (phase_ == Phase::Remove) return removeSource(Context);
                auto Receipt = session_->Commit(Context);
                if (!Receipt.HasValue()) return failed(Receipt.Failure());
                destinationCommitted_ = true;
                if (request_.operation_ == FileOperation::Move && (request_.elevation_ || request_.fileLockInteractions_)) {
                    auto Published = request_.destinationWriter_->Inspect(current().destination_, Context);
                    if (Published.HasValue() && Published.Value()) publishedDestination_ = *Published.Value();
                }
                cleanup_.insert(cleanup_.end(), Receipt.Value().cleanupErrors_.begin(), Receipt.Value().cleanupErrors_.end());
                if (!Receipt.Value().cleanupErrors_.empty()) return failed(Receipt.Value().cleanupErrors_.front());
                if (request_.operation_ == FileOperation::Move)
                {
                    // Destroy every copy-session handle before deleting its source.
                    // A cleanup retry must never repeat a committed copy.
                    session_.reset(); phase_ = Phase::Remove;
                    return removeSource(Context);
                }
                return done(ItemOutcome::Status::Completed, true, request_.operation_ == FileOperation::Move);
            }
            bool deleteCandidate(std::size_t Index) const
            {
                if (request_.operation_ != FileOperation::Delete || request_.deleteMode_ != DeleteMode::Permanent
                    || Index >= items_.size()) return false;
                const auto& Item = items_[Index];
                return !Item.removalError_ && !Item.preparationError_ && !Item.excluded_
                    && !Item.readOnly_ && !Item.source_.entry_.directory_ && !Item.removeDirectory_
                    && !(Item.source_.entry_.attributes_ & 0x400) && Item.source_.entry_.id_.revision_
                    && Item.source_.entry_.locator_.kind_ == LocatorKind::Local && Item.source_.writer_
                    && Item.source_.writer_ == items_[index_].source_.writer_ && Item.source_.writer_->SupportsParallelRemovals();
            }
            JobStep removeBatch(OperationContext Context)
            {
                // One bounded commit collects <=64 ordinary local files. Workers
                // check the real cancellation token before every individual remove.
                // Directory/recycle/readonly and unknown-provider paths stay serial.
                std::array<std::future<JobStep>, 8> Work;
                std::array<JobStep, 64> Results;
                std::array<PlannedItem, 64> Ordered;
                std::size_t Count = 0, Started = 0;
                while (Count < std::size_t{64} && deleteCandidate(index_ + Count)) ++Count;
                std::vector<ItemOutcome> Outcomes; Outcomes.reserve(Count);
                for (std::size_t I = 0; I < Count; ++I)
                    Outcomes.push_back({Serialize(items_[index_ + I].source_.entry_.locator_), ItemOutcome::Status::Completed, {}, false, true, items_[index_ + I].source_.entry_});
                std::vector<ItemOutcome> Completed; Completed.reserve(Count);
                // Same-directory removals serialize in a local filesystem; four
                // workers splitting one folder measured slower than one worker.
                // Such writers get whole directories per worker instead.
                std::array<std::size_t, 65> Groups{};
                std::size_t GroupCount = 0;
                const bool ByDirectory = items_[index_].source_.writer_->SerializesDirectoryRemovals();
                const auto Folder = [&](std::size_t I) {
                    const std::string_view Path = items_[index_ + I].source_.entry_.locator_.root_;
                    return Path.substr(0, Path.find_last_of("/\\") == std::string_view::npos ? 0 : Path.find_last_of("/\\"));
                };
                for (std::size_t I = 0; I < Count; ++I)
                    if (!ByDirectory || !I || Folder(I) != Folder(I - 1)) Groups[GroupCount++] = I;
                Groups[GroupCount] = Count;
                std::atomic<std::size_t> Next{0};
                Context.stopToken_ = mutationStop_;
                Context.allowReadOnlyRemoval_ = request_.readOnly_ == ReadOnlyPolicy::Allow || allowAllReadOnly_;
                try {
                    for (; Started < std::min(deleteWorkers_, GroupCount); ++Started) {
                        Work[Started] = CopyWorkers().Submit([this, &Results, &Next, &Groups, GroupCount, Context] {
                            for (;;) {
                                const auto Group = Next.fetch_add(1); if (Group >= GroupCount) break;
                                for (auto I = Groups[Group]; I < Groups[Group + 1]; ++I) {
                                    const auto& Item = items_[index_ + I];
                                    try {
                                        auto Removed = Item.source_.writer_->Remove(Item.source_.entry_, Context);
                                        Results[I].next_ = Removed.HasValue() ? JobState::Completed : JobState::Failed;
                                        if (!Removed.HasValue()) Results[I].error_ = Removed.Failure();
                                    } catch (...) {
                                        Results[I].next_ = JobState::Failed; Results[I].error_ = Error{ErrorCategory::Internal, "delete-worker-exception"};
                                    }
                                }
                            }
                            return JobStep{JobState::Completed};
                        });
                    }
                } catch (...) {
                    // Submitted workers still finish the bounded queue and must
                    // be joined before any item or provider state can be released.
                }
                if (!Started) { phase_ = Phase::Remove; return removeSource(Context); }
                for (std::size_t I = 0; I < Started; ++I) Work[I].get();
                // Emit successes first. An error prompt, cancellation or terminal
                // failure must never hide successful deletions from the ledger.
                std::size_t Position = 0, Succeeded = 0;
                for (const bool Success : {true, false}) for (std::size_t I = 0; I < Count; ++I) {
                    if ((Results[I].next_ == JobState::Completed) != Success) continue;
                    if (Success) { Completed.push_back(std::move(Outcomes[I])); ++Succeeded; }
                    auto Item = std::move(items_[index_ + I]);
                    if (!Success) Item.removalError_ = std::move(Results[I].error_);
                    Ordered[Position++] = std::move(Item);
                }
                for (std::size_t I = 0; I < Count; ++I) items_[index_ + I] = std::move(Ordered[I]);
                index_ += Succeeded; outcomeIndex_ += Succeeded; deletedFiles_ += Succeeded;
                readOnlyAnswered_ = false; sourceRemoveRetries_ = 0;
                phase_ = index_ == items_.size() && streaming_ && !planningFinished_ ? Phase::Preparing : Phase::Begin;
                auto Step = progress(); Step.items_ = std::move(Completed);
                if (index_ == items_.size() && planningFinished_) Step.next_ = JobState::Completed;
                trimProcessed();
                return Step;
            }
            JobStep removeSource(OperationContext Context)
            {
                Context.allowReadOnlyRemoval_ = request_.readOnly_ == ReadOnlyPolicy::Allow || allowAllReadOnly_ || readOnlyAnswered_;
                if (request_.operation_ == FileOperation::Delete && request_.deleteMode_ == DeleteMode::Recycle) {
                    const auto Recycled = current().source_.writer_->Recycle(current().source_.entry_, Context);
                    if (!Recycled.HasValue()) return failed(Recycled.Failure());
                    auto Step = done(ItemOutcome::Status::Completed, true, true);
                    Step.item_->item_ += "\nrecycle: " + Serialize(Recycled.Value().destination_); return Step;
                }
                // Reuse the planned identity on every attempt. Native Remove
                // verifies it and refuses directories that acquired new children.
                const auto Removed = current().source_.writer_->Remove(current().source_.entry_, Context);
                if (!Removed.HasValue()) {
                    releasePreparedMutations();
                    const auto& Error = Removed.Failure();
                    const auto Kind = current().source_.entry_.locator_.kind_;
                    if (request_.operation_ == FileOperation::Move && (Kind == LocatorKind::Local || Kind == LocatorKind::Smb)
                        && Error.retryable_ && FileInUse(Error.code_) && sourceRemoveRetries_ < 5) {
                        phase_ = Phase::RetryRemove;
                        auto Step = progress();
                        Step.deferFor_ = std::chrono::milliseconds(50u << sourceRemoveRetries_++);
                        // Retain the committed destination even if cancellation
                        // ends the job before its next task continuation.
                        if (!partialOutcome_) {
                            Step.item_ = ItemOutcome{itemLocator(), ItemOutcome::Status::Failed, Error, destinationCommitted_, false, current().source_.entry_};
                            partialOutcome_ = outcomeIndex_++;
                        }
                        return Step;
                    }
                    return failed(Error);
                }
                return done(ItemOutcome::Status::Completed, request_.operation_ == FileOperation::Move, true);
            }
            FileOperationRequest request_;
            std::shared_ptr<MutationScope> mutationScope_;
            bool ownsMutationScope_ = true;
            std::stop_token mutationStop_;
            std::size_t deleteWorkers_ = 0;
            // Active bulk removal of treeRoot_; its outcomes are not in items_.
            std::unique_ptr<ITreeRemoval> tree_;
            PlannedItem treeRoot_;
            std::uint64_t treeItems_ = 0, trimmedItems_ = 0;
            std::size_t movePlannedSize_ = std::numeric_limits<std::size_t>::max();
            std::uint64_t movePlanned_ = 0;
            std::string treeCurrent_, treeSource_;
            // Active bulk copy/move of transferRoot_; its outcomes are not in items_.
            std::unique_ptr<ITreeTransfer> transfer_;
            PlannedItem transferRoot_;
            TreeTransferMode transferMode_ = TreeTransferMode::Copy;
            std::uint64_t transferAccepted_ = 0, transferBytes_ = 0, transferItemsBase_ = 0;
            std::optional<std::uint64_t> transferCountedBytes_, transferCountedItems_;
            std::string transferCurrent_, transferSource_, transferTarget_;
            std::deque<ItemOutcome> transferLeftover_;
            std::deque<std::unique_ptr<PreparedCopy>> copies_;
            std::stop_source copyStop_;
            std::size_t copyWorkers_ = 0;
            Phase phase_ = Phase::Preparing;
            Phase resumePhase_ = Phase::Remove;
            Error lockError_;
            PlannedItem preparationItem_;
            bool preparationRetry_ = false, skipDeleteErrors_ = false;
            std::deque<PlannedItem> pending_;
            std::vector<PlannedItem> items_, directories_;
            std::vector<std::shared_ptr<IStorageProvider>> batchReaders_;
            std::set<std::string> seen_;
            PlannedItem enumerating_;
            std::unique_ptr<IEntryCursor> cursor_;
            std::vector<EnumerationFrame> enumerations_;
            std::vector<std::uint64_t> rootFiles_;
            std::vector<std::optional<bool>> renameRoots_;
            std::shared_ptr<IReadStream> stream_;
            Task<ReadChunk> readAhead_;
            bool readAheadSlot_ = false;
            std::stop_source transferStop_{std::nostopstate};
            std::unique_ptr<std::stop_callback<std::function<void()>>> transferCancellation_;
            std::optional<ContentProducer> producer_;
            Task<Unit> production_;
            std::shared_ptr<std::atomic<std::uint64_t>> productionProgress_;
            std::set<const IStorageProvider*> producedReaders_;
            std::unique_ptr<IWriteSession> session_;
            DestinationExpectation expected_;
            ReadChunk chunk_;
            std::vector<Error> cleanup_;
            std::vector<Locator> skippedSources_, readOnlySources_;
            bool allowAllReadOnly_ = false, readOnlyAnswered_ = false;
            bool streaming_ = false, planningFinished_ = false;
            bool incrementalDelete_ = false, deleteConfirmed_ = false;
            std::uint64_t verifiedItems_ = 0, discoveredFiles_ = 0, deletedFiles_ = 0;
            std::size_t index_ = 0;
            std::uint32_t sourceRemoveRetries_ = 0;
            std::uint64_t bytesTotal_ = 0, bytesDone_ = 0, itemBytes_ = 0, extraBytes_ = 0;
            std::optional<std::uint64_t> listedBytes_;
            CopyOptions effectiveCopyOptions_;
            bool copyOptionsResolved_ = false;
            bool replaceAllowed_ = false, destinationCommitted_ = false;
            std::optional<EntrySnapshot> conflictEntry_;
            Locator automaticOriginal_, automaticTarget_;
            std::vector<Locator> preservedDestinations_;
            unsigned automaticIndex_ = 2;
            bool automaticExisting_ = false, preservedFailure_ = false;
            std::string jobId_;
            FileElevationRequest elevatedRequest_;
            std::unique_ptr<IFileElevationSession> elevatedSession_;
            std::optional<Error> permissionError_;
            std::optional<EntrySnapshot> publishedDestination_;
            std::optional<std::size_t> partialOutcome_;
            std::size_t outcomeIndex_ = 0;
            std::uint64_t elevatedBase_ = 0;
            bool elevationCommitSent_ = false, canElevate_=false;
        };
    }
    namespace {
    // A selected directory snapshot is a lease. Copy/move coordinate independent
    // roots; deletion validates all selected roots before its first mutation.
    class SelectionTask final : public IJobTask {
    public:
        explicit SelectionTask(FileOperationRequest Request) : request_(std::move(Request)) {}
        JobStep Advance(JobStepContext Context) override
        {
            if (const auto Error = Context.operation_.Check({})) return failed(*Error);
            if (!confirmed_ && request_.operation_ == FileOperation::Delete && request_.confirmDelete_) {
                if (!Context.answer_) {
                    JobStep Step{JobState::WaitingForUser};
                    Step.interaction_ = InteractionRequest{"", InteractionKind::DeleteConfirmation, request_.deleteMode_ == DeleteMode::Recycle ? "confirm-recycle-delete" : "confirm-permanent-delete",
                        {InteractionChoice::Confirm, InteractionChoice::Cancel}}; return Step;
                }
                if (Context.answer_->choice_ != InteractionChoice::Confirm) return failed({ErrorCategory::InvalidRequest, "delete-confirmation-required"});
                confirmed_ = true; Context.answer_.reset();
            }
            const bool Unified = request_.operation_ == FileOperation::Delete
                || (request_.calculateSizeBefore_ && (request_.operation_ == FileOperation::Move
                    || (request_.operation_ == FileOperation::Copy && selectionSummarized_ && !listedBytes_)));
            if (Unified && !unifiedPrepared_) {
                // Validate the immutable selected roots before any mutation.
                // Descendant discovery stays incremental inside one FileTask,
                // sharing overlap checks, budgets and readonly decisions. For transfers,
                // the complete folder selection must be counted before copying.
                const auto Reader = request_.selection_->Provider();
                auto Page = request_.selection_->Read(offset_, 256, Context.operation_);
                if (!Page.HasValue()) return failed(Page.Failure());
                if (Page.Value().entries_.empty() || Page.Value().entries_.size() > request_.selection_->Count() - offset_)
                    return failed({ErrorCategory::Internal, "selection-no-progress"});
                for (const auto& Entry : Page.Value().entries_) {
                    if (Entry.id_.providerId_ != Reader->Id() || !SafeName(Entry.name_) || Child(request_.selectionRoot_, Entry.name_) != Entry.locator_)
                        return failed({ErrorCategory::InvalidRequest, "selection-container-mismatch"});
                    selectedSources_.push_back({Entry, Reader, request_.selectionWriter_});
                }
                offset_ += Page.Value().entries_.size();
                if (offset_ == request_.selection_->Count()) {
                    auto Request = request_; Request.selection_.reset(); Request.confirmDelete_ = false; Request.sources_ = std::move(selectedSources_);
                    auto Prepared = PrepareFileOperation(std::move(Request), Context.operation_);
                    if (!Prepared.HasValue()) return failed(Prepared.Failure());
                    inner_ = std::move(Prepared.Value().task_); unifiedPrepared_ = true;
                }
                JobStep Step{JobState::Preparing}; Step.progress_ = JobProgress{};
                if (request_.operation_ == FileOperation::Delete) Step.progress_->deletedFiles_ = 0;
                return Step;
            }
            if (Unified && inner_) {
                auto Step = inner_->Advance(Context);
                if (Step.next_ == JobState::Failed && Step.error_ && Step.error_->category_ == ErrorCategory::Cancelled) Step.next_ = JobState::Cancelled;
                return Step;
            }
            if (request_.operation_ != FileOperation::Delete && !selectionSummarized_) {
                // Read only bounded pages of the immutable selection metadata.
                // No network Stat or recursive directory walk is needed for a
                // flat file selection. Folders require one recursive plan when
                // preflight is enabled; flat files retain their paged copy path.
                auto Page = request_.selection_->Read(summaryOffset_, 256, Context.operation_);
                if (!Page.HasValue()) return failed(Page.Failure());
                if (Page.Value().entries_.empty() || Page.Value().entries_.size() > 256
                    || Page.Value().entries_.size() > request_.selection_->Count() - summaryOffset_)
                    return failed({ErrorCategory::Internal, "selection-no-progress"});
                for (const auto& Entry : Page.Value().entries_) {
                    if (Entry.id_.providerId_ != request_.selection_->Provider()->Id() || !SafeName(Entry.name_)
                        || Child(request_.selectionRoot_, Entry.name_) != Entry.locator_)
                        return failed({ErrorCategory::InvalidRequest, "selection-container-mismatch"});
                    AddListedFileBytes(listedBytes_, Entry, request_.includePattern_);
                }
                summaryOffset_ += Page.Value().entries_.size();
                selectionSummarized_ = !listedBytes_ || summaryOffset_ == request_.selection_->Count();
                JobStep Step{JobState::Preparing}; Step.progress_ = JobProgress{}; Step.progress_->phase_ = JobPhase::Preparing;
                if (selectionSummarized_ && listedBytes_) {
                    Step.progress_->bytesTotal_ = listedBytes_; Step.progress_->plannedItems_ = request_.selection_->Count();
                }
                return Step;
            }
            if (inner_) {
                if (Context.answer_ && Context.answer_->choice_ == InteractionChoice::ConfirmAll) request_.readOnly_ = ReadOnlyPolicy::Allow;
                auto Step = inner_->Advance(Context);
                if (Step.item_ && !Step.replaceItem_) ++emitted_;
                emitted_ += Step.items_.size();
                if (Step.replaceItem_) *Step.replaceItem_ += outcomeBase_;
                if (Step.progress_) {
                    innerBytes_ = Step.progress_->bytesDone_;
                    if (listedBytes_ && innerListedBytes_ && Step.progress_->bytesTotal_ && *Step.progress_->bytesTotal_ >= *innerListedBytes_)
                        innerExtraBytes_ = *Step.progress_->bytesTotal_ - *innerListedBytes_;
                    Step.progress_->bytesDone_ += completedBytes_; Step.progress_->bytesTotal_.reset();
                    // Delete uses one globally validated selection plan. Other
                    // operations still coordinate independent root batches.
                    if (request_.operation_ != FileOperation::Delete) Step.progress_->plannedItems_.reset();
                    if (request_.operation_ != FileOperation::Delete && listedBytes_) {
                        // Child totals describe one root/page. Keep the full
                        // selection total and accumulate copied extra streams.
                        Step.progress_->bytesTotal_ = *listedBytes_ + completedExtraBytes_ + innerExtraBytes_;
                        Step.progress_->plannedItems_ = request_.selection_->Count();
                    }
                }
                if (Step.next_ == JobState::Failed && Step.error_ && Step.error_->category_ == ErrorCategory::Cancelled) {
                    Step.next_ = JobState::Cancelled; return Step;
                }
                if (request_.operation_ == FileOperation::Delete) return Step;
                if (Step.next_ == JobState::Completed || Step.next_ == JobState::Failed) {
                    auto Errors = inner_->Finalize(Step.next_); cleanup_.insert(cleanup_.end(), Errors.begin(), Errors.end()); inner_.reset();
                    if (Step.next_ == JobState::Failed && !Step.item_) {
                        Step.item_ = ItemOutcome{Serialize(current_.locator_), ItemOutcome::Status::Failed, Step.error_, false, false, current_}; ++emitted_;
                    }
                    Step.next_ = JobState::Running; Context.answer_.reset();
                    completedBytes_ += innerBytes_; innerBytes_ = 0;
                    completedExtraBytes_ += innerExtraBytes_; innerExtraBytes_ = 0;
                }
                return Step;
            }
            if (offset_ == request_.selection_->Count()) return JobStep{JobState::Completed};
            const auto SelectedReader = request_.selection_->Provider();
            const bool CopyPage = request_.operation_ == FileOperation::Copy && Context.operation_.fileCopyWorkers_ != 1
                && request_.selectionRoot_.kind_ == LocatorKind::Local && request_.destination_.kind_ == LocatorKind::Local
                && request_.maxBytes_ == std::numeric_limits<std::uint64_t>::max() && !request_.bytesPerSecondLimit_
                && !request_.copyOptions_.streams_ && !Context.operation_.writeCheckpoint_ && !Context.operation_.workload_.IsAutomatic()
                && SelectedReader->SupportsParallelReads() && request_.destinationWriter_->SupportsParallelCopies();
            if (CopyPage && request_.selection_->Count() - offset_ >= 2) {
                auto Page = request_.selection_->Read(offset_, 64, Context.operation_);
                if (!Page.HasValue()) return failed(Page.Failure());
                if (Page.Value().entries_.empty() || Page.Value().entries_.size() > 64
                    || Page.Value().entries_.size() > request_.selection_->Count() - offset_) return failed({ErrorCategory::Internal, "selection-no-progress"});
                std::set<std::string> Names;
                bool Regular = true;
                for (const auto& Entry : Page.Value().entries_) {
                    if (Entry.id_.providerId_ != SelectedReader->Id() || !SafeName(Entry.name_) || Child(request_.selectionRoot_, Entry.name_) != Entry.locator_)
                        return failed({ErrorCategory::InvalidRequest, "selection-container-mismatch"});
                    Regular &= !Entry.directory_ && Entry.size_ <= 1024 * 1024 && !(Entry.attributes_ & (0x400 | 0x200 | 0x800 | 0x4000))
                        && Names.insert(NativeKey(Entry.locator_.root_)).second;
                }
                if (Regular) {
                    auto Request = request_; Request.selection_.reset(); Request.sources_.clear(); Request.continueSelectedFiles_ = true;
                    for (const auto& Entry : Page.Value().entries_) Request.sources_.push_back({Entry, SelectedReader, request_.selectionWriter_});
                    innerListedBytes_ = 0;
                    for (const auto& Entry : Page.Value().entries_) AddListedFileBytes(innerListedBytes_, Entry, request_.includePattern_);
                    current_ = Page.Value().entries_.front();
                    auto Prepared = PrepareFileOperation(std::move(Request), Context.operation_);
                    if (!Prepared.HasValue()) return failed(Prepared.Failure());
                    offset_ += Page.Value().entries_.size(); inner_ = std::move(Prepared.Value().task_); outcomeBase_ = emitted_;
                    return JobStep{JobState::Running};
                }
            }
            auto Page = request_.selection_->Read(offset_, 1, Context.operation_);
            if (!Page.HasValue()) return failed(Page.Failure());
            if (Page.Value().entries_.size() != 1) return failed({ErrorCategory::Internal, "selection-no-progress"});
            current_ = Page.Value().entries_[0]; ++offset_;
            auto Reader = request_.selection_->Provider();
            if (current_.id_.providerId_ != Reader->Id() || !SafeName(current_.name_)
                || Child(request_.selectionRoot_, current_.name_) != current_.locator_)
                return failed({ErrorCategory::InvalidRequest, "selection-container-mismatch"});
            auto Request = request_; Request.selection_.reset(); Request.confirmDelete_ = false;
            Request.sources_ = {{current_, Reader, request_.selectionWriter_}};
            innerListedBytes_ = 0; AddListedFileBytes(innerListedBytes_, current_, request_.includePattern_);
            auto Prepared = PrepareFileOperation(std::move(Request), Context.operation_);
            if (!Prepared.HasValue()) return failed(Prepared.Failure());
            inner_ = std::move(Prepared.Value().task_); outcomeBase_ = emitted_;
            return JobStep{JobState::Running};
        }
        CompletedEffects TakeCompletedEffects() override
        {
            if (!inner_) return {};
            auto Effects = inner_->TakeCompletedEffects();
            if (unifiedPrepared_) return Effects;
            emitted_ += Effects.items_.size();
            if (Effects.progress_) {
                Effects.progress_->bytesDone_ += completedBytes_;
                if (listedBytes_) {
                    Effects.progress_->bytesTotal_ = *listedBytes_ + completedExtraBytes_ + innerExtraBytes_;
                    Effects.progress_->plannedItems_ = request_.selection_->Count();
                } else Effects.progress_->bytesTotal_.reset();
            }
            return Effects;
        }
        std::vector<Error> Finalize(JobState State) override
        {
            if (inner_) { auto Errors = inner_->Finalize(State); cleanup_.insert(cleanup_.end(), Errors.begin(), Errors.end()); inner_.reset(); }
            request_.selection_.reset(); return cleanup_;
        }
    private:
        static JobStep failed(Error Error) { JobStep Step{JobState::Failed}; Step.error_ = std::move(Error); return Step; }
        FileOperationRequest request_;
        std::unique_ptr<IJobTask> inner_;
        EntrySnapshot current_;
        uint64_t offset_ = 0;
        uint64_t completedBytes_ = 0, innerBytes_ = 0;
        uint64_t summaryOffset_ = 0;
        uint64_t completedExtraBytes_ = 0, innerExtraBytes_ = 0;
        std::optional<uint64_t> listedBytes_ = 0;
        std::optional<uint64_t> innerListedBytes_;
        bool selectionSummarized_ = false;
        size_t emitted_ = 0, outcomeBase_ = 0;
        bool confirmed_ = false;
        std::vector<Error> cleanup_;
        std::vector<OperationSource> selectedSources_;
        bool unifiedPrepared_ = false;
    };
    }
    Result<PreparedJob> PrepareFileOperation(FileOperationRequest Request, OperationContext Context)
    {
        if (!Request.conflictDecision_) Request.conflictDecision_ = std::make_shared<std::atomic<ConflictPolicy>>(Request.conflict_);
        if (Request.continueSelectedFiles_ && (Request.selection_ || Request.operation_ != FileOperation::Copy || Request.sources_.size() > 64
                || std::any_of(Request.sources_.begin(), Request.sources_.end(), [](const auto& Source) { return Source.entry_.directory_; })))
            return Error{ErrorCategory::InvalidRequest, "invalid-selected-file-page"};
        if (Request.elevation_ && !Request.elevationApproval_) Request.elevationApproval_ = std::make_shared<FileElevationApproval>();
        if (Request.approvedDestination_ && (Request.selection_ || Request.sources_.size() != 1 || Request.sources_.front().entry_.directory_
                || Request.operation_ != FileOperation::Copy
                || Request.approvedDestination_->exists_ != Request.approvedDestination_->revision_.has_value()))
            return Error{ErrorCategory::InvalidRequest, "invalid-approved-destination"};
        if ((!Request.destinationName_.empty() && (!SafeName(Request.destinationName_) || Request.sources_.size() != 1 || Request.selection_
                || (Request.operation_ != FileOperation::Copy && Request.operation_ != FileOperation::Move)))
            || (Request.createParents_ && (Request.operation_ == FileOperation::Delete || (Request.destination_.kind_ != LocatorKind::Local && Request.destination_.kind_ != LocatorKind::Smb)))
            || (Request.deleteMode_ != DeleteMode::Permanent && Request.deleteMode_ != DeleteMode::Recycle)
            || (Request.readOnly_ != ReadOnlyPolicy::Ask && Request.readOnly_ != ReadOnlyPolicy::Allow && Request.readOnly_ != ReadOnlyPolicy::Skip))
            return Error{ErrorCategory::InvalidRequest, "invalid-file-operation-policy"};
        if (!IsValidCopyPattern(Request.includePattern_) || Request.bytesPerSecondLimit_ > (1ull << 40))
            return Error{ErrorCategory::InvalidRequest, "invalid-copy-options"};
        if (Request.operation_ == FileOperation::Copy || Request.operation_ == FileOperation::Move) {
            if (!Request.destinationWriter_) return Error{ErrorCategory::InvalidRequest, "missing-destination-writer"};
            const auto Support = Request.destinationWriter_->CopySupport();
            if (Request.copyOptions_.verify_ && !Support.verify_)
                return Error{ErrorCategory::Unsupported, "copy-options-unavailable", Request.destinationWriter_->Id()};
        }
        if (Request.selection_) {
            if (!Request.sources_.empty() || !Request.selection_->Provider() || !Request.selection_->Count()
                || Request.selection_->Count() > Request.maxEntries_
                || (Request.operation_ != FileOperation::Copy && Request.operation_ != FileOperation::Move && Request.operation_ != FileOperation::Delete)
                || (Request.operation_ != FileOperation::Copy && !Request.selectionWriter_)
                || (Request.operation_ != FileOperation::Delete && !Request.destinationWriter_))
                return Error{ErrorCategory::InvalidRequest, "invalid-selection-batch"};
            JobRequest Job; Job.correlationId_ = Request.correlationId_; Job.operation_ = Context;
            Job.resourceLanes_.push_back(LaneFor(Request.selectionRoot_));
            if (Request.operation_ != FileOperation::Delete) Job.resourceLanes_.push_back(LaneFor(Request.destination_));
            return PreparedJob{std::move(Job), std::make_unique<SelectionTask>(std::move(Request))};
        }
        if ((!Request.preservePaths_ && Request.operation_ != FileOperation::Copy)
            || Request.operation_ < FileOperation::Copy || Request.operation_ > FileOperation::Rename
            || !Request.maxEntries_ || !Request.maxBytes_
            || Request.sources_.size() > Request.maxEntries_ || (Request.operation_ != FileOperation::MakeDirectory && Request.sources_.empty())
            || (Request.operation_ == FileOperation::MakeDirectory && !Request.sources_.empty())
            || (Request.operation_ == FileOperation::Rename && Request.sources_.size() != 1)
            || (Request.operation_ != FileOperation::Delete && !Request.destinationWriter_))
            return Error{ErrorCategory::InvalidRequest, "invalid-file-operation"};
        JobRequest Job;
        Job.correlationId_ = Request.correlationId_; Job.operation_ = Context; Job.maxItemResults_ = Request.maxEntries_;
        if (Request.operation_ == FileOperation::Move || !Request.calculateSizeBefore_ || (Request.operation_ == FileOperation::Delete && Request.deleteWhileListing_)) {
            // Streaming must reject overlapping selected roots before publishing
            // any output, rather than discovering duplicates later in the tree.
            std::set<std::pair<LocatorKind, std::string>> Roots;
            for (const auto& Source : Request.sources_) {
                const auto Kind = Source.entry_.locator_.kind_;
                if (Kind != LocatorKind::Local && Kind != LocatorKind::Smb) continue;
                if (!Roots.emplace(Kind, NativeKey(Source.entry_.locator_.root_)).second)
                    return Error{ErrorCategory::InvalidRequest, "overlapping-selection"};
            }
            for (const auto& [Kind, Path] : Roots)
                for (auto End = Path.find('\\'); End != std::string::npos; End = Path.find('\\', End + 1))
                    if (End && Roots.contains({Kind, Path.substr(0, End)}))
                        return Error{ErrorCategory::InvalidRequest, "overlapping-selection"};
        }
        if (Request.operation_ != FileOperation::Delete) Job.resourceLanes_.push_back(LaneFor(Request.destination_));
        for (const auto& Source : Request.sources_)
        {
            if (!Source.reader_ || !SafeName(Source.entry_.name_) || ((Request.operation_ == FileOperation::Delete || Request.operation_ == FileOperation::Move || Request.operation_ == FileOperation::Rename) && !Source.writer_))
                return Error{ErrorCategory::InvalidRequest, "invalid-operation-source"};
            if (Request.operation_ == FileOperation::Copy || Request.operation_ == FileOperation::Move)
            {
                const auto Target = Child(Request.destination_, Request.destinationName_.empty() ? Source.entry_.name_ : Request.destinationName_);
                if (SameOrWithin(Target, Source.entry_.locator_)) return Error{ErrorCategory::InvalidRequest, "destination-inside-source"};
            }
            if (Request.operation_ == FileOperation::Rename && Source.entry_.directory_ && SameOrWithin(Request.destination_, Source.entry_.locator_)) return Error{ErrorCategory::InvalidRequest, "destination-inside-source"};
            Job.resourceLanes_.push_back(LaneFor(Source.entry_.locator_));
        }
        const bool Parents = Request.createParents_;
        const auto At = Request.operation_ == FileOperation::Copy || Request.operation_ == FileOperation::Move ? Request.destination_ : DirectoryPreparation::ParentOf(Request.destination_);
        const auto Writer = Request.destinationWriter_; std::unique_ptr<IJobTask> Task = std::make_unique<FileTask>(std::move(Request));
        if (Parents) Task = std::make_unique<DirectoryPreparation>(At, Writer, std::move(Task));
        return PreparedJob{std::move(Job), std::move(Task)};
    }
    Result<JobHandle> SubmitFileOperation(JobScheduler& Scheduler, FileOperationRequest Request, OperationContext Context)
    {
        auto Prepared = PrepareFileOperation(std::move(Request), Context); if (!Prepared.HasValue()) return Prepared.Failure();
        return Scheduler.Submit(std::move(Prepared.Value().request_), std::move(Prepared.Value().task_));
    }
}
