#include "moveOperation.hpp"
#include <set>

namespace hc::operations
{
    using namespace storage;
    namespace
    {
        std::string PathKey(const Locator& At)
        {
            auto Path = At.kind_ == LocatorKind::Archive && !At.nesting_.empty() ? At.nesting_.back().entry_ : At.root_;
            std::replace(Path.begin(), Path.end(), '\\', '/');
            if (Path.starts_with("//?/UNC/")) Path = "//" + Path.substr(8);
            else if (Path.starts_with("//?/")) Path.erase(0, 4);
            for (auto& Letter : Path) if (Letter >= 'A' && Letter <= 'Z') Letter += 'a' - 'A';
            while (Path.size() > 1 && Path.ends_with('/')) Path.pop_back();
            return Path;
        }
        bool Within(const std::string& Path, const std::string& Root)
        { return Path == Root || Path.starts_with(Root + "/"); }
        bool AncestorIn(const std::string& Path, const std::set<std::string>& Paths)
        {
            auto End = Path.find_last_of('/');
            while (End != std::string::npos && End > 0)
            {
                if (Paths.contains(Path.substr(0, End))) return true;
                End = Path.find_last_of('/', End - 1);
            }
            return false;
        }
        class MoveTask final : public IJobTask
        {
            struct Copied { ItemOutcome outcome_; std::size_t index_; bool preserveDirectory_ = false; };
        public:
            MoveTask(MoveOperationRequest Request, std::unique_ptr<IJobTask> Copy) : request_(std::move(Request)), copy_(std::move(Copy)) {}
            JobStep Advance(JobStepContext Context) override
            {
                if (const auto Failure = Context.operation_.Check({})) return failed(*Failure);
                if (phase_ == Phase::Copy)
                {
                    auto Step = copy_->Advance(Context);
                    if (Step.item_)
                    {
                        if (Step.item_->source_)
                        {
                            if (Step.item_->status_ == ItemOutcome::Status::Completed && Step.item_->destinationCommitted_)
                                copied_.push_back({*Step.item_, emitted_});
                            else preserveParents(Step.item_->source_->locator_);
                        }
                        ++emitted_;
                    }
                    if (Step.next_ == JobState::Completed)
                    {
                        // Preserve this step's committed outcome even when a
                        // provider throws while finalizing the inner copy task.
                        try { cleanup_ = copy_->Finalize(JobState::Completed); }
                        catch (...) { cleanup_.push_back({ErrorCategory::Internal, "copy-finalize-exception"}); }
                        copy_.reset();
                        if (!cleanup_.empty()) { Step.next_ = JobState::Failed; Step.error_ = cleanup_.front(); return Step; }
                        // Leave the destination commit boundary. Cancellation
                        // here retains every already-published copy outcome.
                        phase_ = Phase::PlanRemoval; Step.next_ = JobState::Running;
                    }
                    return Step;
                }
                if (phase_ == Phase::PlanRemoval) return planRemoval(Context.operation_);
                if (phase_ == Phase::PreservedReport) return reportNative(false, true);
                if (phase_ == Phase::NativeCheck)
                {
                    if (index_ == copied_.size()) return JobStep{JobState::Completed};
                    if (copied_[index_].preserveDirectory_) return reportNative(false, true);
                    auto Fresh = request_.sourceReader_->Stat(copied_[index_].outcome_.source_->id_, Context.operation_).get();
                    if (!Fresh.HasValue()) return removalFailure(Fresh.Failure());
                    if (Fresh.Value().id_ != copied_[index_].outcome_.source_->id_
                        || Fresh.Value().locator_ != copied_[index_].outcome_.source_->locator_)
                        return removalFailure({ErrorCategory::StaleRevision, "move-source-changed"});
                    phase_ = Phase::NativeRemove;
                    return JobStep{JobState::Committing};
                }
                if (!Context.committing_) return failed({ErrorCategory::Internal, "move-commit-boundary-required"});
                if (phase_ == Phase::NativeRemove)
                {
                    const auto Removed = request_.sourceWriter_->Remove(*copied_[index_].outcome_.source_, Context.operation_);
                    if (!Removed.HasValue()) return removalFailure(Removed.Failure());
                    return reportNative(true, false);
                }
                if (phase_ == Phase::ArchiveCommit)
                {
                    const auto Receipt = removal_->Commit(Context.operation_);
                    if (!Receipt.HasValue()) return failed(Receipt.Failure());
                    cleanup_.insert(cleanup_.end(), Receipt.Value().cleanupErrors_.begin(), Receipt.Value().cleanupErrors_.end());
                    phase_ = Phase::ArchiveReport;
                }
                // The source archive was committed as a unit. Publish every
                // removal outcome before honoring a late cancellation.
                auto Step = update(copied_[index_]);
                Step.item_->sourceRemoved_ = !copied_[index_].preserveDirectory_;
                if (copied_[index_].preserveDirectory_) Step.item_->status_ = ItemOutcome::Status::Skipped;
                ++index_;
                Step.next_ = index_ < copied_.size() ? JobState::Committing : cleanup_.empty() ? JobState::Completed : JobState::Failed;
                if (!cleanup_.empty()) Step.error_ = cleanup_.front();
                return Step;
            }
            std::vector<Error> Finalize(JobState State) override
            {
                if (copy_) { auto Errors = copy_->Finalize(State); cleanup_.insert(cleanup_.end(), Errors.begin(), Errors.end()); copy_.reset(); }
                if (removal_) { auto Errors = removal_->Abort(); cleanup_.insert(cleanup_.end(), Errors.begin(), Errors.end()); removal_.reset(); }
                return cleanup_;
            }
        private:
            enum class Phase { Copy, PlanRemoval, NativeCheck, NativeRemove, ArchiveCommit, ArchiveReport, PreservedReport };
            static JobStep failed(Error Failure) { JobStep Step{JobState::Failed}; Step.error_ = std::move(Failure); return Step; }
            static JobStep update(const Copied& Item)
            { JobStep Step{JobState::Running}; Step.item_ = Item.outcome_; Step.replaceItem_ = Item.index_; return Step; }
            JobStep removalFailure(Error Failure)
            {
                auto Step = update(copied_[index_]); Step.next_ = JobState::Failed; Step.error_ = Failure;
                Step.item_->status_ = ItemOutcome::Status::Failed; Step.item_->error_ = std::move(Failure); return Step;
            }
            JobStep reportNative(bool Removed, bool Skipped)
            {
                auto Step = update(copied_[index_]); Step.item_->sourceRemoved_ = Removed;
                if (Skipped) Step.item_->status_ = ItemOutcome::Status::Skipped;
                ++index_; if (phase_ != Phase::PreservedReport) phase_ = Phase::NativeCheck;
                if (index_ == copied_.size()) Step.next_ = JobState::Completed;
                return Step;
            }
            void preserveParents(const Locator& At)
            {
                auto Path = PathKey(At); auto End = Path.find_last_of('/');
                while (End != std::string::npos && End > 0)
                { preserved_.insert(Path.substr(0, End)); End = Path.find_last_of('/', End - 1); }
            }
            JobStep planRemoval(OperationContext Context)
            {
                if (copied_.empty()) return JobStep{JobState::Completed};
                for (auto& Item : copied_) Item.preserveDirectory_ = Item.outcome_.source_->directory_ && preserved_.contains(PathKey(Item.outcome_.source_->locator_));
                if (!request_.sourceArchive_)
                {
                    // Files first, then deepest directories. Native Remove
                    // refuses nonempty directories, including newly-added files.
                    std::stable_sort(copied_.begin(), copied_.end(), [](const auto& A, const auto& B) {
                        if (A.outcome_.source_->directory_ != B.outcome_.source_->directory_) return !A.outcome_.source_->directory_;
                        return PathKey(A.outcome_.source_->locator_).size() > PathKey(B.outcome_.source_->locator_).size();
                    });
                    phase_ = Phase::NativeCheck; return JobStep{JobState::Running};
                }
                std::stable_sort(copied_.begin(), copied_.end(), [](const auto& A, const auto& B) {
                    return PathKey(A.outcome_.source_->locator_).size() < PathKey(B.outcome_.source_->locator_).size();
                });
                std::vector<ArchiveChange> Changes;
                std::set<std::string> RemovedDirectories;
                for (const auto& Item : copied_)
                {
                    const auto& Entry = *Item.outcome_.source_;
                    const auto Key = PathKey(Entry.locator_);
                    if (Item.preserveDirectory_ || AncestorIn(Key, RemovedDirectories)) continue;
                    Changes.push_back({ArchiveChangeKind::Remove, Entry.locator_.nesting_.back().entry_, {}, {}, Entry.id_});
                    if (Entry.directory_) RemovedDirectories.insert(Key);
                }
                if (Changes.empty()) { phase_ = Phase::PreservedReport; return JobStep{JobState::Running}; }
                auto Opened = request_.sourceArchive_->begin_(Context); if (!Opened.HasValue()) return failed(Opened.Failure());
                removal_ = std::move(Opened.Value()); if (!removal_) return failed({ErrorCategory::Internal, "move-removal-session-required"});
                const auto Prepared = removal_->Prepare(std::move(Changes), Context); if (!Prepared.HasValue()) return failed(Prepared.Failure());
                phase_ = Phase::ArchiveCommit; return JobStep{JobState::Committing};
            }
            MoveOperationRequest request_;
            std::unique_ptr<IJobTask> copy_;
            std::unique_ptr<IArchiveMutationSession> removal_;
            std::vector<Copied> copied_;
            std::set<std::string> preserved_;
            std::vector<Error> cleanup_;
            Phase phase_ = Phase::Copy;
            std::size_t emitted_ = 0, index_ = 0;
        };
    }
    Result<JobHandle> SubmitMoveOperation(JobScheduler& Scheduler, MoveOperationRequest Request, OperationContext Context)
    {
        const auto ValidEndpoint = [](const ArchiveMoveEndpoint& Endpoint) {
            return Endpoint.reader_ && Endpoint.begin_ && Endpoint.root_.kind_ == LocatorKind::Archive && !Endpoint.root_.root_.empty()
                && Endpoint.root_.nesting_.size() == 1 && Endpoint.root_.nesting_[0].entry_ == "/";
        };
        if (!Request.sourceReader_ || Request.sources_.empty() || !Request.maxEntries_ || Request.maxEntries_ > 100000
            || Request.sources_.size() > Request.maxEntries_ || !Request.maxDepth_ || Request.maxDepth_ > 32 || !Request.maxBytes_
            || (Request.sourceArchive_ ? !ValidEndpoint(*Request.sourceArchive_) || Request.sourceArchive_->reader_ != Request.sourceReader_ : !Request.sourceWriter_)
            || (Request.destinationArchive_ ? !ValidEndpoint(*Request.destinationArchive_) || Request.destination_.kind_ != LocatorKind::Archive
                || Request.destination_.root_ != Request.destinationArchive_->root_.root_ : !Request.destinationWriter_
                || (Request.destination_.kind_ != LocatorKind::Local && Request.destination_.kind_ != LocatorKind::Smb && Request.destination_.kind_ != LocatorKind::Plugin)))
            return Error{ErrorCategory::InvalidRequest, "invalid-move-operation"};
        if (Request.sourceArchive_ && Request.destinationArchive_
            && PathKey({LocatorKind::Local, Request.sourceArchive_->root_.root_}) == PathKey({LocatorKind::Local, Request.destinationArchive_->root_.root_}))
            return Error{ErrorCategory::InvalidRequest, "use-in-container-move"};
        std::vector<OperationSource> Sources;
        for (const auto& Entry : Request.sources_)
        {
            if (Entry.id_.providerId_ != Request.sourceReader_->Id()
                || (Request.sourceArchive_ ? Entry.locator_.kind_ != LocatorKind::Archive || Entry.locator_.root_ != Request.sourceArchive_->root_.root_
                    || Entry.locator_.nesting_.size() != 1 || Entry.locator_.nesting_[0].provider_ != Request.sourceArchive_->root_.nesting_[0].provider_
                    : Entry.locator_.kind_ != LocatorKind::Local && Entry.locator_.kind_ != LocatorKind::Smb && Entry.locator_.kind_ != LocatorKind::Plugin))
                return Error{ErrorCategory::InvalidRequest, "invalid-move-source"};
            if (!Request.sourceArchive_ && Request.destinationArchive_)
            {
                const auto Destination = PathKey({LocatorKind::Local, Request.destination_.root_});
                if (Within(Destination, PathKey(Entry.locator_))) return Error{ErrorCategory::InvalidRequest, "destination-inside-source"};
            }
            Sources.push_back({Entry, Request.sourceReader_, Request.sourceWriter_});
        }
        Result<PreparedJob> Copy = Error{ErrorCategory::Internal, "move-copy-required"};
        if (Request.destinationArchive_)
        {
            ArchiveOperationRequest Archive;
            Archive.archive_ = Request.destinationArchive_->root_; Archive.reader_ = Request.destinationArchive_->reader_; Archive.begin_ = Request.destinationArchive_->begin_;
            Archive.destination_ = Request.destination_; Archive.sources_ = std::move(Sources); Archive.conflict_ = Request.conflict_; Archive.correlationId_ = Request.correlationId_;
            Archive.limits_ = Request.destinationArchive_->limits_;
            Archive.limits_.maxEntries_ = Request.maxEntries_; Archive.limits_.maxDepth_ = Request.maxDepth_; Archive.limits_.maxInputBytes_ = Request.maxBytes_;
            Copy = PrepareArchiveOperation(std::move(Archive), Context);
        }
        else
        {
            FileOperationRequest Native;
            Native.sources_ = std::move(Sources); Native.destination_ = Request.destination_; Native.destinationWriter_ = Request.destinationWriter_;
            Native.conflict_ = Request.conflict_; Native.maxEntries_ = Request.maxEntries_; Native.maxDepth_ = Request.maxDepth_; Native.maxBytes_ = Request.maxBytes_; Native.correlationId_ = Request.correlationId_;
            Copy = PrepareFileOperation(std::move(Native), Context);
        }
        if (!Copy.HasValue()) return Copy.Failure();
        return Scheduler.Submit(std::move(Copy.Value().request_), std::make_unique<MoveTask>(std::move(Request), std::move(Copy.Value().task_)));
    }
}
