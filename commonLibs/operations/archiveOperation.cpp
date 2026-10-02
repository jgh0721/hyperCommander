#include "archiveOperation.hpp"
#include "../storage/materializedContent.hpp"
#include "selectionPlan.hpp"
#include <set>
#include <deque>

namespace hc::operations
{
    using namespace storage;
    namespace
    {
        bool SafeName(const std::string& Name)
        { return !Name.empty() && Name != "." && Name != ".." && Name.find_first_of("/\\:") == std::string::npos && Name.find('\0') == std::string::npos; }
        std::string Child(std::string Parent, const std::string& Name) { if (!Parent.ends_with('/')) Parent += '/'; return Parent + Name; }
        std::string Parent(const std::string& Path) { const auto Slash = Path.find_last_of('/'); return Slash == 0 ? "/" : Path.substr(0, Slash); }
        bool Within(const std::string& Path, const std::string& Root) { return Path == Root || Path.starts_with(Root + "/"); }
        std::string Lane(std::string Path)
        {
            std::replace(Path.begin(), Path.end(), '/', '\\');
            if (Path.starts_with("\\\\?\\UNC\\")) Path = "\\\\" + Path.substr(8);
            else if (Path.starts_with("\\\\?\\")) Path.erase(0, 4);
            for (auto& Letter : Path) if (Letter >= 'A' && Letter <= 'Z') Letter += 'a' - 'A';
            return "archive:" + Path;
        }
        class ArchiveTask final : public IJobTask
        {
            struct Item { OperationSource source_; std::string destination_; std::size_t depth_ = 0; };
        public:
            explicit ArchiveTask(ArchiveOperationRequest Request) : request_(std::move(Request))
            {
                const auto Destination = request_.destination_.nesting_.empty() ? "/" : request_.destination_.nesting_.back().entry_;
                for (const auto& Source : request_.sources_)
                    pending_.push_back({Source, request_.operation_ == FileOperation::Rename ? Destination : Child(Destination, Source.entry_.name_)});
                if (request_.operation_ == FileOperation::MakeDirectory) pending_.push_back({{}, Destination});
            }
            JobStep Advance(JobStepContext Context) override
            {
                if (const auto Failure = Context.operation_.Check({"wcx"})) return failed(*Failure);
                if (phase_ == Phase::Plan) return plan(Context.operation_);
                if (phase_ == Phase::Conflict)
                {
                    if (!Context.answer_) return failed({ErrorCategory::Internal, "archive-conflict-answer-required"});
                    if (Context.answer_->choice_ == InteractionChoice::Skip)
                    {
                        return skip();
                    }
                    if (Context.answer_->choice_ == InteractionChoice::Rename)
                    {
                        if (!SafeName(Context.answer_->value_)) return failed({ErrorCategory::InvalidRequest, "unsafe-destination-name"});
                        current_.destination_ = Child(Parent(current_.destination_), Context.answer_->value_);
                        return destination(Context.operation_);
                    }
                    if (Context.answer_->choice_ != InteractionChoice::Replace || !replaceAllowed_)
                        return failed({ErrorCategory::InvalidRequest, "invalid-archive-conflict-choice"});
                    return accept(Context.operation_);
                }
                if (phase_ == Phase::Confirm)
                {
                    if (!Context.answer_) return failed({ErrorCategory::Internal, "archive-delete-answer-required"});
                    if (Context.answer_->choice_ != InteractionChoice::Confirm) return failed({ErrorCategory::InvalidRequest, "archive-delete-confirmation-required"});
                    phase_ = Phase::Prepare; return progress();
                }
                if (phase_ == Phase::Prepare)
                {
                    if (!changes_.empty() || (request_.beginRemoval_ && !outcomes_.empty()))
                    {
                        auto Opened = request_.begin_(Context.operation_); if (!Opened.HasValue()) return failed(Opened.Failure());
                        batch_ = std::move(Opened.Value());
                        if (!batch_) return failed({ErrorCategory::Internal, "archive-batch-required"});
                        const auto Prepared = prepareBatch(Context.operation_); if (!Prepared.HasValue()) return failed(Prepared.Failure());
                    }
                    phase_ = Phase::Commit;
                    auto Step = progress(); Step.next_ = JobState::Committing; return Step;
                }
                if (!Context.committing_) return failed({ErrorCategory::Internal, "archive-commit-boundary-required"});
                if (phase_ == Phase::Commit)
                {
                    if (batch_)
                    {
                        auto Receipt = batch_->Commit(Context.operation_); if (!Receipt.HasValue()) return failed(Receipt.Failure());
                        committed_ = true;
                        cleanup_ = std::move(Receipt.Value().cleanupErrors_);
                    }
                    phase_ = Phase::Report;
                }
                // Keep the commit boundary while publishing the whole batch's
                // outcomes: a late cancellation cannot hide committed members.
                JobStep Step{JobState::Committing};
                if (reported_ < outcomes_.size())
                {
                    auto Outcome = outcomes_[reported_++];
                    Outcome.destinationCommitted_ = Outcome.destinationCommitted_ || (committed_ && request_.operation_ != FileOperation::Delete);
                    Outcome.sourceRemoved_ = committed_ && (request_.operation_ == FileOperation::Delete || request_.operation_ == FileOperation::Rename || request_.operation_ == FileOperation::Move);
                    if (committed_ && request_.beginRemoval_ && Outcome.source_)
                        Outcome.sourceRemoved_ = !Outcome.source_->directory_ || !preserved_.contains(Outcome.source_->locator_.nesting_.back().entry_);
                    Step.item_ = std::move(Outcome);
                }
                if (reported_ == outcomes_.size())
                {
                    Step.next_ = cleanup_.empty() ? JobState::Completed : JobState::Failed;
                    if (!cleanup_.empty()) Step.error_ = cleanup_.front();
                }
                return Step;
            }
            std::vector<Error> Finalize(JobState) override
            {
                cursor_.reset();
                if (batch_) { const auto Errors = batch_->Abort(); cleanup_.insert(cleanup_.end(), Errors.begin(), Errors.end()); batch_.reset(); }
                return cleanup_;
            }
        private:
            enum class Phase { Plan, Conflict, Confirm, Prepare, Commit, Report };
            Locator at(const std::string& Path) const { auto At = request_.archive_; At.nesting_.back().entry_ = Path; return At; }
            std::string itemLocator() const { return Serialize(current_.source_.reader_ ? current_.source_.entry_.locator_ : at(current_.destination_)); }
            ItemOutcome outcome(ItemOutcome::Status Status = ItemOutcome::Status::Completed) const
            {
                ItemOutcome Outcome{itemLocator(), Status};
                if (current_.source_.reader_) Outcome.source_ = current_.source_.entry_;
                return Outcome;
            }
            JobStep progress() const
            {
                JobStep Step{phase_ == Phase::Plan ? JobState::Preparing : JobState::Running};
                Step.progress_ = JobProgress{0, {}, current_.source_.entry_.name_.empty() ? current_.destination_ : current_.source_.entry_.name_}; return Step;
            }
            JobStep skip()
            {
                if (request_.beginRemoval_)
                    for (auto Path = Parent(current_.source_.entry_.locator_.nesting_.back().entry_); Path != "/"; Path = Parent(Path)) preserved_.insert(Path);
                auto Step = progress(); Step.item_ = outcome(ItemOutcome::Status::Skipped); phase_ = Phase::Plan; return Step;
            }
            Result<Unit> prepareBatch(OperationContext Context)
            {
                if (!request_.beginRemoval_) return batch_->Prepare(std::move(changes_), Context);
                std::vector<ArchiveChange> Removes;
                std::set<std::string> Covered;
                auto Items = outcomes_;
                std::stable_sort(Items.begin(), Items.end(), [](const auto& A, const auto& B) {
                    return A.source_->locator_.nesting_.back().entry_.size() < B.source_->locator_.nesting_.back().entry_.size();
                });
                for (const auto& Item : Items)
                {
                    const auto& Entry = *Item.source_; const auto& Path = Entry.locator_.nesting_.back().entry_;
                    if (Entry.directory_ && preserved_.contains(Path)) continue;
                    if (std::ranges::any_of(Covered, [&](const auto& Root) { return Within(Path, Root); })) continue;
                    Removes.push_back({ArchiveChangeKind::Remove, Path, {}, {}, Entry.id_});
                    if (Entry.directory_) Covered.insert(Path);
                }
                if (Removes.empty())
                {
                    if (changes_.empty()) { batch_.reset(); return Unit{}; }
                    return batch_->Prepare(std::move(changes_), Context);
                }
                auto Removal = request_.beginRemoval_(Context); if (!Removal.HasValue()) return Removal.Failure();
                return batch_->PrepareTogether(std::move(changes_), std::move(Removal.Value()), std::move(Removes), Context);
            }
            JobStep failed(Error Failure) const { JobStep Step{JobState::Failed}; Step.error_ = std::move(Failure); return Step; }
            JobStep plan(OperationContext Context)
            {
                Context.freshEnumerationMetadata_ = true;
                if (cursor_)
                {
                    auto Page = cursor_->Next(Context.maxPageItems_, Context).get(); if (!Page.HasValue()) return failed(Page.Failure());
                    if (Page.Value().entries_.empty() && !Page.Value().end_) return failed({ErrorCategory::Internal, "archive-source-cursor-no-progress"});
                    if (seen_.size() + pending_.size() + Page.Value().entries_.size() > request_.limits_.maxEntries_)
                        return failed({ErrorCategory::BudgetExceeded, "archive-operation-entry-budget"});
                    for (auto& Entry : Page.Value().entries_)
                    {
                        if (!SafeName(Entry.name_)) return failed({ErrorCategory::InvalidRequest, "unsafe-source-name"});
                        pending_.push_back({{Entry, enumerating_.source_.reader_, {}}, Child(enumerating_.destination_, Entry.name_), enumerating_.depth_ + 1});
                    }
                    if (Page.Value().end_) cursor_.reset();
                    return progress();
                }
                if (pending_.empty())
                {
                    phase_ = Phase::Prepare;
                    if (request_.operation_ == FileOperation::Delete && request_.confirmDelete_)
                    {
                        phase_ = Phase::Confirm; auto Step = progress(); Step.next_ = JobState::WaitingForUser;
                        Step.interaction_ = InteractionRequest{"", InteractionKind::DeleteConfirmation, "confirm-archive-delete", {InteractionChoice::Confirm, InteractionChoice::Cancel}};
                        return Step;
                    }
                    return progress();
                }
                current_ = std::move(pending_.front()); pending_.pop_front();
                if (current_.depth_ > request_.limits_.maxDepth_ || seen_.size() >= request_.limits_.maxEntries_)
                    return failed({ErrorCategory::BudgetExceeded, "archive-operation-tree-budget"});
                if (current_.source_.reader_)
                {
                    if (!seen_.insert(Serialize(current_.source_.entry_.locator_)).second) return failed({ErrorCategory::InvalidRequest, "overlapping-selection"});
                    auto Fresh = current_.source_.reader_->Stat(current_.source_.entry_.id_, Context).get(); if (!Fresh.HasValue()) return failed(Fresh.Failure());
                    if (Fresh.Value().id_ != current_.source_.entry_.id_ || Fresh.Value().locator_ != current_.source_.entry_.locator_
                        || !SafeName(Fresh.Value().name_) || Fresh.Value().attributes_ & 0x400)
                        return failed({ErrorCategory::StaleRevision, "archive-source-changed"});
                    current_.source_.entry_ = std::move(Fresh.Value());
                    if (!current_.source_.entry_.directory_ && request_.operation_ != FileOperation::Delete)
                    {
                        if (current_.source_.entry_.size_ > request_.limits_.maxInputBytes_ - inputBytes_)
                            return failed({ErrorCategory::BudgetExceeded, "archive-operation-byte-budget"});
                        inputBytes_ += current_.source_.entry_.size_;
                    }
                }
                if (request_.operation_ == FileOperation::Delete)
                {
                    changes_.push_back({ArchiveChangeKind::Remove, current_.source_.entry_.locator_.nesting_.back().entry_, {}, {}, current_.source_.entry_.id_});
                    outcomes_.push_back(outcome()); return progress();
                }
                return destination(Context);
            }
            JobStep destination(OperationContext Context)
            {
                if (destinations_.contains(current_.destination_)) return failed({ErrorCategory::InvalidRequest, "archive-overlapping-destination"});
                const auto Existing = request_.reader_->Resolve(at(current_.destination_), Context).get();
                expected_ = {}; replaceAllowed_ = false;
                if (Existing.HasValue())
                {
                    if (request_.operation_ == FileOperation::Copy && Existing.Value().directory_ && current_.source_.entry_.directory_)
                        return accept(Context, true);
                    expected_ = {true, Existing.Value().id_.revision_};
                    replaceAllowed_ = request_.operation_ == FileOperation::Copy && !Existing.Value().directory_ && !current_.source_.entry_.directory_;
                    if (request_.operation_ == FileOperation::Copy && request_.conflict_ == ConflictPolicy::Skip)
                    { return skip(); }
                    if (replaceAllowed_ && request_.conflict_ == ConflictPolicy::Replace) return accept(Context);
                    phase_ = Phase::Conflict; auto Step = progress(); Step.next_ = JobState::WaitingForUser;
                    Step.interaction_ = InteractionRequest{"", InteractionKind::Overwrite, "archive-destination-exists", {InteractionChoice::Rename, InteractionChoice::Cancel}, Existing.Value().id_};
                    if (request_.operation_ == FileOperation::Copy) Step.interaction_->choices_.push_back(InteractionChoice::Skip);
                    if (replaceAllowed_) Step.interaction_->choices_.push_back(InteractionChoice::Replace);
                    return Step;
                }
                if (Existing.Failure().category_ != ErrorCategory::NotFound) return failed(Existing.Failure());
                return accept(Context);
            }
            JobStep accept(OperationContext Context, bool MergeDirectory = false)
            {
                destinations_.insert(current_.destination_);
                if (request_.operation_ == FileOperation::Rename || request_.operation_ == FileOperation::Move)
                    changes_.push_back({ArchiveChangeKind::Rename, current_.source_.entry_.locator_.nesting_.back().entry_, current_.destination_, {}, current_.source_.entry_.id_, expected_});
                else if (request_.operation_ == FileOperation::MakeDirectory || current_.source_.entry_.directory_)
                {
                    if (!MergeDirectory) changes_.push_back({ArchiveChangeKind::MakeDirectory, current_.destination_});
                }
                else
                    changes_.push_back({ArchiveChangeKind::Put, current_.destination_, {}, std::make_shared<ProviderContentSource>(current_.source_.reader_, current_.source_.entry_), {}, expected_});
                auto Outcome = outcome(); Outcome.destinationCommitted_ = MergeDirectory;
                outcomes_.push_back(std::move(Outcome));
                if (request_.operation_ == FileOperation::Copy && current_.source_.entry_.directory_)
                {
                    auto Cursor = current_.source_.reader_->Enumerate(current_.source_.entry_.locator_, Context).get(); if (!Cursor.HasValue()) return failed(Cursor.Failure());
                    cursor_ = std::move(Cursor.Value()); enumerating_ = current_;
                }
                phase_ = Phase::Plan; return progress();
            }
            ArchiveOperationRequest request_;
            Phase phase_ = Phase::Plan;
            std::deque<Item> pending_;
            Item current_, enumerating_;
            std::unique_ptr<IEntryCursor> cursor_;
            std::set<std::string> seen_, destinations_, preserved_;
            std::vector<ArchiveChange> changes_;
            std::vector<ItemOutcome> outcomes_;
            DestinationExpectation expected_;
            std::unique_ptr<IArchiveMutationSession> batch_;
            std::vector<Error> cleanup_;
            std::uint64_t inputBytes_ = 0;
            std::size_t reported_ = 0;
            bool replaceAllowed_ = false, committed_ = false;
        };
    }
    Result<PreparedJob> PrepareArchiveOperation(ArchiveOperationRequest Request, OperationContext Context)
    {
        if (Request.selection_) {
            if (!Request.sources_.empty() || !Request.selection_->Provider() || !Request.selection_->Count()
                || Request.selection_->Count() > Request.limits_.maxEntries_ || Request.selectionRoot_.root_.empty()
                || (Request.selectionRoot_.kind_ == LocatorKind::Archive && Request.selectionRoot_.nesting_.empty()))
                return Error{ErrorCategory::InvalidRequest, "invalid-archive-selection"};
            JobRequest Job; Job.operation_ = Context; Job.correlationId_ = Request.correlationId_; Job.maxItemResults_ = Request.limits_.maxEntries_;
            Job.resourceLanes_ = {ResourceLane(Request.archive_), ResourceLane(Request.selectionRoot_)};
            auto Selection = Request.selection_; auto Root = Request.selectionRoot_; auto Limit = Request.limits_.maxEntries_;
            auto Reader = Selection->Provider(); Request.selection_.reset();
            return PreparedJob{std::move(Job), std::make_unique<SelectionPlan>(Selection, Root,
                [Request = std::move(Request), Reader](std::vector<EntrySnapshot> Entries, OperationContext Operation) mutable -> Result<PreparedJob> {
                    for (auto& Entry : Entries) Request.sources_.push_back({std::move(Entry), Reader, {}});
                    return PrepareArchiveOperation(std::move(Request), Operation);
                }, Limit)};
        }
        const auto ValidLocation = [&](const Locator& At) { return SameArchiveContainer(At, Request.archive_) && At.nesting_.back().entry_.starts_with('/'); };
        if (!Request.reader_ || !Request.begin_ || Request.archive_.kind_ != LocatorKind::Archive || Request.archive_.root_.empty()
            || (Request.archive_.nesting_.empty() || Request.archive_.nesting_.size() > 8) || Request.archive_.nesting_.back().entry_ != "/"
            || !Request.limits_.maxEntries_ || !Request.limits_.maxDepth_ || Request.limits_.maxDepth_ > 32
            || !Request.limits_.maxInputBytes_ || Request.sources_.size() > Request.limits_.maxEntries_
            || (Request.operation_ != FileOperation::Copy && Request.operation_ != FileOperation::Delete && Request.operation_ != FileOperation::Rename && Request.operation_ != FileOperation::MakeDirectory && Request.operation_ != FileOperation::Move)
            || (Request.beginRemoval_ && Request.operation_ != FileOperation::Copy)
            || (Request.operation_ != FileOperation::Delete && !ValidLocation(Request.destination_))
            || (Request.operation_ == FileOperation::MakeDirectory ? !Request.sources_.empty() : Request.sources_.empty())
            || (Request.operation_ == FileOperation::Rename && Request.sources_.size() != 1) || !Context.maxReadBytes_ || !Context.maxPageItems_)
            return Error{ErrorCategory::InvalidRequest, "invalid-archive-operation"};
        for (const auto& Source : Request.sources_)
        {
            if (!Source.reader_ || !SafeName(Source.entry_.name_)) return Error{ErrorCategory::InvalidRequest, "invalid-archive-source"};
            if (Request.beginRemoval_ && (Source.entry_.locator_.kind_ != LocatorKind::Archive || Source.entry_.locator_.nesting_.empty()))
                return Error{ErrorCategory::InvalidRequest, "invalid-joint-archive-source"};
            if (Request.operation_ != FileOperation::Copy && (!ValidLocation(Source.entry_.locator_) || Source.reader_ != Request.reader_))
                return Error{ErrorCategory::InvalidRequest, "foreign-archive-source"};
            if ((Request.operation_ == FileOperation::Copy || Request.operation_ == FileOperation::Move) && ValidLocation(Source.entry_.locator_)
                && Within(Child(Request.destination_.nesting_.back().entry_, Source.entry_.name_), Source.entry_.locator_.nesting_.back().entry_))
                return Error{ErrorCategory::InvalidRequest, "destination-inside-source"};
        }
        JobRequest Job; Job.correlationId_ = Request.correlationId_; Job.operation_ = Context; Job.maxItemResults_ = Request.limits_.maxEntries_;
        Job.resourceLanes_.push_back(Lane(Request.archive_.root_));
        for (const auto& Source : Request.sources_)
            Job.resourceLanes_.push_back(ResourceLane(Source.entry_.locator_));
        return PreparedJob{std::move(Job), std::make_unique<ArchiveTask>(std::move(Request))};
    }
    Result<JobHandle> SubmitArchiveOperation(JobScheduler& Scheduler, ArchiveOperationRequest Request, OperationContext Context)
    {
        auto Prepared = PrepareArchiveOperation(std::move(Request), Context); if (!Prepared.HasValue()) return Prepared.Failure();
        return Scheduler.Submit(std::move(Prepared.Value().request_), std::move(Prepared.Value().task_));
    }
}
