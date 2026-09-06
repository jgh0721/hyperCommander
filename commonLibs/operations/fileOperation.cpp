#include "fileOperation.hpp"
#include <set>

namespace hc::operations
{
    using namespace storage;
    namespace
    {
        bool SafeName(const std::string& Name)
        {
            return !Name.empty() && Name != "." && Name != ".." && Name.find_first_of("/\\:") == std::string::npos
                && Name.find('\0') == std::string::npos;
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
            if (To.kind_ != From.kind_ || (To.kind_ != LocatorKind::Local && To.kind_ != LocatorKind::Smb)) return false;
            const auto A = NativeKey(To.root_), B = NativeKey(From.root_);
            return A == B || A.starts_with(B + "\\");
        }
        std::string LaneFor(const Locator& At)
        {
            const auto Path = NativeKey(At.root_);
            if (At.kind_ == LocatorKind::Archive) return "archive:" + Path;
            if (Path.starts_with("\\\\"))
            {
                const auto Server = Path.find('\\', 2);
                const auto Share = Server == std::string::npos ? Server : Path.find('\\', Server + 1);
                return "share:" + Path.substr(0, Share);
            }
            return "volume:" + Path.substr(0, 2);
        }
        struct PlannedItem
        {
            OperationSource source_;
            Locator destination_;
            std::size_t depth_ = 0;
            bool removeDirectory_ = false;
        };
        class FileTask final : public IJobTask
        {
        public:
            explicit FileTask(FileOperationRequest Request) : request_(std::move(Request))
            {
                for (auto& Source : request_.sources_)
                    pending_.push_back({Source, request_.operation_ == FileOperation::Rename ? request_.destination_ : Child(request_.destination_, Source.entry_.name_)});
            }
            JobStep Advance(JobStepContext Context) override
            {
                if (const auto Error = Context.operation_.Check({})) return failed(*Error);
                if (phase_ == Phase::Preparing) return prepare(Context.operation_);
                if (phase_ == Phase::Confirm)
                {
                    if (!Context.answer_)
                    {
                        JobStep Step{JobState::WaitingForUser};
                        Step.interaction_ = InteractionRequest{"", InteractionKind::DeleteConfirmation, "confirm-permanent-delete",
                            {InteractionChoice::Confirm, InteractionChoice::Cancel}};
                        return Step;
                    }
                    phase_ = Phase::Begin;
                }
                if (phase_ == Phase::Conflict) return conflict(Context);
                if (phase_ == Phase::Begin) return begin(Context.operation_);
                if (phase_ == Phase::OpenRead)
                {
                    auto ReadContext = Context.operation_;
                    ReadContext.denyConcurrentMutation_ = true;
                    auto Opened = current().source_.reader_->OpenRead(current().source_.entry_.id_, ReadContext).get();
                    if (!Opened.HasValue())
                    {
                        if (Opened.Failure().retryable_ && Context.retryAttempt_ < 3)
                        { JobStep Step{JobState::Running}; Step.error_ = Opened.Failure(); Step.retry_ = true; return Step; }
                        return failed(Opened.Failure());
                    }
                    stream_ = std::move(Opened.Value());
                    auto Created = request_.destinationWriter_->BeginWrite(current().destination_, expected_, Context.operation_);
                    if (!Created.HasValue()) return failed(Created.Failure());
                    session_ = std::move(Created.Value());
                    phase_ = Phase::Read;
                    return progress();
                }
                if (phase_ == Phase::Read)
                {
                    auto Chunk = stream_->Read(Context.operation_.maxReadBytes_, Context.operation_).get();
                    if (!Chunk.HasValue()) return failed(Chunk.Failure());
                    if (Chunk.Value().bytes_.empty() && !Chunk.Value().eof_) return failed({ErrorCategory::Internal, "stream-no-progress"});
                    if (Chunk.Value().bytes_.size() > request_.maxBytes_ - bytesDone_)
                        return failed({ErrorCategory::BudgetExceeded, "copy-byte-budget"});
                    chunk_ = std::move(Chunk.Value());
                    phase_ = Phase::Write;
                    return progress();
                }
                if (phase_ == Phase::Write)
                {
                    if (!chunk_.bytes_.empty())
                    {
                        auto Written = session_->Append(chunk_.bytes_, Context.operation_);
                        if (!Written.HasValue()) return failed(Written.Failure());
                        bytesDone_ += chunk_.bytes_.size();
                        itemBytes_ += chunk_.bytes_.size();
                    }
                    if (!chunk_.eof_) { phase_ = Phase::Read; return progress(); }
                    if (itemBytes_ != current().source_.entry_.size_) return failed({ErrorCategory::StaleRevision, "source-size-changed"});
                    auto Verified = current().source_.reader_->Stat(current().source_.entry_.id_, Context.operation_).get();
                    if (!Verified.HasValue()) return failed(Verified.Failure());
                    auto Metadata = session_->SetMetadata({current().source_.entry_.modifiedUtcMs_, current().source_.entry_.attributes_}, Context.operation_);
                    if (!Metadata.HasValue()) return failed(Metadata.Failure());
                    stream_.reset();
                    phase_ = Phase::Commit;
                    auto Step = progress(); Step.next_ = JobState::Committing; return Step;
                }
                if (!Context.committing_) return failed({ErrorCategory::Internal, "commit-boundary-required"});
                return commit(Context.operation_);
            }
            std::vector<Error> Finalize(JobState) override
            {
                stream_.reset();
                if (session_)
                {
                    const auto Cleaned = session_->Abort();
                    if (!Cleaned.HasValue()) cleanup_.push_back(Cleaned.Failure());
                    session_.reset();
                }
                return cleanup_;
            }
        private:
            enum class Phase { Preparing, Confirm, Begin, Conflict, OpenRead, Read, Write, Commit, MakeDirectory, Remove, Rename };
            PlannedItem& current() { return items_[index_]; }
            std::string itemLocator()
            { return Serialize(current().source_.entry_.locator_.root_.empty() ? current().destination_ : current().source_.entry_.locator_); }
            JobStep progress()
            {
                JobStep Step{phase_ == Phase::Preparing ? JobState::Preparing : JobState::Running};
                Step.progress_ = JobProgress{bytesDone_, bytesTotal_, index_ < items_.size() ? current().source_.entry_.name_ : std::string{}};
                return Step;
            }
            JobStep failed(Error Error)
            {
                JobStep Step{JobState::Failed}; Step.error_ = Error;
                if (phase_ != Phase::Preparing && index_ < items_.size())
                    Step.item_ = ItemOutcome{itemLocator(), ItemOutcome::Status::Failed, Error, destinationCommitted_, false, current().source_.entry_};
                return Step;
            }
            JobStep done(ItemOutcome::Status Status = ItemOutcome::Status::Completed, bool Destination = false, bool Removed = false)
            {
                auto Step = progress();
                if (Status == ItemOutcome::Status::Skipped && request_.operation_ == FileOperation::Move)
                    skippedSources_.push_back(current().source_.entry_.locator_);
                // Move directories produce one outcome when source removal runs.
                if (!(request_.operation_ == FileOperation::Move && current().source_.entry_.directory_ && !current().removeDirectory_))
                    Step.item_ = ItemOutcome{itemLocator(), Status, {}, Destination, Removed, current().source_.entry_};
                session_.reset(); stream_.reset(); chunk_ = {}; itemBytes_ = 0; destinationCommitted_ = false;
                ++index_;
                phase_ = Phase::Begin;
                if (index_ == items_.size()) Step.next_ = JobState::Completed;
                return Step;
            }
            JobStep prepare(OperationContext Context)
            {
                Context.freshEnumerationMetadata_ = true;
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
                    if (!Page.HasValue()) return failed(Page.Failure());
                    if (Page.Value().entries_.empty() && !Page.Value().end_) return failed({ErrorCategory::Internal, "cursor-no-progress"});
                    if (pending_.size() + seen_.size() + Page.Value().entries_.size() > request_.maxEntries_)
                        return failed({ErrorCategory::BudgetExceeded, "operation-entry-budget"});
                    for (auto& Entry : Page.Value().entries_)
                    {
                        if (!SafeName(Entry.name_)) return failed({ErrorCategory::InvalidRequest, "unsafe-entry-name"});
                        pending_.push_back({{Entry, enumerating_.source_.reader_, enumerating_.source_.writer_},
                            Child(enumerating_.destination_, Entry.name_), enumerating_.depth_ + 1});
                    }
                    if (Page.Value().end_) cursor_.reset();
                    return progress();
                }
                if (pending_.empty())
                {
                    for (auto It = directories_.rbegin(); It != directories_.rend(); ++It) items_.push_back(*It);
                    phase_ = request_.operation_ == FileOperation::Delete && request_.confirmDelete_ ? Phase::Confirm : Phase::Begin;
                    return progress();
                }
                auto Item = std::move(pending_.front()); pending_.pop_front();
                if (Item.depth_ > request_.maxDepth_ || seen_.size() >= request_.maxEntries_)
                    return failed({ErrorCategory::BudgetExceeded, "operation-tree-budget"});
                if (!seen_.insert(Serialize(Item.source_.entry_.locator_)).second) return failed({ErrorCategory::InvalidRequest, "overlapping-selection"});
                auto Fresh = Item.source_.reader_->Stat(Item.source_.entry_.id_, Context).get();
                if (!Fresh.HasValue()) return failed(Fresh.Failure());
                if (Fresh.Value().locator_ != Item.source_.entry_.locator_ || Fresh.Value().id_.providerId_ != Item.source_.reader_->Id()
                    || Fresh.Value().id_.key_ != Item.source_.entry_.id_.key_ || !SafeName(Fresh.Value().name_))
                    return failed({ErrorCategory::Internal, "provider-returned-different-entry"});
                Item.source_.entry_ = std::move(Fresh.Value());
                if (Item.source_.entry_.attributes_ & 0x400) return failed({ErrorCategory::Unsupported, "reparse-entry-denied"});
                if (!Item.source_.entry_.directory_)
                {
                    if (Item.source_.entry_.size_ > request_.maxBytes_ - bytesTotal_) return failed({ErrorCategory::BudgetExceeded, "operation-byte-budget"});
                    bytesTotal_ += Item.source_.entry_.size_;
                }
                if (!Item.source_.entry_.directory_ || request_.operation_ != FileOperation::Delete) items_.push_back(Item);
                if (Item.source_.entry_.directory_ && request_.operation_ != FileOperation::Rename)
                {
                    if (request_.operation_ == FileOperation::Move || request_.operation_ == FileOperation::Delete)
                    { auto Removed = Item; Removed.removeDirectory_ = true; directories_.push_back(std::move(Removed)); }
                    auto Cursor = Item.source_.reader_->Enumerate(Item.source_.entry_.locator_, Context).get();
                    if (!Cursor.HasValue()) return failed(Cursor.Failure());
                    cursor_ = std::move(Cursor.Value()); enumerating_ = std::move(Item);
                }
                return progress();
            }
            JobStep begin(OperationContext Context)
            {
                if (index_ == items_.size()) return JobStep{JobState::Completed};
                expected_ = {}; destinationCommitted_ = false;
                const bool Removing = request_.operation_ == FileOperation::Delete || current().removeDirectory_;
                const bool Renaming = request_.operation_ == FileOperation::Rename;
                auto Writer = Removing || Renaming ? current().source_.writer_ : request_.destinationWriter_;
                const auto Kind = Removing ? Mutation::Remove : Renaming ? Mutation::Rename
                    : request_.operation_ == FileOperation::MakeDirectory || current().source_.entry_.directory_ ? Mutation::MakeDirectory : Mutation::CreateContent;
                const auto Supported = Writer->Supports(Kind, Removing ? current().source_.entry_.locator_ : current().destination_, Context);
                if (!Supported.HasValue()) return failed(Supported.Failure());
                if (Supported.Value().availability_ == Availability::Unsupported)
                    return failed({ErrorCategory::Unsupported, Supported.Value().reason_, Writer->Id()});
                if (current().removeDirectory_ && request_.operation_ == FileOperation::Move)
                    for (const auto& Skipped : skippedSources_)
                        if (SameOrWithin(Skipped, current().source_.entry_.locator_)) return done(ItemOutcome::Status::Skipped, true);
                if (request_.operation_ == FileOperation::MakeDirectory) phase_ = Phase::MakeDirectory;
                else if (request_.operation_ == FileOperation::Delete || current().removeDirectory_) phase_ = Phase::Remove;
                else if (request_.operation_ == FileOperation::Rename) phase_ = Phase::Rename;
                else
                {
                    const auto Existing = request_.destinationWriter_->Inspect(current().destination_, Context);
                    if (!Existing.HasValue()) return failed(Existing.Failure());
                    if (Existing.Value())
                    {
                        if (Existing.Value()->directory_ && current().source_.entry_.directory_) return done(ItemOutcome::Status::Completed, true);
                        expected_ = {true, Existing.Value()->id_.revision_};
                        if (request_.conflict_ == ConflictPolicy::Skip && !current().source_.entry_.directory_) return done(ItemOutcome::Status::Skipped);
                        if (request_.conflict_ != ConflictPolicy::Replace || Existing.Value()->directory_ || current().source_.entry_.directory_)
                        {
                            replaceAllowed_ = !Existing.Value()->directory_ && !current().source_.entry_.directory_;
                            phase_ = Phase::Conflict;
                            JobStep Step{JobState::WaitingForUser};
                            Step.interaction_ = InteractionRequest{"", InteractionKind::Overwrite, "destination-exists",
                                {InteractionChoice::Rename, InteractionChoice::Cancel}, Existing.Value()->id_};
                            if (!current().source_.entry_.directory_) Step.interaction_->choices_.push_back(InteractionChoice::Skip);
                            if (replaceAllowed_) Step.interaction_->choices_.push_back(InteractionChoice::Replace);
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
                if (Context.answer_->choice_ == InteractionChoice::Skip) return done(ItemOutcome::Status::Skipped);
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
                if (Context.answer_->choice_ != InteractionChoice::Replace || !replaceAllowed_) return failed({ErrorCategory::InvalidRequest, "invalid-conflict-choice"});
                phase_ = Phase::OpenRead;
                return progress();
            }
            JobStep commit(OperationContext Context)
            {
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
                    return done(ItemOutcome::Status::Completed, true, true);
                }
                if (phase_ == Phase::Remove)
                {
                    // Directory revisions identify the selected object; child
                    // mutations do not invalidate that identity. Remove itself
                    // must still refuse a directory that is no longer empty.
                    const auto Removed = current().source_.writer_->Remove(current().source_.entry_, Context);
                    if (!Removed.HasValue()) return failed(Removed.Failure());
                    return done(ItemOutcome::Status::Completed, request_.operation_ == FileOperation::Move, true);
                }
                auto Receipt = session_->Commit(Context);
                if (!Receipt.HasValue()) return failed(Receipt.Failure());
                destinationCommitted_ = true;
                cleanup_.insert(cleanup_.end(), Receipt.Value().cleanupErrors_.begin(), Receipt.Value().cleanupErrors_.end());
                if (!Receipt.Value().cleanupErrors_.empty()) return failed(Receipt.Value().cleanupErrors_.front());
                if (request_.operation_ == FileOperation::Move)
                {
                    const auto Removed = current().source_.writer_->Remove(current().source_.entry_, Context);
                    if (!Removed.HasValue()) return failed(Removed.Failure());
                }
                return done(ItemOutcome::Status::Completed, true, request_.operation_ == FileOperation::Move);
            }
            FileOperationRequest request_;
            Phase phase_ = Phase::Preparing;
            std::deque<PlannedItem> pending_;
            std::vector<PlannedItem> items_, directories_;
            std::set<std::string> seen_;
            PlannedItem enumerating_;
            std::unique_ptr<IEntryCursor> cursor_;
            std::shared_ptr<IReadStream> stream_;
            std::unique_ptr<IWriteSession> session_;
            DestinationExpectation expected_;
            ReadChunk chunk_;
            std::vector<Error> cleanup_;
            std::vector<Locator> skippedSources_;
            std::size_t index_ = 0;
            std::uint64_t bytesTotal_ = 0, bytesDone_ = 0, itemBytes_ = 0;
            bool replaceAllowed_ = false, destinationCommitted_ = false;
        };
    }
    Result<PreparedJob> PrepareFileOperation(FileOperationRequest Request, OperationContext Context)
    {
        if (Request.operation_ < FileOperation::Copy || Request.operation_ > FileOperation::Rename
            || !Request.maxEntries_ || Request.maxEntries_ > 100000 || !Request.maxDepth_ || Request.maxDepth_ > 256 || !Request.maxBytes_
            || Request.sources_.size() > Request.maxEntries_ || (Request.operation_ != FileOperation::MakeDirectory && Request.sources_.empty())
            || (Request.operation_ == FileOperation::MakeDirectory && !Request.sources_.empty())
            || (Request.operation_ == FileOperation::Rename && Request.sources_.size() != 1)
            || (Request.operation_ != FileOperation::Delete && !Request.destinationWriter_))
            return Error{ErrorCategory::InvalidRequest, "invalid-file-operation"};
        JobRequest Job;
        Job.correlationId_ = Request.correlationId_; Job.operation_ = Context; Job.maxItemResults_ = Request.maxEntries_;
        if (Request.operation_ != FileOperation::Delete) Job.resourceLanes_.push_back(LaneFor(Request.destination_));
        for (const auto& Source : Request.sources_)
        {
            if (!Source.reader_ || !SafeName(Source.entry_.name_) || ((Request.operation_ == FileOperation::Delete || Request.operation_ == FileOperation::Move || Request.operation_ == FileOperation::Rename) && !Source.writer_))
                return Error{ErrorCategory::InvalidRequest, "invalid-operation-source"};
            if (Request.operation_ == FileOperation::Copy || Request.operation_ == FileOperation::Move)
            {
                const auto Target = Child(Request.destination_, Source.entry_.name_);
                if (SameOrWithin(Target, Source.entry_.locator_)) return Error{ErrorCategory::InvalidRequest, "destination-inside-source"};
            }
            Job.resourceLanes_.push_back(LaneFor(Source.entry_.locator_));
        }
        return PreparedJob{std::move(Job), std::make_unique<FileTask>(std::move(Request))};
    }
    Result<JobHandle> SubmitFileOperation(JobScheduler& Scheduler, FileOperationRequest Request, OperationContext Context)
    {
        auto Prepared = PrepareFileOperation(std::move(Request), Context); if (!Prepared.HasValue()) return Prepared.Failure();
        return Scheduler.Submit(std::move(Prepared.Value().request_), std::move(Prepared.Value().task_));
    }
}
