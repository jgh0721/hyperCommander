#include "isoJob.hpp"
#include "builtinIso.hpp"
#include "UniqueLibs/nativeWritableStorage.hpp"
#include "UniqueLibs/storageProviders.hpp"
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

namespace hc::iso {
using namespace hc::storage;
using namespace hc::operations;
namespace {
constexpr uint64_t ImageLimit = 128ull * 1024 * 1024 * 1024;
class CreateTask final : public IJobTask {
public:
    explicit CreateTask(CreateRequest Request) : request_(std::move(Request)) {}
    JobStep Advance(JobStepContext Context) override
    {
        if (const auto Failure = Context.operation_.Check({"iso"})) return failed(*Failure);
        if (phase_ == Phase::Initialize) {
            auto Reader = MakeNativeReadProvider(request_.destination_.kind_);
            writer_ = MakeNativeWritableStorage(request_.destination_.kind_, Reader);
            auto Existing = writer_->Inspect(request_.destination_, Context.operation_); if (!Existing.HasValue()) return failed(Existing.Failure());
            if (Existing.Value()) {
                if (Existing.Value()->directory_) return failed({ErrorCategory::InvalidRequest, "iso-destination-is-directory"});
                expected_ = {true, Existing.Value()->id_.revision_}; phase_ = Phase::Conflict;
                JobStep Step{JobState::WaitingForUser}; Step.interaction_ = InteractionRequest{"", InteractionKind::Overwrite,
                    "기존 ISO 파일을 교체합니다. 작성·검증이 끝날 때까지 원본을 보존합니다.", {InteractionChoice::Replace, InteractionChoice::Cancel}, Existing.Value()->id_}; return Step;
            }
            phase_ = Phase::PrepareInputs;
        }
        if (phase_ == Phase::Conflict) {
            if (!Context.answer_ || Context.answer_->choice_ != InteractionChoice::Replace) return failed({ErrorCategory::Cancelled, "iso-replacement-cancelled"});
            phase_ = Phase::PrepareInputs;
        }
        if (phase_ == Phase::PrepareInputs) {
            temp_ = std::make_unique<QTemporaryDir>(QDir::tempPath() + "/hc-iso-job-XXXXXX");
            if (!temp_->isValid() || !QDir().mkdir(temp_->filePath("input"))) return failed({ErrorCategory::Unavailable, "iso-staging-directory"});
            FileOperationRequest Copy; Copy.sources_ = request_.sources_; Copy.destination_ = {LocatorKind::Local, temp_->filePath("input").toStdString()};
            auto Reader = MakeNativeReadProvider(LocatorKind::Local); Copy.destinationWriter_ = MakeNativeWritableStorage(LocatorKind::Local, Reader);
            Copy.maxDepth_ = 128; Copy.maxBytes_ = 64ull * 1024 * 1024 * 1024; Copy.conflict_ = ConflictPolicy::Ask;
            auto Prepared = PrepareFileOperation(std::move(Copy), Context.operation_); if (!Prepared.HasValue()) return failed(Prepared.Failure());
            copy_ = std::move(Prepared.Value().task_); phase_ = Phase::CopyInputs; return progress("입력 파일 준비");
        }
        if (phase_ == Phase::CopyInputs) {
            // Inner commits only publish temporary source copies. The outer job
            // enters its irreversible boundary only for the final ISO target.
            auto InnerContext = Context; InnerContext.committing_ = innerCommitting_;
            auto Step = copy_->Advance(std::move(InnerContext)); innerCommitting_ = Step.next_ == JobState::Committing;
            if (Step.item_ && Step.item_->source_) copiedSources_.push_back(*Step.item_->source_);
            Step.item_.reset();
            if (Step.next_ == JobState::Completed) {
                auto Errors = copy_->Finalize(JobState::Completed); copy_.reset();
                if (!Errors.empty()) { cleanup_ = Errors; return failed(Errors.front()); }
                phase_ = Phase::Collect; return progress("이미지 파일 목록 확인");
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
                if (Info.isSymLink()) return failed({ErrorCategory::Unsupported, "iso-source-link"});
                auto Name = Root.relativeFilePath(Info.filePath());
                if (Name != Name.normalized(QString::NormalizationForm_C)) return failed({ErrorCategory::InvalidRequest, "iso-name-requires-nfc:" + Name.toStdString()});
                for (const auto& Part : Name.split('/')) if ((request_.profile_ == 1 || request_.profile_ == 2) && Part.size() > 64) return failed({ErrorCategory::Unsupported, "iso-joliet-name-too-long:" + Name.toStdString()});
                if (Info.isDir()) Name += '/'; names_.push_back(Name.toStdString());
                if (names_.size() > 100000) return failed({ErrorCategory::BudgetExceeded, "iso-entry-budget"});
            }
            if (iterator_->hasNext()) return progress("이미지 파일 목록 확인");
            iterator_.reset();
            QFile Ini(temp_->filePath("pkplugin.ini"));
            const auto Settings = "[IsoImage]\r\nProfile=" + QByteArray::number(request_.profile_) + "\r\nVolumeLabel=" + request_.volumeLabel_.toUtf8() + "\r\n";
            if (!Ini.open(QIODevice::WriteOnly) || Ini.write(Settings) != Settings.size()) return failed({ErrorCategory::Unavailable, "iso-options-write"}); Ini.close();
            auto Options = hc::plugins::BuiltinIsoOptions(); Options.defaultIniPath_ = Ini.fileName(); backend_ = hc::plugins::MakeApprovedWcxBackend(Options);
            phase_ = Phase::Build; return progress("ISO 작성 및 원본 내용 대조");
        }
        if (phase_ == Phase::Build) {
            const auto Result = backend_->Update({temp_->filePath("image.iso").toStdString(), temp_->filePath("input").toStdString(), names_, {}, ImageLimit}, Context.operation_);
            if (!Result.HasValue()) return failed(Result.Failure());
            auto Listing = backend_->Enumerate(temp_->filePath("image.iso").toStdString(), true, Context.operation_); if (!Listing.HasValue()) return failed(Listing.Failure());
            listing_ = std::move(Listing.Value()); phase_ = Phase::Inspect; return progress("작성 결과 재확인");
        }
        if (phase_ == Phase::Inspect) {
            auto Page = listing_->Next(128, Context.operation_); if (!Page.HasValue()) return failed(Page.Failure());
            if (!Page.Value().end_) return progress("작성 결과 재확인"); listing_.reset();
            auto Session = writer_->BeginWrite(request_.destination_, expected_, Context.operation_); if (!Session.HasValue()) return failed(Session.Failure());
            output_ = std::move(Session.Value()); image_ = std::make_unique<QFile>(temp_->filePath("image.iso"));
            if (!image_->open(QIODevice::ReadOnly)) return failed({ErrorCategory::Unavailable, "iso-staging-open"});
            phase_ = Phase::CopyOutput; return progress("대상 볼륨에 저장");
        }
        if (phase_ == Phase::CopyOutput) {
            const auto Bytes = image_->read(1024 * 1024);
            if (Bytes.isEmpty() && !image_->atEnd()) return failed({ErrorCategory::Unavailable, "iso-staging-read"});
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
                if (Fresh.Value().id_ != Entry.id_) return failed({ErrorCategory::StaleRevision, "iso-source-changed-before-commit"});
                return progress("입력 변경 여부 확인");
            }
            phase_ = Phase::Commit; return JobStep{JobState::Committing};
        }
        if (!Context.committing_) return failed({ErrorCategory::Internal, "iso-commit-boundary-required"});
        auto Receipt = output_->Commit(Context.operation_); if (!Receipt.HasValue()) return failed(Receipt.Failure());
        cleanup_ = std::move(Receipt.Value().cleanupErrors_);
        JobStep Step{JobState::Completed}; Step.item_ = ItemOutcome{Serialize(request_.destination_), ItemOutcome::Status::Completed}; Step.item_->destinationCommitted_ = true;
        if (!cleanup_.empty()) { Step.next_ = JobState::Failed; Step.error_ = cleanup_.front(); } return Step;
    }
    std::vector<Error> Finalize(JobState State) override
    {
        listing_.reset(); backend_.reset(); image_.reset(); iterator_.reset();
        if (copy_) { auto Errors = copy_->Finalize(State); cleanup_.insert(cleanup_.end(), Errors.begin(), Errors.end()); copy_.reset(); }
        if (output_) { auto Removed = output_->Abort(); if (!Removed.HasValue()) cleanup_.push_back(Removed.Failure()); output_.reset(); }
        if (temp_) { if (!temp_->remove()) { temp_->setAutoRemove(false); cleanup_.push_back({ErrorCategory::Unavailable, "iso-staging-cleanup:" + temp_->path().toStdString()}); } temp_.reset(); }
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
    std::shared_ptr<hc::plugins::IArchiveBackend> backend_;
    std::unique_ptr<hc::plugins::IArchiveListing> listing_;
    std::shared_ptr<IWritableStorage> writer_;
    std::unique_ptr<IWriteSession> output_;
    std::unique_ptr<QFile> image_;
    std::vector<Error> cleanup_;
};
}
Result<PreparedJob> PrepareCreate(CreateRequest Request, OperationContext Context)
{
    if (Request.sources_.empty() || Request.profile_ > 3 || (Request.destination_.kind_ != LocatorKind::Local && Request.destination_.kind_ != LocatorKind::Smb)) return Error{ErrorCategory::InvalidRequest, "iso-invalid-create-request"};
    if (!QFileInfo(QString::fromStdString(Request.destination_.root_)).isAbsolute()) return Error{ErrorCategory::InvalidRequest, "iso-absolute-destination-required"};
    if (Request.volumeLabel_.isEmpty() || Request.volumeLabel_.size() > (Request.profile_ == 0 ? 32 : Request.profile_ == 3 ? 30 : 16)) return Error{ErrorCategory::InvalidRequest, "iso-volume-label-length"};
    for (const auto C : Request.volumeLabel_) if (C.unicode() < 32 || C.unicode() > 126) return Error{ErrorCategory::InvalidRequest, "iso-volume-label-ascii-required"};
    const auto Target = QDir::cleanPath(QString::fromStdString(Request.destination_.root_));
    for (const auto& Source : Request.sources_) {
        if (!Source.reader_ || Source.reader_ != Request.sources_.front().reader_) return Error{ErrorCategory::InvalidRequest, "iso-source-provider-mismatch"};
        const auto Path = QDir::cleanPath(QString::fromStdString(Source.entry_.locator_.root_));
        if (Target.compare(Path, Qt::CaseInsensitive) == 0 || (Source.entry_.directory_ && Target.startsWith(Path + '/', Qt::CaseInsensitive))) return Error{ErrorCategory::InvalidRequest, "iso-output-inside-input"};
    }
    JobRequest Job; Job.correlationId_ = Request.correlationId_; Job.operation_ = Context;
    Job.resourceLanes_.push_back("iso:" + Target.toCaseFolded().toStdString());
    return PreparedJob{std::move(Job), std::make_unique<CreateTask>(std::move(Request))};
}
}
