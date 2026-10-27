#include "stdafx.h"
#include "HyperCommander/UIs/permissionDialogs.hpp"
#include "qtMessageDialog.hpp"
#include "fileJobUi.hpp"
#include "UniqueLibs/qtArchivePassword.hpp"
#include "folderSize.hpp"
#include "HyperCommander/UIs/fileToolsDialogs.hpp"
#include "HyperCommander/UIs/fileLockDialog.hpp"
#include "nativeFileLocks.hpp"
#include "settings/appSettings.hpp"
#include "qtJobJournal.hpp"
#include "HyperCommander/UIs/cmpPanel.hpp"
#include "HyperCommander/UIs/Components/cmpView.hpp"
#include "commonLibs/columns/builtInColumns.hpp"
#include "nativeWritableStorage.hpp"
#include "nativeFileElevation.hpp"
#include "storageProviders.hpp"
#include "commonLibs/operations/itemOutcomeStore.hpp"
#include "commonLibs/operations/writeRecovery.hpp"
#include <QtConcurrent>
#include <QStandardPaths>
#include <QTreeWidget>
#include <QHeaderView>
#include <QCheckBox>
#include <QSpinBox>
#include <QDateTime>
#include <QStatusBar>
#include <QSignalBlocker>
#include <QGroupBox>
#include <fmstyle/ThemeManager.h>
#include "qtDialogFrame.hpp"
#include <QScrollArea>
#include <QScreen>
#include <fmwidgets/Button.h>
#include <fmwidgets/WrapLayout.h>
#include <fmwidgets/DialogHeader.h>
#include <fmwidgets/TransferGraph.h>
#include <fmwidgets/ProgressBar.h>
#include <fmwidgets/Label.h>
#include <QGridLayout>

using namespace hc::operations;
using namespace hc::storage;

namespace
{
    QString StateText(JobState State)
    {
        switch (State)
        {
        case JobState::Queued: return QObject::tr("대기");
        case JobState::Preparing: return QObject::tr("항목 확인 중");
        case JobState::Running: return QObject::tr("실행 중");
        case JobState::WaitingForUser: return QObject::tr("응답 필요");
        case JobState::Paused: return QObject::tr("일시 정지");
        case JobState::Committing: return QObject::tr("변경 확정 중");
        case JobState::Completed: return QObject::tr("완료");
        case JobState::Failed: return QObject::tr("실패");
        case JobState::Cancelled: return QObject::tr("취소됨");
        }
        return {};
    }
    QString ErrorText(const Error& Value)
    {
        if(Value.code_.starts_with("elevation-outcome-unknown"))return QObject::tr("도우미와 연결이 끊겨 변경 결과를 확정하지 못했습니다. 대상·원본과 작업 기록을 확인하세요. 자동 재실행하지 않습니다.");
        if(Value.code_=="elevation-cleanup-pending")return QObject::tr("도우미의 임시 파일 정리 완료를 아직 확인하지 못했습니다. 작업 기록과 보존 파일을 확인하세요.");
        if(Value.code_.starts_with("security-status-save-failed"))return QObject::tr("보안 작업 결과 기록을 저장하지 못했습니다. 원래 권한 백업은 유지됩니다. 현재 권한과 복구 결과를 확인하세요.");
        if(Value.code_.starts_with("permission-history-save-failed"))return QObject::tr("권한 작업은 처리했지만 요청 이력을 저장하지 못했습니다. 작업 결과와 보안 백업을 확인하세요.");
        if (Value.code_ == "recovery-committed-checkpoints") return QObject::tr("중단 전 목적지 저장 완료 기록(건):");
        if (Value.code_ == "recovery-artifact-preserved") return QObject::tr("확인을 위해 보존한 파일:");
        if (Value.code_ == "write-recovery-original-missing") return QObject::tr("원본 위치와 백업을 찾을 수 없습니다. 보존된 파일을 확인하세요.");
        if (Value.code_ == "destination-added-hidden-attribute")
            return QObject::tr("파일 내용은 복사되었지만 대상에서 숨김 속성이 추가되었습니다. 대상 서버의 숨김 파일 정책을 확인하세요. 이동 작업의 원본은 유지됩니다.");
        if (Value.code_ == "destination-attributes-mismatch")
            return QObject::tr("파일 내용은 복사되었지만 대상의 파일 속성이 원본과 다릅니다. 대상의 속성 지원 및 정책을 확인하세요. 이동 작업의 원본은 유지됩니다.");
        if (Value.code_ == "revision-changed")
            return QObject::tr("항목의 식별 정보가 작업 시작 이후 달라져 작업을 중단했습니다. 폴더를 새로 고친 뒤 남은 항목을 확인하고 다시 시도하세요. (revision-changed)");
        return QString::fromStdString(Value.code_);
    }
    QString JobStatusText(const JobSnapshot& Snapshot)
    {
        if (Snapshot.error_ && Snapshot.error_->code_ == "job-interrupted") return QObject::tr("중단 · 확인 필요");
        if (!IsTerminal(Snapshot.state_)) {
            if (Snapshot.cancelRequested_) return QObject::tr("취소 후 정리 중");
            if (Snapshot.deferred_) return Snapshot.pauseRequested_ ? QObject::tr("대기열 · 일시 정지 요청됨") : QObject::tr("대기열 · 시작 대기");
            if (Snapshot.pauseRequested_ && Snapshot.state_ != JobState::Paused)
                return StateText(Snapshot.state_) + QObject::tr(" · 일시 정지 요청 중");
        }
        return StateText(Snapshot.state_);
    }
    QString DisplayPath(const std::string& Value)
    {
        if (Value == Serialize(Locator{})) return {};
        const auto At = ParseLocator(Value);
        if (!At.HasValue()) return QString::fromStdString(Value);
        auto Path = QString::fromStdString(At.Value().root_);
        for (const auto& Hop : At.Value().nesting_) Path += " > " + QString::fromStdString(Hop.entry_);
        return Path;
    }
    QString OperationText(JobKind Kind)
    {
        switch (Kind) {
        case JobKind::Copy: return QObject::tr("복사");
        case JobKind::Move: return QObject::tr("이동");
        case JobKind::Delete: return QObject::tr("영구 삭제");
        case JobKind::Recycle: return QObject::tr("휴지통으로 이동");
        case JobKind::Extract: return QObject::tr("압축 해제");
        case JobKind::ArchiveCreate: return QObject::tr("압축 파일 생성");
        case JobKind::ArchiveSeparate: return QObject::tr("압축 파일 개별 생성");
        case JobKind::IsoCreate: return QObject::tr("ISO 생성");
        case JobKind::ArchiveAdd: return QObject::tr("압축에 추가");
        case JobKind::ArchiveMove: return QObject::tr("압축 내부 이동");
        case JobKind::ArchiveDelete: return QObject::tr("압축 내부 삭제");
        case JobKind::ArchiveRename: return QObject::tr("압축 내부 이름 변경");
        case JobKind::ArchiveMakeDirectory: return QObject::tr("압축 내부 새 폴더");
        case JobKind::Rename: return QObject::tr("이름 변경");
        case JobKind::BatchRename: return QObject::tr("다중 이름 변경");
        case JobKind::RenameUndo: return QObject::tr("이름 변경 실행 취소");
        case JobKind::RenameRedo: return QObject::tr("이름 변경 다시 실행");
        case JobKind::RenameResume: return QObject::tr("중단된 이름 변경 이어 실행");
        case JobKind::RenameRestore: return QObject::tr("중단된 이름 변경 원래 이름 복구");
        case JobKind::MakeFile: return QObject::tr("새 파일");
        case JobKind::MakeDirectory: return QObject::tr("새 폴더");
        case JobKind::SecurityChange: return QObject::tr("소유권·권한 변경");
        case JobKind::SecurityRestore: return QObject::tr("원래 권한 복원");
        default: return {};
        }
    }
    QString PhaseText(JobPhase Phase)
    {
        switch (Phase) {
        case JobPhase::Preparing: return QObject::tr("변경 준비");
        case JobPhase::Copying: return QObject::tr("대상에 복사");
        case JobPhase::Verifying: return QObject::tr("결과 확인");
        case JobPhase::RemovingSource: return QObject::tr("원본 제거");
        case JobPhase::PreparingInputs: return QObject::tr("입력 준비");
        case JobPhase::BuildingArchive: return QObject::tr("압축 생성");
        case JobPhase::SavingOutput: return QObject::tr("대상 저장");
        case JobPhase::RewritingArchive: return QObject::tr("압축 재작성");
        case JobPhase::Committing: return QObject::tr("변경 확정");
        case JobPhase::Inspecting: return QObject::tr("기록·파일 확인");
        case JobPhase::TemporaryNames: return QObject::tr("임시 이름 적용");
        case JobPhase::ApplyingNames: return QObject::tr("최종 이름 적용");
        case JobPhase::RestoringNames: return QObject::tr("원래 이름 복원");
        case JobPhase::Creating: return QObject::tr("생성");
        case JobPhase::BackingUpSecurity: return QObject::tr("기존 권한 백업");
        case JobPhase::ApplyingSecurity: return QObject::tr("권한 변경");
        case JobPhase::RestoringSecurity: return QObject::tr("권한 복원");
        default: return {};
        }
    }
    std::vector<JobPhase> JobPhases(JobKind Kind)
    {
        switch (Kind) {
        case JobKind::Move: return {JobPhase::Copying, JobPhase::Verifying, JobPhase::RemovingSource};
        case JobKind::ArchiveCreate: case JobKind::ArchiveSeparate: case JobKind::IsoCreate:
            return {JobPhase::PreparingInputs, JobPhase::BuildingArchive, JobPhase::SavingOutput, JobPhase::Committing};
        case JobKind::ArchiveAdd: case JobKind::ArchiveMove: case JobKind::ArchiveDelete:
        case JobKind::ArchiveRename: case JobKind::ArchiveMakeDirectory:
            return {JobPhase::Preparing, JobPhase::RewritingArchive, JobPhase::Committing};
        case JobKind::BatchRename: case JobKind::RenameUndo: case JobKind::RenameRedo:
        case JobKind::RenameResume: case JobKind::RenameRestore:
            return {JobPhase::Inspecting, JobPhase::TemporaryNames,
                Kind == JobKind::RenameUndo || Kind == JobKind::RenameRestore ? JobPhase::RestoringNames : JobPhase::ApplyingNames};
        case JobKind::MakeFile: case JobKind::MakeDirectory:
            return {JobPhase::Preparing, JobPhase::Creating, JobPhase::Verifying};
        case JobKind::SecurityChange: return {JobPhase::BackingUpSecurity, JobPhase::ApplyingSecurity, JobPhase::Verifying};
        case JobKind::SecurityRestore: return {JobPhase::Inspecting, JobPhase::RestoringSecurity, JobPhase::Verifying};
        default: return {};
        }
    }
    enum class RateMode { None, Bytes, Items, Files };
    RateMode ProgressRate(const JobSnapshot& Snapshot)
    {
        const auto Kind = Snapshot.metadata_.kind_;
        const auto& Progress = Snapshot.progress_;
        if (Kind == JobKind::Delete || (Kind == JobKind::Unknown && Progress.deletedFiles_))
            return Progress.deletedFiles_ ? RateMode::Files : RateMode::Items;
        if (Kind == JobKind::Recycle || Kind == JobKind::Rename || Kind == JobKind::BatchRename
            || Kind == JobKind::RenameUndo || Kind == JobKind::RenameRedo || Kind == JobKind::RenameResume || Kind == JobKind::RenameRestore)
            return RateMode::Items;
        if (Kind == JobKind::ArchiveCreate || Kind == JobKind::ArchiveSeparate || Kind == JobKind::IsoCreate)
            return Progress.phase_ == JobPhase::SavingOutput ? RateMode::Bytes : RateMode::None;
        if (Kind == JobKind::Copy || Kind == JobKind::Extract || Kind == JobKind::Unknown) return RateMode::Bytes;
        if (Kind == JobKind::Move) {
            if (Progress.phase_ == JobPhase::RemovingSource) return RateMode::None;
            return Progress.phase_ == JobPhase::ApplyingNames ? RateMode::Items : RateMode::Bytes;
        }
        return RateMode::None;
    }
    uint64_t ProcessedItems(const JobSnapshot& Snapshot)
    {
        return Snapshot.progress_.itemsDone_.value_or(Snapshot.completedItems_ + Snapshot.skippedItems_ + Snapshot.failedItems_);
    }
    uint64_t RateValue(const JobSnapshot& Snapshot)
    {
        const auto Mode = ProgressRate(Snapshot);
        return Mode == RateMode::Files ? Snapshot.progress_.deletedFiles_.value_or(0)
            : Mode == RateMode::Items ? ProcessedItems(Snapshot) : Snapshot.progress_.bytesDone_;
    }
    bool CloseFinishedJob(const JobResult& Result)
    {
        const auto& Snapshot = Result.snapshot_;
        if ((Snapshot.state_ != JobState::Completed && Snapshot.state_ != JobState::Cancelled) || !Result.cleanupErrors_.empty()) return false;
        if (Snapshot.error_ && Snapshot.error_->category_ != ErrorCategory::Cancelled) return false;
        const auto CancelledFailures = std::ranges::count_if(Result.items_, [](const auto& Item) {
            return Item.status_ == ItemOutcome::Status::Failed && Item.error_ && Item.error_->category_ == ErrorCategory::Cancelled;
        });
        return Snapshot.failedItems_ == 0 || (Snapshot.state_ == JobState::Cancelled && Snapshot.failedItems_ == uint64_t(CancelledFailures));
    }
}

FileJobUi::FileJobUi(QWidget* Main, std::shared_ptr<hc::commands::EventHub> Events)
    : QObject(Main), journal_(MakeFileJobJournal(QDir(QStandardPaths::isTestModeEnabled()
          ? QCoreApplication::applicationDirPath() : QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)).filePath("operation-journal"),false,true,false)),
      scheduler_(std::move(Events), 2, 1024, journal_), main_(Main)
{
    previousPermissionController_=QCoreApplication::instance()->property("hc.permission.controller").value<QObject*>();
    QCoreApplication::instance()->setProperty("hc.permission.controller",QVariant::fromValue<QObject*>(this));
    Main->installEventFilter(this);
    const auto Defaults = hc::settings::AppSettings().CommittedObject()["options"].toObject();
    SetFileJobPersistence(journal_, Defaults["enableJobRecovery"].toBool(false), Defaults["saveJobProgress"].toBool(false));
    appliedConcurrency_ = Defaults["jobConcurrency"].toInt(2);
    scheduler_.SetConcurrencyLimit(appliedConcurrency_);
    dialog_ = new QDialog(Main);
    dialog_->setObjectName("hc.fileJobs");
    dialog_->setWindowTitle(tr("파일 작업"));
    dialog_->resize(840, 480);
    dialog_->setMinimumWidth(520);
    dialog_->installEventFilter(this);
    auto* Layout = new QVBoxLayout(dialog_);
    // Metrics and interaction rows grow after the window first opens. Keep
    // their minimum height enforced so the graph cannot cover its controls.
    Layout->setVerticalSizeConstraint(QLayout::SetMinimumSize);
    Layout->setSpacing(18);
    Layout->setAlignment(Qt::AlignTop);
    jobs_ = new QComboBox(dialog_); jobs_->setObjectName("hc.jobs.selection");
    jobs_->hide();
    status_ = new QLabel(dialog_); status_->setObjectName("hc.jobs.status"); status_->setWordWrap(true);
    status_->setTextFormat(Qt::PlainText);
    const auto AddEndpoint = [&](const char* Prefix, QWidget*& Block, QLabel*& Path, QLabel*& Name) {
        Block = new QWidget(dialog_); Block->setObjectName(QString::fromLatin1(Prefix));
        auto* Row = new QVBoxLayout(Block); Row->setContentsMargins(0, 0, 0, 0); Row->setSpacing(6);
        auto* Elided = new fm::ui::Label(Block); Elided->setTextRole(fm::ui::Label::Meta); Elided->setElideMode(Qt::ElideMiddle);
        Path = Elided; Path->setObjectName(QString::fromLatin1(Prefix) + ".path"); Path->setTextFormat(Qt::PlainText);
        Name = new QLabel(Block); Name->setObjectName(QString::fromLatin1(Prefix) + ".name");
        Name->setTextFormat(Qt::PlainText); Name->setWordWrap(true); Name->setMinimumWidth(0);
        auto Policy = Name->sizePolicy(); Policy.setHorizontalPolicy(QSizePolicy::Ignored); Name->setSizePolicy(Policy);
        auto Font = Name->font(); Font.setWeight(QFont::DemiBold); Name->setFont(Font);
        Row->addWidget(Path); Row->addWidget(Name); Layout->addWidget(Block);
    };
    AddEndpoint("hc.jobs.source", sourceBlock_, sourcePath_, sourceName_);
    AddEndpoint("hc.jobs.destination", destinationBlock_, destinationPath_, destinationName_);
    phaseBlock_ = new QWidget(dialog_); phaseBlock_->setObjectName("hc.jobs.phases");
    auto* Phases = new fm::ui::WrapLayout(phaseBlock_); Phases->setContentsMargins(0, 0, 0, 0);
    Layout->addWidget(phaseBlock_); phaseBlock_->hide();
    currentBlock_ = new QWidget(dialog_); currentBlock_->setObjectName("hc.jobs.current");
    auto* CurrentLayout = new QVBoxLayout(currentBlock_); CurrentLayout->setContentsMargins(0, 0, 0, 0); CurrentLayout->setSpacing(7);
    auto* CurrentTop = new QHBoxLayout;
    currentTitle_ = new QLabel(tr("현재 파일"), currentBlock_); currentPercent_ = new QLabel(currentBlock_);
    currentPercent_->setObjectName("hc.jobs.currentPercent"); CurrentTop->addWidget(currentTitle_); CurrentTop->addStretch(); CurrentTop->addWidget(currentPercent_);
    CurrentLayout->addLayout(CurrentTop);
    currentProgress_ = new fm::ui::ProgressBar(currentBlock_); currentProgress_->setObjectName("hc.jobs.currentProgress");
    currentProgress_->setTextVisible(false); currentProgress_->setFixedHeight(9); currentProgress_->setAccessibleName(tr("현재 파일 진행률"));
    CurrentLayout->addWidget(currentProgress_);
    currentMetrics_ = new QLabel(currentBlock_); currentMetrics_->setObjectName("hc.jobs.currentMetrics"); currentMetrics_->setTextFormat(Qt::PlainText); currentMetrics_->setWordWrap(true);
    CurrentLayout->addWidget(currentMetrics_); Layout->addWidget(currentBlock_);
    auto* Overall = new QWidget(dialog_); auto* OverallLayout = new QVBoxLayout(Overall); OverallLayout->setContentsMargins(0, 0, 0, 0); OverallLayout->setSpacing(7);
    auto* OverallTop = new QHBoxLayout;
    overallTitle_ = new QLabel(tr("전체 진행"), Overall); overallPercent_ = new QLabel(Overall); overallPercent_->setObjectName("hc.jobs.overallPercent");
    OverallTop->addWidget(overallTitle_); OverallTop->addStretch(); OverallTop->addWidget(overallPercent_); OverallLayout->addLayout(OverallTop);
    progress_ = new fm::ui::ProgressBar(Overall); progress_->setObjectName("hc.jobs.overallProgress"); progress_->setTextVisible(false);
    progress_->setFixedHeight(9); progress_->setAccessibleName(tr("전체 작업 진행률")); OverallLayout->addWidget(progress_);
    metricsLayout_ = new QBoxLayout(QBoxLayout::LeftToRight); metricsLayout_->setSpacing(12);
    overallMetrics_ = new QLabel(Overall); overallMetrics_->setObjectName("hc.jobs.overallMetrics"); overallMetrics_->setTextFormat(Qt::PlainText); overallMetrics_->setWordWrap(true);
    metrics_ = new QLabel(Overall); metrics_->setTextFormat(Qt::PlainText); metrics_->setWordWrap(true); metrics_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    metrics_->setObjectName("hc.jobs.metrics"); metricsLayout_->addWidget(overallMetrics_, 1); metricsLayout_->addWidget(metrics_); OverallLayout->addLayout(metricsLayout_); Layout->addWidget(Overall);
    notice_ = new QLabel(dialog_); notice_->setObjectName("hc.jobs.notice"); notice_->setTextFormat(Qt::PlainText); notice_->setWordWrap(true); notice_->hide(); Layout->addWidget(notice_);
    graph_ = new fm::ui::TransferGraph(dialog_); graph_->setObjectName("hc.jobs.graph");
    graph_->setTitle(tr("처리 속도 · 실제 관측값")); graph_->setFixedHeight(160); Layout->addWidget(graph_);
    axisBlock_ = new QWidget(dialog_); auto* Display = new QHBoxLayout(axisBlock_); Display->setContentsMargins(0, 0, 0, 0);
    auto* Toggle = graphToggle_ = new fm::ui::Button(tr("그래프 표시(&D)"), dialog_); Toggle->setObjectName("hc.jobs.toggleDetails");
    auto* Axis = new QComboBox(dialog_); Axis->addItems({tr("진행률 축"), tr("시간 축 · 최근 60초")});
    graphAxis_ = Axis; Axis->setAccessibleName(tr("처리 속도 그래프 축")); Axis->setObjectName("hc.jobs.graphAxis");
    Display->addStretch(); Display->addWidget(Axis); Layout->addWidget(axisBlock_);
    graph_->hide(); Axis->hide(); axisBlock_->hide();
    connect(Axis, &QComboBox::currentIndexChanged, this, [this](int Index) { graph_->setAxis(Index ? fm::ui::TransferGraph::Time : fm::ui::TransferGraph::Progress); });
    details_ = new QPlainTextEdit(dialog_); details_->setReadOnly(true); details_->setAccessibleName(tr("선택 작업 상세 결과"));
    details_->setObjectName("hc.jobs.details"); details_->setMinimumHeight(40); details_->setMaximumHeight(160); Layout->addWidget(details_);
    pagesBlock_ = new QWidget(dialog_); auto* Pages = new QHBoxLayout(pagesBlock_); Pages->setContentsMargins(0, 0, 0, 0);
    previousPage_ = new fm::ui::Button(tr("이전 결과"), dialog_); previousPage_->setObjectName("hc.jobs.previousResults");
    nextPage_ = new fm::ui::Button(tr("다음 결과"), dialog_); nextPage_->setObjectName("hc.jobs.nextResults");
    Pages->addWidget(previousPage_); Pages->addWidget(nextPage_); Pages->addStretch(); Layout->addWidget(pagesBlock_); pagesBlock_->hide();
    connect(previousPage_, &QPushButton::clicked, this, [this] { resultOffset_ = resultOffset_ >= 200 ? resultOffset_ - 200 : 0; updateSelection(); });
    connect(nextPage_, &QPushButton::clicked, this, [this] { resultOffset_ += 200; updateSelection(); });
    renamed_ = new QLineEdit(dialog_); renamed_->setPlaceholderText(tr("새 이름"));
    renamed_->setAccessibleName(tr("충돌한 항목의 새 이름"));
    renamed_->setObjectName("hc.jobs.newName"); Layout->addWidget(renamed_);
    connect(Toggle, &QPushButton::clicked, this, [this, Toggle, Axis] {
        detailed_ = !detailed_;
        updateSelection(); fitProgressDialog();
    });
    autoClose_ = new QCheckBox(tr("완료되면 창 닫기(&W)"), dialog_); autoClose_->setObjectName("hc.jobs.closeOnSuccess");
    autoClose_->setToolTip(tr("정상 완료 또는 사용자 취소의 정리가 끝나면 창을 닫습니다. 실패·정리 오류가 있으면 결과를 유지합니다."));
    answersBlock_ = new QWidget(dialog_); auto* Buttons = new fm::ui::WrapLayout(answersBlock_); Buttons->setContentsMargins(0, 0, 0, 0);
    Layout->addWidget(answersBlock_); answersBlock_->hide();
    const std::vector<std::pair<InteractionChoice, QString>> Choices{
        {InteractionChoice::Confirm, tr("영구 삭제")}, {InteractionChoice::ConfirmAll, tr("모두 허용")}, {InteractionChoice::Replace, tr("덮어쓰기")},
        {InteractionChoice::Retry, tr("다시 시도")}, {InteractionChoice::Skip, tr("건너뛰기")}, {InteractionChoice::Rename, tr("이름 변경")}
    };
    for (const auto& [Choice, Text] : Choices)
    {
        auto* Button = new fm::ui::Button(Text, dialog_);
        Button->setObjectName("hc.jobs.choice." + QString::number(static_cast<int>(Choice)));
        choices_[Choice] = Button; Buttons->addWidget(Button);
        connect(Button, &QPushButton::clicked, this, [this, Choice] { answer(Choice); });
    }
    permission_ = new fm::ui::Button(tr("권한 오류 자세히(&P)"), dialog_); permission_->setObjectName("hc.jobs.permission"); Buttons->addWidget(permission_);
    fileLocks_ = new fm::ui::Button(tr("파일 사용 정보(&L)"), dialog_); fileLocks_->setObjectName("hc.jobs.fileLocks"); Buttons->addWidget(fileLocks_);
    connect(fileLocks_, &QPushButton::clicked, this, [this] {
        const auto Current = scheduler_.Snapshot(selectedId()); if (!Current.HasValue()) return;
        const auto Snapshot = Current.Value();
        const bool Waiting = Snapshot.interaction_ && Snapshot.interaction_->kind_ == InteractionKind::RetryFailure
            && Snapshot.interaction_->reason_.starts_with("file-in-use:");
        const bool PermissionInspection = Snapshot.interaction_ && Snapshot.interaction_->kind_ == InteractionKind::Elevation;
        const auto Target = Waiting || PermissionInspection ? Snapshot.interaction_->source_ : Snapshot.error_ ? Snapshot.error_->nativePath_ : std::string{};
        if (!hc::filelocks::IsLocalLockPath(QString::fromStdString(Target))) return;
        hc::ui::FileLockDialogRequest Request;
        Request.source_ = QString::fromStdString(Target);
        Request.destination_ = Waiting ? QString::fromStdString(Snapshot.interaction_->destination_) : QString();
        Request.reason_ = Waiting ? QString::fromStdString(Snapshot.interaction_->reason_.substr(12))
            : Snapshot.error_ ? ErrorText(*Snapshot.error_) : Snapshot.interaction_ ? QString::fromStdString(Snapshot.interaction_->reason_) : QString();
        Request.canRetry_ = Waiting;
        Request.canSkip_ = Waiting && std::find(Snapshot.interaction_->choices_.begin(),Snapshot.interaction_->choices_.end(),InteractionChoice::Skip) != Snapshot.interaction_->choices_.end();
        QPointer<FileJobUi> Owner(this);
        const auto InteractionId = Snapshot.interaction_ ? Snapshot.interaction_->id_ : std::string{};
        Request.current_ = [Owner, Id = Snapshot.id_, InteractionId] {
            if (!Owner) return false;
            const auto Now = Owner->Scheduler().Snapshot(Id); if (!Now.HasValue()) return false;
            return InteractionId.empty() || (Now.Value().state_ == JobState::WaitingForUser && !Now.Value().cancelRequested_
                && Now.Value().interaction_ && Now.Value().interaction_->id_ == InteractionId);
        };
        hc::ui::FileLockDialog Dialog(std::move(Request), dialog_); Dialog.exec();
        if (!Owner || !Waiting) return;
        const auto Fresh = scheduler_.Snapshot(Snapshot.id_);
        if (!Fresh.HasValue() || Fresh.Value().state_ != JobState::WaitingForUser || Fresh.Value().cancelRequested_
            || !Fresh.Value().interaction_ || Fresh.Value().interaction_->id_ != InteractionId) return;
        using Reply = hc::ui::FileLockDialog::Reply;
        if (Dialog.SelectedReply() == Reply::CancelJob) scheduler_.Cancel(Snapshot.id_);
        else if (Dialog.SelectedReply() == Reply::Retry || Dialog.SelectedReply() == Reply::Skip)
            scheduler_.Reply(Snapshot.id_, {InteractionId, Dialog.SelectedReply() == Reply::Retry ? InteractionChoice::Retry : InteractionChoice::Skip, {}});
        updateSelection();
    });
    connect(permission_, &QPushButton::clicked, this, [this] {
        const auto Current=scheduler_.Snapshot(selectedId()); if(!Current.HasValue())return;
        const auto S=Current.Value(); const auto Op=static_cast<hc::operations::FileOperation>(jobs_->currentData(Qt::UserRole+1).toInt());
        hc::ui::PermissionRequest Request; Request.screen_=Op==hc::operations::FileOperation::Move?"A03":Op==hc::operations::FileOperation::Delete?"A04":Op==hc::operations::FileOperation::Rename?"A05":Op==hc::operations::FileOperation::MakeDirectory?"A06":"A02";
        if(S.error_&&S.error_->code_.starts_with("elevation-outcome-unknown"))Request.screen_="A08";
        Request.source_=QString::fromStdString(S.progress_.currentItem_); Request.destination_=endpoints_.contains(S.id_)?endpoints_.at(S.id_):QString(); Request.reason_=S.error_?ErrorText(*S.error_):tr("실제 작업에서 확인된 권한 오류 또는 권한 응답 요청입니다.");
        if(S.interaction_) {
            const auto Has=[&](InteractionChoice C){const auto& V=S.interaction_->choices_;return std::find(V.begin(),V.end(),C)!=V.end();};
            Request.canSkip_=Has(InteractionChoice::Skip);Request.canRetry_=Has(InteractionChoice::Retry);Request.canCopyOnly_=Has(InteractionChoice::CopyOnly);
            if(Op==hc::operations::FileOperation::Move)Request.screen_=Request.canCopyOnly_?"A03":"A02";
            Request.canElevate_=Has(InteractionChoice::Elevate);Request.canApplyAll_=Has(InteractionChoice::ElevateAll)||Has(InteractionChoice::SkipAll);Request.jobId_=QString::fromStdString(S.id_);
            if(!S.interaction_->source_.empty())Request.source_=QString::fromStdString(S.interaction_->source_);
            if(!S.interaction_->destination_.empty())Request.destination_=QString::fromStdString(S.interaction_->destination_);
            Request.reason_=QString::fromStdString(S.interaction_->reason_);
        }
        hc::ui::PermissionDialog Dialog(Request,dialog_); Dialog.exec();
        if(S.interaction_) {
            std::optional<InteractionChoice> Choice;
            switch(Dialog.ResultDecision()) {
            case hc::ui::PermissionDialog::Elevate:Choice=Dialog.ApplyAll()?InteractionChoice::ElevateAll:InteractionChoice::Elevate;break;
            case hc::ui::PermissionDialog::Skip:Choice=Dialog.ApplyAll()?InteractionChoice::SkipAll:InteractionChoice::Skip;break;
            case hc::ui::PermissionDialog::CopyOnly:Choice=InteractionChoice::CopyOnly;break;
            case hc::ui::PermissionDialog::Retry:Choice=InteractionChoice::Retry;break;
            default:if(Dialog.result()==QDialog::Rejected)Choice=InteractionChoice::Cancel;break;
            }
            if(Choice)scheduler_.Reply(S.id_,{S.interaction_->id_,*Choice,{}});updateSelection();
        }
    });
    pause_ = new fm::ui::Button(tr("일시 정지"), dialog_); pause_->setObjectName("hc.jobs.pause");
    cancel_ = new fm::ui::Button(tr("취소(&C)"), dialog_); cancel_->setObjectName("hc.jobs.cancel");
    auto* Hide = new fm::ui::Button(tr("백그라운드로(&B)"), dialog_); Hide->setObjectName("hc.jobs.background");
    {
        hc::ui::DialogFrameOptions Frame; Frame.header_ = false;
        auto* Footer = hc::ui::ApplyDialogFrame(dialog_, Frame);
        auto* Outer = qobject_cast<QVBoxLayout*>(dialog_->layout()); Outer->setVerticalSizeConstraint(QLayout::SetMinimumSize);
        hc::ui::PlaceHeaderBand(dialog_, status_);
        delete Footer->takeAt(0);
        auto* Controls = new QWidget(Footer->parentWidget()); Footer->addWidget(Controls);
        progressFooter_ = new QGridLayout(Controls); progressFooter_->setContentsMargins(0, 0, 0, 0); progressFooter_->setHorizontalSpacing(12); progressFooter_->setColumnStretch(0, 1);
        footerLeft_ = new QWidget(Controls); auto* Left = new fm::ui::WrapLayout(footerLeft_); Left->setContentsMargins(0, 0, 0, 0);
        for (auto* Item : {static_cast<QWidget*>(Hide), static_cast<QWidget*>(Toggle), static_cast<QWidget*>(autoClose_)}) { Item->setParent(footerLeft_); Left->addWidget(Item); }
        footerRight_ = new QWidget(Controls); auto* Right = new QHBoxLayout(footerRight_); Right->setContentsMargins(0, 0, 0, 0); Right->setSpacing(8);
        pause_->setParent(footerRight_); cancel_->setParent(footerRight_); Right->addWidget(pause_); Right->addWidget(cancel_);
        progressFooter_->addWidget(footerLeft_, 0, 0); progressFooter_->addWidget(footerRight_, 0, 1, Qt::AlignRight);
        QWidget::setTabOrder(Hide, Toggle); QWidget::setTabOrder(Toggle, autoClose_);
        QWidget::setTabOrder(autoClose_, pause_); QWidget::setTabOrder(pause_, cancel_);
    }
    manager_ = new QDialog(Main); manager_->setObjectName("hc.jobManager");
    manager_->setWindowTitle(tr("백그라운드 작업 관리자")); manager_->resize(920, 780); manager_->setMinimumSize(640, 380);
    if (const auto* Screen = Main->screen()) {
        const auto Available = Screen->availableGeometry().size();
        manager_->resize(qMin(920, Available.width() - 40), qMin(780, Available.height() - 80));
    }
    auto* ManagerLayout = new QVBoxLayout(manager_);
    auto* ManagerHeader = new fm::ui::DialogHeader(manager_); ManagerHeader->setTitle(tr("파일 작업 관리자"));
    ManagerHeader->setSubtitle(tr("창을 닫아도 작업은 계속됩니다. 변경 확정 중에는 현재 저장을 마친 뒤 제어 요청을 처리합니다."));
    ManagerLayout->addWidget(ManagerHeader);
    summary_ = new QLabel(manager_); summary_->setObjectName("hc.jobs.summary"); summary_->setWordWrap(true); summary_->setTextFormat(Qt::PlainText); ManagerLayout->addWidget(summary_);
    auto* FilterRow = new QHBoxLayout;
    filter_ = new QComboBox(manager_); filter_->addItems({tr("전체"), tr("진행 중"), tr("응답 필요"), tr("완료"), tr("실패·취소")});
    filter_->setAccessibleName(tr("작업 상태 필터")); filter_->setObjectName("hc.jobs.filter");
    search_ = new QLineEdit(manager_); search_->setPlaceholderText(tr("작업 이름 또는 경로 검색")); search_->setAccessibleName(tr("작업 검색"));
    search_->setObjectName("hc.jobs.search"); FilterRow->addWidget(filter_); FilterRow->addWidget(search_, 1); ManagerLayout->addLayout(FilterRow);
    jobList_ = new QTreeWidget(manager_); jobList_->setObjectName("hc.jobs.list"); jobList_->setAccessibleName(tr("파일 작업 목록"));
    jobList_->setHeaderLabels({tr("작업"), tr("상태"), tr("처리량"), tr("속도"), tr("현재 항목")});
    jobList_->setRootIsDecorated(false); jobList_->setAlternatingRowColors(true);
    jobList_->setSelectionMode(QAbstractItemView::SingleSelection); jobList_->setUniformRowHeights(true);
    jobList_->header()->setSectionResizeMode(QHeaderView::Interactive); jobList_->setColumnWidth(0, 260); jobList_->setColumnWidth(1, 115);
    jobList_->setColumnWidth(2, 170); jobList_->setColumnWidth(3, 110); jobList_->setMinimumHeight(100); ManagerLayout->addWidget(jobList_, 2);
    auto* Controls = new QScrollArea(manager_); Controls->setObjectName("hc.jobs.controls");
    Controls->setWidgetResizable(true); Controls->setFrameShape(QFrame::NoFrame); Controls->setMinimumHeight(120);
    auto* ControlBody = new QWidget(Controls); auto* ControlRows = new QVBoxLayout(ControlBody); ControlRows->setContentsMargins(0, 0, 0, 0);
    Controls->setWidget(ControlBody); ManagerLayout->addWidget(Controls, 3);
    const auto Group = [&](const QString& Title, const char* Name, QLabel*& Summary) {
        auto* Box = new QGroupBox(Title, ControlBody); Box->setObjectName(Name); auto* Rows = new QVBoxLayout(Box);
        Summary = new QLabel(Box); Summary->setTextFormat(Qt::PlainText); Summary->setWordWrap(true);
        Summary->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred); Rows->addWidget(Summary); ControlRows->addWidget(Box);
        return Rows;
    };
    auto* SelectedRows = Group(tr("선택한 작업"), "hc.jobs.selectedGroup", selectionSummary_); selectionSummary_->setObjectName("hc.jobs.selectedSummary");
    auto* QueueRows = Group(tr("대기열"), "hc.jobs.queueGroup", queueSummary_); queueSummary_->setObjectName("hc.jobs.queueSummary");
    auto* BatchRows = Group(tr("전체 작업"), "hc.jobs.batchGroup", batchSummary_); batchSummary_->setObjectName("hc.jobs.batchSummary");
    auto* HistoryRows = Group(tr("이력 · 중단 작업 확인"), "hc.jobs.historyGroup", historySummary_); historySummary_->setObjectName("hc.jobs.historySummary");
    ControlRows->addStretch();
    auto* QueueControls = new fm::ui::WrapLayout;
    auto* StartQueue = new fm::ui::Button(tr("대기열 시작(&S)"), manager_); StartQueue->setObjectName("hc.jobs.startQueue");
    queueUp_ = new fm::ui::Button(tr("위로(&U)"), manager_); queueUp_->setObjectName("hc.jobs.queueUp");
    queueDown_ = new fm::ui::Button(tr("아래로(&N)"), manager_); queueDown_->setObjectName("hc.jobs.queueDown");
    for (auto* Button : {StartQueue, static_cast<fm::ui::Button*>(queueUp_), static_cast<fm::ui::Button*>(queueDown_)}) Button->setAutoDefault(false);
    concurrency_ = new QSpinBox(manager_); concurrency_->setRange(1, 2); concurrency_->setValue(static_cast<int>(scheduler_.ConcurrencyLimit()));
    concurrency_->setObjectName("hc.jobs.concurrency"); concurrency_->setAccessibleName(tr("동시 실행 작업 수"));
    concurrency_->setToolTip(tr("1~2개 작업을 동시에 실행합니다. 줄이면 기존 작업이 끝날 때까지 기다립니다."));
    QueueControls->addWidget(StartQueue); QueueControls->addWidget(queueUp_); QueueControls->addWidget(queueDown_);
    auto* ConcurrencyGroup = new QWidget(manager_); auto* ConcurrencyLayout = new QHBoxLayout(ConcurrencyGroup);
    ConcurrencyLayout->setContentsMargins(0, 0, 0, 0);
    auto* ConcurrencyLabel = new QLabel(tr("동시 실행(&J)"), ConcurrencyGroup); ConcurrencyLabel->setBuddy(concurrency_);
    ConcurrencyLayout->addWidget(ConcurrencyLabel); ConcurrencyLayout->addWidget(concurrency_); QueueControls->addWidget(ConcurrencyGroup);
    QueueRows->addLayout(QueueControls);
    connect(StartQueue, &QPushButton::clicked, this, [this] { controlAll(JobControl::Start); });
    connect(queueUp_, &QPushButton::clicked, this, [this] { reorderSelected(-1); });
    connect(queueDown_, &QPushButton::clicked, this, [this] { reorderSelected(1); });
    connect(concurrency_, &QSpinBox::valueChanged, this, [this](int Value) { scheduler_.SetConcurrencyLimit(Value); });
    auto* BatchControls = new fm::ui::WrapLayout;
    for (const auto& Control : std::vector<std::pair<JobControl, QString>>{{JobControl::Pause, tr("모두 일시 정지(&A)")}, {JobControl::Resume, tr("일시 정지 모두 해제(&R)")}, {JobControl::Cancel, tr("모든 작업 취소(&X)")}}) {
        auto* Button = new fm::ui::Button(Control.second, manager_); Button->setAutoDefault(false);
        Button->setObjectName("hc.jobs.all." + QString::number(static_cast<int>(Control.first))); BatchControls->addWidget(Button);
        Button->setToolTip(Control.first == JobControl::Cancel ? tr("완료된 변경은 유지하고 남은 작업을 취소합니다. 진행 중인 변경 확정과 정리가 끝나면 취소됩니다.")
            : Control.first == JobControl::Pause ? tr("취소가 아닌 일시 정지를 요청합니다. 진행 중인 변경 확정이 끝난 안전한 시점에 멈춥니다.")
            : tr("일시 정지 요청을 해제합니다. 시작 전 대기열은 대기열 시작으로 실행하며, 응답 대기 작업은 답변이 필요합니다."));
        connect(Button, &QPushButton::clicked, this, [this, Kind = Control.first] { controlAll(Kind); });
    }
    BatchRows->addLayout(BatchControls);
    auto* ManagerButtons = new fm::ui::WrapLayout;
    managerOpen_ = new fm::ui::Button(tr("상세 보기(&D)"), manager_);
    managerOpen_->setObjectName("hc.jobs.managerOpen");
    managerPause_ = new fm::ui::Button(tr("일시 정지(&P)"), manager_); managerPause_->setObjectName("hc.jobs.managerPause");
    managerCancel_ = new fm::ui::Button(tr("작업 취소(&C)"), manager_); managerCancel_->setObjectName("hc.jobs.managerCancel");
    managerStart_ = new fm::ui::Button(tr("선택 작업 시작(&G)"), manager_); managerStart_->setObjectName("hc.jobs.managerStart");
    managerRetry_ = new fm::ui::Button(tr("실패 항목 재시도(&T)"), manager_); managerRetry_->setObjectName("hc.jobs.managerRetry");
    managerRetry_->setToolTip(tr("현재 세션의 일반 파일 복사·이동에서 목적지 저장 전 실패한 항목을 재검증합니다. 중단 기록·폴더·압축·삭제는 상세 결과를 확인하세요."));
    connect(managerStart_, &QPushButton::clicked, this, [this] {
        auto Result = scheduler_.Control({selectedId()}, JobControl::Start);
        if (!Result.HasValue()) hc::ui::MessageDialog::warning(manager_, tr("대기열 시작 실패"), ErrorText(Result.Failure()));
        updateManager(); updateSelection();
    });
    connect(managerRetry_, &QPushButton::clicked, this, [this] { retrySelected(); });
    auto* RenameHistory = new fm::ui::Button(tr("이름 변경 이력(&L)"), manager_); RenameHistory->setObjectName("hc.jobs.renameHistory"); RenameHistory->setAutoDefault(false);
    auto* HistoryControls = new fm::ui::WrapLayout; HistoryControls->addWidget(RenameHistory);
    connect(RenameHistory, &QPushButton::clicked, this, [this] { hc::ui::RenameHistoryDialog History(this, manager_); History.exec(); });
    auto* Clear = new fm::ui::Button(tr("끝난 작업 이력 지우기(&H)"), manager_); clearHistory_ = Clear; Clear->setObjectName("hc.jobs.clearHistory");
    Clear->setAutoDefault(false);
    Clear->setToolTip(tr("완료·실패·취소 기록을 지웁니다. 실행 중인 작업이나 파일에는 영향을 주지 않습니다."));
    HistoryControls->addWidget(Clear); HistoryRows->addLayout(HistoryControls);
    auto* CloseManager = new fm::ui::Button(tr("닫기(&O)"), manager_); CloseManager->setObjectName("hc.jobs.close");
    for (auto* Button : {managerOpen_, managerStart_, managerPause_, managerCancel_, managerRetry_}) {
        Button->setAutoDefault(false); ManagerButtons->addWidget(Button);
    }
    SelectedRows->addLayout(ManagerButtons);
    auto* ManagerFooter = new QHBoxLayout; ManagerFooter->addStretch(); ManagerFooter->addWidget(CloseManager);
    CloseManager->setAutoDefault(false); ManagerLayout->addLayout(ManagerFooter);
    {
        hc::ui::DialogFrameOptions Frame; Frame.header_ = false;
        auto* Footer = hc::ui::ApplyDialogFrame(manager_, Frame);
        hc::ui::PlaceHeaderBand(manager_, ManagerHeader);
        hc::ui::MoveToFooter(Footer, CloseManager);
    }
    std::vector<QWidget*> FocusOrder{filter_, search_, jobList_, managerOpen_, managerStart_, managerPause_, managerCancel_, managerRetry_,
        StartQueue, queueUp_, queueDown_, concurrency_};
    for (const auto Control : {JobControl::Pause, JobControl::Resume, JobControl::Cancel})
        FocusOrder.push_back(manager_->findChild<QPushButton*>("hc.jobs.all." + QString::number(static_cast<int>(Control))));
    FocusOrder.insert(FocusOrder.end(), {RenameHistory, Clear, CloseManager});
    for (size_t I = 1; I < FocusOrder.size(); ++I) QWidget::setTabOrder(FocusOrder[I - 1], FocusOrder[I]);
    connect(CloseManager, &QPushButton::clicked, manager_, &QDialog::hide);
    connect(managerOpen_, &QPushButton::clicked, this, [this] { showProgress(); });
    connect(jobList_, &QTreeWidget::itemDoubleClicked, this, [this] { showProgress(); });
    connect(jobList_, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem* Item) {
        if (Item) jobs_->setCurrentIndex(jobs_->findData(Item->data(0, Qt::UserRole)));
        updateSelection();
    });
    connect(filter_, &QComboBox::currentIndexChanged, this, [this] { updateManager(); updateSelection(); });
    connect(search_, &QLineEdit::textChanged, this, [this] { updateManager(); updateSelection(); });
    connect(managerPause_, &QPushButton::clicked, this, [this] { controlSelected(true); });
    connect(managerCancel_, &QPushButton::clicked, this, [this] { controlSelected(false); });
    if (auto* Window = qobject_cast<QMainWindow*>(Main)) {
        indicator_ = new fm::ui::Button(tr("파일 작업"), Window); indicator_->setObjectName("hc.jobs.indicator");
        Window->statusBar()->addPermanentWidget(indicator_);
        connect(indicator_, &QPushButton::clicked, this, [this] { Show(); });
    }
    connect(Clear, &QPushButton::clicked, this, [this, Clear] {
        if (clearingHistory_) return;
        clearingHistory_ = true;
        std::vector<std::string> Ids;
        for (const auto& [Id, Result] : results_) Ids.push_back(Id);
        Clear->setEnabled(false);
        using EraseResult = std::pair<std::vector<std::string>, std::optional<Error>>;
        auto* Watcher = new QFutureWatcher<EraseResult>(this);
        connect(Watcher, &QFutureWatcher<EraseResult>::finished, this, [this, Watcher, Clear] {
            const auto Result = Watcher->result(); Watcher->deleteLater(); clearingHistory_ = false;
            for (const auto& Id : Result.first)
            {
                scheduler_.Forget(Id); results_.erase(Id); samples_.erase(Id); sampleKinds_.erase(Id); samplePhases_.erase(Id); endpoints_.erase(Id); sizeFormats_.erase(Id); retryRequests_.erase(Id); preparedRetries_.erase(Id); retryContexts_.erase(Id);
                const auto Row = jobs_->findData(QString::fromStdString(Id)); if (Row >= 0) jobs_->removeItem(Row);
            }
            updateSelection(); updateManager();
            if (Result.second) hc::ui::MessageDialog::warning(dialog_, tr("이력 삭제 실패"), QString::fromStdString(Result.second->code_));
        });
        Watcher->setFuture(QtConcurrent::run([Journal = journal_, Ids = std::move(Ids)]() -> EraseResult {
            EraseResult Result;
            for (const auto& Id : Ids)
            {
                auto Removed = Journal->Forget(Id); if (!Removed.HasValue()) { Result.second = Removed.Failure(); break; }
                Result.first.push_back(Id);
            }
            return Result;
        }));
    });
    connect(Hide, &QPushButton::clicked, dialog_, &QDialog::hide);
    connect(pause_, &QPushButton::clicked, this, [this] { controlSelected(true); });
    connect(cancel_, &QPushButton::clicked, this, [this] { controlSelected(false); });
    connect(jobs_, &QComboBox::currentIndexChanged, this, [this] { renamed_->clear(); resultOffset_ = 0; updateSelection(); });
    timer_ = new QTimer(this);
    connect(timer_, &QTimer::timeout, this, [this] { updateJobs(); });
    timer_->start(250);
    auto* Recovery = new QFutureWatcher<Result<std::vector<JobResult>>>(this);
    connect(Recovery, &QFutureWatcher<Result<std::vector<JobResult>>>::finished, this, [this, Recovery] {
        auto Recovered = Recovery->result(); Recovery->deleteLater();
        if (!Recovered.HasValue())
        {
            JobResult Failure; Failure.snapshot_.id_ = "hc-job-journal-recovery"; Failure.snapshot_.state_ = JobState::Failed;
            Failure.snapshot_.error_ = Recovered.Failure(); Recovered = std::vector<JobResult>{Failure};
        }
        for (auto& Item : Recovered.Value())
        {
            const auto Id = Item.snapshot_.id_; if (pending_.contains(Id) || results_.contains(Id)) continue;
            const auto Label = Item.snapshot_.metadata_.label_;
            endpoints_[Id] = DisplayPath(Item.snapshot_.metadata_.source_) + QStringLiteral(" → ") + DisplayPath(Item.snapshot_.metadata_.destination_);
            results_.emplace(Id, std::move(Item)); jobs_->addItem(tr("이전 작업 · %1").arg(Label.empty() ? QString::fromStdString(Id) : QString::fromStdString(Label)), QString::fromStdString(Id));
        }
        updateSelection();
    });
    Recovery->setFuture(QtConcurrent::run([Journal = journal_]() -> Result<std::vector<JobResult>> {
        auto Results = Journal->Recover(); if (!Results.HasValue()) return Results.Failure();
        for (auto& Job : Results.Value()) {
            if (!Job.writes_ || !Job.snapshot_.error_ || Job.snapshot_.error_->code_ != "job-interrupted") continue;
            uint64_t CommittedWrites = 0;
            for (uint64_t Row = 0; Row < Job.writes_->Count(); ++Row) {
                auto Page = Job.writes_->Read(Row, 1); if (!Page.HasValue()) return Page.Failure();
                auto Record = DecodeWriteRecovery(Page.Value().items_.front()); if (!Record.HasValue()) return Record.Failure();
                if (Record.Value().phase_ == WritePhase::Aborted) continue;
                if (Record.Value().phase_ == WritePhase::Finished) {
                    ++CommittedWrites;
                    continue;
                }
                auto Recovered = RecoverNativeWrite(Record.Value());
                if (!Recovered.HasValue()) { Job.cleanupErrors_.push_back(Recovered.Failure()); continue; }
                // Keep the interrupted checkpoint: recovery is idempotent and
                // preserved artifacts must remain visible on subsequent starts.
                Job.cleanupErrors_.insert(Job.cleanupErrors_.end(), Recovered.Value().cleanupErrors_.begin(), Recovered.Value().cleanupErrors_.end());
                if (Recovered.Value().destinationCommitted_) ++CommittedWrites;
                for (const auto& At : Recovered.Value().preservedArtifacts_)
                    Job.cleanupErrors_.push_back({ErrorCategory::Unavailable, "recovery-artifact-preserved", {}, {}, false, At.root_});
            }
            if (CommittedWrites) Job.cleanupErrors_.push_back({ErrorCategory::Unavailable, "recovery-committed-checkpoints", {}, {}, false, std::to_string(CommittedWrites)});
        }
        return Results;
    }));
    updateSelection();
}

FileJobUi::~FileJobUi() { timer_->stop(); scheduler_.Shutdown(); if(hc::permissions::Controller()==this)QCoreApplication::instance()->setProperty("hc.permission.controller",QVariant::fromValue<QObject*>(previousPermissionController_.data())); }

std::shared_ptr<IFileElevationService> FileJobUi::ElevationService() const {
    if(!hc::settings::AppSettings().Read(hc::settings::Section::Options).toObject()["allowElevation"].toBool(true))return {};
    return hc::permissions::MakeFileElevationService();
}
QString FileJobUi::SubmitSecurityJob(const QString& Path,bool Grant,const QJsonObject& Restore) {
    auto Service=ElevationService();if(!Service)return tr("설정에서 관리자 실행을 허용해야 합니다.");
    auto Reader=MakeNativeReadProvider(LocatorKind::Local);auto Writer=MakeNativeWritableStorage(LocatorKind::Local,Reader);
    auto Source=Writer->Inspect({LocatorKind::Local,Path.toStdString()},{});if(!Source.HasValue()||!Source.Value())return tr("항목의 파일 ID를 확인하지 못했습니다.");
    FileElevationRequest Request;Request.source_=*Source.Value();Request.destination_=Source.Value()->locator_;Request.grantAccess_=Grant;
    Request.action_=Restore.isEmpty()?FileElevationAction::SecurityChange:FileElevationAction::SecurityRestore;
    Request.backupPath_=Restore["backupPath"].toString().toStdString();Request.backupIdentity_=Restore["backupIdentity"].toString().toStdString();Request.securityDigest_=Restore["appliedDigest"].toString().toStdString();
    PreparedJob Job;Job.request_.maxRetries_=0;Job.request_.resourceLanes_={FileOperationLane(Request.destination_)};
    Job.request_.metadata_.source_=Path.toStdString();Job.request_.metadata_.destination_=Path.toStdString();Job.request_.metadata_.options_=Restore.isEmpty()?"security-backup-before-change":"security-identity-and-digest-restore";
    Job.request_.metadata_.kind_ = Restore.isEmpty() ? JobKind::SecurityChange : JobKind::SecurityRestore;
    Job.task_=std::make_unique<FileElevationJob>(std::move(Request),std::move(Service));auto Started=Submit(std::move(Job),Restore.isEmpty()?tr("소유권·권한 변경"):tr("원래 권한 복원"));
    return Started.HasValue()?QString():QString::fromStdString(Started.Failure().code_);
}

Result<JobHandle> FileJobUi::Submit(FileOperationRequest Request, OperationContext Context, bool Deferred)
{
    Request.fileLockInteractions_ = true;
    Request.calculateSizeBefore_ = hc::settings::AppSettings().CommittedObject()["options"].toObject()["folderSizeBeforeOperations"].toBool(true);
    if ((Request.selection_ && hc::folders::IsNetwork(Request.selectionRoot_))
        || std::any_of(Request.sources_.begin(), Request.sources_.end(), [](const auto& Source) { return hc::folders::IsNetwork(Source.entry_.locator_); }))
        Request.calculateSizeBefore_ = false;
    Request.deleteWhileListing_ = hc::settings::AppSettings().CommittedObject()["options"].toObject()["deleteWhileListing"].toBool(true);
    Context.providerInteractions_ = true;
    if (Request.operation_ == hc::operations::FileOperation::Copy && Request.selection_ && Request.calculateSizeBefore_
        && Request.selectionRoot_.kind_ == LocatorKind::Local && Request.destination_.kind_ == LocatorKind::Local
        && !hc::settings::AppSettings().CommittedObject()["options"].toObject()["enableJobRecovery"].toBool(false))
        Context.fileCopyWorkers_ = 0;
    Context.flushFileContents_ = !hc::settings::AppSettings().CommittedObject()["options"].toObject()["delayedFileWrites"].toBool(true);
    if (closing_) return Error{ErrorCategory::Cancelled, "window-closing"};
    const auto Label = Request.sources_.empty() ? Request.destination_.root_ : Request.sources_.front().entry_.name_;
    const auto Operation = Request.operation_;
    const auto Destination = Operation == hc::operations::FileOperation::Delete ? QString{} : DisplayPath(Serialize(Request.destination_));
    const auto Source = Request.sources_.empty() ? DisplayPath(Serialize(Request.selectionRoot_)) : DisplayPath(Serialize(Request.sources_.front().entry_.locator_));
    if(!Request.elevation_)Request.elevation_=ElevationService();
    auto Prepared = PrepareFileOperation(Request, Context); if (!Prepared.HasValue()) return Prepared.Failure();
    Prepared.Value().request_.deferred_ = Deferred;
    QString Options = tr("부모 폴더: %1").arg(Request.createParents_ ? tr("만듦") : tr("만들지 않음"));
    if (Operation == hc::operations::FileOperation::Copy || Operation == hc::operations::FileOperation::Move)
        Options += tr(" · 충돌: %1 · 대상 이름: %2")
            .arg(Request.conflict_ == ConflictPolicy::Ask ? tr("확인") : Request.conflict_ == ConflictPolicy::Replace ? tr("교체") : tr("건너뜀"))
            .arg(Request.destinationName_.empty() ? tr("원래 이름") : QString::fromStdString(Request.destinationName_));
    if (Operation == hc::operations::FileOperation::Copy || Operation == hc::operations::FileOperation::Move) {
        Options += tr(" · 필터: %1 · 속도: %2 · 파일 메타데이터: %3 · SHA-256: %4 · DACL: %5 · ADS: %6")
            .arg(QString::fromStdString(Request.includePattern_)).arg(Request.bytesPerSecondLimit_ ? tr("%1 B/s").arg(Request.bytesPerSecondLimit_) : tr("제한 없음"))
            .arg(Request.copyOptions_.metadata_ ? tr("유지") : tr("기본값")).arg(Request.copyOptions_.verify_ ? tr("검증") : tr("사용 안 함"))
            .arg(Request.copyOptions_.acl_ ? tr("복사") : tr("사용 안 함")).arg(Request.copyOptions_.streams_ ? tr("복사") : tr("사용 안 함"));
    }
    if (Operation == hc::operations::FileOperation::Delete)
        Options = tr("삭제: %1 · 읽기 전용: %2")
            .arg(Request.deleteMode_ == DeleteMode::Recycle ? tr("휴지통") : tr("영구 삭제"))
            .arg(Request.readOnly_ == ReadOnlyPolicy::Ask ? tr("확인") : Request.readOnly_ == ReadOnlyPolicy::Allow ? tr("허용") : tr("건너뜀"));
    Prepared.Value().request_.metadata_ = {Label, Source.toStdString(), Destination.toStdString(), Options.toStdString(), {}};
    Prepared.Value().request_.metadata_.kind_ = Operation == hc::operations::FileOperation::Copy ? JobKind::Copy
        : Operation == hc::operations::FileOperation::Move ? JobKind::Move
        : Operation == hc::operations::FileOperation::Delete ? (Request.deleteMode_ == DeleteMode::Recycle ? JobKind::Recycle : JobKind::Delete)
        : Operation == hc::operations::FileOperation::Rename ? JobKind::Rename : JobKind::MakeDirectory;
    auto Submitted = track(scheduler_.Submit(std::move(Prepared.Value().request_), std::move(Prepared.Value().task_)), Operation, Label);
    if (Submitted.HasValue()) {
        if (!Request.selection_ && !Request.sources_.empty() && (Operation == hc::operations::FileOperation::Copy || Operation == hc::operations::FileOperation::Move)
            && std::ranges::all_of(Request.sources_, [](const auto& Source) { return !Source.entry_.directory_ && Source.entry_.locator_.kind_ == LocatorKind::Local; })) {
            retryRequests_[Submitted.Value().id_] = Request; retryContexts_[Submitted.Value().id_] = Context;
        }
        endpoints_[Submitted.Value().id_] = Source + (Destination.isEmpty() ? QString{} : QStringLiteral(" → ") + Destination); updateSelection(); }
    return Submitted;
}

Result<JobHandle> FileJobUi::Submit(ArchiveOperationRequest Request, OperationContext Context, bool Deferred)
{
    Context.providerInteractions_ = true;
    Context.flushFileContents_ = !hc::settings::AppSettings().CommittedObject()["options"].toObject()["delayedFileWrites"].toBool(true);
    if (closing_) return Error{ErrorCategory::Cancelled, "window-closing"};
    const auto Label = Request.sources_.empty() ? Request.destination_.nesting_.empty() ? Request.destination_.root_ : Request.destination_.nesting_.back().entry_ : Request.sources_.front().entry_.name_;
    const auto Operation = Request.operation_;
    const auto Destination = DisplayPath(Serialize(Request.destination_));
    const auto Source = Request.sources_.empty() ? DisplayPath(Serialize(Request.selectionRoot_)) : DisplayPath(Serialize(Request.sources_.front().entry_.locator_));
    auto Prepared = PrepareArchiveOperation(Request, Context); if (!Prepared.HasValue()) return Prepared.Failure();
    Prepared.Value().request_.deferred_ = Deferred;
    Prepared.Value().request_.metadata_ = {Label, Source.toStdString(), Destination.toStdString(), "conflict=" + std::to_string(static_cast<int>(Request.conflict_)), {}};
    Prepared.Value().request_.metadata_.kind_ = Operation == hc::operations::FileOperation::Copy ? JobKind::ArchiveAdd
        : Operation == hc::operations::FileOperation::Move ? JobKind::ArchiveMove
        : Operation == hc::operations::FileOperation::Delete ? JobKind::ArchiveDelete
        : Operation == hc::operations::FileOperation::Rename ? JobKind::ArchiveRename : JobKind::ArchiveMakeDirectory;
    auto Submitted = track(scheduler_.Submit(std::move(Prepared.Value().request_), std::move(Prepared.Value().task_)), Operation, Label);
    if (Submitted.HasValue()) { endpoints_[Submitted.Value().id_] = Source + (Destination.isEmpty() ? QString{} : QStringLiteral(" → ") + Destination); updateSelection(); }
    return Submitted;
}

Result<JobHandle> FileJobUi::Submit(MoveOperationRequest Request, OperationContext Context, bool Deferred)
{
    Context.providerInteractions_ = true;
    Context.flushFileContents_ = !hc::settings::AppSettings().CommittedObject()["options"].toObject()["delayedFileWrites"].toBool(true);
    if (closing_) return Error{ErrorCategory::Cancelled, "window-closing"};
    const auto Label = Request.sources_.empty() ? Request.destination_.root_ : Request.sources_.front().name_;
    const auto Destination = DisplayPath(Serialize(Request.destination_));
    const auto Source = Request.sources_.empty() ? DisplayPath(Serialize(Request.selectionRoot_)) : DisplayPath(Serialize(Request.sources_.front().locator_));
    auto Prepared = PrepareMoveOperation(Request, Context); if (!Prepared.HasValue()) return Prepared.Failure();
    Prepared.Value().request_.deferred_ = Deferred;
    Prepared.Value().request_.metadata_ = {Label, Source.toStdString(), Destination.toStdString(), "conflict=" + std::to_string(static_cast<int>(Request.conflict_)), {}};
    Prepared.Value().request_.metadata_.kind_ = JobKind::Move;
    auto Submitted = track(scheduler_.Submit(std::move(Prepared.Value().request_), std::move(Prepared.Value().task_)), hc::operations::FileOperation::Move, Label);
    if (Submitted.HasValue()) { endpoints_[Submitted.Value().id_] = Source + (Destination.isEmpty() ? QString{} : QStringLiteral(" → ") + Destination); updateSelection(); }
    return Submitted;
}

Result<JobHandle> FileJobUi::Submit(PreparedJob Job, const QString& Label, PreparedRetry Retry, std::function<void(const std::string&)> Resubmitted)
{
    Job.request_.operation_.providerInteractions_ = true;
    Job.request_.operation_.flushFileContents_ = !hc::settings::AppSettings().CommittedObject()["options"].toObject()["delayedFileWrites"].toBool(true);
    if (closing_) return Error{ErrorCategory::Cancelled, "window-closing"};
    Job.request_.metadata_.label_ = Label.toStdString();
    const auto Presentation = Job.request_.metadata_.kind_;
    const bool Renaming = Presentation == JobKind::Rename || Presentation == JobKind::BatchRename || Presentation == JobKind::RenameUndo
        || Presentation == JobKind::RenameRedo || Presentation == JobKind::RenameResume || Presentation == JobKind::RenameRestore;
    const auto Kind = Renaming ? hc::operations::FileOperation::Rename : hc::operations::FileOperation::Copy;
    const auto Context = Job.request_.operation_;
    auto Submitted = track(scheduler_.Submit(std::move(Job.request_), std::move(Job.task_)), Kind, Label.toStdString());
    if (Submitted.HasValue()) jobs_->setItemText(jobs_->findData(QString::fromStdString(Submitted.Value().id_)), Label);
    if (Submitted.HasValue() && Retry) { preparedRetries_[Submitted.Value().id_] = {std::move(Retry), std::move(Resubmitted)}; retryContexts_[Submitted.Value().id_] = Context; }
    return Submitted;
}

Result<JobHandle> FileJobUi::track(Result<JobHandle> Submitted, hc::operations::FileOperation Operation, const std::string& Label)
{
    const auto OperationLabel = Operation == hc::operations::FileOperation::Copy ? tr("복사")
        : Operation == hc::operations::FileOperation::Move ? tr("이동") : Operation == hc::operations::FileOperation::Delete ? tr("삭제")
        : Operation == hc::operations::FileOperation::Rename ? tr("이름 변경") : tr("새 폴더");
    if (!Submitted.HasValue()) return Submitted.Failure();
    const auto& Handle = Submitted.Value();
    pending_.emplace(Handle.id_, Handle);
    hc::columns::FormatOptions SizeFormat; SizeFormat.size_=hc::columns::SizeFormat::Adaptive; SizeFormat.precision_=1;
    for (auto* Panel:main_->findChildren<CmpPanel*>()) if (Panel->property("hc.activePanel").toBool())
        if (auto* Grid=qobject_cast<CGridView*>(Panel->GetFocusView(Panel->CurrentTabIndex())))
            for (const auto& Column:Grid->ColumnProfile().columns_) if (Column.fieldId_=="file.size" && Column.template_.isEmpty()) { SizeFormat=Column.format_; break; }
    sizeFormats_[Handle.id_]=QJsonObject{{"size",int(SizeFormat.size_)},{"precision",SizeFormat.precision_},{"grouping",SizeFormat.grouping_}};
    jobs_->addItem(OperationLabel + " · " + QString::fromStdString(Label), QString::fromStdString(Handle.id_));
    jobs_->setItemData(jobs_->count() - 1, static_cast<int>(Operation), Qt::UserRole + 1);
    jobs_->setCurrentIndex(jobs_->count() - 1);
    const auto Defaults = hc::settings::AppSettings().CommittedObject()["options"].toObject();
    notifications_[Handle.id_] = Defaults["jobNotification"].toString("attention");
    const auto Snapshot = scheduler_.Snapshot(Handle.id_);
    if (Defaults["queueInsertion"] == "first" && Snapshot.HasValue() && Snapshot.Value().deferred_) {
        auto Queue = scheduler_.Snapshots();
        std::erase_if(Queue, [](const auto& Value) { return !Value.deferred_; });
        std::sort(Queue.begin(), Queue.end(), [&](const auto& A, const auto& B) {
            if (A.id_ == Handle.id_ || B.id_ == Handle.id_) return A.id_ == Handle.id_ && B.id_ != Handle.id_;
            return A.queueOrder_ < B.queueOrder_;
        });
        std::vector<std::string> Ids; for (const auto& Value : Queue) Ids.push_back(Value.id_);
        if (!Ids.empty()) {
            const auto Reordered = scheduler_.ReorderQueued(Ids);
            if (!Reordered.HasValue()) hc::ui::MessageDialog::warning(manager_, tr("대기열 순서 변경 실패"), tr("작업은 접수되었습니다. 대기열이 변경되어 기본 순서를 적용하지 못했습니다. 현재 순서를 확인하세요."));
        }
    }
    autoClose_->setChecked(Defaults["jobCloseOnSuccess"].toBool(true));
    if (detailed_ != Defaults["jobDetailed"].toBool(false)) dialog_->findChild<QPushButton*>("hc.jobs.toggleDetails")->click();
    if (Snapshot.HasValue() && Snapshot.Value().deferred_) Show(); else showProgress();
    return Handle;
}

void FileJobUi::ApplyDefaults()
{
    const auto Options = hc::settings::AppSettings().CommittedObject()["options"].toObject();
    SetFileJobPersistence(journal_, Options["enableJobRecovery"].toBool(false), Options["saveJobProgress"].toBool(false));
    const bool ShowRecords = Options["showJobDetails"].toBool(false);
    if (ShowRecords != appliedShowRecords_) {
        appliedShowRecords_ = ShowRecords;
        updateSelection(); dialog_->resize(dialog_->width(),std::min(dialog_->sizeHint().height(),dialog_->screen()->availableGeometry().height()-80));
    }
    const auto Limit = Options["jobConcurrency"].toInt(2);
    if (Limit == appliedConcurrency_) return; // Preserve a session override until the stored default changes.
    if (scheduler_.SetConcurrencyLimit(Limit).HasValue()) {
        appliedConcurrency_ = Limit;
        const QSignalBlocker Block(concurrency_); concurrency_->setValue(Limit);
    }
}

void FileJobUi::Show() { updateManager(); manager_->show(); manager_->raise(); manager_->activateWindow(); updateSelection(); }
void FileJobUi::showProgress() { updateSelection(); fitProgressDialog(); dialog_->show(); dialog_->raise(); dialog_->activateWindow(); }
void FileJobUi::controlSelected(bool Pause)
{
    if (!Pause) {
        const auto Snapshot = scheduler_.Snapshot(selectedId());
        if (Snapshot.HasValue() && IsTerminal(Snapshot.Value().state_)) { dialog_->hide(); return; }
    }
    const auto Id = selectedId();
    const auto Current = scheduler_.Snapshot(Id);
    if (!Current.HasValue()) return;
    const auto Result = Pause ? scheduler_.Pause(Id, !Current.Value().pauseRequested_) : scheduler_.Cancel(Id);
    if (!Result.HasValue()) hc::ui::MessageDialog::warning(dialog_->isVisible() ? dialog_ : manager_, tr("작업 제어 실패"), ErrorText(Result.Failure()));
    updateSelection(); updateManager();
}
std::string FileJobUi::selectedId() const { return jobs_->currentData().toString().toStdString(); }

void FileJobUi::controlAll(JobControl Control)
{
    std::vector<std::string> Ids;
    for (const auto& Snapshot : scheduler_.Snapshots())
        if (acceptsControl(Snapshot, Control)) Ids.push_back(Snapshot.id_);
    if (Ids.empty()) return;
    auto Result = scheduler_.Control(Ids, Control);
    if (!Result.HasValue()) hc::ui::MessageDialog::warning(manager_, tr("일괄 제어 실패"), tr("작업 상태가 변경되었습니다. 목록을 확인하고 다시 실행하세요: %1").arg(ErrorText(Result.Failure())));
    updateManager(); updateSelection();
}
void FileJobUi::reorderSelected(int Direction)
{
    auto Values = scheduler_.Snapshots();
    std::erase_if(Values, [](const auto& Snapshot) { return !Snapshot.deferred_; });
    std::sort(Values.begin(), Values.end(), [](const auto& A, const auto& B) { return A.queueOrder_ < B.queueOrder_; });
    const auto Found = std::find_if(Values.begin(), Values.end(), [this](const auto& Snapshot) { return Snapshot.id_ == selectedId(); });
    if (Found == Values.end()) return;
    const auto Index = std::distance(Values.begin(), Found), Next = Index + Direction;
    if (Next < 0 || Next >= static_cast<std::ptrdiff_t>(Values.size())) return;
    std::swap(Values[Index], Values[Next]); std::vector<std::string> Ids;
    for (const auto& Snapshot : Values) Ids.push_back(Snapshot.id_);
    auto Result = scheduler_.ReorderQueued(Ids);
    if (!Result.HasValue()) hc::ui::MessageDialog::warning(manager_, tr("순서 변경 실패"), ErrorText(Result.Failure()));
    updateManager(); updateSelection();
}
bool FileJobUi::canRetry(const std::string& Id) const
{
    if (!preparingRetry_.empty()) return false;
    const auto Snapshot=scheduler_.Snapshot(Id);if(Snapshot.HasValue()&&Snapshot.Value().error_&&Snapshot.Value().error_->code_.starts_with("elevation-outcome-unknown"))return false;
    const auto Found = results_.find(Id);
    if (Found == results_.end() || (!retryRequests_.contains(Id) && !preparedRetries_.contains(Id)) || !Found->second.cleanupErrors_.empty()) return false;
    const auto Current = scheduler_.Snapshot(Id);
    if (!Current.HasValue() || Current.Value().state_ != JobState::Failed || !Current.Value().retriedAs_.empty()) return false;
    return preparedRetries_.contains(Id) || Current.Value().failedItems_ > 0;
}
void FileJobUi::retrySelected()
{
    const auto Id = selectedId(); if (!canRetry(Id)) return;
    if (preparedRetries_.contains(Id)) {
        auto Record = preparedRetries_.at(Id); auto Context = retryContexts_.at(Id); const auto Original = results_.at(Id);
        Context.stopToken_ = {}; Context.deadline_ = std::chrono::steady_clock::time_point::max();
        auto Prepared = Record.build_(Original, Context);
        if (!Prepared.HasValue()) { hc::ui::MessageDialog::warning(manager_, tr("재시도 접수 실패"), ErrorText(Prepared.Failure())); return; }
        Prepared.Value().request_.metadata_ = Original.snapshot_.metadata_; Prepared.Value().request_.metadata_.retryOf_ = Id;
        const auto Label = QString::fromStdString(Original.snapshot_.metadata_.label_);
        auto Submitted = Submit(std::move(Prepared.Value()), Label, Record.build_, Record.submitted_);
        if (!Submitted.HasValue()) { hc::ui::MessageDialog::warning(manager_, tr("재시도 접수 실패"), ErrorText(Submitted.Failure())); return; }
        if (Record.submitted_) Record.submitted_(Submitted.Value().id_);
        preparedRetries_.erase(Id); retryContexts_.erase(Id); updateManager(); updateSelection(); return;
    }
    auto Request = retryRequests_.at(Id); auto Context = retryContexts_.at(Id); const auto Original = results_.at(Id);
    // A new user action gets a fresh cancellation/deadline, with the original
    // immutable source identity. Destination conflicts are asked again.
    Context.stopToken_ = {}; Context.deadline_ = std::chrono::steady_clock::time_point::max();
    Context.flushFileContents_ = !hc::settings::AppSettings().CommittedObject()["options"].toObject()["delayedFileWrites"].toBool(true);
    Request.conflict_ = ConflictPolicy::Ask; Request.correlationId_.clear();
    managerRetry_->setEnabled(false);
    preparingRetry_ = Id; updateManagerSelection(&Original.snapshot_);
    using RetryResult = Result<FileOperationRequest>;
    auto* Watcher = new QFutureWatcher<RetryResult>(this);
    connect(Watcher, &QFutureWatcher<RetryResult>::finished, this, [this, Watcher, Id, Context] {
        auto Candidate = Watcher->result(); Watcher->deleteLater();
        preparingRetry_.clear();
        if (!Candidate.HasValue()) { hc::ui::MessageDialog::warning(manager_, tr("재시도 검증 실패"), ErrorText(Candidate.Failure())); updateSelection(); return; }
        if (closing_) return;
        const auto Parent = scheduler_.Snapshot(Id); if (!Parent.HasValue() || !canRetry(Id)) { updateSelection(); return; }
        const auto Request = Candidate.Value(); auto Prepared = PrepareFileOperation(Request, Context);
        if (!Prepared.HasValue()) { hc::ui::MessageDialog::warning(manager_, tr("재시도 접수 실패"), ErrorText(Prepared.Failure())); updateSelection(); return; }
        Prepared.Value().request_.metadata_ = Parent.Value().metadata_;
        Prepared.Value().request_.metadata_.retryOf_ = Id; Prepared.Value().request_.metadata_.options_ = tr("충돌: 다시 확인 · 재시도: 저장 전 실패한 항목만").toStdString();
        auto Submitted = track(scheduler_.Submit(std::move(Prepared.Value().request_), std::move(Prepared.Value().task_)), Request.operation_, Request.sources_.front().entry_.name_);
        if (!Submitted.HasValue()) { hc::ui::MessageDialog::warning(manager_, tr("재시도 접수 실패"), ErrorText(Submitted.Failure())); updateSelection(); return; }
        const auto NewId = Submitted.Value().id_; retryRequests_[NewId] = Request; retryContexts_[NewId] = Context; endpoints_[NewId] = endpoints_[Id];
        if (onResubmitted_) onResubmitted_(NewId, Request);
        updateManager(); updateSelection();
    });
    Watcher->setFuture(QtConcurrent::run([Request = std::move(Request), Context, Original]() mutable -> RetryResult {
        std::vector<ItemOutcome> Failed;
        if (Original.outcomes_) {
            for (uint64_t Offset = 0; Offset < Original.outcomes_->Count(); Offset += 200) {
                auto Page = Original.outcomes_->Read(Offset, 200); if (!Page.HasValue()) return Page.Failure();
                for (const auto& Item : Page.Value().items_) if (Item.status_ == ItemOutcome::Status::Failed) Failed.push_back(Item);
            }
        } else for (const auto& Item : Original.items_) if (Item.status_ == ItemOutcome::Status::Failed) Failed.push_back(Item);
        std::vector<OperationSource> Sources;
        for (const auto& Item : Failed) {
            if (!Item.source_ || Item.destinationCommitted_ || Item.sourceRemoved_ || Item.source_->directory_)
                return Error{ErrorCategory::Unsupported, "retry-partially-committed-item-needs-review"};
            const auto Found = std::find_if(Request.sources_.begin(), Request.sources_.end(), [&](const auto& Source) { return Serialize(Source.entry_.id_) == Serialize(Item.source_->id_); });
            if (Found == Request.sources_.end()) return Error{ErrorCategory::StaleRevision, "retry-source-not-in-original-selection"};
            auto Current = Found->reader_->Resolve(Found->entry_.locator_, Context).get(); if (!Current.HasValue()) return Current.Failure();
            if (Serialize(Current.Value().id_) != Serialize(Found->entry_.id_) || Current.Value().size_ != Found->entry_.size_
                || Current.Value().modifiedUtcMs_ != Found->entry_.modifiedUtcMs_)
                return Error{ErrorCategory::StaleRevision, "retry-source-changed"};
            Sources.push_back(*Found);
        }
        if (Sources.empty()) return Error{ErrorCategory::InvalidRequest, "retry-no-failed-items"};
        Request.sources_ = std::move(Sources); return Request;
    }));
}
double FileJobUi::measuredRate(const JobSnapshot& Snapshot) const
{
    if (ProgressRate(Snapshot) == RateMode::None) return 0;
    if ((Snapshot.state_ != JobState::Running && Snapshot.state_ != JobState::Committing
            && !(Snapshot.state_ == JobState::Preparing && deletionJob(Snapshot))) || Snapshot.pauseRequested_ || Snapshot.cancelRequested_) return 0;
    const auto Found = samples_.find(Snapshot.id_); if (Found == samples_.end() || Found->second.size() < 2) return 0;
    const auto& History = Found->second; const auto& Latest = History.back();
    const auto Begin = std::find_if(History.begin(), History.end(), [&](const auto& Sample) { return Sample.elapsed_ >= Latest.elapsed_ - 2000; });
    auto First = Begin;
    for (auto It = Begin; It != History.end(); ++It) if (It->paused_) First = std::next(It);
    if (First == History.end() || Latest.elapsed_ <= First->elapsed_ || Latest.bytes_ < First->bytes_) return 0;
    return 1000.0 * (Latest.bytes_ - First->bytes_) / (Latest.elapsed_ - First->elapsed_);
}
bool FileJobUi::deletionJob(const JobSnapshot& Snapshot) const
{
    if (Snapshot.metadata_.kind_ == JobKind::Delete || Snapshot.metadata_.kind_ == JobKind::Recycle || Snapshot.metadata_.kind_ == JobKind::ArchiveDelete) return true;
    if (Snapshot.progress_.deletedFiles_) return true;
    const int At = jobs_->findData(QString::fromStdString(Snapshot.id_));
    return At >= 0 && jobs_->itemData(At, Qt::UserRole + 1).toInt() == int(hc::operations::FileOperation::Delete);
}
bool FileJobUi::acceptsControl(const JobSnapshot& Snapshot, JobControl Control) const
{
    if (IsTerminal(Snapshot.state_) || Snapshot.cancelRequested_) return false;
    switch (Control) {
    case JobControl::Start: return Snapshot.deferred_;
    case JobControl::Pause: return !Snapshot.pauseRequested_ && Snapshot.state_ != JobState::Paused;
    case JobControl::Resume: return Snapshot.pauseRequested_ || Snapshot.state_ == JobState::Paused;
    case JobControl::Cancel: return true;
    }
    return false;
}
void FileJobUi::updateManagerSelection(const JobSnapshot* Snapshot)
{
    const bool Visible = Snapshot && jobList_->currentItem() && !jobList_->currentItem()->isHidden()
        && jobList_->currentItem()->data(0, Qt::UserRole).toString().toStdString() == Snapshot->id_;
    managerOpen_->setEnabled(Visible); managerStart_->setEnabled(Visible && acceptsControl(*Snapshot, JobControl::Start));
    managerPause_->setEnabled(Visible && (acceptsControl(*Snapshot, JobControl::Pause) || acceptsControl(*Snapshot, JobControl::Resume)));
    managerPause_->setText(pause_->text()); managerCancel_->setEnabled(Visible && acceptsControl(*Snapshot, JobControl::Cancel));
    managerRetry_->setEnabled(Visible && canRetry(Snapshot->id_));
    clearHistory_->setEnabled(!clearingHistory_ && preparingRetry_.empty() && !results_.empty());
    managerRetry_->setText(Visible && preparingRetry_ == Snapshot->id_ ? tr("재시도 확인 중…") : tr("실패 항목 재시도(&T)"));
    std::vector<JobSnapshot> Queue;
    for (const auto& Item : scheduler_.Snapshots()) if (acceptsControl(Item, JobControl::Start)) Queue.push_back(Item);
    std::sort(Queue.begin(), Queue.end(), [](const auto& A, const auto& B) { return A.queueOrder_ < B.queueOrder_; });
    const auto At = std::find_if(Queue.begin(), Queue.end(), [&](const auto& Item) { return Visible && Item.id_ == Snapshot->id_; });
    queueUp_->setEnabled(At != Queue.end() && At != Queue.begin());
    queueDown_->setEnabled(At != Queue.end() && std::next(At) != Queue.end());
    queueSummary_->setText(tr("시작 전 대기열 %1개 · 선택: %2").arg(Queue.size()).arg(At != Queue.end() ? QString::fromStdString(At->metadata_.label_) : tr("대기 작업을 선택하세요")));
    if (!Visible) { selectionSummary_->setText(tr("선택한 작업 없음 · 목록에서 작업을 선택하세요.")); return; }
    QStringList Lines{QString::fromStdString(Snapshot->metadata_.label_), JobStatusText(*Snapshot)};
    const auto Path = endpoints_.contains(Snapshot->id_) ? endpoints_.at(Snapshot->id_)
        : DisplayPath(Snapshot->metadata_.source_) + (Snapshot->metadata_.destination_.empty() ? QString{} : QStringLiteral(" → ") + DisplayPath(Snapshot->metadata_.destination_));
    if (!Path.isEmpty()) Lines.append(tr("대상: %1").arg(Path));
    Lines.append(tr("선택 1개 작업 · 완료 %1 / 건너뜀 %2 / 실패 %3 · 전체 %4")
        .arg(Snapshot->completedItems_).arg(Snapshot->skippedItems_).arg(Snapshot->failedItems_)
        .arg(Snapshot->progress_.plannedItems_ ? tr("%1개 항목").arg(*Snapshot->progress_.plannedItems_) : tr("집계 중")));
    if (!preparingRetry_.empty() && preparingRetry_ == Snapshot->id_) Lines.append(tr("실패 항목과 원본을 확인하고 있습니다. 확인 중에는 추가 재시도를 접수하지 않습니다."));
    else if (Snapshot->cancelRequested_ && !IsTerminal(Snapshot->state_)) Lines.append(tr("현재 변경 확정과 정리를 마친 뒤 취소됩니다. 완료된 변경은 유지됩니다."));
    else if (Snapshot->error_ && Snapshot->error_->code_ == "job-interrupted") Lines.append(tr("이전 실행이 중단되어 자동 실행하지 않습니다. 상세 결과를 확인하세요. 이름 변경은 이력에서 이어 실행하거나 원래 이름으로 복구할 수 있습니다."));
    else if (Snapshot->state_ == JobState::Paused) Lines.append(tr("일시 정지되었습니다. 재개하면 남은 작업을 이어 실행합니다."));
    else if (Snapshot->pauseRequested_) Lines.append(tr("안전한 시점에 일시 정지합니다. 요청을 해제하면 계속 진행합니다."));
    else if (Snapshot->deferred_) Lines.append(tr("시작 전 대기 작업입니다. 선택 작업 시작 또는 대기열 시작으로 실행하세요."));
    else if (Snapshot->state_ == JobState::WaitingForUser) Lines.append(tr("상세 보기에서 질문에 응답하면 이어서 진행합니다."));
    else if (!Snapshot->retriedAs_.empty()) Lines.append(tr("재시도가 새 작업으로 접수되었습니다. 목록에서 후속 작업의 결과를 확인하세요."));
    else if (Snapshot->state_ == JobState::Failed) Lines.append(canRetry(Snapshot->id_) ? tr("목적지 저장 전 실패한 항목만 원본을 다시 확인하여 재시도합니다. 완료 항목은 유지됩니다.") : tr("자동 재시도 대상이 아닙니다. 상세 결과와 원본·대상을 확인하세요."));
    selectionSummary_->setText(Lines.join('\n'));
    managerCancel_->setToolTip(tr("선택한 작업의 남은 항목을 취소합니다. 이미 완료된 변경은 유지하고 현재 변경 확정 후 정리합니다."));
}

void FileJobUi::updateJobs()
{
    if (!hc::settings::AppSettings().Editing()) ApplyDefaults();
    for (auto It = pending_.begin(); It != pending_.end();)
    {
        if (It->second.completion_.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) { ++It; continue; }
        const auto Stored = results_.insert_or_assign(It->first, It->second.completion_.get()).first;
        if (auto Scope = passwordScopes_.find(It->first); Scope != passwordScopes_.end()) { Scope->second.request_stop(); passwordScopes_.erase(Scope); }
        It = pending_.erase(It);
        if (Stored->second.snapshot_.state_ != JobState::Failed || (Stored->second.snapshot_.failedItems_ == 0 && !preparedRetries_.contains(Stored->first)) || !Stored->second.cleanupErrors_.empty()) {
            retryRequests_.erase(Stored->first); retryContexts_.erase(Stored->first);
            preparedRetries_.erase(Stored->first);
        }
        if (Stored->second.snapshot_.state_ == JobState::Failed && !closing_) {
            const auto& Snapshot = Stored->second.snapshot_;
            auto* Message = new hc::ui::MessageDialog(QMessageBox::Warning, tr("파일 작업 실패"),
                tr("파일 작업을 완료하지 못했습니다. 완료 %1 / 건너뜀 %2 / 실패 %3")
                    .arg(Snapshot.completedItems_).arg(Snapshot.skippedItems_).arg(Snapshot.failedItems_),
                QMessageBox::Ok, dialog_->isVisible() ? dialog_ : main_.data());
            Message->setObjectName("hc.jobs.failure");
            Message->setProperty("hc.jobs.id", QString::fromStdString(Snapshot.id_));
            Message->setAttribute(Qt::WA_DeleteOnClose);
            Message->setTextFormat(Qt::PlainText);
            QString Information;
            if (Snapshot.error_) {
                const auto& Error = *Snapshot.error_;
                Information = ErrorText(Error);
                if (!Error.nativePath_.empty()) Information += "\n" + DisplayPath(Error.nativePath_);
            }
            if (!Snapshot.metadata_.source_.empty()) Information += tr("\n원본: %1").arg(DisplayPath(Snapshot.metadata_.source_));
            if (!Snapshot.metadata_.destination_.empty()) Information += tr("\n대상: %1").arg(DisplayPath(Snapshot.metadata_.destination_));
            Message->setInformativeText(Information.trimmed());
            // Consume each completion once; other jobs and controls stay usable.
            Message->setWindowModality(Qt::NonModal);
            Message->show();
        }
        if (onFinished_) onFinished_(Stored->second);
        const auto Notification = notifications_[Stored->first]; notifications_.erase(Stored->first);
        if (auto* Window = qobject_cast<QMainWindow*>(main_.data()); Window && !dialog_->isVisible() && Notification != "none") {
            if (Notification == "attention" || Notification == "sound") QApplication::alert(Window, 3000);
            if (Notification == "sound") QApplication::beep();
            const auto Message = tr("파일 작업 %1 · 완료 %2 / 실패 %3").arg(StateText(Stored->second.snapshot_.state_)).arg(Stored->second.snapshot_.completedItems_).arg(Stored->second.snapshot_.failedItems_);
            Window->statusBar()->showMessage(Message, 10000);
            Window->setProperty("hc.jobs.lastNotification", Message);
        }
        if (Stored->first == selectedId() && autoClose_->isChecked() && CloseFinishedJob(Stored->second)) dialog_->hide();
    }
    recordSamples();
    updateManager();
    // Snapshot polling recovers even when a bounded event subscriber drops progress.
    if (dialog_->isVisible() || manager_->isVisible()) updateSelection();
    if (closing_ && pending_.empty() && main_) { timer_->stop(); main_->close(); }
}

void FileJobUi::updateSelection()
{
    previousPage_->setEnabled(false); nextPage_->setEnabled(false);
    auto Current = scheduler_.Snapshot(selectedId());
    if (!Current.HasValue()) if (const auto Found = results_.find(selectedId()); Found != results_.end()) Current = Found->second.snapshot_;
    const bool Valid = Current.HasValue();
    const auto Snapshot = Valid ? Current.Value() : JobSnapshot{};
    const bool Active = Valid && !IsTerminal(Snapshot.state_);
    pause_->setEnabled(Active && !Snapshot.cancelRequested_ && Snapshot.state_ != JobState::Committing);
    pause_->setText(Snapshot.state_ == JobState::Paused ? tr("재개(&P)") : Snapshot.pauseRequested_ ? tr("일시 정지 요청 해제(&P)") : tr("일시 정지(&P)"));
    if (managerPause_) updateManagerSelection(Valid ? &Snapshot : nullptr);
    cancel_->setEnabled(Valid && (!Active || (!Snapshot.cancelRequested_ && Snapshot.state_ != JobState::Committing)));
    cancel_->setText(Active ? tr("취소(&C)") : tr("닫기(&C)"));
    for (const auto& [Choice, Button] : choices_)
        Button->setVisible(Active && !Snapshot.cancelRequested_ && Snapshot.interaction_
            && std::find(Snapshot.interaction_->choices_.begin(), Snapshot.interaction_->choices_.end(), Choice) != Snapshot.interaction_->choices_.end());
    const bool Password = Active && !Snapshot.cancelRequested_ && Snapshot.interaction_ && Snapshot.interaction_->kind_ == InteractionKind::Password;
    const bool FileInUse = Active && !Snapshot.cancelRequested_ && Snapshot.interaction_ && Snapshot.interaction_->kind_ == InteractionKind::RetryFailure
        && Snapshot.interaction_->reason_.starts_with("file-in-use:");
    const bool PermissionInspection = Active && !Snapshot.cancelRequested_ && Snapshot.interaction_ && Snapshot.interaction_->kind_ == InteractionKind::Elevation;
    const auto LockTarget = FileInUse || PermissionInspection ? QString::fromStdString(Snapshot.interaction_->source_)
        : Snapshot.error_ ? QString::fromStdString(Snapshot.error_->nativePath_) : QString();
    fileLocks_->setVisible(Valid && hc::filelocks::IsLocalLockPath(LockTarget));
    const auto Echo = Password ? QLineEdit::Password : QLineEdit::Normal;
    if (renamed_->echoMode() != Echo) { renamed_->clear(); renamed_->setEchoMode(Echo); }
    renamed_->setPlaceholderText(Password ? tr("암호") : tr("새 이름"));
    renamed_->setAccessibleName(Password ? tr("압축 파일 암호") : tr("충돌한 항목의 새 이름"));
    choices_.at(InteractionChoice::Confirm)->setText(Password ? tr("확인")
        : Snapshot.interaction_ && Snapshot.interaction_->kind_ == InteractionKind::ReadOnlyRemoval ? tr("허용")
        : Snapshot.interaction_ && Snapshot.interaction_->reason_ == "confirm-recycle-delete" ? tr("휴지통으로") : tr("영구 삭제"));
    renamed_->setVisible(!Password && choices_.at(InteractionChoice::Rename)->isVisibleTo(dialog_));
    if (Password) {
        choices_.at(InteractionChoice::Confirm)->setText(tr("비밀번호 입력…"));
        if (!passwordDialogOpen_ && passwordRequestId_ != Snapshot.interaction_->id_) {
            passwordRequestId_ = Snapshot.interaction_->id_;
            QTimer::singleShot(0, this, [this, Id = Snapshot.id_, Query = Snapshot.interaction_->id_] {
                const auto Current = scheduler_.Snapshot(Id);
                if (!passwordDialogOpen_ && selectedId() == Id && Current.HasValue() && !Current.Value().cancelRequested_
                    && Current.Value().interaction_ && Current.Value().interaction_->id_ == Query) {
                    hc::storage::OperationContext Context; Context.stopToken_ = passwordScopes_[Id].get_token();
                    const auto& Request = *Current.Value().interaction_;
                    if (auto Password = hc::passwords::TryAutomaticArchivePassword(Request.source_, Request.passwordAttempt_, Context))
                        scheduler_.Reply(Id, {Query, InteractionChoice::Confirm, Password->Value()});
                }
            });
        }
    }
    const bool ShowRecords = hc::settings::AppSettings().CommittedObject()["options"].toObject()["showJobDetails"].toBool(false);
    details_->setVisible(ShowRecords);
    pagesBlock_->setVisible(ShowRecords);
    previousPage_->setVisible(ShowRecords); nextPage_->setVisible(ShowRecords);
    QString Notice;
    if (Snapshot.error_) Notice = ErrorText(*Snapshot.error_);
    if (Snapshot.interaction_) {
        if (!Notice.isEmpty()) Notice += "\n";
        Notice += Password ? tr("압축 파일의 암호를 입력하세요.") : Snapshot.interaction_->kind_ == InteractionKind::DeleteConfirmation
            ? Snapshot.interaction_->reason_ == "confirm-recycle-delete" ? tr("선택한 항목을 Windows 휴지통으로 이동합니다.") : tr("선택한 항목과 하위 항목을 영구 삭제합니다. 복원할 수 없습니다.")
            : Snapshot.interaction_->kind_ == InteractionKind::ReadOnlyRemoval ? tr("읽기 전용 항목의 삭제를 허용할지 선택하세요. 모두 허용은 이 작업 안에서 적용됩니다.")
            : Snapshot.interaction_->kind_ == InteractionKind::Overwrite ? tr("대상에 같은 이름의 항목이 있습니다. 처리 방법을 선택하세요.")
            : Snapshot.interaction_->kind_ == InteractionKind::Elevation ? tr("작업에 필요한 권한을 얻지 못했습니다. 권한 오류 자세히에서 지원 가능한 응답을 확인하세요.")
            : FileInUse ? tr("파일이 사용 중입니다. 파일 사용 정보에서 점유 항목을 확인하거나 파일을 닫은 뒤 다시 시도하세요.")
            : Snapshot.interaction_->kind_ == InteractionKind::RetryFailure ? tr("작업 실패 후 재시도 또는 취소 응답을 기다립니다.")
            : tr("작업을 계속하려면 아래에서 응답을 선택하세요.");
    }
    notice_->setText(Notice); notice_->setVisible(Valid && !Notice.isEmpty());
    if (!Valid) {
        permission_->hide(); metrics_->clear(); graph_->hide(); graphAxis_->hide(); graphToggle_->hide(); axisBlock_->hide(); answersBlock_->hide();
        sourceBlock_->hide(); destinationBlock_->hide(); currentBlock_->hide(); phaseBlock_->hide();
        status_->setText(tr("파일 작업이 없습니다.")); details_->clear(); overallMetrics_->clear(); overallPercent_->clear();
        progress_->setRange(0, 1); progress_->setValue(0); progress_->setFormat({}); return;
    }
    updateProgressView(Snapshot);
    QString Details;
    bool PermissionError = Snapshot.error_ && Snapshot.error_->category_ == ErrorCategory::AccessDenied;
    if (!Snapshot.metadata_.options_.empty() && !Snapshot.metadata_.options_.starts_with("extract;")) Details += tr("옵션: %1\n").arg(QString::fromStdString(Snapshot.metadata_.options_));
    if (!Snapshot.metadata_.retryOf_.empty()) Details += tr("재시도 원본 작업: %1\n").arg(QString::fromStdString(Snapshot.metadata_.retryOf_));
    if (Snapshot.error_ && Snapshot.error_->code_ == "job-interrupted") Details += tr("이전 실행이 중단되었습니다. 자동 실행하지 않습니다. 원본·목적지·보존 파일을 확인하세요.\n");
    if (Snapshot.interaction_)
        Details += Password ? tr("압축 파일의 암호를 입력하세요. 다른 파일 작업은 계속 진행할 수 있습니다.") : Snapshot.interaction_->kind_ == InteractionKind::DeleteConfirmation
            ? Snapshot.interaction_->reason_ == "confirm-recycle-delete" ? tr("선택한 항목을 Windows 휴지통으로 이동합니다.") : tr("선택한 항목과 하위 항목을 영구 삭제합니다. 휴지통에서 복원할 수 없습니다.")
            : Snapshot.interaction_->kind_ == InteractionKind::ReadOnlyRemoval ? tr("읽기 전용 항목의 삭제를 허용할지 선택하세요. 모두 허용은 이 작업 안에서만 적용됩니다.")
            : Snapshot.interaction_->kind_==InteractionKind::Elevation ? tr("작업에 필요한 권한을 얻지 못했습니다. 권한 오류 자세히에서 지원 가능한 응답을 확인하세요.")
            : Snapshot.interaction_->kind_==InteractionKind::RetryFailure ? tr("작업 실패 후 재시도 또는 취소 응답을 기다립니다.") : tr("목적지에 같은 이름의 항목이 있습니다. 처리 방법을 선택하세요.");
    if (Snapshot.error_) Details += "\n" + ErrorText(*Snapshot.error_);
    if (const auto It = results_.find(Snapshot.id_); It != results_.end())
    {
        loadResultPage(Snapshot.id_);
        std::size_t Shown = 0;
        const auto& Items = It->second.outcomes_ ? resultPage_ : It->second.items_;
        for (const auto& Item : Items)
        {
            if(Item.error_ && Item.error_->category_==ErrorCategory::AccessDenied) PermissionError=true;
            if (++Shown > 200) { Details += tr("\n표시 한도: 처음 200개 항목. 전체 결과는 작업 이력에 보관됩니다."); break; }
            Details += "\n" + DisplayPath(Item.item_) + " — " + (Item.status_ == ItemOutcome::Status::Completed ? tr("완료")
                : Item.status_ == ItemOutcome::Status::Skipped ? tr("건너뜀") : tr("실패"));
            if(Item.source_&&Item.source_->hints_.contains("securityBackup")) {
                const auto Backup=QJsonDocument::fromJson(QByteArray::fromStdString(Item.source_->hints_.at("securityBackup"))).object();
                if(!Backup["backupPath"].toString().isEmpty())Details+=tr("\n원래 권한 백업: %1").arg(Backup["backupPath"].toString());
                if(Backup["rollbackCode"].toInt())Details+=tr("\n원래 권한 복구 실패: Windows 오류 %1").arg(Backup["rollbackCode"].toInt());
            }
            if (Item.error_) Details += ": " + ErrorText(*Item.error_);
            if (Item.destinationCommitted_ && !Item.sourceRemoved_) Details += tr(" (목적지 저장됨, 원본 유지)");
        }
        for (const auto& Error : It->second.cleanupErrors_) Details += tr("\n완료 후 확인: %1 %2").arg(ErrorText(Error), QString::fromStdString(Error.userAction_));
    }
    permission_->setVisible((Snapshot.error_&&Snapshot.error_->code_.starts_with("elevation-outcome-unknown")) || PermissionError || (Snapshot.interaction_ && Snapshot.interaction_->kind_==InteractionKind::Elevation));
    Details = Details.trimmed();
    if (details_->toPlainText() != Details) details_->setPlainText(Details);
    const auto LineHeight=QFontMetrics(details_->font()).lineSpacing();
    const auto ContentHeight=QFontMetrics(details_->font()).boundingRect(QRect(0,0,std::max(100,details_->viewport()->width()-24),INT_MAX),Qt::TextWordWrap,Details).height()+16;
    details_->setMaximumHeight(std::clamp(ContentHeight,std::max(40,LineHeight+16),std::max(LineHeight+16,std::min(180,LineHeight*8+16))));
    details_->setVisible(ShowRecords && !Details.isEmpty());
    previousPage_->setVisible(ShowRecords && results_.contains(Snapshot.id_));
    nextPage_->setVisible(ShowRecords && results_.contains(Snapshot.id_));
    dialog_->layout()->activate();
    const bool Answers = !permission_->isHidden() || !fileLocks_->isHidden()
        || std::ranges::any_of(choices_, [&](const auto& Entry) { return !Entry.second->isHidden(); });
    answersBlock_->setVisible(Answers);
}

void FileJobUi::layoutProgressFooter()
{
    if (!progressFooter_ || !footerLeft_ || !footerRight_) return;
    const auto Available = dialog_->width() - 40;
    int LeftWidth = 0;
    for (int Index = 0; Index < footerLeft_->layout()->count(); ++Index) {
        const auto* Item = footerLeft_->layout()->itemAt(Index);
        if (!Item->isEmpty()) LeftWidth += Item->sizeHint().width() + (LeftWidth ? 8 : 0);
    }
    const bool Wrap = LeftWidth + footerRight_->sizeHint().width() + 12 > Available;
    int Row = 0, Column = 0, Rows = 0, Columns = 0;
    progressFooter_->getItemPosition(progressFooter_->indexOf(footerRight_), &Row, &Column, &Rows, &Columns);
    if (Row != (Wrap ? 1 : 0)) {
        progressFooter_->removeWidget(footerRight_);
        progressFooter_->addWidget(footerRight_, Wrap ? 1 : 0, Wrap ? 0 : 1, 1, Wrap ? 2 : 1, Qt::AlignRight);
    }
}

void FileJobUi::fitProgressDialog()
{
    if (auto* Body = dialog_->findChild<QWidget*>("hc.dialog.body")) {
        Body->layout()->invalidate(); Body->layout()->activate();
    }
    auto* Layout = dialog_->layout(); Layout->invalidate(); Layout->activate();
    const auto Height = Layout->hasHeightForWidth() ? Layout->totalHeightForWidth(dialog_->width()) : Layout->totalSizeHint().height();
    dialog_->setMinimumHeight(Layout->totalMinimumSize().height());
    dialog_->resize(dialog_->width(), std::min(Height, dialog_->screen()->availableGeometry().height() - 80));
}

void FileJobUi::updateProgressView(const JobSnapshot& Snapshot)
{
    const auto Kind = Snapshot.metadata_.kind_;
    const auto& Progress = Snapshot.progress_;
    const bool Active = !IsTerminal(Snapshot.state_);
    const bool Delete = deletionJob(Snapshot) || Kind == JobKind::ArchiveDelete;
    const bool Rename = Kind == JobKind::Rename || Kind == JobKind::ArchiveRename || Kind == JobKind::BatchRename
        || Kind == JobKind::RenameUndo || Kind == JobKind::RenameRedo || Kind == JobKind::RenameResume || Kind == JobKind::RenameRestore;
    const bool Create = Kind == JobKind::MakeFile || Kind == JobKind::MakeDirectory || Kind == JobKind::ArchiveMakeDirectory;
    const bool Security = Kind == JobKind::SecurityChange || Kind == JobKind::SecurityRestore;
    auto Operation = OperationText(Kind);
    if (Operation.isEmpty()) Operation = jobs_->currentText().section(QStringLiteral(" · "), 0, 0);
    if (Operation.isEmpty()) Operation = tr("파일 작업");
    const auto Planned = Progress.plannedItems_ ? Progress.plannedItems_ : !Active
        ? std::optional<uint64_t>{Snapshot.completedItems_ + Snapshot.skippedItems_ + Snapshot.failedItems_} : std::optional<uint64_t>{};
    const auto Counts = tr("예정 %1 / 완료 %2 / 건너뜀 %3 / 실패 %4")
        .arg(Planned ? tr("%1개").arg(*Planned) : tr("집계 중"))
        .arg(Snapshot.completedItems_).arg(Snapshot.skippedItems_).arg(Snapshot.failedItems_);
    const auto Title = Operation + QStringLiteral(" - ") + JobStatusText(Snapshot) + QStringLiteral(" · ") + Counts;
    status_->setText(Title); dialog_->setWindowTitle(Title);
    const auto Endpoint = [&](QLabel* PathLabel, QLabel* NameLabel, const QString& Label,
        const std::string& CurrentPath, const std::string& Root, bool FolderFallback) {
        auto Path = DisplayPath(CurrentPath.empty() ? Root : CurrentPath);
        auto Name = QString::fromStdString(Progress.currentItem_);
        const auto LastSeparator = std::max(Path.lastIndexOf('/'), Path.lastIndexOf('\\'));
        const bool RootIsItem = !Name.isEmpty() && Path.mid(LastSeparator + 1).compare(Name, Qt::CaseInsensitive) == 0;
        if (!CurrentPath.empty() || !FolderFallback || Name.isEmpty() || RootIsItem) {
            const auto Parsed = ParseLocator(CurrentPath.empty() ? Root : CurrentPath);
            if (Parsed.HasValue() && !Parsed.Value().nesting_.empty()) {
                auto Parent = Parsed.Value(); const auto Entry = QString::fromStdString(Parent.nesting_.back().entry_);
                const auto Separator = std::max(Entry.lastIndexOf('/'), Entry.lastIndexOf('\\'));
                Name = Entry.mid(Separator + 1); auto ParentEntry = Separator >= 0 ? Entry.left(Separator) : QString{};
                Parent.nesting_.back().entry_ = ParentEntry.isEmpty() ? "/" : ParentEntry.toStdString();
                Path = DisplayPath(Serialize(Parent));
            } else {
                const auto Separator = std::max(Path.lastIndexOf('/'), Path.lastIndexOf('\\'));
                Name = Path.mid(Separator + 1); Path = Separator >= 0 ? Path.left(Separator + (Separator == 2 ? 1 : 0)) : QString{};
            }
        } else if (Name.contains('/') || Name.contains('\\')) Name = Name.mid(std::max(Name.lastIndexOf('/'), Name.lastIndexOf('\\')) + 1);
        if (Name.isEmpty()) Name = QString::fromStdString(Snapshot.metadata_.label_);
        PathLabel->setText(Label + (Path.isEmpty() ? QString{} : QStringLiteral("   ") + Path));
        PathLabel->setToolTip(Path); NameLabel->setText(Name); NameLabel->setToolTip(Name);
    };
    const bool HasSource = !Create && (!Progress.source_.empty() || !Snapshot.metadata_.source_.empty() || !Progress.currentItem_.empty());
    const bool HasDestination = !Delete && !Security && (!Progress.destination_.empty() || !Snapshot.metadata_.destination_.empty());
    sourceBlock_->setVisible(HasSource); destinationBlock_->setVisible(HasDestination);
    const bool RestoreNames = Kind == JobKind::RenameUndo || Kind == JobKind::RenameRestore;
    const bool ResumeNames = Kind == JobKind::RenameResume;
    if (HasSource) Endpoint(sourcePath_, sourceName_, Security ? (Kind == JobKind::SecurityRestore ? tr("복원 경로") : tr("적용 경로"))
        : RestoreNames || ResumeNames ? tr("현재 이름") : Rename ? tr("변경 전") : Delete ? tr("삭제")
        : Kind == JobKind::ArchiveCreate || Kind == JobKind::ArchiveSeparate ? tr("압축") : Operation, Progress.source_, Snapshot.metadata_.source_, !Rename && !Security);
    if (HasDestination) Endpoint(destinationPath_, destinationName_, RestoreNames ? tr("복원 이름") : ResumeNames ? tr("최종 이름") : Rename ? tr("변경 후") : Create ? tr("생성 위치") : tr("대상"),
        Progress.destination_, Snapshot.metadata_.destination_, !Rename && !Create && Kind != JobKind::ArchiveCreate && Kind != JobKind::ArchiveSeparate && Kind != JobKind::IsoCreate);
    const auto Phases = Kind == JobKind::Move && Progress.phase_ == JobPhase::ApplyingNames ? std::vector<JobPhase>{} : JobPhases(Kind);
    phaseBlock_->setVisible(!Phases.empty());
    const auto PhaseKey = QString::number(int(Kind)) + ':' + QString::number(int(Progress.phase_)) + ':' + QString::number(int(Snapshot.state_));
    if (phaseBlock_->property("hc.jobs.phaseKey").toString() != PhaseKey) {
        phaseBlock_->setProperty("hc.jobs.phaseKey", PhaseKey);
        auto* Layout = phaseBlock_->layout();
        while (auto* Item = Layout->takeAt(0)) { delete Item->widget(); delete Item; }
        for (size_t Index = 0; Index < Phases.size(); ++Index) {
            const bool Current = Phases[Index] == Progress.phase_;
            auto* Label = new fm::ui::Label(QString::number(Index + 1) + ". " + (Kind == JobKind::IsoCreate && Phases[Index] == JobPhase::BuildingArchive ? tr("ISO 생성") : PhaseText(Phases[Index])), phaseBlock_);
            Label->setTextRole(fm::ui::Label::Meta); Label->setTone(Current ? fm::ui::Label::Accent : fm::ui::Label::Secondary);
            Label->setProperty("hc.jobs.activePhase", Current); Label->setMargin(5); Label->setFrameShape(QFrame::StyledPanel);
            Layout->addWidget(Label);
        }
    }
    hc::columns::FormatOptions SizeFormat; SizeFormat.size_ = hc::columns::SizeFormat::Adaptive; SizeFormat.precision_ = 2;
    if (const auto It = sizeFormats_.find(Snapshot.id_); It != sizeFormats_.end()) {
        SizeFormat.size_ = hc::columns::SizeFormat(It->second["size"].toInt()); SizeFormat.precision_ = It->second["precision"].toInt(1); SizeFormat.grouping_ = It->second["grouping"].toBool();
    }
    const auto SizeText = [&](uint64_t Bytes) { return hc::columns::Format(hc::columns::CellValue::Ready(quint64(Bytes)), SizeFormat); };
    const auto Mode = ProgressRate(Snapshot);
    const bool Bytes = Mode == RateMode::Bytes;
    const bool Current = Bytes && Progress.currentTotal_.has_value() && Kind != JobKind::ArchiveCreate && Kind != JobKind::IsoCreate;
    currentBlock_->setVisible(Current);
    const auto State = Snapshot.state_ == JobState::Paused || Snapshot.state_ == JobState::WaitingForUser || Snapshot.deferred_
        ? fm::ui::ProgressBar::Paused : Snapshot.state_ == JobState::Failed ? fm::ui::ProgressBar::Error : fm::ui::ProgressBar::Normal;
    const auto SetBar = [&](QProgressBar* Bar, QLabel* Percent, uint64_t Done, std::optional<uint64_t> Total) {
        static_cast<fm::ui::ProgressBar*>(Bar)->setState(State);
        Bar->setFixedHeight(9);
        if (!Total && Active) {
            const bool Suspended = State == fm::ui::ProgressBar::Paused || Snapshot.cancelRequested_;
            Bar->setRange(0, Suspended ? 1000 : 0); Bar->setValue(0); Percent->setText(QStringLiteral("—"));
        } else {
            Bar->setRange(0, 1000);
            const auto Value = Total && *Total ? int(std::min(1000.0, 1000.0 * Done / *Total)) : Snapshot.state_ == JobState::Completed ? 1000 : 0;
            Bar->setValue(Value); Percent->setText(Total ? QString::number(Value / 10) + '%' : QStringLiteral("—"));
        }
        Bar->setFormat({});
    };
    if (Current) {
        currentTitle_->setText(Kind == JobKind::ArchiveSeparate ? tr("현재 출력 저장") : tr("현재 파일"));
        SetBar(currentProgress_, currentPercent_, Progress.currentBytes_, Progress.currentTotal_);
        auto Text = tr("현재 파일 %1 / %2").arg(SizeText(Progress.currentBytes_), SizeText(*Progress.currentTotal_));
        if (Progress.activeFiles_ > 1) Text += tr(" · 병렬 처리 %1개").arg(Progress.activeFiles_);
        if (Kind == JobKind::ArchiveSeparate && Progress.outputIndex_ && Progress.outputCount_) Text += tr(" · %1 / %2번째 출력").arg(*Progress.outputIndex_ + 1).arg(*Progress.outputCount_);
        currentMetrics_->setText(Text);
    }
    const bool OutputCount = Kind == JobKind::ArchiveSeparate;
    const auto Total = OutputCount ? Progress.outputCount_ : Bytes ? Progress.bytesTotal_ : Mode == RateMode::None ? std::optional<uint64_t>{} : Progress.plannedItems_;
    const auto Done = OutputCount ? Progress.outputIndex_.value_or(Snapshot.completedItems_ + Snapshot.skippedItems_)
        : Bytes ? Progress.bytesDone_ : ProcessedItems(Snapshot);
    SetBar(progress_, overallPercent_, Done, Total);
    const bool PhaseTotal = Kind == JobKind::ArchiveCreate || Kind == JobKind::IsoCreate || (Mode == RateMode::None && !Total);
    overallTitle_->setText(PhaseTotal ? tr("현재 단계 · %1").arg(PhaseText(Progress.phase_).isEmpty() ? JobStatusText(Snapshot) : PhaseText(Progress.phase_)) : tr("전체 진행"));
    QString Overall;
    if (OutputCount) Overall = tr("생성 완료 %1 / %2개").arg(Done).arg(Total.value_or(0));
    else if (Bytes) Overall = Total ? tr("처리량 %1 / %2").arg(SizeText(Done), SizeText(*Total))
        : tr("처리량 %1 · 전체 크기 집계 중").arg(SizeText(Done));
    else if (Total) Overall = tr("처리 항목 %1 / %2개").arg(Done).arg(*Total);
    else Overall = PhaseText(Progress.phase_).isEmpty() ? tr("전체 집계 중") : PhaseText(Progress.phase_) + tr(" 중입니다.");
    if (Delete && Progress.deletedFiles_) Overall += tr(" · 삭제한 파일 %1개").arg(*Progress.deletedFiles_);
    if (Delete && Progress.discoveredItems_ && !Progress.plannedItems_) Overall += tr(" · 확인한 항목 %1개 · 전체 집계 중").arg(*Progress.discoveredItems_);
    overallMetrics_->setText(Overall);
    const auto Rate = measuredRate(Snapshot);
    const auto RateTotal = Mode == RateMode::Files ? (Progress.plannedItems_ ? Progress.discoveredFiles_ : std::optional<uint64_t>{})
        : Mode == RateMode::Items ? Progress.plannedItems_ : Progress.bytesTotal_;
    const auto RateDone = RateValue(Snapshot);
    const auto Remaining = Active && Rate > 0 && RateTotal && *RateTotal > RateDone ? tr("약 %1초").arg((*RateTotal - RateDone) / Rate, 0, 'f', 0) : QStringLiteral("—");
    const auto Now = IsTerminal(Snapshot.state_) ? Snapshot.updated_ : std::chrono::steady_clock::now();
    const auto Elapsed = Snapshot.submitted_ == std::chrono::steady_clock::time_point{} ? 0LL : std::chrono::duration_cast<std::chrono::seconds>(Now - Snapshot.submitted_).count();
    const auto RateText = Bytes ? fm::ui::TransferGraph::formatRate(Rate) : QLocale().toString(Rate, 'f', 1) + (Mode == RateMode::Files ? tr(" 파일/초") : tr(" 항목/초"));
    metrics_->setText((Mode == RateMode::None ? QString{} : tr("속도 %1   ").arg(RateText)) + tr("남은 시간 %1   경과 %2초").arg(Remaining).arg(Elapsed));
    const bool WrapMetrics = overallMetrics_->fontMetrics().horizontalAdvance(Overall) + metrics_->fontMetrics().horizontalAdvance(metrics_->text()) + 12 > dialog_->width() - 40;
    metrics_->setWordWrap(WrapMetrics && metrics_->fontMetrics().horizontalAdvance(metrics_->text()) > dialog_->width() - 40);
    metricsLayout_->setDirection(WrapMetrics ? QBoxLayout::TopToBottom : QBoxLayout::LeftToRight);
    metricsLayout_->setAlignment(metrics_, Qt::AlignRight);
    graphToggle_->setVisible(Mode != RateMode::None);
    graphToggle_->setText(detailed_ ? tr("그래프 숨김(&D)") : tr("그래프 표시(&D)"));
    graph_->setVisible(detailed_ && Mode != RateMode::None); graphAxis_->setVisible(detailed_ && Mode != RateMode::None); axisBlock_->setVisible(detailed_ && Mode != RateMode::None);
    if (Mode != RateMode::None) updateGraph(Snapshot);
    layoutProgressFooter();
    const auto Shape = QString::number(HasSource) + QString::number(HasDestination) + QString::number(Current)
        + QString::number(!Phases.empty()) + QString::number(detailed_ && Mode != RateMode::None);
    if (dialog_->property("hc.jobs.presentationShape").toString() != Shape) {
        dialog_->setProperty("hc.jobs.presentationShape", Shape);
        if (dialog_->isVisible()) QTimer::singleShot(0, this, [this] { fitProgressDialog(); });
    }
}

void FileJobUi::recordSamples()
{
    for (const auto& Snapshot : scheduler_.Snapshots()) {
        if (Snapshot.submitted_ == std::chrono::steady_clock::time_point{}) continue;
        auto& History = samples_[Snapshot.id_];
        const auto Now = IsTerminal(Snapshot.state_) ? Snapshot.updated_ : std::chrono::steady_clock::now();
        const auto Elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Now - Snapshot.submitted_).count();
        const auto Mode = int(ProgressRate(Snapshot));
        const auto Value = RateValue(Snapshot);
        const bool Creation = Snapshot.metadata_.kind_ == JobKind::ArchiveCreate || Snapshot.metadata_.kind_ == JobKind::ArchiveSeparate || Snapshot.metadata_.kind_ == JobKind::IsoCreate;
        const auto Phase = Creation ? Snapshot.progress_.phase_ : JobPhase::None;
        if (sampleKinds_[Snapshot.id_] != Mode || samplePhases_[Snapshot.id_] != Phase
            || (!History.empty() && Value < uint64_t(History.back().bytes_))) {
            History.clear(); sampleKinds_[Snapshot.id_] = Mode; samplePhases_[Snapshot.id_] = Phase;
            if (graphId_ == Snapshot.id_) graphId_.clear();
        }
        if (!History.empty() && (Elapsed <= History.back().elapsed_ || (!IsTerminal(Snapshot.state_) && Elapsed - History.back().elapsed_ < 500))) continue;
        History.push_back({Elapsed, static_cast<qint64>(std::min<uint64_t>(Value, INT64_MAX)),
            Snapshot.state_ == JobState::Paused || Snapshot.state_ == JobState::WaitingForUser || Snapshot.deferred_});
        // A bounded recent history avoids UI memory growing with long-running jobs.
        if (History.size() > 600) History.erase(History.begin());
    }
}
void FileJobUi::updateGraph(const JobSnapshot& Snapshot)
{
    const auto Mode = ProgressRate(Snapshot);
    graph_->setUnit(Mode == RateMode::Bytes ? fm::ui::TransferGraph::Bytes : fm::ui::TransferGraph::Count);
    graph_->setCountSuffix(Mode == RateMode::Files ? tr(" 파일/초") : tr(" 항목/초"));
    graph_->setTitle(Mode == RateMode::Bytes ? tr("처리 속도 · 실제 관측값") : Mode == RateMode::Files ? tr("처리 속도 · 파일 수") : tr("처리 속도 · 항목 수"));
    const auto Count = Mode == RateMode::Files ? (Snapshot.progress_.plannedItems_ ? Snapshot.progress_.discoveredFiles_.value_or(0) : 0)
        : Mode == RateMode::Items ? Snapshot.progress_.plannedItems_.value_or(0) : Snapshot.progress_.bytesTotal_.value_or(0);
    const auto Total = static_cast<qint64>(std::min<uint64_t>(Count, INT64_MAX));
    if (graphId_ != Snapshot.id_ || graphTotal_ != Total) {
        graphId_ = Snapshot.id_; graphTotal_ = Total; graphElapsed_ = -1; graph_->start(Total);
    }
    if (const auto It = samples_.find(Snapshot.id_); It != samples_.end())
        for (const auto& Sample : It->second) if (Sample.elapsed_ > graphElapsed_) {
            graph_->setPaused(Sample.paused_); graph_->addSample(Sample.bytes_, Sample.elapsed_); graphElapsed_ = Sample.elapsed_;
        }
    graph_->setPaused(Snapshot.state_ == JobState::Paused || Snapshot.state_ == JobState::WaitingForUser || Snapshot.deferred_);
}
void FileJobUi::updateManager()
{
    QSignalBlocker Block(jobList_);
    std::map<std::string, JobSnapshot> Snapshots;
    for (const auto& [Id, Result] : results_) Snapshots[Id] = Result.snapshot_;
    for (const auto& Snapshot : scheduler_.Snapshots()) Snapshots[Snapshot.id_] = Snapshot;
    std::vector<JobSnapshot> Queue;
    for (const auto& [Id, Snapshot] : Snapshots) if (acceptsControl(Snapshot, JobControl::Start)) Queue.push_back(Snapshot);
    std::sort(Queue.begin(), Queue.end(), [](const auto& A, const auto& B) { return A.queueOrder_ < B.queueOrder_; });
    std::map<std::string, size_t> Positions;
    for (size_t Index = 0; Index < Queue.size(); ++Index) Positions[Queue[Index].id_] = Index + 1;
    int Active = 0, Waiting = 0, Finished = 0, Failed = 0, Queued = 0, Delayed = 0; double TotalRate = 0, DeleteRate = 0, DeleteItemRate = 0;
    for (int Row = jobList_->topLevelItemCount() - 1; Row >= 0; --Row)
        if (jobs_->findData(jobList_->topLevelItem(Row)->data(0, Qt::UserRole)) < 0) delete jobList_->takeTopLevelItem(Row);
    std::map<QString, QTreeWidgetItem*> Rows;
    for (int I = 0; I < jobList_->topLevelItemCount(); ++I) Rows[jobList_->topLevelItem(I)->data(0, Qt::UserRole).toString()] = jobList_->topLevelItem(I);
    for (int Row = 0; Row < jobs_->count(); ++Row) {
        const auto Id = jobs_->itemData(Row).toString();
        const auto Found = Snapshots.find(Id.toStdString()); if (Found == Snapshots.end()) continue;
        const auto& Snapshot = Found->second;
        const bool Terminal = IsTerminal(Snapshot.state_);
        Queued += acceptsControl(Snapshot, JobControl::Start);
        const auto Mode = ProgressRate(Snapshot);
        if (Mode == RateMode::Files) DeleteRate += measuredRate(Snapshot);
        else if (Mode == RateMode::Items) DeleteItemRate += measuredRate(Snapshot);
        else if (Mode == RateMode::Bytes) TotalRate += measuredRate(Snapshot);
        Active += !Terminal; Waiting += !Terminal && !Snapshot.cancelRequested_ && Snapshot.state_ == JobState::WaitingForUser;
        Finished += Snapshot.state_ == JobState::Completed;
        Failed += Snapshot.state_ == JobState::Failed || Snapshot.state_ == JobState::Cancelled;
        QTreeWidgetItem* Item = Rows.contains(Id) ? Rows.at(Id) : nullptr;
        if (!Item) { Item = new QTreeWidgetItem(jobList_); Item->setData(0, Qt::UserRole, Id); }
        Item->setText(0, (Positions.contains(Snapshot.id_) ? tr("대기 #%1 · ").arg(Positions.at(Snapshot.id_)) : QString{}) + jobs_->itemText(Row));
        Item->setData(0, Qt::UserRole + 2, QVariant::fromValue<qulonglong>(Snapshot.queueOrder_));
        Item->setText(1, JobStatusText(Snapshot));
        bool Stalled = false;
        if (Snapshot.state_ == JobState::Running && !Snapshot.pauseRequested_ && samples_.contains(Snapshot.id_)) {
            const auto& History = samples_.at(Snapshot.id_);
            if (!History.empty()) {
                auto Unchanged = History.end() - 1;
                while (Unchanged != History.begin() && (Unchanged - 1)->bytes_ == History.back().bytes_ && !(Unchanged - 1)->paused_) --Unchanged;
                Stalled = History.back().elapsed_ - Unchanged->elapsed_ >= 10000;
            }
        }
        Delayed += Stalled; if (Stalled) Item->setText(1, Item->text(1) + tr(" · 지연"));
        const bool Delete = deletionJob(Snapshot);
        Item->setText(2, Delete ? (Snapshot.progress_.deletedFiles_ ? tr("삭제한 파일 %1개 · %2") : tr("삭제한 항목 %1개 · %2")).arg(Snapshot.progress_.deletedFiles_.value_or(Snapshot.completedItems_)).arg(Snapshot.progress_.plannedItems_ ? tr("집계 완료") : tr("전체 집계 중"))
            : tr("%1 / %2 B").arg(Snapshot.progress_.bytesDone_).arg(Snapshot.progress_.bytesTotal_ ? QString::number(*Snapshot.progress_.bytesTotal_) : tr("미상")));
        const auto Rate = Mode == RateMode::Bytes ? fm::ui::TransferGraph::formatRate(measuredRate(Snapshot))
            : Mode == RateMode::None ? QStringLiteral("—") : QLocale().toString(measuredRate(Snapshot), 'f', 1) + (Mode == RateMode::Files ? tr(" 파일/초") : tr(" 항목/초"));
        Item->setText(3, (Snapshot.state_ == JobState::Running || Snapshot.state_ == JobState::Committing || (Delete && Snapshot.state_ == JobState::Preparing)) && !Snapshot.pauseRequested_ ? Rate : QStringLiteral("—"));
        Item->setText(4, QString::fromStdString(Snapshot.progress_.currentItem_));
        const auto Endpoint = endpoints_.contains(Snapshot.id_) ? endpoints_.at(Snapshot.id_) : QString{};
        Item->setToolTip(0, jobs_->itemText(Row) + "\n" + Endpoint + "\n" + Id + "\n" + (Snapshot.submittedUtcMs_ ? QLocale().toString(QDateTime::fromMSecsSinceEpoch(Snapshot.submittedUtcMs_), QLocale::ShortFormat) : tr("이전 버전: 접수 시각 미상")));
        const int Filter = filter_->currentIndex();
        const bool Match = Filter == 0 || (Filter == 1 && !Terminal) || (Filter == 2 && Snapshot.state_ == JobState::WaitingForUser)
            || (Filter == 3 && Snapshot.state_ == JobState::Completed) || (Filter == 4 && (Snapshot.state_ == JobState::Failed || Snapshot.state_ == JobState::Cancelled));
        Item->setHidden(!Match || !(Item->text(0) + Item->text(4) + Endpoint).contains(search_->text(), Qt::CaseInsensitive));
        if (Id.toStdString() == selectedId()) jobList_->setCurrentItem(Item);
    }
    summary_->setToolTip(tr("속도는 최근 2초의 실제 처리량입니다. 삭제는 파일 수, 복사·이동은 바이트로 집계합니다. 10초 이상 변화가 없으면 지연으로 표시하며, 실패를 뜻하지 않습니다. 그래프는 최근 600개 표본을 보관합니다."));
    auto Rates = fm::ui::TransferGraph::formatRate(TotalRate) + tr(" · 삭제 %1 파일/초").arg(QLocale().toString(DeleteRate, 'f', 1));
    if (DeleteItemRate > 0) Rates += tr(" · 항목 %1개/초").arg(QLocale().toString(DeleteItemRate, 'f', 1));
    // Status counts as colored chips (board Prop-Jobs): active · queue · waiting · done · failed, then the rates.
    const auto& ChipColors = fm::style::themeColorsFor(summary_);
    const auto Chip = [&ChipColors](const QString& Text, int Count, fm::style::Token Fg, fm::style::Token Bg) {
        return QString("<span style=\"background:%1;color:%2;\">&nbsp;%3 <b>%4</b>&nbsp;</span>").arg(ChipColors[Bg].name(), ChipColors[Fg].name(), Text.toHtmlEscaped()).arg(Count);
    };
    using fm::style::Token;
    summary_->setTextFormat(Qt::RichText);
    summary_->setText(Chip(tr("미완료"), Active, Token::AccentFg, Token::AccentSoft) + "&nbsp; " + Chip(tr("대기열"), Queued, Token::Fg2, Token::Alt) + "&nbsp; "
        + Chip(tr("응답 필요"), Waiting, Token::Warn, Token::WarnBg) + "&nbsp; " + Chip(tr("완료"), Finished, Token::Ok, Token::OkBg) + "&nbsp; "
        + Chip(tr("실패·취소"), Failed, Token::Danger, Token::DangerBg)
        + "<br><span style=\"color:" + ChipColors[Token::Fg2].name() + ";\">" + tr("전체 속도 %1 · 10초 이상 진행 지연 %2").arg(Rates).arg(Delayed).toHtmlEscaped() + "</span>");
    std::map<QTreeWidgetItem*, bool> Hidden;
    for (int Index = 0; Index < jobList_->topLevelItemCount(); ++Index) Hidden[jobList_->topLevelItem(Index)] = jobList_->topLevelItem(Index)->isHidden();
    std::vector<QTreeWidgetItem*> Ordered;
    while (jobList_->topLevelItemCount()) Ordered.push_back(jobList_->takeTopLevelItem(0));
    std::stable_sort(Ordered.begin(), Ordered.end(), [](auto* A, auto* B) { return A->data(0, Qt::UserRole + 2).toULongLong() < B->data(0, Qt::UserRole + 2).toULongLong(); });
    for (auto* Item : Ordered) { jobList_->addTopLevelItem(Item); Item->setHidden(Hidden.at(Item)); }
    for (auto* Item : Ordered) if (Item->data(0, Qt::UserRole).toString().toStdString() == selectedId()) jobList_->setCurrentItem(Item);
    std::map<JobControl, int> Eligible;
    int Paused = 0, Cleaning = 0; QStringList Running;
    for (const auto& Snapshot : scheduler_.Snapshots()) {
        for (const auto Control : {JobControl::Start, JobControl::Pause, JobControl::Resume, JobControl::Cancel}) Eligible[Control] += acceptsControl(Snapshot, Control);
        if (IsTerminal(Snapshot.state_)) continue;
        Paused += !Snapshot.cancelRequested_ && (Snapshot.pauseRequested_ || Snapshot.state_ == JobState::Paused);
        Cleaning += Snapshot.cancelRequested_;
        if (!Snapshot.deferred_ && !Snapshot.cancelRequested_ && !Snapshot.pauseRequested_
            && (Snapshot.state_ == JobState::Preparing || Snapshot.state_ == JobState::Running || Snapshot.state_ == JobState::Committing)) {
            const auto Path = Snapshot.metadata_.destination_.empty() ? Snapshot.metadata_.source_ : Snapshot.metadata_.destination_;
            Running.append(QString::fromStdString(Snapshot.metadata_.label_) + (Path.empty() ? QString{} : QStringLiteral(" → ") + DisplayPath(Path)));
        }
    }
    batchSummary_->setText(tr("전체 작업에 적용 · 실행 %1개 / 일시 정지·요청 %2개 / 응답 대기 %3개 / 취소 후 정리 %4개\n현재 실행: %5")
        .arg(Running.size()).arg(Paused).arg(Waiting).arg(Cleaning).arg(Running.isEmpty() ? tr("없음") : Running.join(QStringLiteral(" · "))));
    manager_->findChild<QPushButton*>("hc.jobs.startQueue")->setEnabled(Eligible[JobControl::Start] > 0);
    for (const auto Control : {JobControl::Pause, JobControl::Resume, JobControl::Cancel})
        manager_->findChild<QPushButton*>("hc.jobs.all." + QString::number(static_cast<int>(Control)))->setEnabled(Eligible[Control] > 0);
    historySummary_->setText(tr("끝난 작업 %1개 · 기록만 지우며 파일은 유지합니다. 중단된 이름 변경의 이어 실행·원래 이름 복구는 이름 변경 이력에서 선택하세요.").arg(results_.size()));
    clearHistory_->setEnabled(!clearingHistory_ && preparingRetry_.empty() && !results_.empty());
    if (indicator_) indicator_->setText(tr("파일 작업 %1 · 응답 필요 %2").arg(Active).arg(Waiting));
    const auto Current = Snapshots.find(selectedId());
    updateManagerSelection(Current == Snapshots.end() ? nullptr : &Current->second);
}

void FileJobUi::loadResultPage(const std::string& Id)
{
    const auto Found = results_.find(Id);
    if (Found == results_.end() || !Found->second.outcomes_) return;
    auto Store = Found->second.outcomes_;
    if (resultPageId_ == Id && loadedOffset_ == resultOffset_) {
        previousPage_->setEnabled(!pageLoading_ && resultOffset_ > 0);
        nextPage_->setEnabled(!pageLoading_ && resultOffset_ + resultPage_.size() < Store->Count()); return;
    }
    resultPageId_ = Id; loadedOffset_ = resultOffset_; resultPage_.clear(); pageLoading_ = true;
    const auto Generation = ++pageGeneration_; const auto Offset = resultOffset_;
    auto* Watcher = new QFutureWatcher<Result<ItemOutcomePage>>(this);
    connect(Watcher, &QFutureWatcher<Result<ItemOutcomePage>>::finished, this, [this, Watcher, Generation] {
        auto Page = Watcher->result(); Watcher->deleteLater(); if (Generation != pageGeneration_) return;
        pageLoading_ = false;
        if (Page.HasValue()) resultPage_ = std::move(Page.Value().items_);
        else resultPage_.push_back({"", ItemOutcome::Status::Failed, Page.Failure()});
        updateSelection();
    });
    Watcher->setFuture(QtConcurrent::run([Store, Offset] { return Store->Read(Offset, 200); }));
}

void FileJobUi::answer(InteractionChoice Choice)
{
    const auto Current = scheduler_.Snapshot(selectedId());
    if (!Current.HasValue() || !Current.Value().interaction_) return;
    if (Choice == InteractionChoice::Confirm && Current.Value().interaction_->kind_ == InteractionKind::Password) {
        if (passwordDialogOpen_) return;
        passwordDialogOpen_ = true; auto Reset = qScopeGuard([this] { passwordDialogOpen_ = false; });
        const auto Id = Current.Value().id_; const auto Query = *Current.Value().interaction_;
        auto Source = passwordScopes_[Id];
        hc::storage::OperationContext Context; Context.stopToken_ = Source.get_token();
        QTimer Cancellation;
        connect(&Cancellation, &QTimer::timeout, this, [this, Id, Query, Source]() mutable {
            const auto Now = scheduler_.Snapshot(Id);
            if (!Now.HasValue() || Now.Value().cancelRequested_ || !Now.Value().interaction_ || Now.Value().interaction_->id_ != Query.id_)
                Source.request_stop();
        });
        Cancellation.start(50);
        auto Reply = hc::plugins::PromptArchivePassword(Query.source_, Query.passwordAttempt_, Context);
        auto Fresh = scheduler_.Snapshot(Id);
        if (!Fresh.HasValue() || Fresh.Value().cancelRequested_ || !Fresh.Value().interaction_ || Fresh.Value().interaction_->id_ != Query.id_) return;
        if (Reply.HasValue()) scheduler_.Reply(Id, {Query.id_, InteractionChoice::Confirm, Reply.Value().Value()});
        else scheduler_.Reply(Id, {Query.id_, InteractionChoice::Cancel, {}});
        updateSelection(); return;
    }
    const auto Replied = scheduler_.Reply(selectedId(), {Current.Value().interaction_->id_, Choice, renamed_->text().toStdString()});
    if (!Replied.HasValue()) status_->setText(tr("응답을 적용할 수 없습니다: %1").arg(QString::fromStdString(Replied.Failure().code_)));
    else { renamed_->clear(); updateSelection(); }
}

bool FileJobUi::eventFilter(QObject* Object, QEvent* Event)
{
    if (Object == dialog_ && Event->type() == QEvent::Resize && progressFooter_ && metricsLayout_) {
        layoutProgressFooter();
        const auto Snapshot = scheduler_.Snapshot(selectedId());
        if (Snapshot.HasValue()) updateProgressView(Snapshot.Value());
    }
    if (Object == main_ && Event->type() == QEvent::Close && !pending_.empty())
    {
        closing_ = true;
        for (const auto& [Id, Handle] : pending_) scheduler_.Cancel(Id);
        static_cast<QCloseEvent*>(Event)->ignore();
        Show();
        return true;
    }
    return QObject::eventFilter(Object, Event);
}
