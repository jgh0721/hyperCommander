#include "fileOperation.hpp"
#include <set>
#include "directoryPreparation.hpp"

namespace hc::operations
{
    using namespace storage;
    namespace
    {
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
            bool removeDirectory_ = false;
            bool readOnly_ = false, excluded_ = false;
        };
        class FileTask final : public IJobTask
        {
        public:
            explicit FileTask(FileOperationRequest Request) : request_(std::move(Request))
            {
                // Ordered archive reads and unconfirmed/recycle deletion need a
                // complete plan. Native confirmed operations may start as entries
                // arrive when the user disables the optional size prescan.
                streaming_ = !request_.calculateSizeBefore_
                    && (request_.operation_ == FileOperation::Copy || request_.operation_ == FileOperation::Move
                        || (request_.operation_ == FileOperation::Delete && !request_.confirmDelete_ && request_.deleteMode_ == DeleteMode::Permanent))
                    && std::ranges::all_of(request_.sources_, [](const auto& Source) {
                        return (Source.entry_.locator_.kind_ == LocatorKind::Local || Source.entry_.locator_.kind_ == LocatorKind::Smb)
                            && !Source.reader_->UsesOrderedReads();
                    });
                for (auto& Source : request_.sources_)
                    pending_.push_back({Source, request_.operation_ == FileOperation::Rename ? request_.destination_ : Child(request_.destination_, request_.destinationName_.empty() ? Source.entry_.name_ : request_.destinationName_)});
            }
            JobStep Advance(JobStepContext Context) override
            {
                jobId_ = Context.jobId_;
                if (const auto Error = Context.operation_.Check({})) return failed(*Error);
                if (phase_ == Phase::ElevationPrompt) return permission(Context);
                if (phase_ == Phase::ElevationRun) return elevated(Context);
                if (phase_ == Phase::Preparing) return prepare(Context.operation_);
                if (phase_ == Phase::Confirm)
                {
                    if (!Context.answer_)
                    {
                        JobStep Step{JobState::WaitingForUser};
                        Step.interaction_ = InteractionRequest{"", InteractionKind::DeleteConfirmation, request_.deleteMode_ == DeleteMode::Recycle ? "confirm-recycle-delete" : "confirm-permanent-delete",
                            {InteractionChoice::Confirm, InteractionChoice::Cancel}};
                        return Step;
                    }
                    if (Context.answer_->choice_ != InteractionChoice::Confirm) return failed({ErrorCategory::InvalidRequest, "delete-confirmation-required"});
                    phase_ = Phase::Begin;
                }
                if (phase_ == Phase::ReadOnly) {
                    if (!Context.answer_) return readOnlyQuestion();
                    if (Context.answer_->choice_ == InteractionChoice::Skip) return done(ItemOutcome::Status::Skipped);
                    if (Context.answer_->choice_ != InteractionChoice::Confirm && Context.answer_->choice_ != InteractionChoice::ConfirmAll) return failed({ErrorCategory::InvalidRequest, "readonly-answer-required"});
                    if (Context.answer_->choice_ == InteractionChoice::ConfirmAll) allowAllReadOnly_ = true;
                    readOnlyAnswered_ = true; phase_ = Phase::Begin; return progress();
                }
                if (phase_ == Phase::Conflict) return conflict(Context);
                if (phase_ == Phase::Begin) return begin(Context.operation_);
                if (phase_ == Phase::OpenRead)
                {
                    auto ReadContext = Context.operation_;
                    ReadContext.denyConcurrentMutation_ = true;
                    ReadContext.orderedBatchRead_=current().source_.reader_->UsesOrderedReads();
                    auto Opened = current().source_.reader_->OpenRead(current().source_.entry_.id_, ReadContext).get();
                    if (!Opened.HasValue())
                    {
                        if (Opened.Failure().retryable_ && Context.retryAttempt_ < 3)
                        { JobStep Step{JobState::Running}; Step.error_ = Opened.Failure(); Step.retry_ = true; return Step; }
                        return failed(Opened.Failure());
                    }
                    stream_ = std::move(Opened.Value());
                    auto WriteContext = Context.operation_; WriteContext.expectedWriteBytes_ = current().source_.entry_.size_;
                    auto Created = request_.destinationWriter_->BeginCopy(current().destination_, expected_, request_.copyOptions_, WriteContext);
                    if (!Created.HasValue()) return failed(Created.Failure());
                    session_ = std::move(Created.Value());
                    auto Configured = session_->ConfigureCopy(current().source_.entry_, request_.copyOptions_, WriteContext);
                    if (!Configured.HasValue()) return failed(Configured.Failure());
                    phase_ = Phase::Read;
                    return progress();
                }
                if (phase_ == Phase::Read)
                {
                    auto Chunk = stream_->Read(request_.bytesPerSecondLimit_ ? std::min<std::size_t>(Context.operation_.maxReadBytes_, std::max<std::uint64_t>(1, request_.bytesPerSecondLimit_ / 10)) : Context.operation_.maxReadBytes_, Context.operation_).get();
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
                    phase_ = Phase::Extras;
                    return paced(chunk_.bytes_.size());
                }
                if (phase_ == Phase::Extras) {
                    auto ExtraContext = Context.operation_;
                    if (request_.bytesPerSecondLimit_) ExtraContext.maxReadBytes_ = std::min<std::size_t>(ExtraContext.maxReadBytes_, std::max<std::uint64_t>(1, request_.bytesPerSecondLimit_ / 10));
                    auto Extra = session_->AdvanceCopyExtras(request_.maxBytes_ - bytesDone_, ExtraContext);
                    if (!Extra.HasValue()) return failed(Extra.Failure());
                    bytesDone_ += Extra.Value().bytes_;
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
            std::vector<Error> Finalize(JobState) override
            {
                stream_.reset();
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
                return cleanup_;
            }
        private:
            enum class Phase { Preparing, Confirm, ReadOnly, Begin, Conflict, OpenRead, Read, Write, Extras, Verify, ReadyCommit, Commit, MakeDirectory, Remove, Rename, ElevationPrompt, ElevationRun };
            PlannedItem& current() { return items_[index_]; }
            std::string itemLocator()
            { return Serialize(current().source_.entry_.locator_.root_.empty() ? current().destination_ : current().source_.entry_.locator_); }
            JobStep progress()
            {
                JobStep Step{phase_ == Phase::Preparing ? JobState::Preparing : JobState::Running};
                Step.progress_ = JobProgress{bytesDone_, bytesTotal_, index_ < items_.size() ? current().source_.entry_.name_ : std::string{}};
                if (phase_ == Phase::Preparing || request_.copyOptions_.streams_ || !request_.calculateSizeBefore_) Step.progress_->bytesTotal_.reset();
                else Step.progress_->plannedItems_ = request_.operation_ == FileOperation::Move
                    ? std::ranges::count_if(items_, [](const auto& Item) { return !Item.source_.entry_.directory_ || Item.removeDirectory_; }) : items_.size();
                Step.progress_->currentBytes_ = itemBytes_;
                if (index_ < items_.size() && !current().source_.entry_.directory_) Step.progress_->currentTotal_ = current().source_.entry_.size_;
                return Step;
            }
            JobStep paced(std::uint64_t Bytes)
            {
                auto Step = progress();
                if (request_.bytesPerSecondLimit_ && Bytes) Step.deferFor_ = std::chrono::milliseconds((Bytes * 1000 + request_.bytesPerSecondLimit_ - 1) / request_.bytesPerSecondLimit_);
                return Step;
            }
            JobStep failed(Error Error)
            {
                if (Error.category_ == ErrorCategory::AccessDenied && request_.elevation_ && index_ < items_.size()
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
                return terminalFailure(std::move(Error));
            }
            JobStep terminalFailure(Error Error)
            {
                JobStep Step{JobState::Failed}; Step.error_ = Error;
                if (phase_ != Phase::Preparing && index_ < items_.size())
                    Step.item_ = ItemOutcome{itemLocator(), ItemOutcome::Status::Failed, Error, destinationCommitted_, false, current().source_.entry_};
                if (Step.item_ && partialOutcome_) Step.replaceItem_ = partialOutcome_;
                return Step;
            }
            JobStep done(ItemOutcome::Status Status = ItemOutcome::Status::Completed, bool Destination = false, bool Removed = false)
            {
                auto Step = progress();
                if (Status == ItemOutcome::Status::Skipped && (request_.operation_ == FileOperation::Move || request_.operation_ == FileOperation::Delete))
                    skippedSources_.push_back(current().source_.entry_.locator_);
                // Move directories produce one outcome when source removal runs.
                if (!(request_.operation_ == FileOperation::Move && current().source_.entry_.directory_ && !current().removeDirectory_))
                    Step.item_ = ItemOutcome{itemLocator(), Status, {}, Destination, Removed, current().source_.entry_};
                if (Step.item_) { if (partialOutcome_) Step.replaceItem_ = partialOutcome_; else ++outcomeIndex_; }
                partialOutcome_.reset(); publishedDestination_.reset();
                session_.reset(); stream_.reset(); chunk_ = {}; itemBytes_ = 0; destinationCommitted_ = false;
                ++index_; readOnlyAnswered_ = false;
                phase_ = Phase::Begin;
                if (index_ == items_.size()) {
                    if (streaming_ && !planningFinished_) { phase_ = Phase::Preparing; Step.next_ = JobState::Preparing; }
                    else Step.next_ = JobState::Completed;
                }
                return Step;
            }
            FileElevationRequest elevationRequest() const
            {
                FileElevationRequest Request; Request.jobId_ = jobId_;
                const auto& Item = items_[index_];
                if (!Item.source_.entry_.locator_.root_.empty()) { Request.source_ = Item.source_.entry_; Request.sourceReader_ = Item.source_.reader_; }
                Request.destination_ = Item.destination_; Request.expected_ = expected_; Request.copyOptions_ = request_.copyOptions_;
                Request.maxBytes_ = request_.maxBytes_ - (bytesDone_ - itemBytes_); Request.bytesPerSecondLimit_ = request_.bytesPerSecondLimit_;
                Request.allowReadOnlyRemoval_ = request_.readOnly_ == ReadOnlyPolicy::Allow || allowAllReadOnly_ || readOnlyAnswered_;
                if (destinationCommitted_ && request_.operation_ == FileOperation::Move) {
                    Request.action_ = FileElevationAction::Remove; Request.requiresPublishedDestination_ = true; Request.publishedDestination_ = publishedDestination_;
                }
                else if (request_.operation_ == FileOperation::Delete || Item.removeDirectory_)
                    Request.action_ = request_.operation_ == FileOperation::Delete && request_.deleteMode_ == DeleteMode::Recycle ? FileElevationAction::Recycle : FileElevationAction::Remove;
                else if (request_.operation_ == FileOperation::Rename) Request.action_ = FileElevationAction::Rename;
                else if (request_.operation_ == FileOperation::MakeDirectory || Item.source_.entry_.directory_) Request.action_ = FileElevationAction::MakeDirectory;
                else Request.action_ = FileElevationAction::Copy;
                if(request_.operation_==FileOperation::Delete||Item.removeDirectory_)Request.destination_=Item.source_.entry_.locator_;
                if(Request.action_==FileElevationAction::MakeDirectory)Request.source_.reset();
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
                    auto Step = progress(); Step.next_ = JobState::WaitingForUser;
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
                    phase_ = destinationCommitted_ ? Phase::Remove : Phase::Begin;
                    auto Step = progress(); if (destinationCommitted_) Step.next_ = JobState::Committing; return Step;
                }
                if (Choice != InteractionChoice::Elevate && Choice != InteractionChoice::ElevateAll) return terminalFailure({ErrorCategory::InvalidRequest, "invalid-elevation-answer"});
                if (Choice == InteractionChoice::ElevateAll) request_.elevationApproval_->Choose(jobId_, approvalScope(), InteractionChoice::Elevate);
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
                    auto Failed = terminalFailure(*Status.Value().error_);
                    if (Status.Value().error_->category_ == ErrorCategory::Cancelled) Failed.next_ = JobState::Cancelled;
                    return Failed;
                }
                if (elevatedRequest_.action_ == FileElevationAction::Copy && request_.operation_ == FileOperation::Move) {
                    publishedDestination_ = Status.Value().destination_;
                    phase_ = Phase::Remove; auto Next = progress(); Next.next_ = JobState::Committing; return Next;
                }
                return done(ItemOutcome::Status::Completed, destinationCommitted_, Status.Value().sourceRemoved_);
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
                    planningFinished_ = true;
                    for (auto It = directories_.rbegin(); It != directories_.rend(); ++It) items_.push_back(*It);
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
                        for (auto& [Reader,Entries]:Groups) if (Reader->UsesOrderedReads()) {
                            auto Prepared=Reader->PrepareReadBatch(std::move(Entries),Context).get(); if (!Prepared.HasValue()) return failed(Prepared.Failure());
                            const auto Owner=std::find_if(items_.begin(),items_.end(),[&](const PlannedItem& Item){return Item.source_.reader_.get()==Reader;});
                            batchReaders_.push_back(Owner->source_.reader_);
                        }
                    }
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
                    Item.excluded_ = (request_.operation_ == FileOperation::Copy || request_.operation_ == FileOperation::Move) && !Matches(Item.source_.entry_.name_, request_.includePattern_);
                    if (!Item.excluded_) {
                        if (Item.source_.entry_.size_ > request_.maxBytes_ - bytesTotal_) return failed({ErrorCategory::BudgetExceeded, "operation-byte-budget"});
                        bytesTotal_ += Item.source_.entry_.size_;
                    }
                }
                Item.readOnly_ = (Item.source_.entry_.attributes_ & 1) != 0;
                const bool Recycling = request_.operation_ == FileOperation::Delete && request_.deleteMode_ == DeleteMode::Recycle;
                if (Item.readOnly_) readOnlySources_.push_back(Item.source_.entry_.locator_);
                if (Recycling ? Item.depth_ == 0 : !Item.source_.entry_.directory_ || request_.operation_ != FileOperation::Delete) items_.push_back(Item);
                if (Item.source_.entry_.directory_ && request_.operation_ != FileOperation::Rename)
                {
                    if (request_.operation_ == FileOperation::Move || (request_.operation_ == FileOperation::Delete && !Recycling))
                    { auto Removed = Item; Removed.removeDirectory_ = true; directories_.push_back(std::move(Removed)); }
                    auto Cursor = Item.source_.reader_->Enumerate(Item.source_.entry_.locator_, Context).get();
                    if (!Cursor.HasValue()) return failed(Cursor.Failure());
                    cursor_ = std::move(Cursor.Value()); enumerating_ = std::move(Item);
                }
                if (streaming_ && index_ < items_.size()) phase_ = Phase::Begin;
                return progress();
            }
            JobStep readOnlyQuestion() {
                JobStep Step{JobState::WaitingForUser}; Step.interaction_ = InteractionRequest{"", InteractionKind::ReadOnlyRemoval, "read-only-removal",
                    {InteractionChoice::Confirm, InteractionChoice::ConfirmAll, InteractionChoice::Skip, InteractionChoice::Cancel}, current().source_.entry_.id_}; return Step;
            }
            JobStep begin(OperationContext Context)
            {
                if (index_ == items_.size()) return JobStep{JobState::Completed};
                if (current().excluded_) return done(ItemOutcome::Status::Skipped);
                expected_ = {}; destinationCommitted_ = false;
                const bool Removing = request_.operation_ == FileOperation::Delete || current().removeDirectory_;
                const bool Renaming = request_.operation_ == FileOperation::Rename;
                auto Writer = Removing || Renaming ? current().source_.writer_ : request_.destinationWriter_;
                if (Removing && request_.operation_ == FileOperation::Delete) {
                    for (const auto& At : skippedSources_) if (SameOrWithin(At, current().source_.entry_.locator_)) return done(ItemOutcome::Status::Skipped);
                    if (current().readOnly_ && request_.readOnly_ != ReadOnlyPolicy::Allow && !allowAllReadOnly_ && !readOnlyAnswered_) {
                        if (request_.readOnly_ == ReadOnlyPolicy::Skip) return done(ItemOutcome::Status::Skipped);
                        phase_ = Phase::ReadOnly; return readOnlyQuestion();
                    }
                }
                const auto Kind = Removing ? request_.operation_ == FileOperation::Delete && request_.deleteMode_ == DeleteMode::Recycle ? Mutation::Recycle : Mutation::Remove : Renaming ? Mutation::Rename
                    : request_.operation_ == FileOperation::MakeDirectory || current().source_.entry_.directory_ ? Mutation::MakeDirectory : Mutation::CreateContent;
                const auto Supported = Writer->Supports(Kind, Removing ? current().source_.entry_.locator_ : current().destination_, Context);
                if (!Supported.HasValue()) return failed(Supported.Failure());
                if (Supported.Value().availability_ == Availability::Unsupported)
                    return failed({ErrorCategory::Unsupported, Supported.Value().reason_, Writer->Id()});
                if (current().removeDirectory_ && request_.operation_ == FileOperation::Move)
                    for (const auto& Skipped : skippedSources_)
                        if (SameOrWithin(Skipped, current().source_.entry_.locator_)) return done(ItemOutcome::Status::Skipped, true);
                if (request_.operation_ == FileOperation::MakeDirectory) {
                    const auto Existing = Writer->Inspect(current().destination_, Context); if (!Existing.HasValue()) return failed(Existing.Failure());
                    if (Existing.Value()) { if (!Existing.Value()->directory_) return failed({ErrorCategory::InvalidRequest, "destination-is-not-directory", Writer->Id()}); return done(ItemOutcome::Status::Completed, true); }
                    phase_ = Phase::MakeDirectory;
                }
                else if (request_.operation_ == FileOperation::Delete || current().removeDirectory_) phase_ = Phase::Remove;
                else if (request_.operation_ == FileOperation::Rename) phase_ = Phase::Rename;
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
                    Context.allowReadOnlyRemoval_ = request_.readOnly_ == ReadOnlyPolicy::Allow || allowAllReadOnly_ || readOnlyAnswered_;
                    if (request_.operation_ == FileOperation::Delete && request_.deleteMode_ == DeleteMode::Recycle) {
                        const auto Recycled = current().source_.writer_->Recycle(current().source_.entry_, Context);
                        if (!Recycled.HasValue()) return failed(Recycled.Failure());
                        auto Step = done(ItemOutcome::Status::Completed, true, true);
                        // Preserve the returned recycle location in the human-readable
                        // item record. Native Shell restoration remains available.
                        Step.item_->item_ += "\nrecycle: " + Serialize(Recycled.Value().destination_); return Step;
                    }
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
                if (request_.operation_ == FileOperation::Move && request_.elevation_) {
                    auto Published = request_.destinationWriter_->Inspect(current().destination_, Context);
                    if (Published.HasValue() && Published.Value()) publishedDestination_ = *Published.Value();
                }
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
            std::vector<std::shared_ptr<IStorageProvider>> batchReaders_;
            std::set<std::string> seen_;
            PlannedItem enumerating_;
            std::unique_ptr<IEntryCursor> cursor_;
            std::shared_ptr<IReadStream> stream_;
            std::unique_ptr<IWriteSession> session_;
            DestinationExpectation expected_;
            ReadChunk chunk_;
            std::vector<Error> cleanup_;
            std::vector<Locator> skippedSources_, readOnlySources_;
            bool allowAllReadOnly_ = false, readOnlyAnswered_ = false;
            bool streaming_ = false, planningFinished_ = false;
            std::size_t index_ = 0;
            std::uint64_t bytesTotal_ = 0, bytesDone_ = 0, itemBytes_ = 0;
            bool replaceAllowed_ = false, destinationCommitted_ = false;
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
    // A selected directory snapshot is a lease, not an in-memory source array.
    // Each independent root is coordinated in one bounded continuation; errors
    // preserve completed roots and remain visible in the shared outcome store.
    class SelectionTask final : public IJobTask {
    public:
        explicit SelectionTask(FileOperationRequest Request) : request_(std::move(Request)) {}
        JobStep Advance(JobStepContext Context) override
        {
            if (!confirmed_ && request_.operation_ == FileOperation::Delete && request_.confirmDelete_) {
                if (!Context.answer_) {
                    JobStep Step{JobState::WaitingForUser};
                    Step.interaction_ = InteractionRequest{"", InteractionKind::DeleteConfirmation, request_.deleteMode_ == DeleteMode::Recycle ? "confirm-recycle-delete" : "confirm-permanent-delete",
                        {InteractionChoice::Confirm, InteractionChoice::Cancel}}; return Step;
                }
                if (Context.answer_->choice_ != InteractionChoice::Confirm) return failed({ErrorCategory::InvalidRequest, "delete-confirmation-required"});
                confirmed_ = true; Context.answer_.reset();
            }
            if (inner_) {
                if (Context.answer_ && Context.answer_->choice_ == InteractionChoice::ConfirmAll) request_.readOnly_ = ReadOnlyPolicy::Allow;
                auto Step = inner_->Advance(Context);
                if (Step.item_ && !Step.replaceItem_) ++emitted_;
                if (Step.replaceItem_) *Step.replaceItem_ += outcomeBase_;
                if (Step.progress_) {
                    innerBytes_ = Step.progress_->bytesDone_;
                    Step.progress_->bytesDone_ += completedBytes_; Step.progress_->bytesTotal_.reset();
                    // This is a streaming inner batch, not a complete global plan.
                    Step.progress_->plannedItems_.reset();
                }
                if (Step.next_ == JobState::Completed || Step.next_ == JobState::Failed) {
                    auto Errors = inner_->Finalize(Step.next_); cleanup_.insert(cleanup_.end(), Errors.begin(), Errors.end()); inner_.reset();
                    if (Step.next_ == JobState::Failed && !Step.item_) {
                        Step.item_ = ItemOutcome{Serialize(current_.locator_), ItemOutcome::Status::Failed, Step.error_, false, false, current_}; ++emitted_;
                    }
                    Step.next_ = JobState::Running; Context.answer_.reset();
                    completedBytes_ += innerBytes_; innerBytes_ = 0;
                }
                return Step;
            }
            if (offset_ == request_.selection_->Count()) return JobStep{JobState::Completed};
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
            auto Prepared = PrepareFileOperation(std::move(Request), Context.operation_);
            if (!Prepared.HasValue()) return failed(Prepared.Failure());
            inner_ = std::move(Prepared.Value().task_); outcomeBase_ = emitted_;
            return JobStep{JobState::Running};
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
        size_t emitted_ = 0, outcomeBase_ = 0;
        bool confirmed_ = false;
        std::vector<Error> cleanup_;
    };
    }
    Result<PreparedJob> PrepareFileOperation(FileOperationRequest Request, OperationContext Context)
    {
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
            if ((Request.copyOptions_.verify_ && !Support.verify_) || (Request.copyOptions_.acl_ && !Support.acl_) || (Request.copyOptions_.streams_ && !Support.streams_))
                return Error{ErrorCategory::Unsupported, "copy-options-unavailable", Request.destinationWriter_->Id()};
        }
        if (Request.selection_) {
            if (!Request.sources_.empty() || !Request.selection_->Provider() || !Request.selection_->Count()
                || (Request.operation_ != FileOperation::Copy && Request.operation_ != FileOperation::Move && Request.operation_ != FileOperation::Delete)
                || (Request.operation_ != FileOperation::Copy && !Request.selectionWriter_)
                || (Request.operation_ != FileOperation::Delete && !Request.destinationWriter_))
                return Error{ErrorCategory::InvalidRequest, "invalid-selection-batch"};
            JobRequest Job; Job.correlationId_ = Request.correlationId_; Job.operation_ = Context;
            Job.resourceLanes_.push_back(LaneFor(Request.selectionRoot_));
            if (Request.operation_ != FileOperation::Delete) Job.resourceLanes_.push_back(LaneFor(Request.destination_));
            return PreparedJob{std::move(Job), std::make_unique<SelectionTask>(std::move(Request))};
        }
        if (Request.operation_ < FileOperation::Copy || Request.operation_ > FileOperation::Rename
            || !Request.maxEntries_ || !Request.maxDepth_ || !Request.maxBytes_
            || Request.sources_.size() > Request.maxEntries_ || (Request.operation_ != FileOperation::MakeDirectory && Request.sources_.empty())
            || (Request.operation_ == FileOperation::MakeDirectory && !Request.sources_.empty())
            || (Request.operation_ == FileOperation::Rename && Request.sources_.size() != 1)
            || (Request.operation_ != FileOperation::Delete && !Request.destinationWriter_))
            return Error{ErrorCategory::InvalidRequest, "invalid-file-operation"};
        JobRequest Job;
        Job.correlationId_ = Request.correlationId_; Job.operation_ = Context; Job.maxItemResults_ = Request.maxEntries_;
        if (!Request.calculateSizeBefore_) {
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
