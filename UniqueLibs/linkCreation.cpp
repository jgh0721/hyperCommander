#include "stdafx.h"
#include "linkCreation.hpp"

/// 모듈
/// UIs
/// 기타 헤더파일
/// 고유 라이브러리
#include "storageProviders.hpp"
#include "nativeWritableStorage.hpp"
/// 공통 라이브러리
#include "cmnIncludeLibrary.hpp"
/// 외부 라이브러리
/// 선언 헤더파일들
/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QLoggingCategory>

DECLARE_CMNLIBSV2_NAMESPACE
Q_LOGGING_CATEGORY(linkCreationLog, "hc.files.links")

namespace hc::links {
namespace {
using namespace operations;
using namespace storage;
Locator Location(const QString& Path) {
    return {Path.startsWith("//") || Path.startsWith("\\\\") ? LocatorKind::Smb : LocatorKind::Local, Path.toStdString()};
}
class LinkTask final : public IJobTask {
public:
    explicit LinkTask(Request Value) : request_(std::move(Value)) {}
    JobStep Advance(JobStepContext Context) override {
        if (Context.operation_.stopToken_.stop_requested()) return {JobState::Cancelled};
        if (pendingInteraction_) {
            auto Step = std::move(*pendingInteraction_); pendingInteraction_.reset(); return Step;
        }
        auto Step = advance(Context);
        // Native creation can fail after the commit gate (permissions, sharing, races).
        // Leave that uncancellable phase before asking the user to retry or elevate.
        if (Context.committing_ && Step.next_ == JobState::WaitingForUser) {
            pendingInteraction_ = std::move(Step); return {};
        }
        return Step;
    }
private:
    JobStep advance(JobStepContext Context) {
        if (Context.operation_.stopToken_.stop_requested()) return {JobState::Cancelled};
        if (index_ >= request_.sources_.size()) return {JobState::Completed};
        const auto Source = request_.sources_[index_];
        if (target_.isEmpty()) target_ = request_.targetIsName_ ? request_.destination_
            : QDir(request_.destination_).filePath(QFileInfo(Source).fileName());
        if (session_) {
            auto Status = session_->Poll();
            if (!Status.HasValue()) { cleanupSession(); return fail(Status.Failure()); }
            if (Status.Value().finished_) {
                const auto Error = Status.Value().error_; const bool Done = Status.Value().destinationCommitted_;
                cleanupSession();
                if (Done) return finish(ItemOutcome::Status::Completed);
                return fail(Error.value_or(storage::Error{ErrorCategory::Internal, "link-elevation-incomplete"}));
            }
            if (Status.Value().readyToCommit_) {
                if (!Context.committing_) return {JobState::Committing};
                if (!commitSent_) {
                    auto Committed = session_->Commit();
                    if (!Committed.HasValue()) { cleanupSession(); return fail(Committed.Failure()); }
                    commitSent_ = true;
                }
            }
            JobStep Waiting; Waiting.deferFor_ = std::chrono::milliseconds(40); return Waiting;
        }
        if (Context.answer_) {
            const auto Choice = Context.answer_->choice_;
            if (Choice == InteractionChoice::Cancel) return {JobState::Cancelled};
            if (Choice == InteractionChoice::Skip || Choice == InteractionChoice::SkipAll) {
                if (Choice == InteractionChoice::SkipAll) skipAll_ = true;
                return finish(ItemOutcome::Status::Skipped);
            }
            if (Choice == InteractionChoice::RenameIncoming || Choice == InteractionChoice::Rename) rename_ = true;
            if (Choice == InteractionChoice::Elevate && request_.elevation_) {
                auto Begun = request_.elevation_->Begin(elevationRequest(Context.jobId_), Context.operation_);
                if (!Begun.HasValue()) return fail(Begun.Failure());
                session_ = std::move(Begun.Value()); commitSent_ = false; return {};
            }
        }

        const auto Parent = QFileInfo(target_).absolutePath();
        if (const auto Code = Validate(Source, target_, request_.kind_, request_.relative_)) return error(Code, Context);

        // Every API below creates exclusively. A collision racing this check returns here on retry.
        if (exists(target_)) {
            if (request_.conflict_ == "skip" || skipAll_) return finish(ItemOutcome::Status::Skipped);
            if (request_.conflict_ == "rename" || rename_) {
                const QFileInfo Original(target_);
                const auto Stem = Original.completeBaseName(), Suffix = Original.suffix();
                bool Found = false;
                for (int Number = 2; Number <= 10000; ++Number) {
                    const auto Name = Stem + QStringLiteral(" (%1)").arg(Number) + (Suffix.isEmpty() ? QString{} : '.' + Suffix);
                    const auto Candidate = QDir(Original.absolutePath()).filePath(Name);
                    if (!exists(Candidate)) { target_ = Candidate; Found = true; break; }
                }
                if (!Found) return error(ERROR_ALREADY_EXISTS, Context);
            } else return interaction(InteractionKind::Overwrite, QObject::tr("같은 이름의 항목이 있습니다. 기존 항목은 덮어쓰지 않습니다."),
                {InteractionChoice::RenameIncoming, InteractionChoice::Skip, InteractionChoice::SkipAll, InteractionChoice::Cancel});
        }
        if (!Context.committing_) {
            JobStep Ready{JobState::Committing}; Ready.progress_ = progress(); return Ready;
        }
        if (request_.createParents_ && !QDir(Parent).exists() && !QDir().mkpath(Parent))
            return error(ERROR_PATH_NOT_FOUND, Context);
        const auto Code = Create(Source, target_, request_.kind_, request_.relative_);
        if (Code == ERROR_ALREADY_EXISTS || Code == ERROR_FILE_EXISTS) return {}; // re-evaluate exclusive-create race
        if (Code) return error(Code, Context);
        qCInfo(linkCreationLog) << "Created" << KindName(request_.kind_) << target_;
        return finish(ItemOutcome::Status::Completed);
    }
public:
    CompletedEffects TakeCompletedEffects() override {
        CompletedEffects Effects;
        if (session_) {
            session_->Cancel(); auto Errors = session_->Close(); cleanup_.insert(cleanup_.end(), Errors.begin(), Errors.end());
            const auto Status = session_->Poll();
            if (Status.HasValue() && Status.Value().destinationCommitted_)
                Effects.items_.push_back({target_.toStdString(), ItemOutcome::Status::Completed, {}, true});
            else if (!Status.HasValue() || !Status.Value().finished_)
                cleanup_.push_back({ErrorCategory::Unavailable, "link-elevation-result-pending", {}, {}, false,
                    QObject::tr("관리자 작업의 결과를 확인하지 못했습니다. 대상 경로를 확인하세요.").toStdString(), target_.toStdString()});
            session_.reset();
        }
        return Effects;
    }
    std::vector<Error> Finalize(JobState) override { cleanupSession(); return cleanup_; }
private:
    static bool exists(const QString& Path) {
        const auto Native = filetools::NativePath(Path);
        WIN32_FIND_DATAW Data{}; const auto Find = FindFirstFileW(reinterpret_cast<LPCWSTR>(Native.utf16()), &Data);
        if (Find == INVALID_HANDLE_VALUE) return false;
        FindClose(Find); return true; // includes dangling reparse entries
    }
    JobProgress progress() const {
        JobProgress Progress; Progress.phase_ = JobPhase::Creating;
        Progress.itemsDone_ = index_; Progress.plannedItems_ = request_.sources_.size();
        Progress.source_ = request_.sources_.value(index_).toStdString(); Progress.destination_ = target_.toStdString();
        Progress.currentItem_ = QFileInfo(request_.sources_.value(index_)).fileName().toStdString(); return Progress;
    }
    JobStep finish(ItemOutcome::Status Status) {
        JobStep Step; Step.item_ = ItemOutcome{target_.toStdString(), Status, {}, Status == ItemOutcome::Status::Completed};
        ++index_; Step.progress_ = progress(); target_.clear(); rename_ = false;
        return Step;
    }
    JobStep fail(Error Failure) {
        auto Step = finish(ItemOutcome::Status::Failed); Step.item_->error_ = std::move(Failure); return Step;
    }
    JobStep interaction(InteractionKind Type, const QString& Reason, std::vector<InteractionChoice> Choices) {
        JobStep Step{JobState::WaitingForUser};
        InteractionRequest Ask; Ask.kind_ = Type; Ask.reason_ = Reason.toStdString(); Ask.choices_ = std::move(Choices);
        Ask.source_ = request_.sources_[index_].toStdString(); Ask.destination_ = target_.toStdString();
        Step.interaction_ = std::move(Ask); Step.progress_ = progress(); return Step;
    }
    FileElevationRequest elevationRequest(const std::string& JobId) {
        FileElevationRequest Value; Value.jobId_ = JobId;
        Value.action_ = request_.kind_ == Kind::Hard ? FileElevationAction::HardLink
            : request_.kind_ == Kind::Junction ? FileElevationAction::Junction : FileElevationAction::SymbolicLink;
        Value.relativeLink_ = request_.relative_; Value.destination_ = Location(target_);
        auto Reader = MakeNativeReadProvider(LocatorKind::Local);
        auto Writer = MakeNativeWritableStorage(LocatorKind::Local, Reader);
        auto Source = Writer->Inspect(Location(request_.sources_[index_]), {});
        if (Source.HasValue() && Source.Value()) Value.source_ = *Source.Value();
        return Value;
    }
    JobStep error(DWORD Code, const JobStepContext& Context) {
        // The interaction renderer accepts stable error codes, not already formatted prose.
        // Passing Text() here made it treat the entire explanation/path as an unknown code.
        const auto Text = QStringLiteral("win32:%1").arg(Code);
        qCWarning(linkCreationLog) << "Creation failed" << KindName(request_.kind_) << Code << target_;
        std::vector<InteractionChoice> Choices{InteractionChoice::Retry, InteractionChoice::Skip, InteractionChoice::Cancel};
        if ((Code == ERROR_ACCESS_DENIED || Code == ERROR_PRIVILEGE_NOT_HELD) && request_.elevation_
            && request_.elevation_->CanExecute(elevationRequest(Context.jobId_))) Choices.insert(Choices.begin(), InteractionChoice::Elevate);
        return interaction(InteractionKind::RetryFailure, Text, std::move(Choices));
    }
    void cleanupSession() {
        if (!session_) return;
        session_->Cancel(); auto Errors = session_->Close(); cleanup_.insert(cleanup_.end(), Errors.begin(), Errors.end()); session_.reset();
    }
    Request request_; qsizetype index_ = 0; QString target_; bool rename_ = false, skipAll_ = false, commitSent_ = false;
    std::unique_ptr<IFileElevationSession> session_; std::vector<Error> cleanup_;
    std::optional<JobStep> pendingInteraction_;
};
}
operations::PreparedJob Prepare(Request Value) {
    operations::PreparedJob Job; Job.request_.maxRetries_ = 0;
    Job.request_.metadata_.kind_ = operations::JobKind::MakeLink;
    Job.request_.metadata_.label_ = QObject::tr("링크 만들기").toStdString();
    Job.request_.metadata_.source_ = Value.sources_.value(0).toStdString();
    Job.request_.metadata_.destination_ = Value.destination_.toStdString();
    Job.request_.metadata_.options_ = KindName(Value.kind_).toStdString();
    Job.request_.resourceLanes_.push_back(operations::FileOperationLane(Location(Value.destination_)));
    for (const auto& Source : Value.sources_) Job.request_.resourceLanes_.push_back(operations::FileOperationLane(Location(Source)));
    Job.task_ = std::make_unique<LinkTask>(std::move(Value)); return Job;
}
}
