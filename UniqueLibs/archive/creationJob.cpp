#include "stdafx.h"
#include "creationJob.hpp"
#include "UniqueLibs/nativeFilePath.hpp"
#include "creationSettings.hpp"

#include "UniqueLibs/nativeWritableStorage.hpp"
#include "UniqueLibs/storageProviders.hpp"

namespace hc::archive {
using namespace storage;
using namespace operations;
namespace {
class CreateTask final : public IJobTask {
public:
    explicit CreateTask(CreateJobRequest Request) : request_(std::move(Request)) {}
    JobStep Advance(JobStepContext Context) override
    {
        auto Step = advance(std::move(Context));
        if (!Step.progress_) Step.progress_ = JobProgress{};
        present(*Step.progress_);
        if (Step.next_ == JobState::Completed) {
            Step.progress_->phase_ = JobPhase::Committing;
            Step.progress_->phaseItemsDone_.reset(); Step.progress_->phaseItemsTotal_.reset();
            if (!outputs_.empty()) {
                Step.progress_->source_ = Serialize(outputs_.back().source_);
                Step.progress_->destination_ = Serialize(outputs_.back().destination_);
            }
        }
        return Step;
    }
    void present(JobProgress& Progress) const
    {
        Progress.phase_ = phase_ == Phase::Build ? JobPhase::BuildingArchive : phase_ == Phase::Save ? JobPhase::SavingOutput
            : phase_ == Phase::Recheck ? JobPhase::Verifying : phase_ == Phase::Commit ? JobPhase::Committing : JobPhase::PreparingInputs;
        if (!outputs_.empty()) { Progress.outputIndex_ = group_; Progress.outputCount_ = outputs_.size(); }
        if (!outputs_.empty()) Progress.plannedItems_ = outputs_.size();
        if (group_ < outputs_.size()) {
            const auto& Plan = outputs_[group_];
            if (!Plan.source_.root_.empty()) Progress.source_ = Serialize(Plan.source_);
            Progress.destination_ = Serialize(Plan.destination_);
            if (phase_ == Phase::PrepareInputs || phase_ == Phase::DirectoryMetadata) {
                const auto Done = phase_ == Phase::DirectoryMetadata ? Plan.entries_.size() : std::min(index_, Plan.entries_.size());
                Progress.phaseItemsDone_ = Done; Progress.phaseItemsTotal_ = Plan.entries_.size();
                if (inputBytes_.size() == Plan.entries_.size() + 1) {
                    Progress.bytesDone_ = inputBytes_[Done] + (copy_ ? Progress.currentBytes_ : 0);
                    if(inputBytes_.back())Progress.bytesTotal_=inputBytes_.back();else Progress.bytesTotal_.reset();
                }
            } else if (phase_ == Phase::Recheck) {
                Progress.phaseItemsDone_ = index_; Progress.phaseItemsTotal_ = Plan.entries_.size() + Plan.directories_.size();
            }
        }
        if (Progress.phase_ == JobPhase::SavingOutput && Progress.bytesTotal_) {
            Progress.currentBytes_ = Progress.bytesDone_; Progress.currentTotal_ = Progress.bytesTotal_;
        }
    }
    JobStep advance(JobStepContext Context)
    {
        if (const auto Stop = Context.operation_.Check({"archive-create"})) return failed(*Stop);
        if (phase_ == Phase::Admission) {
            auto Checked = RevalidateCreator(request_.creator_, Context.operation_); if (!Checked.HasValue()) return failed(Checked.Failure());
            PlanRequest Plan{request_.sources_, request_.selection_, request_.value_.sourceRoot_, request_.value_.output_,
                request_.value_.format_, request_.value_.composition_, request_.completedOutputs_, request_.value_.outputNames_};
            planner_ = std::make_unique<CreationPlanner>(std::move(Plan)); phase_ = Phase::Plan;
        }
        if (phase_ == Phase::Plan) {
            auto Step = planner_->Advance(Context); if (Step.next_ != JobState::Completed) return Step;
            outputs_ = planner_->Take(); planner_.reset(); phase_ = Phase::Initialize;
        }
        if (group_ >= outputs_.size()) return JobStep{JobState::Completed};
        auto& Plan = outputs_[group_];
        if (phase_ == Phase::Initialize) {
            if (Plan.entries_.empty() && Plan.skippedEmpty_) { auto Step = progress("출력 건너뛰기"); Step.item_ = ItemOutcome{Serialize(Plan.destination_), ItemOutcome::Status::Skipped}; ++group_; return Step; }
            auto Reader = MakeNativeReadProvider(Plan.destination_.kind_); writer_ = MakeNativeWritableStorage(Plan.destination_.kind_, Reader);
            auto Existing = writer_->Inspect(Plan.destination_, Context.operation_); if (!Existing.HasValue()) return failed(Existing.Failure());
            expected_ = {};
            if (Existing.Value()) {
                if (Existing.Value()->directory_) return failed({ErrorCategory::InvalidRequest, "archive-create-output-is-directory"});
                expected_ = {true, Existing.Value()->id_.revision_}; phase_ = Phase::Conflict;
                JobStep Step{JobState::WaitingForUser}; Step.interaction_ = InteractionRequest{"", InteractionKind::Overwrite,
                    "기존 파일을 작성·검증 후 교체합니다.", {InteractionChoice::Replace, InteractionChoice::Skip, InteractionChoice::Cancel},
                    Existing.Value()->id_, {}, Plan.destination_.root_}; return Step;
            }
            phase_ = Phase::CreateStage;
        }
        if (phase_ == Phase::Conflict) {
            if (!Context.answer_ || Context.answer_->choice_ == InteractionChoice::Cancel) return failed({ErrorCategory::Cancelled, "archive-create-replacement-cancelled"});
            if (Context.answer_->choice_ == InteractionChoice::Skip) { auto Step = progress("출력 건너뛰기"); Step.item_ = ItemOutcome{Serialize(Plan.destination_), ItemOutcome::Status::Skipped}; ++group_; phase_ = Phase::Initialize; return Step; }
            if (Context.answer_->choice_ != InteractionChoice::Replace) return failed({ErrorCategory::InvalidRequest, "archive-create-invalid-answer"});
            phase_ = Phase::CreateStage;
        }
        if (phase_ == Phase::CreateStage) {
            const auto DestinationParent = QFileInfo(QString::fromStdString(Plan.destination_.root_)).absolutePath();
            temp_ = std::make_shared<QTemporaryDir>(QDir(DestinationParent).filePath(".hc-archive-XXXXXX"));
            if (!temp_->isValid() || !QDir().mkdir(temp_->filePath("input"))) return failed({ErrorCategory::Unavailable, "archive-create-staging-directory"});
            Plan.inputPaths_.clear(); Plan.inputDirectory_.clear(); currentPaths_.clear(); activeInput_.clear();
            const auto Builtin = request_.creator_.descriptor_.kind_ == plugins::ArchiveCreatorKind::BuiltinSevenZip
                || request_.creator_.descriptor_.kind_ == plugins::ArchiveCreatorKind::BuiltinIso;
            QString CommonRoot; bool NativeTree = request_.value_.composition_.includeSubdirectories_;
            for (const auto& Entry : Plan.entries_) {
                const auto& At = Entry.source_.entry_.locator_;
                const bool Native = At.nesting_.empty() && (At.kind_ == LocatorKind::Local || At.kind_ == LocatorKind::Smb);
                const auto Path = QDir::cleanPath(QDir::fromNativeSeparators(QString::fromStdString(At.root_)));
                auto Root = Path;
                const auto Tail = '/' + Entry.member_;
                if (!Native || !Path.endsWith(Tail, Qt::CaseInsensitive)) NativeTree = false;
                else {
                    Root.chop(Tail.size()); if (Root.endsWith(':')) Root += '/';
                    if (CommonRoot.isEmpty()) CommonRoot = Root;
                    else if (CommonRoot.compare(Root, Qt::CaseInsensitive)) NativeTree = false;
                }
                Plan.inputPaths_.push_back(Builtin && Native ? Path : QDir(temp_->filePath("input")).filePath(Entry.member_));
                currentPaths_[Entry.member_.toStdString()] = Native ? At.root_ : Serialize(At);
            }
            if (!Builtin && NativeTree && !CommonRoot.isEmpty()) {
                Plan.inputDirectory_ = CommonRoot;
                for (size_t Index = 0; Index < Plan.entries_.size(); ++Index)
                    Plan.inputPaths_[Index] = QDir::fromNativeSeparators(QString::fromStdString(Plan.entries_[Index].source_.entry_.locator_.root_));
            }
            inputBytes_ = {0};
            for (size_t Index=0;Index<Plan.entries_.size();++Index){
                const auto& Entry=Plan.entries_[Index];
                const bool CopyInput=Plan.inputPaths_[Index]==QDir(temp_->filePath("input")).filePath(Entry.member_);
                inputBytes_.push_back(inputBytes_.back()+(CopyInput&&!Entry.source_.entry_.directory_?Entry.source_.entry_.size_:0));
            }
            index_ = 0; phase_ = Phase::PrepareInputs; return progress("입력 파일 준비");
        }
        if (phase_ == Phase::PrepareInputs) {
            if (copy_) {
                auto Inner = Context; Inner.committing_ = innerCommitting_; Inner.operation_.writeCheckpoint_ = {};
                auto Step = copy_->Advance(Inner); innerCommitting_ = Step.next_ == JobState::Committing; Step.item_.reset();
                if (Step.next_ == JobState::Completed) {
                    auto Errors = copy_->Finalize(JobState::Completed); copy_.reset(); if (!Errors.empty()) return failed(Errors.front());
                    ++index_; return progress("입력 파일 준비");
                }
                if (Step.next_ == JobState::Failed || Step.next_ == JobState::Cancelled) return Step;
                if (Step.next_ == JobState::Committing) Step.next_ = JobState::Running;
                if (Step.progress_ && index_ < Plan.entries_.size()) Step.progress_->currentItem_ = currentPaths_[Plan.entries_[index_].member_.toStdString()]; return Step;
            }
            while (index_ < Plan.entries_.size()) {
                const auto& Entry = Plan.entries_[index_]; const auto Path = QDir(temp_->filePath("input")).filePath(Entry.member_);
                if (Plan.inputPaths_[index_] != Path) { ++index_; continue; }
                if (Entry.source_.entry_.directory_) {
                    if (!QDir().mkpath(Path)) return failed({ErrorCategory::Unavailable, "archive-create-staging-mkdir"}); ++index_; return progress("입력 파일 준비");
                }
                FileOperationRequest Copy; Copy.sources_ = {Entry.source_};
                Copy.destination_ = {Plan.destination_.kind_, QFileInfo(Path).absolutePath().toStdString()}; Copy.destinationName_ = QFileInfo(Path).fileName().toStdString();
                Copy.destinationWriter_ = MakeNativeWritableStorage(Plan.destination_.kind_, MakeNativeReadProvider(Plan.destination_.kind_));
                Copy.createParents_ = true; Copy.maxDepth_ = 1;
                auto Prepared = PrepareFileOperation(std::move(Copy), Context.operation_); if (!Prepared.HasValue()) return failed(Prepared.Failure());
                copy_ = std::move(Prepared.Value().task_); innerCommitting_ = false; return progress("입력 파일 준비");
            }
            index_ = Plan.entries_.size(); phase_ = Phase::DirectoryMetadata;
        }
        if (phase_ == Phase::DirectoryMetadata) {
            while (index_) {
                const auto& Entry = Plan.entries_[--index_]; if (!Entry.source_.entry_.directory_) continue;
                if (Plan.inputPaths_[index_] != QDir(temp_->filePath("input")).filePath(Entry.member_)) continue;
                const auto Path = QDir::toNativeSeparators(QDir(temp_->filePath("input")).filePath(Entry.member_));
                const auto Handle = CreateFileW(reinterpret_cast<LPCWSTR>(hc::storage::native::FilePath(Path).utf16()), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
                if (Handle == INVALID_HANDLE_VALUE) return failed({ErrorCategory::Unavailable, "archive-create-directory-metadata-open"});
                const auto Ticks = uint64_t(Entry.source_.entry_.modifiedUtcMs_ + 11644473600000LL) * 10000; FILETIME Time{DWORD(Ticks), DWORD(Ticks >> 32)};
                const bool Saved = SetFileTime(Handle, nullptr, nullptr, &Time); CloseHandle(Handle);
                if (!Saved || !SetFileAttributesW(reinterpret_cast<LPCWSTR>(Path.utf16()), FILE_ATTRIBUTE_DIRECTORY | (Entry.source_.entry_.attributes_ & (FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_ARCHIVE))))
                    return failed({ErrorCategory::Unavailable, "archive-create-directory-metadata"});
                return progress("입력 폴더 속성 보존");
            }
            phase_ = Phase::Build; return progress("압축 생성");
        }
        if (phase_ == Phase::Build) {
            auto Checked = RevalidateCreator(request_.creator_, Context.operation_); if (!Checked.HasValue()) return failed(Checked.Failure());
            auto Operation = Context.operation_;
            Operation.reportProgress_ = [this, Publish = Context.publishProgress_, Previous = Operation.reportProgress_](const OperationContext::Progress& Value) {
                if (Previous) Previous(Value);
                if (!Publish) return;
                JobProgress Progress;
                Progress.bytesDone_ = Value.bytesDone_; Progress.bytesTotal_ = Value.bytesTotal_;
                if (Value.itemsTotal_) { Progress.phaseItemsDone_ = Value.itemsDone_; Progress.phaseItemsTotal_ = Value.itemsTotal_; }
                if (!Value.currentItem_.empty()) {
                    const auto Found = currentPaths_.find(Value.currentItem_);
                    activeInput_ = Found == currentPaths_.end() ? Value.currentItem_ : Found->second;
                }
                Progress.currentItem_ = activeInput_; present(Progress);
                if (Value.verifying_) Progress.phase_ = JobPhase::Verifying;
                Publish(Progress);
            };
            if (Operation.passwordPrompt_) Operation.passwordPrompt_ = [Prompt = Operation.passwordPrompt_, Target = Plan.destination_.root_](const std::string&, unsigned Attempt, const OperationContext& C) { return Prompt(Target, Attempt, C); };
            if (Operation.interactionStart_) Operation.interactionStart_ = [Prompt = Operation.interactionStart_, Target = Plan.destination_.root_](ProviderInteractionRequest R, const OperationContext& C) { R.subject_ = Target; return Prompt(R, C); };
            if (Operation.interactionPrompt_) Operation.interactionPrompt_ = [Prompt = Operation.interactionPrompt_, Target = Plan.destination_.root_](ProviderInteractionRequest R, const OperationContext& C) { R.subject_ = Target; return Prompt(R, C); };
            auto Prepared = CreatePreparedArchive(request_.creator_, request_.value_.settings_, request_.value_.format_, Plan, temp_->filePath("input"), temp_->path(), Operation);
            if (!Prepared.HasValue()) return failed(Prepared.Failure()); prepared_ = std::move(Prepared.Value());
            auto WriteContext = Context.operation_; WriteContext.expectedWriteBytes_ = uint64_t(QFileInfo(prepared_.path_).size());
            WriteContext.producingContent_ = true;
            auto Session = writer_->BeginWrite(Plan.destination_, expected_, WriteContext); if (!Session.HasValue()) return failed(Session.Failure());
            output_ = std::move(Session.Value());
            auto Adopted = output_->AdoptContent({prepared_.path_.toStdString(), temp_}, WriteContext);
            if (!Adopted.HasValue()) return failed(Adopted.Failure());
            index_ = 0; phase_ = Phase::Recheck; return progress("입력 변경 여부 확인");
        }
        if (phase_ == Phase::Save) {
            const auto Bytes = file_->read(1024 * 1024);
            if (Bytes.isEmpty() && !file_->atEnd()) return failed({ErrorCategory::Unavailable, "archive-create-prepared-read"});
            if (!Bytes.isEmpty()) {
                auto Written = output_->Append({reinterpret_cast<const std::byte*>(Bytes.constData()), size_t(Bytes.size())}, Context.operation_); if (!Written.HasValue()) return failed(Written.Failure());
                auto Step = progress("대상 볼륨에 저장"); Step.progress_->bytesDone_ = uint64_t(file_->pos()); Step.progress_->bytesTotal_ = uint64_t(file_->size()); return Step;
            }
            file_.reset(); index_ = 0; phase_ = Phase::Recheck;
        }
        if (phase_ == Phase::Recheck) {
            const OperationSource* Source = nullptr;
            if (index_ < Plan.entries_.size()) Source = &Plan.entries_[index_].source_;
            else if (index_ - Plan.entries_.size() < Plan.directories_.size()) Source = &Plan.directories_[index_ - Plan.entries_.size()];
            if (Source) {
                ++index_; auto Fresh = Source->reader_->Stat(Source->entry_.id_, Context.operation_).get(); if (!Fresh.HasValue()) return failed(Fresh.Failure());
                if (Fresh.Value().id_ != Source->entry_.id_) return failed({ErrorCategory::StaleRevision, "archive-create-source-changed-before-commit"});
                return progress("입력 변경 여부 확인");
            }
            phase_ = Phase::Commit; return JobStep{JobState::Committing};
        }
        if (!Context.committing_) return failed({ErrorCategory::Internal, "archive-create-commit-boundary"});
        auto Committed = output_->Commit(Context.operation_); if (!Committed.HasValue()) return failed(Committed.Failure());
        cleanup_.insert(cleanup_.end(), Committed.Value().cleanupErrors_.begin(), Committed.Value().cleanupErrors_.end());
        auto Step = progress("출력 생성 완료"); Step.item_ = ItemOutcome{Serialize(Plan.destination_), ItemOutcome::Status::Completed}; Step.item_->destinationCommitted_ = true;
        output_.reset(); prepared_ = {}; if (!temp_->remove()) cleanup_.push_back({ErrorCategory::Unavailable, "archive-create-staging-cleanup:" + temp_->path().toStdString()}); temp_.reset();
        ++group_; phase_ = Phase::Initialize;
        if (!cleanup_.empty()) { Step.next_ = JobState::Failed; Step.error_ = cleanup_.front(); }
        else if (group_ == outputs_.size()) Step.next_ = JobState::Completed; return Step;
    }
    std::vector<Error> Finalize(JobState State) override
    {
        file_.reset(); planner_.reset();
        if (copy_) { auto Errors = copy_->Finalize(State); cleanup_.insert(cleanup_.end(), Errors.begin(), Errors.end()); copy_.reset(); }
        if (output_) { auto Aborted = output_->Abort(); if (!Aborted.HasValue()) cleanup_.push_back(Aborted.Failure()); output_.reset(); }
        prepared_ = {}; if (temp_) { if (!temp_->remove()) { temp_->setAutoRemove(false); cleanup_.push_back({ErrorCategory::Unavailable, "archive-create-staging-cleanup:" + temp_->path().toStdString()}); } temp_.reset(); }
        return cleanup_;
    }
private:
    enum class Phase { Admission, Plan, Initialize, Conflict, CreateStage, PrepareInputs, DirectoryMetadata, Build, Save, Recheck, Commit };
    JobStep failed(Error Error) { JobStep Step{JobState::Failed}; Step.error_ = std::move(Error); return Step; }
    JobStep progress(const char*) const {
        JobStep Step{JobState::Running};
        Step.progress_ = JobProgress{};
        if(group_<outputs_.size()){
            const auto& Plan=outputs_[group_];
            if(phase_==Phase::Build)Step.progress_->currentItem_=activeInput_;
            else if(phase_==Phase::Commit||phase_==Phase::Save)Step.progress_->currentItem_=Plan.destination_.root_;
            else if(phase_==Phase::Recheck&&index_&&index_<=Plan.entries_.size())Step.progress_->currentItem_=currentPaths_.at(Plan.entries_[index_-1].member_.toStdString());
            else if(index_<Plan.entries_.size())Step.progress_->currentItem_=currentPaths_.at(Plan.entries_[index_].member_.toStdString());
        }
        return Step;
    }
    CreateJobRequest request_; Phase phase_ = Phase::Admission;
    std::unique_ptr<CreationPlanner> planner_; std::vector<OutputPlan> outputs_; size_t group_ = 0, index_ = 0;
    std::shared_ptr<QTemporaryDir> temp_; std::unique_ptr<IJobTask> copy_; bool innerCommitting_ = false;
    std::map<std::string, std::string> currentPaths_; std::string activeInput_;
    DestinationExpectation expected_; std::shared_ptr<IWritableStorage> writer_; std::unique_ptr<IWriteSession> output_;
    PreparedArchiveOutput prepared_; std::unique_ptr<QFile> file_; std::vector<Error> cleanup_;
    std::vector<uint64_t> inputBytes_;
};
}
Result<PreparedJob> PrepareCreation(CreateJobRequest Request, OperationContext Context)
{
    auto Checked = plugins::ValidateArchiveCreateSelection(Request.creator_.descriptor_, Request.value_.creator_.identity_, Request.value_.format_); if (!Checked.HasValue()) return Checked.Failure();
    if (!Request.creator_.available_ || Request.creator_.descriptor_.id_ != Request.value_.creator_.id_) return Error{ErrorCategory::Unavailable, "archive-create-creator-unavailable"};
    if ((!Request.selection_ || !Request.selection_->Count()) && Request.sources_.empty()) return Error{ErrorCategory::InvalidRequest, "archive-create-empty-selection"};
    Checked = plugins::ValidateArchiveCreateSelection(Request.value_.creator_, Request.creator_.descriptor_.identity_, Request.value_.format_); if (!Checked.HasValue()) return Checked.Failure();
    const auto Kind = Request.creator_.descriptor_.kind_;
    if ((Kind == plugins::ArchiveCreatorKind::BuiltinSevenZip && !std::holds_alternative<plugins::SevenZipCreateSettings>(Request.value_.settings_))
        || (Kind == plugins::ArchiveCreatorKind::BuiltinIso && !std::holds_alternative<plugins::IsoCreateSettings>(Request.value_.settings_))
        || ((Kind == plugins::ArchiveCreatorKind::Wcx || Kind == plugins::ArchiveCreatorKind::DopusVfs) && !std::holds_alternative<plugins::ExternalArchiveCreateSettings>(Request.value_.settings_)))
        return Error{ErrorCategory::InvalidRequest, "archive-create-settings-kind-mismatch"};
    if (const auto* Settings = std::get_if<plugins::SevenZipCreateSettings>(&Request.value_.settings_)) { auto Valid = plugins::ValidateSevenZipSettings(Request.value_.format_, *Settings); if (!Valid.HasValue()) return Valid.Failure(); }
    if (const auto* Settings = std::get_if<plugins::IsoCreateSettings>(&Request.value_.settings_)) { auto Valid = ValidateIsoSettings(*Settings); if (!Valid.HasValue()) return Valid.Failure(); }
    auto Valid = ValidateOutputLocation(Request.value_.output_, Request.sources_); if (!Valid.HasValue()) return Valid.Failure();
    Request.value_.composition_.omitSelectedFolderRoot_ = Request.value_.composition_.OmitSelectedFolderRoot();
    JobRequest Job; Job.correlationId_ = Request.value_.correlationId_; Job.operation_ = Context;
    Job.deferred_ = Request.value_.schedule_ == plugins::ArchiveCreateSchedule::Queued;
    Job.resourceLanes_ = {FileOperationLane(Request.value_.output_)};
    if (Request.selection_) Job.resourceLanes_.push_back(FileOperationLane(Request.value_.sourceRoot_));
    for (const auto& Source : Request.sources_) Job.resourceLanes_.push_back(FileOperationLane(Source.entry_.locator_));
    // External creators may share their mutable INI/package settings. Built-ins
    // create independent codec sessions and can run on disjoint paths together.
    if (Kind == plugins::ArchiveCreatorKind::Wcx || Kind == plugins::ArchiveCreatorKind::DopusVfs)
        Job.resourceLanes_.push_back("archive-creator:" + Request.value_.creator_.id_);
    Job.metadata_.destination_ = Request.value_.output_.root_; Job.metadata_.options_ = Request.value_.format_ + " · " + Request.value_.creator_.displayName_;
    Job.metadata_.source_ = Request.sources_.size() == 1 ? Serialize(Request.sources_.front().entry_.locator_) : Request.value_.sourceRoot_.root_;
    Job.metadata_.kind_ = Request.value_.composition_.separateArchives_ ? JobKind::ArchiveSeparate
        : Request.value_.format_ == "iso" ? JobKind::IsoCreate : JobKind::ArchiveCreate;
    return PreparedJob{std::move(Job), std::make_unique<CreateTask>(std::move(Request))};
}
CreateJobRequest RetryCreation(CreateJobRequest Request, const JobResult& Result)
{
    for (const auto& Item : Result.items_) if (Item.destinationCommitted_) Request.completedOutputs_.insert(Item.item_);
    Request.value_.schedule_ = plugins::ArchiveCreateSchedule::Immediate; Request.value_.correlationId_.clear(); return Request;
}
}
