#include "stdafx.h"
#include "creationJob.hpp"
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
        if (const auto Stop = Context.operation_.Check({"archive-create"})) return failed(*Stop);
        if (phase_ == Phase::Admission) {
            auto Checked = RevalidateCreator(request_.creator_, Context.operation_); if (!Checked.HasValue()) return failed(Checked.Failure());
            PlanRequest Plan{request_.sources_, request_.selection_, request_.value_.sourceRoot_, request_.value_.output_,
                request_.value_.format_, request_.value_.composition_, request_.completedOutputs_};
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
            temp_ = std::make_unique<QTemporaryDir>(QDir::tempPath() + "/hc-archive-create-XXXXXX");
            if (!temp_->isValid() || !QDir().mkdir(temp_->filePath("input"))) return failed({ErrorCategory::Unavailable, "archive-create-staging-directory"});
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
                if (Step.progress_) Step.progress_->currentItem_ = "입력 준비: " + Step.progress_->currentItem_; return Step;
            }
            if (index_ < Plan.entries_.size()) {
                const auto& Entry = Plan.entries_[index_]; const auto Path = QDir(temp_->filePath("input")).filePath(Entry.member_);
                if (Entry.source_.entry_.directory_) {
                    if (!QDir().mkpath(Path)) return failed({ErrorCategory::Unavailable, "archive-create-staging-mkdir"}); ++index_; return progress("입력 파일 준비");
                }
                FileOperationRequest Copy; Copy.sources_ = {Entry.source_};
                Copy.destination_ = {LocatorKind::Local, QFileInfo(Path).absolutePath().toStdString()}; Copy.destinationName_ = QFileInfo(Path).fileName().toStdString();
                Copy.destinationWriter_ = MakeNativeWritableStorage(LocatorKind::Local, MakeNativeReadProvider(LocatorKind::Local));
                Copy.createParents_ = true; Copy.maxDepth_ = 1;
                auto Prepared = PrepareFileOperation(std::move(Copy), Context.operation_); if (!Prepared.HasValue()) return failed(Prepared.Failure());
                copy_ = std::move(Prepared.Value().task_); innerCommitting_ = false; return progress("입력 파일 준비");
            }
            index_ = Plan.entries_.size(); phase_ = Phase::DirectoryMetadata;
        }
        if (phase_ == Phase::DirectoryMetadata) {
            while (index_) {
                const auto& Entry = Plan.entries_[--index_]; if (!Entry.source_.entry_.directory_) continue;
                const auto Path = QDir::toNativeSeparators(QDir(temp_->filePath("input")).filePath(Entry.member_));
                const auto Handle = CreateFileW(reinterpret_cast<LPCWSTR>(Path.utf16()), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
                if (Handle == INVALID_HANDLE_VALUE) return failed({ErrorCategory::Unavailable, "archive-create-directory-metadata-open"});
                const auto Ticks = uint64_t(Entry.source_.entry_.modifiedUtcMs_ + 11644473600000LL) * 10000; FILETIME Time{DWORD(Ticks), DWORD(Ticks >> 32)};
                const bool Saved = SetFileTime(Handle, nullptr, nullptr, &Time); CloseHandle(Handle);
                if (!Saved || !SetFileAttributesW(reinterpret_cast<LPCWSTR>(Path.utf16()), FILE_ATTRIBUTE_DIRECTORY | (Entry.source_.entry_.attributes_ & (FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_ARCHIVE))))
                    return failed({ErrorCategory::Unavailable, "archive-create-directory-metadata"});
                return progress("입력 폴더 속성 보존");
            }
            phase_ = Phase::Build;
        }
        if (phase_ == Phase::Build) {
            auto Checked = RevalidateCreator(request_.creator_, Context.operation_); if (!Checked.HasValue()) return failed(Checked.Failure());
            auto Operation = Context.operation_;
            if (Operation.passwordPrompt_) Operation.passwordPrompt_ = [Prompt = Operation.passwordPrompt_, Target = Plan.destination_.root_](const std::string&, unsigned Attempt, const OperationContext& C) { return Prompt(Target, Attempt, C); };
            if (Operation.interactionStart_) Operation.interactionStart_ = [Prompt = Operation.interactionStart_, Target = Plan.destination_.root_](ProviderInteractionRequest R, const OperationContext& C) { R.subject_ = Target; return Prompt(R, C); };
            if (Operation.interactionPrompt_) Operation.interactionPrompt_ = [Prompt = Operation.interactionPrompt_, Target = Plan.destination_.root_](ProviderInteractionRequest R, const OperationContext& C) { R.subject_ = Target; return Prompt(R, C); };
            auto Prepared = CreatePreparedArchive(request_.creator_, request_.value_.settings_, request_.value_.format_, Plan, temp_->filePath("input"), temp_->path(), Operation);
            if (!Prepared.HasValue()) return failed(Prepared.Failure()); prepared_ = std::move(Prepared.Value());
            auto WriteContext = Context.operation_; WriteContext.expectedWriteBytes_ = uint64_t(QFileInfo(prepared_.path_).size());
            auto Session = writer_->BeginWrite(Plan.destination_, expected_, WriteContext); if (!Session.HasValue()) return failed(Session.Failure());
            output_ = std::move(Session.Value()); file_ = std::make_unique<QFile>(prepared_.path_);
            if (!file_->open(QIODevice::ReadOnly)) return failed({ErrorCategory::Unavailable, "archive-create-prepared-open"});
            phase_ = Phase::Save; return progress("대상 볼륨에 저장");
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
    JobStep progress(const char* Message) const {
        JobStep Step{JobState::Running}; const auto Name = group_ < outputs_.size() ? QFileInfo(QString::fromStdString(outputs_[group_].destination_.root_)).fileName().toStdString() : std::string{};
        Step.progress_ = JobProgress{0, {}, std::to_string(group_) + "/" + std::to_string(outputs_.size()) + " · " + Name + " · " + Message}; return Step;
    }
    CreateJobRequest request_; Phase phase_ = Phase::Admission;
    std::unique_ptr<CreationPlanner> planner_; std::vector<OutputPlan> outputs_; size_t group_ = 0, index_ = 0;
    std::unique_ptr<QTemporaryDir> temp_; std::unique_ptr<IJobTask> copy_; bool innerCommitting_ = false;
    DestinationExpectation expected_; std::shared_ptr<IWritableStorage> writer_; std::unique_ptr<IWriteSession> output_;
    PreparedArchiveOutput prepared_; std::unique_ptr<QFile> file_; std::vector<Error> cleanup_;
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
    Job.resourceLanes_ = {FileOperationLane(Request.value_.output_), "archive-creator:" + Request.value_.creator_.id_};
    Job.metadata_.destination_ = Request.value_.output_.root_; Job.metadata_.options_ = Request.value_.format_ + " · " + Request.value_.creator_.displayName_;
    return PreparedJob{std::move(Job), std::make_unique<CreateTask>(std::move(Request))};
}
CreateJobRequest RetryCreation(CreateJobRequest Request, const JobResult& Result)
{
    for (const auto& Item : Result.items_) if (Item.destinationCommitted_) Request.completedOutputs_.insert(Item.item_);
    Request.value_.schedule_ = plugins::ArchiveCreateSchedule::Immediate; Request.value_.correlationId_.clear(); return Request;
}
}
