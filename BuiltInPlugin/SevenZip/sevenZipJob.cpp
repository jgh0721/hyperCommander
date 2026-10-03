#include "sevenZipJob.hpp"
#include "builtinSevenZip.hpp"

#include "UniqueLibs/nativeWritableStorage.hpp"
#include "UniqueLibs/storageProviders.hpp"
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#define NOMINMAX
#include <Windows.h>

namespace hc::sevenzip {
using namespace hc::storage;
using namespace hc::operations;
namespace {
class CreateTask final : public IJobTask {
public:
    explicit CreateTask(CreateRequest Request) : request_(std::move(Request)) {}
    JobStep Advance(JobStepContext Context) override
    {
        if (const auto Failure = Context.operation_.Check({"sevenzip"})) return failed(*Failure);
        if (phase_ == Phase::Initialize) {
            auto Reader = MakeNativeReadProvider(request_.destination_.kind_);
            writer_ = MakeNativeWritableStorage(request_.destination_.kind_, Reader);
            auto Existing = writer_->Inspect(request_.destination_, Context.operation_); if (!Existing.HasValue()) return failed(Existing.Failure());
            if (Existing.Value()) {
                if (Existing.Value()->directory_) return failed({ErrorCategory::InvalidRequest, "sevenzip-destination-is-directory"});
                expected_ = {true, Existing.Value()->id_.revision_}; phase_ = Phase::Conflict;
                JobStep Step{JobState::WaitingForUser}; Step.interaction_ = InteractionRequest{"", InteractionKind::Overwrite,
                    "기존 압축 파일을 교체합니다. 작성·검증이 끝날 때까지 원본을 보존합니다.", {InteractionChoice::Replace, InteractionChoice::Cancel}, Existing.Value()->id_}; return Step;
            }
            phase_ = Phase::PrepareInputs;
        }
        if (phase_ == Phase::Conflict) {
            if (!Context.answer_ || Context.answer_->choice_ != InteractionChoice::Replace) return failed({ErrorCategory::Cancelled, "sevenzip-replacement-cancelled"});
            phase_ = Phase::PrepareInputs;
        }
        if (phase_ == Phase::PrepareInputs) {
            temp_ = std::make_unique<QTemporaryDir>(QDir::tempPath() + "/hc-sevenzip-job-XXXXXX");
            if (!temp_->isValid() || !QDir().mkdir(temp_->filePath("input"))) return failed({ErrorCategory::Unavailable, "sevenzip-staging-directory"});
            FileOperationRequest Copy; Copy.sources_ = request_.sources_; Copy.destination_ = {LocatorKind::Local, temp_->filePath("input").toStdString()};
            auto Reader = MakeNativeReadProvider(LocatorKind::Local); Copy.destinationWriter_ = MakeNativeWritableStorage(LocatorKind::Local, Reader);
            Copy.maxDepth_ = std::numeric_limits<size_t>::max(); Copy.conflict_ = ConflictPolicy::Ask;
            auto Prepared = PrepareFileOperation(std::move(Copy), Context.operation_); if (!Prepared.HasValue()) return failed(Prepared.Failure());
            copy_ = std::move(Prepared.Value().task_); phase_ = Phase::CopyInputs; return progress("입력 파일 준비");
        }
        if (phase_ == Phase::CopyInputs) {
            // Inner commits only publish temporary source copies. The outer job
            // enters its irreversible boundary only for the final archive target.
            auto InnerContext = Context; InnerContext.committing_ = innerCommitting_;
            InnerContext.operation_.writeCheckpoint_ = {}; // Private input copies are not published destinations.
            auto Step = copy_->Advance(std::move(InnerContext)); innerCommitting_ = Step.next_ == JobState::Committing;
            if (Step.item_ && Step.item_->source_) copiedSources_.push_back(*Step.item_->source_);
            Step.item_.reset();
            if (Step.next_ == JobState::Completed) {
                auto Errors = copy_->Finalize(JobState::Completed); copy_.reset();
                if (!Errors.empty()) { cleanup_ = Errors; return failed(Errors.front()); }
                // The generic mkdir contract has no metadata setter. Restore
                // directory metadata on private copies after all children exist.
                for (const auto& Entry : copiedSources_) if (Entry.directory_) {
                    if (const auto Stop = Context.operation_.Check({"sevenzip"})) return failed(*Stop);
                    const auto EntryPath = [](const Locator& At) { return QDir::fromNativeSeparators(QString::fromStdString(At.nesting_.empty() ? At.root_ : At.nesting_.back().entry_)); };
                    QString Relative;
                    for (const auto& Source : request_.sources_) {
                        const auto Base = EntryPath(Source.entry_.locator_), Path = EntryPath(Entry.locator_);
                        if (Source.entry_.id_ == Entry.id_) Relative = QString::fromStdString(Source.entry_.name_);
                        else if (Source.entry_.directory_ && Path.startsWith(Base + '/')) Relative = QString::fromStdString(Source.entry_.name_) + Path.mid(Base.size());
                        if (!Relative.isEmpty()) break;
                    }
                    if (Relative.isEmpty()) return failed({ErrorCategory::Internal, "sevenzip-directory-source-mapping"});
                    const auto Path = QDir::toNativeSeparators(QDir(temp_->filePath("input")).filePath(Relative));
                    const auto Handle = CreateFileW(reinterpret_cast<const wchar_t*>(Path.utf16()), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
                    if (Handle == INVALID_HANDLE_VALUE) return failed({ErrorCategory::Unavailable, "sevenzip-directory-metadata-open"});
                    const auto Ticks = uint64_t(Entry.modifiedUtcMs_ + 11644473600000LL) * 10000; FILETIME Time{DWORD(Ticks), DWORD(Ticks >> 32)};
                    const bool Saved = SetFileTime(Handle, nullptr, nullptr, &Time); CloseHandle(Handle);
                    if (!Saved || !SetFileAttributesW(reinterpret_cast<const wchar_t*>(Path.utf16()), FILE_ATTRIBUTE_DIRECTORY | (Entry.attributes_ & (FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_ARCHIVE))))
                        return failed({ErrorCategory::Unavailable, "sevenzip-directory-metadata"});
                }
                phase_ = Phase::Collect; return progress("압축 파일 목록 확인");
            }
            if (Step.next_ == JobState::Committing) Step.next_ = JobState::Running;
            if (Step.progress_) Step.progress_->currentItem_ = "입력 준비: " + Step.progress_->currentItem_;
            return Step;
        }
        if (phase_ == Phase::Collect) {
            const QDir Root(temp_->filePath("input"));
            if (!iterator_) iterator_ = std::make_unique<QDirIterator>(Root.path(), QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System, QDirIterator::Subdirectories);
            for (int I = 0; I < 128 && iterator_->hasNext(); ++I) {
                iterator_->next(); const auto Info = iterator_->fileInfo();
                if (Info.isSymLink()) return failed({ErrorCategory::Unsupported, "sevenzip-source-link"});
                auto Name = Root.relativeFilePath(Info.filePath());
                if (Info.isDir()) Name += '/'; names_.push_back(Name.toStdString());
                if (names_.size() > UINT32_MAX) return failed({ErrorCategory::BudgetExceeded, "sevenzip-sdk-item-count"});
            }
            if (iterator_->hasNext()) return progress("압축 파일 목록 확인");
            iterator_.reset();
            phase_ = Phase::Build; return progress("압축 작성 및 원본 내용 대조");
        }
        if (phase_ == Phase::Build) {
            preparedPath_ = temp_->filePath("archive." + QString::fromStdString(request_.format_));
            hc::plugins::ArchiveUpdate Update{preparedPath_.toStdString(), temp_->filePath("input").toStdString(), names_};
            Update.creation_ = request_.options_; Update.encrypt_ = request_.encrypt_;
            auto Backend = hc::plugins::MakeApprovedWcxBackend(hc::plugins::BuiltinSevenZipOptions());
            auto UpdateContext = Context.operation_;
            if (UpdateContext.passwordPrompt_) UpdateContext.passwordPrompt_ = [Prompt = UpdateContext.passwordPrompt_, Target = request_.destination_.root_](const std::string&, unsigned Attempt, const OperationContext& Operation) { return Prompt(Target, Attempt, Operation); };
            auto Prepared = Backend->Update(std::move(Update), UpdateContext);
            if (!Prepared.HasValue()) return failed(Prepared.Failure());
            phase_ = Phase::Inspect; return progress("작성 결과 재확인");
        }
        if (phase_ == Phase::Inspect) {
            auto WriteContext = Context.operation_; WriteContext.expectedWriteBytes_ = uint64_t(QFileInfo(preparedPath_).size());
            auto Session = writer_->BeginWrite(request_.destination_, expected_, WriteContext); if (!Session.HasValue()) return failed(Session.Failure());
            output_ = std::move(Session.Value()); image_ = std::make_unique<QFile>(preparedPath_);
            if (!image_->open(QIODevice::ReadOnly)) return failed({ErrorCategory::Unavailable, "sevenzip-staging-open"});
            phase_ = Phase::CopyOutput; return progress("대상 볼륨에 저장");
        }
        if (phase_ == Phase::CopyOutput) {
            const auto Bytes = image_->read(1024 * 1024);
            if (Bytes.isEmpty() && !image_->atEnd()) return failed({ErrorCategory::Unavailable, "sevenzip-staging-read"});
            if (!Bytes.isEmpty()) {
                const auto Written = output_->Append({reinterpret_cast<const std::byte*>(Bytes.constData()), size_t(Bytes.size())}, Context.operation_); if (!Written.HasValue()) return failed(Written.Failure());
                JobStep Step{JobState::Running}; Step.progress_ = JobProgress{uint64_t(image_->pos()), uint64_t(image_->size()), "대상 볼륨에 저장"}; return Step;
            }
            image_.reset(); phase_ = Phase::Recheck; return progress("입력 변경 여부 확인");
        }
        if (phase_ == Phase::Recheck) {
            if (rechecked_ < copiedSources_.size()) {
                const auto& Entry = copiedSources_[rechecked_++];
                auto Reader = request_.sources_.front().reader_;
                auto Fresh = Reader->Stat(Entry.id_, Context.operation_).get();
                if (!Fresh.HasValue()) return failed(Fresh.Failure());
                if (Fresh.Value().id_ != Entry.id_) return failed({ErrorCategory::StaleRevision, "sevenzip-source-changed-before-commit"});
                return progress("입력 변경 여부 확인");
            }
            phase_ = Phase::Commit; return JobStep{JobState::Committing};
        }
        if (!Context.committing_) return failed({ErrorCategory::Internal, "sevenzip-commit-boundary-required"});
        auto Receipt = output_->Commit(Context.operation_); if (!Receipt.HasValue()) return failed(Receipt.Failure());
        cleanup_ = std::move(Receipt.Value().cleanupErrors_);
        JobStep Step{JobState::Completed}; Step.item_ = ItemOutcome{Serialize(request_.destination_), ItemOutcome::Status::Completed}; Step.item_->destinationCommitted_ = true;
        if (!cleanup_.empty()) { Step.next_ = JobState::Failed; Step.error_ = cleanup_.front(); } return Step;
    }
    std::vector<Error> Finalize(JobState State) override
    {
        image_.reset(); iterator_.reset();
        if (copy_) { auto Errors = copy_->Finalize(State); cleanup_.insert(cleanup_.end(), Errors.begin(), Errors.end()); copy_.reset(); }
        if (output_) { auto Removed = output_->Abort(); if (!Removed.HasValue()) cleanup_.push_back(Removed.Failure()); output_.reset(); }
        if (temp_) { if (!temp_->remove()) { temp_->setAutoRemove(false); cleanup_.push_back({ErrorCategory::Unavailable, "sevenzip-staging-cleanup:" + temp_->path().toStdString()}); } temp_.reset(); }
        return cleanup_;
    }
private:
    enum class Phase { Initialize, Conflict, PrepareInputs, CopyInputs, Collect, Build, Inspect, CopyOutput, Recheck, Commit };
    JobStep failed(Error Failure) { JobStep Step{JobState::Failed}; Step.error_ = std::move(Failure); return Step; }
    JobStep progress(const char* Text) { JobStep Step{JobState::Running}; Step.progress_ = JobProgress{0, {}, Text}; return Step; }
    CreateRequest request_;
    Phase phase_ = Phase::Initialize;
    DestinationExpectation expected_;
    std::unique_ptr<QTemporaryDir> temp_;
    std::unique_ptr<QDirIterator> iterator_;
    std::unique_ptr<IJobTask> copy_;
    bool innerCommitting_ = false;
    std::vector<EntrySnapshot> copiedSources_;
    size_t rechecked_ = 0;
    std::vector<std::string> names_;
    QString preparedPath_;
    std::shared_ptr<IWritableStorage> writer_;
    std::unique_ptr<IWriteSession> output_;
    std::unique_ptr<QFile> image_;
    std::vector<Error> cleanup_;
};
}
Result<PreparedJob> PrepareCreate(CreateRequest Request, OperationContext Context)
{
    if (Request.sources_.empty() || (Request.format_ != "7z" && Request.format_ != "zip") || Request.options_.level_ > 9 || (Request.destination_.kind_ != LocatorKind::Local && Request.destination_.kind_ != LocatorKind::Smb)) return Error{ErrorCategory::InvalidRequest, "sevenzip-invalid-create-request"};
    if (!QFileInfo(QString::fromStdString(Request.destination_.root_)).isAbsolute()) return Error{ErrorCategory::InvalidRequest, "sevenzip-absolute-destination-required"};
    const auto Target = QDir::cleanPath(QString::fromStdString(Request.destination_.root_));
    for (const auto& Source : Request.sources_) {
        if (!Source.reader_ || Source.reader_ != Request.sources_.front().reader_) return Error{ErrorCategory::InvalidRequest, "sevenzip-source-provider-mismatch"};
        const auto Path = QDir::cleanPath(QString::fromStdString(Source.entry_.locator_.root_));
        if (Target.compare(Path, Qt::CaseInsensitive) == 0 || (Source.entry_.directory_ && Target.startsWith(Path + '/', Qt::CaseInsensitive))) return Error{ErrorCategory::InvalidRequest, "sevenzip-output-inside-input"};
    }
    JobRequest Job; Job.correlationId_ = Request.correlationId_; Job.operation_ = Context;
    Job.resourceLanes_.push_back("sevenzip:" + Target.toCaseFolded().toStdString());
    return PreparedJob{std::move(Job), std::make_unique<CreateTask>(std::move(Request))};
}
}
