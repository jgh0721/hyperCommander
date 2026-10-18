#include "stdafx.h"
#include "HyperCommander/UIs/permissionDialogs.hpp"
#include "fileJobUi.hpp"
#include "HyperCommander/UIs/fileToolsDialogs.hpp"
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
#include <fmwidgets/Button.h>
#include <fmwidgets/WrapLayout.h>
#include <fmwidgets/DialogHeader.h>
#include <fmwidgets/TransferGraph.h>

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
        return QString::fromStdString(Value.code_);
    }
    QString DisplayPath(const std::string& Value)
    {
        const auto At = ParseLocator(Value);
        if (!At.HasValue()) return QString::fromStdString(Value);
        auto Path = QString::fromStdString(At.Value().root_);
        for (const auto& Hop : At.Value().nesting_) Path += " > " + QString::fromStdString(Hop.entry_);
        return Path;
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
    dialog_->resize(700, 650);
    dialog_->setMinimumSize(520, 350);
    auto* Layout = new QVBoxLayout(dialog_);
    header_ = new fm::ui::DialogHeader(dialog_);
    header_->setTitle(tr("파일 작업 진행")); header_->setSubtitleElide(Qt::ElideMiddle);
    Layout->addWidget(header_);
    jobs_ = new QComboBox(dialog_); jobs_->setObjectName("hc.jobs.selection");
    jobs_->hide();
    status_ = new QLabel(dialog_); status_->setObjectName("hc.jobs.status"); status_->setWordWrap(true);
    status_->setTextFormat(Qt::PlainText);
    Layout->addWidget(status_);
    progress_ = new QProgressBar(dialog_); progress_->setAccessibleName(tr("선택 작업 진행률")); Layout->addWidget(progress_);
    metrics_ = new QLabel(dialog_); metrics_->setTextFormat(Qt::PlainText); metrics_->setWordWrap(true);
    metrics_->setObjectName("hc.jobs.metrics"); Layout->addWidget(metrics_);
    graph_ = new fm::ui::TransferGraph(dialog_); graph_->setObjectName("hc.jobs.graph");
    graph_->setTitle(tr("처리 속도 · 실제 관측값")); graph_->setMinimumHeight(130); Layout->addWidget(graph_, 1);
    auto* Display = new QHBoxLayout;
    auto* Toggle = new fm::ui::Button(tr("간단히 보기(&D)"), dialog_); Toggle->setObjectName("hc.jobs.toggleDetails");
    auto* Axis = new QComboBox(dialog_); Axis->addItems({tr("진행률 축"), tr("시간 축 · 최근 60초")});
    Axis->setAccessibleName(tr("처리 속도 그래프 축"));
    Display->addWidget(Toggle); Display->addWidget(Axis); Display->addStretch(); Layout->addLayout(Display);
    connect(Axis, &QComboBox::currentIndexChanged, this, [this](int Index) { graph_->setAxis(Index ? fm::ui::TransferGraph::Time : fm::ui::TransferGraph::Progress); });
    details_ = new QPlainTextEdit(dialog_); details_->setReadOnly(true); details_->setAccessibleName(tr("선택 작업 상세 결과"));
    details_->setObjectName("hc.jobs.details"); details_->setMinimumHeight(40); Layout->addWidget(details_);
    auto* Pages = new QHBoxLayout;
    previousPage_ = new fm::ui::Button(tr("이전 결과"), dialog_); previousPage_->setObjectName("hc.jobs.previousResults");
    nextPage_ = new fm::ui::Button(tr("다음 결과"), dialog_); nextPage_->setObjectName("hc.jobs.nextResults");
    Pages->addWidget(previousPage_); Pages->addWidget(nextPage_); Pages->addStretch(); Layout->addLayout(Pages);
    connect(previousPage_, &QPushButton::clicked, this, [this] { resultOffset_ = resultOffset_ >= 200 ? resultOffset_ - 200 : 0; updateSelection(); });
    connect(nextPage_, &QPushButton::clicked, this, [this] { resultOffset_ += 200; updateSelection(); });
    renamed_ = new QLineEdit(dialog_); renamed_->setPlaceholderText(tr("새 이름"));
    renamed_->setAccessibleName(tr("충돌한 항목의 새 이름"));
    renamed_->setObjectName("hc.jobs.newName"); Layout->addWidget(renamed_);
    connect(Toggle, &QPushButton::clicked, this, [this, Toggle, Axis] {
        detailed_ = !detailed_; Toggle->setText(detailed_ ? tr("간단히 보기(&D)") : tr("자세히 보기(&D)"));
        graph_->setVisible(detailed_); Axis->setVisible(detailed_);
        updateSelection(); dialog_->resize(700, detailed_ ? 650 : 350);
    });
    autoClose_ = new QCheckBox(tr("성공하면 창 닫기(&W)"), dialog_); autoClose_->setObjectName("hc.jobs.closeOnSuccess"); Layout->addWidget(autoClose_);
    auto* Buttons = new fm::ui::WrapLayout; Layout->addLayout(Buttons);
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
    cancel_ = new fm::ui::Button(tr("작업 취소(&C)"), dialog_); cancel_->setObjectName("hc.jobs.cancel");
    Buttons->addWidget(pause_); Buttons->addWidget(cancel_);
    auto* Hide = new fm::ui::Button(tr("백그라운드로(&B)"), dialog_); Hide->setObjectName("hc.jobs.background"); Buttons->addWidget(Hide);
    manager_ = new QDialog(Main); manager_->setObjectName("hc.jobManager");
    manager_->setWindowTitle(tr("백그라운드 작업관리자")); manager_->resize(920, 540); manager_->setMinimumSize(640, 380);
    auto* ManagerLayout = new QVBoxLayout(manager_);
    auto* ManagerHeader = new fm::ui::DialogHeader(manager_); ManagerHeader->setTitle(tr("파일 작업관리자"));
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
    jobList_->setColumnWidth(2, 170); jobList_->setColumnWidth(3, 110); ManagerLayout->addWidget(jobList_, 1);
    auto* QueueControls = new fm::ui::WrapLayout;
    auto* StartQueue = new fm::ui::Button(tr("대기열 시작(&S)"), manager_); StartQueue->setObjectName("hc.jobs.startQueue");
    queueUp_ = new fm::ui::Button(tr("위로(&U)"), manager_); queueUp_->setObjectName("hc.jobs.queueUp");
    queueDown_ = new fm::ui::Button(tr("아래로(&N)"), manager_); queueDown_->setObjectName("hc.jobs.queueDown");
    concurrency_ = new QSpinBox(manager_); concurrency_->setRange(1, 2); concurrency_->setValue(static_cast<int>(scheduler_.ConcurrencyLimit()));
    concurrency_->setObjectName("hc.jobs.concurrency"); concurrency_->setAccessibleName(tr("동시 실행 작업 수"));
    concurrency_->setToolTip(tr("1~2개 작업을 동시에 실행합니다. 줄이면 기존 작업이 끝날 때까지 기다립니다."));
    QueueControls->addWidget(StartQueue); QueueControls->addWidget(queueUp_); QueueControls->addWidget(queueDown_);
    auto* ConcurrencyGroup = new QWidget(manager_); auto* ConcurrencyLayout = new QHBoxLayout(ConcurrencyGroup);
    ConcurrencyLayout->setContentsMargins(0, 0, 0, 0);
    auto* ConcurrencyLabel = new QLabel(tr("동시 실행(&J)"), ConcurrencyGroup); ConcurrencyLabel->setBuddy(concurrency_);
    ConcurrencyLayout->addWidget(ConcurrencyLabel); ConcurrencyLayout->addWidget(concurrency_); QueueControls->addWidget(ConcurrencyGroup);
    ManagerLayout->addLayout(QueueControls);
    connect(StartQueue, &QPushButton::clicked, this, [this] { controlAll(JobControl::Start); });
    connect(queueUp_, &QPushButton::clicked, this, [this] { reorderSelected(-1); });
    connect(queueDown_, &QPushButton::clicked, this, [this] { reorderSelected(1); });
    connect(concurrency_, &QSpinBox::valueChanged, this, [this](int Value) { scheduler_.SetConcurrencyLimit(Value); });
    auto* BatchControls = new fm::ui::WrapLayout;
    for (const auto& Control : std::vector<std::pair<JobControl, QString>>{{JobControl::Pause, tr("모두 정지(&A)")}, {JobControl::Resume, tr("모두 재개(&R)")}, {JobControl::Cancel, tr("모두 취소(&X)")}}) {
        auto* Button = new fm::ui::Button(Control.second, manager_); Button->setAutoDefault(false);
        Button->setObjectName("hc.jobs.all." + QString::number(static_cast<int>(Control.first))); BatchControls->addWidget(Button);
        connect(Button, &QPushButton::clicked, this, [this, Kind = Control.first] { controlAll(Kind); });
    }
    ManagerLayout->addLayout(BatchControls);
    auto* ManagerButtons = new fm::ui::WrapLayout;
    managerOpen_ = new fm::ui::Button(tr("상세 보기(&D)"), manager_);
    managerPause_ = new fm::ui::Button(tr("일시 정지(&P)"), manager_); managerPause_->setObjectName("hc.jobs.managerPause");
    managerCancel_ = new fm::ui::Button(tr("작업 취소(&C)"), manager_); managerCancel_->setObjectName("hc.jobs.managerCancel");
    managerStart_ = new fm::ui::Button(tr("선택 작업 시작(&G)"), manager_); managerStart_->setObjectName("hc.jobs.managerStart");
    managerRetry_ = new fm::ui::Button(tr("실패 항목 재시도(&T)"), manager_); managerRetry_->setObjectName("hc.jobs.managerRetry");
    managerRetry_->setToolTip(tr("현재 세션의 일반 파일 복사·이동에서 목적지 저장 전 실패한 항목을 재검증합니다. 중단 기록·폴더·압축·삭제는 상세 결과를 확인하세요."));
    connect(managerStart_, &QPushButton::clicked, this, [this] {
        auto Result = scheduler_.Control({selectedId()}, JobControl::Start);
        if (!Result.HasValue()) QMessageBox::warning(manager_, tr("대기열 시작 실패"), ErrorText(Result.Failure()));
        updateManager(); updateSelection();
    });
    connect(managerRetry_, &QPushButton::clicked, this, [this] { retrySelected(); });
    auto* RenameHistory = new fm::ui::Button(tr("이름 변경 이력(&L)"), manager_); RenameHistory->setObjectName("hc.jobs.renameHistory"); RenameHistory->setAutoDefault(false);
    BatchControls->addWidget(RenameHistory);
    connect(RenameHistory, &QPushButton::clicked, this, [this] { hc::ui::RenameHistoryDialog History(this, manager_); History.exec(); });
    auto* Clear = new fm::ui::Button(tr("완료 이력 지우기(&H)"), manager_);
    auto* CloseManager = new fm::ui::Button(tr("닫기(&O)"), manager_); CloseManager->setObjectName("hc.jobs.close");
    for (auto* Button : {managerOpen_, managerStart_, managerPause_, managerCancel_, managerRetry_, static_cast<QPushButton*>(Clear), static_cast<QPushButton*>(CloseManager)}) {
        Button->setAutoDefault(false); ManagerButtons->addWidget(Button);
    }
    ManagerLayout->addLayout(ManagerButtons);
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
        std::vector<std::string> Ids;
        for (const auto& [Id, Result] : results_) Ids.push_back(Id);
        Clear->setEnabled(false);
        using EraseResult = std::pair<std::vector<std::string>, std::optional<Error>>;
        auto* Watcher = new QFutureWatcher<EraseResult>(this);
        connect(Watcher, &QFutureWatcher<EraseResult>::finished, this, [this, Watcher, Clear] {
            const auto Result = Watcher->result(); Watcher->deleteLater(); Clear->setEnabled(true);
            for (const auto& Id : Result.first)
            {
                scheduler_.Forget(Id); results_.erase(Id); samples_.erase(Id); endpoints_.erase(Id); sizeFormats_.erase(Id); retryRequests_.erase(Id); retryContexts_.erase(Id);
                const auto Row = jobs_->findData(QString::fromStdString(Id)); if (Row >= 0) jobs_->removeItem(Row);
            }
            updateSelection();
            if (Result.second) QMessageBox::warning(dialog_, tr("이력 삭제 실패"), QString::fromStdString(Result.second->code_));
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
    Job.task_=std::make_unique<FileElevationJob>(std::move(Request),std::move(Service));auto Started=Submit(std::move(Job),Restore.isEmpty()?tr("소유권·권한 변경"):tr("원래 권한 복원"));
    return Started.HasValue()?QString():QString::fromStdString(Started.Failure().code_);
}

Result<JobHandle> FileJobUi::Submit(FileOperationRequest Request, OperationContext Context, bool Deferred)
{
    Request.calculateSizeBefore_ = hc::settings::AppSettings().CommittedObject()["options"].toObject()["folderSizeBeforeOperations"].toBool(true);
    Context.providerInteractions_ = true;
    Context.flushFileContents_ = !hc::settings::AppSettings().CommittedObject()["options"].toObject()["delayedFileWrites"].toBool(true);
    if (closing_) return Error{ErrorCategory::Cancelled, "window-closing"};
    const auto Label = Request.sources_.empty() ? Request.destination_.root_ : Request.sources_.front().entry_.name_;
    const auto Operation = Request.operation_;
    const auto Destination = DisplayPath(Serialize(Request.destination_));
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
    auto Submitted = track(scheduler_.Submit(std::move(Prepared.Value().request_), std::move(Prepared.Value().task_)), hc::operations::FileOperation::Move, Label);
    if (Submitted.HasValue()) { endpoints_[Submitted.Value().id_] = Source + (Destination.isEmpty() ? QString{} : QStringLiteral(" → ") + Destination); updateSelection(); }
    return Submitted;
}

Result<JobHandle> FileJobUi::Submit(PreparedJob Job, const QString& Label)
{
    Job.request_.operation_.providerInteractions_ = true;
    Job.request_.operation_.flushFileContents_ = !hc::settings::AppSettings().CommittedObject()["options"].toObject()["delayedFileWrites"].toBool(true);
    if (closing_) return Error{ErrorCategory::Cancelled, "window-closing"};
    Job.request_.metadata_.label_ = Label.toStdString();
    const auto Kind = Job.request_.metadata_.options_.starts_with("rename-record=") ? hc::operations::FileOperation::Rename : hc::operations::FileOperation::Copy;
    auto Submitted = track(scheduler_.Submit(std::move(Job.request_), std::move(Job.task_)), Kind, Label.toStdString());
    if (Submitted.HasValue()) jobs_->setItemText(jobs_->findData(QString::fromStdString(Submitted.Value().id_)), Label);
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
            if (!Reordered.HasValue()) QMessageBox::warning(manager_, tr("대기열 순서 변경 실패"), tr("작업은 접수되었습니다. 대기열이 변경되어 기본 순서를 적용하지 못했습니다. 현재 순서를 확인하세요."));
        }
    }
    autoClose_->setChecked(Defaults["jobCloseOnSuccess"].toBool(true));
    if (detailed_ != Defaults["jobDetailed"].toBool(true)) dialog_->findChild<QPushButton*>("hc.jobs.toggleDetails")->click();
    if (Snapshot.HasValue() && Snapshot.Value().deferred_) Show(); else showProgress();
    return Handle;
}

void FileJobUi::ApplyDefaults()
{
    const auto Options = hc::settings::AppSettings().CommittedObject()["options"].toObject();
    SetFileJobPersistence(journal_, Options["enableJobRecovery"].toBool(false), Options["saveJobProgress"].toBool(false));
    const auto Limit = Options["jobConcurrency"].toInt(2);
    if (Limit == appliedConcurrency_) return; // Preserve a session override until the stored default changes.
    if (scheduler_.SetConcurrencyLimit(Limit).HasValue()) {
        appliedConcurrency_ = Limit;
        const QSignalBlocker Block(concurrency_); concurrency_->setValue(Limit);
    }
}

void FileJobUi::Show() { updateManager(); manager_->show(); manager_->raise(); updateSelection(); }
void FileJobUi::showProgress() { dialog_->show(); dialog_->raise(); updateSelection(); }
void FileJobUi::controlSelected(bool Pause)
{
    const auto Id = selectedId();
    const auto Current = scheduler_.Snapshot(Id);
    if (!Current.HasValue()) return;
    const auto Result = Pause ? scheduler_.Pause(Id, !Current.Value().pauseRequested_) : scheduler_.Cancel(Id);
    if (!Result.HasValue()) QMessageBox::warning(dialog_->isVisible() ? dialog_ : manager_, tr("작업 제어 실패"), ErrorText(Result.Failure()));
    updateSelection(); updateManager();
}
std::string FileJobUi::selectedId() const { return jobs_->currentData().toString().toStdString(); }

void FileJobUi::controlAll(JobControl Control)
{
    std::vector<std::string> Ids;
    for (const auto& Snapshot : scheduler_.Snapshots())
        if (!IsTerminal(Snapshot.state_) && !Snapshot.cancelRequested_ && (Control != JobControl::Start || Snapshot.deferred_)) Ids.push_back(Snapshot.id_);
    if (Ids.empty()) return;
    auto Result = scheduler_.Control(Ids, Control);
    if (!Result.HasValue()) QMessageBox::warning(manager_, tr("일괄 제어 실패"), tr("작업 상태가 변경되었습니다. 목록을 확인하고 다시 실행하세요: %1").arg(ErrorText(Result.Failure())));
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
    if (!Result.HasValue()) QMessageBox::warning(manager_, tr("순서 변경 실패"), ErrorText(Result.Failure()));
    updateManager(); updateSelection();
}
bool FileJobUi::canRetry(const std::string& Id) const
{
    const auto Snapshot=scheduler_.Snapshot(Id);if(Snapshot.HasValue()&&Snapshot.Value().error_&&Snapshot.Value().error_->code_.starts_with("elevation-outcome-unknown"))return false;
    const auto Found = results_.find(Id);
    if (Found == results_.end() || !retryRequests_.contains(Id) || !Found->second.cleanupErrors_.empty()) return false;
    const auto Current = scheduler_.Snapshot(Id);
    if (!Current.HasValue() || Current.Value().state_ != JobState::Failed || !Current.Value().retriedAs_.empty()) return false;
    return Current.Value().failedItems_ > 0;
}
void FileJobUi::retrySelected()
{
    const auto Id = selectedId(); if (!canRetry(Id)) return;
    auto Request = retryRequests_.at(Id); auto Context = retryContexts_.at(Id); const auto Original = results_.at(Id);
    // A new user action gets a fresh cancellation/deadline, with the original
    // immutable source identity. Destination conflicts are asked again.
    Context.stopToken_ = {}; Context.deadline_ = std::chrono::steady_clock::time_point::max();
    Context.flushFileContents_ = !hc::settings::AppSettings().CommittedObject()["options"].toObject()["delayedFileWrites"].toBool(true);
    Request.conflict_ = ConflictPolicy::Ask; Request.correlationId_.clear();
    managerRetry_->setEnabled(false);
    using RetryResult = Result<FileOperationRequest>;
    auto* Watcher = new QFutureWatcher<RetryResult>(this);
    connect(Watcher, &QFutureWatcher<RetryResult>::finished, this, [this, Watcher, Id, Context] {
        auto Candidate = Watcher->result(); Watcher->deleteLater();
        if (!Candidate.HasValue()) { QMessageBox::warning(manager_, tr("재시도 검증 실패"), ErrorText(Candidate.Failure())); updateSelection(); return; }
        if (closing_) return;
        const auto Parent = scheduler_.Snapshot(Id); if (!Parent.HasValue()) { updateSelection(); return; }
        const auto Request = Candidate.Value(); auto Prepared = PrepareFileOperation(Request, Context);
        if (!Prepared.HasValue()) { QMessageBox::warning(manager_, tr("재시도 접수 실패"), ErrorText(Prepared.Failure())); updateSelection(); return; }
        Prepared.Value().request_.metadata_ = Parent.Value().metadata_;
        Prepared.Value().request_.metadata_.retryOf_ = Id; Prepared.Value().request_.metadata_.options_ = tr("충돌: 다시 확인 · 재시도: 저장 전 실패한 항목만").toStdString();
        auto Submitted = track(scheduler_.Submit(std::move(Prepared.Value().request_), std::move(Prepared.Value().task_)), Request.operation_, Request.sources_.front().entry_.name_);
        if (!Submitted.HasValue()) { QMessageBox::warning(manager_, tr("재시도 접수 실패"), ErrorText(Submitted.Failure())); updateSelection(); return; }
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
    if ((Snapshot.state_ != JobState::Running && Snapshot.state_ != JobState::Committing) || Snapshot.pauseRequested_ || Snapshot.cancelRequested_) return 0;
    const auto Found = samples_.find(Snapshot.id_); if (Found == samples_.end() || Found->second.size() < 2) return 0;
    const auto& History = Found->second; const auto& Latest = History.back();
    const auto Begin = std::find_if(History.begin(), History.end(), [&](const auto& Sample) { return Sample.elapsed_ >= Latest.elapsed_ - 2000; });
    auto First = Begin;
    for (auto It = Begin; It != History.end(); ++It) if (It->paused_) First = std::next(It);
    if (First == History.end() || Latest.elapsed_ <= First->elapsed_ || Latest.bytes_ < First->bytes_) return 0;
    return 1000.0 * (Latest.bytes_ - First->bytes_) / (Latest.elapsed_ - First->elapsed_);
}

void FileJobUi::updateJobs()
{
    if (!hc::settings::AppSettings().Editing()) ApplyDefaults();
    for (auto It = pending_.begin(); It != pending_.end();)
    {
        if (It->second.completion_.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) { ++It; continue; }
        const auto Stored = results_.insert_or_assign(It->first, It->second.completion_.get()).first;
        It = pending_.erase(It);
        if (Stored->second.snapshot_.state_ != JobState::Failed || Stored->second.snapshot_.failedItems_ == 0 || !Stored->second.cleanupErrors_.empty()) {
            retryRequests_.erase(Stored->first); retryContexts_.erase(Stored->first);
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
        if (Stored->first == selectedId() && autoClose_->isChecked() && Stored->second.snapshot_.state_ == JobState::Completed
            && Stored->second.snapshot_.failedItems_ == 0 && Stored->second.cleanupErrors_.empty()) dialog_->hide();
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
    pause_->setEnabled(Active && !Snapshot.cancelRequested_);
    pause_->setText(Snapshot.pauseRequested_ ? tr("재개(&P)") : tr("일시 정지(&P)"));
    if (managerPause_) {
        const bool SelectedVisible = jobList_->currentItem() && !jobList_->currentItem()->isHidden();
        managerOpen_->setEnabled(Valid && SelectedVisible);
        managerStart_->setEnabled(Active && SelectedVisible && Snapshot.deferred_ && !Snapshot.cancelRequested_);
        managerRetry_->setEnabled(SelectedVisible && canRetry(Snapshot.id_));
        queueUp_->setEnabled(managerStart_->isEnabled()); queueDown_->setEnabled(managerStart_->isEnabled());
        managerPause_->setEnabled(pause_->isEnabled() && SelectedVisible);
        managerPause_->setText(pause_->text());
        managerCancel_->setEnabled(Active && !Snapshot.cancelRequested_ && SelectedVisible);
    }
    cancel_->setEnabled(Active && !Snapshot.cancelRequested_);
    for (const auto& [Choice, Button] : choices_)
        Button->setVisible(Active && !Snapshot.cancelRequested_ && Snapshot.interaction_
            && std::find(Snapshot.interaction_->choices_.begin(), Snapshot.interaction_->choices_.end(), Choice) != Snapshot.interaction_->choices_.end());
    const bool Password = Active && !Snapshot.cancelRequested_ && Snapshot.interaction_ && Snapshot.interaction_->kind_ == InteractionKind::Password;
    const auto Echo = Password ? QLineEdit::Password : QLineEdit::Normal;
    if (renamed_->echoMode() != Echo) { renamed_->clear(); renamed_->setEchoMode(Echo); }
    renamed_->setPlaceholderText(Password ? tr("암호") : tr("새 이름"));
    renamed_->setAccessibleName(Password ? tr("압축 파일 암호") : tr("충돌한 항목의 새 이름"));
    choices_.at(InteractionChoice::Confirm)->setText(Password ? tr("확인")
        : Snapshot.interaction_ && Snapshot.interaction_->kind_ == InteractionKind::ReadOnlyRemoval ? tr("허용")
        : Snapshot.interaction_ && Snapshot.interaction_->reason_ == "confirm-recycle-delete" ? tr("휴지통으로") : tr("영구 삭제"));
    renamed_->setVisible(Password || choices_.at(InteractionChoice::Rename)->isVisibleTo(dialog_));
    details_->setVisible(detailed_ || Snapshot.interaction_.has_value() || Snapshot.error_.has_value());
    previousPage_->setVisible(detailed_); nextPage_->setVisible(detailed_);
    if (!Valid) { permission_->hide(); metrics_->clear(); graph_->hide(); header_->setSubtitle({}); status_->setText(tr("파일 작업이 없습니다.")); details_->clear(); progress_->setRange(0, 1); progress_->setValue(0); progress_->setFormat({}); return; }
    graph_->setVisible(detailed_);
    header_->setTitle(jobs_->currentText());
    header_->setSubtitle(!Snapshot.metadata_.destination_.empty() ? tr("대상: %1").arg(DisplayPath(Snapshot.metadata_.destination_))
        : endpoints_.contains(Snapshot.id_) ? endpoints_.at(Snapshot.id_) : QString{});
    updateGraph(Snapshot);
    status_->setText(tr("%1 · 완료 %2 / 건너뜀 %3 / 실패 %4\n%5")
        .arg(Snapshot.cancelRequested_ && Active ? tr("취소 후 정리 중") : StateText(Snapshot.state_))
        .arg(Snapshot.completedItems_).arg(Snapshot.skippedItems_).arg(Snapshot.failedItems_)
        .arg(QString::fromStdString(Snapshot.progress_.currentItem_)));
    const auto Operation = static_cast<hc::operations::FileOperation>(jobs_->currentData(Qt::UserRole + 1).toInt());
    const bool ByteProgress = Operation == hc::operations::FileOperation::Copy || Operation == hc::operations::FileOperation::Move;
    const auto Total = ByteProgress ? Snapshot.progress_.bytesTotal_.value_or(0) : 0;
    const auto Now = IsTerminal(Snapshot.state_) ? Snapshot.updated_ : std::chrono::steady_clock::now();
    const auto Elapsed = Snapshot.submitted_ == std::chrono::steady_clock::time_point{} ? -1LL
        : std::chrono::duration_cast<std::chrono::seconds>(Now - Snapshot.submitted_).count();
    const bool Measuring = (Snapshot.state_ == JobState::Running || Snapshot.state_ == JobState::Committing) && !Snapshot.pauseRequested_ && !Snapshot.cancelRequested_;
    const auto Rate = measuredRate(Snapshot);
    const auto Remaining = Total > Snapshot.progress_.bytesDone_ && Rate > 0 ? QString::number((Total - Snapshot.progress_.bytesDone_) / Rate, 'f', 0) + tr("초") : tr("계산 대기");
    metrics_->setText(tr("속도 %1 · 남은 시간 %2 · 경과 %3").arg(fm::ui::TransferGraph::formatRate(Rate),
        Active && Measuring ? Remaining : QStringLiteral("—"), Elapsed >= 0 ? QString::number(Elapsed) + tr("초") : QStringLiteral("—")));
    dialog_->setWindowTitle(tr("%1 · %2").arg(StateText(Snapshot.state_), jobs_->currentText()));
    const auto ProgressTotal=Total ? Total : Snapshot.progress_.plannedItems_.value_or(0);
    const auto ProgressDone=Total ? Snapshot.progress_.bytesDone_ : Snapshot.completedItems_+Snapshot.skippedItems_+Snapshot.failedItems_;
    if (Active && ProgressTotal == 0) { progress_->setRange(0, 0); }
    else
    {
        progress_->setRange(0, 1000);
        progress_->setValue(ProgressTotal ? static_cast<int>(std::min(1000.0, 1000.0 * ProgressDone / ProgressTotal)) : Snapshot.state_ == JobState::Completed ? 1000 : 0);
        progress_->setFormat(ProgressTotal ? QStringLiteral("%p%") : QString{});
    }
    hc::columns::FormatOptions SizeFormat; SizeFormat.size_=hc::columns::SizeFormat::Adaptive; SizeFormat.precision_=1;
    if (const auto It=sizeFormats_.find(Snapshot.id_); It!=sizeFormats_.end()) {
        SizeFormat.size_=hc::columns::SizeFormat(It->second["size"].toInt()); SizeFormat.precision_=It->second["precision"].toInt(1); SizeFormat.grouping_=It->second["grouping"].toBool();
    }
    const auto SizeText=[&](uint64_t Bytes) { return hc::columns::Format(hc::columns::CellValue::Ready(quint64(Bytes)),SizeFormat); };
    QStringList Extra;
    if (ByteProgress) Extra.append(Snapshot.progress_.bytesTotal_ ? tr("처리량 %1 / %2").arg(SizeText(Snapshot.progress_.bytesDone_),SizeText(*Snapshot.progress_.bytesTotal_))
        : (Active ? tr("처리량 %1 · 전체 크기 집계 중") : tr("처리량 %1")).arg(SizeText(Snapshot.progress_.bytesDone_)));
    else Extra.append(tr("%1개 항목 처리").arg(Snapshot.completedItems_+Snapshot.skippedItems_));
    if (Snapshot.progress_.currentTotal_) Extra.append(tr("현재 파일 %1 / %2").arg(SizeText(Snapshot.progress_.currentBytes_),SizeText(*Snapshot.progress_.currentTotal_)));
    if (Snapshot.progress_.plannedItems_) Extra.append(tr("예정 %1개").arg(*Snapshot.progress_.plannedItems_));
    metrics_->setText(metrics_->text()+"\n"+Extra.join("\n"));
    bool PermissionError=Snapshot.error_ && Snapshot.error_->category_==ErrorCategory::AccessDenied;
    QString Details;
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
    details_->setMaximumHeight(std::clamp(ContentHeight,std::max(40,LineHeight+16),std::max(40,LineHeight*8+16)));
    details_->setVisible(!Details.isEmpty() && (detailed_ || Snapshot.interaction_.has_value() || Snapshot.error_.has_value()));
    previousPage_->setVisible(detailed_ && results_.contains(Snapshot.id_));
    nextPage_->setVisible(detailed_ && results_.contains(Snapshot.id_));
}

void FileJobUi::recordSamples()
{
    for (const auto& Snapshot : scheduler_.Snapshots()) {
        if (Snapshot.submitted_ == std::chrono::steady_clock::time_point{}) continue;
        auto& History = samples_[Snapshot.id_];
        const auto Now = IsTerminal(Snapshot.state_) ? Snapshot.updated_ : std::chrono::steady_clock::now();
        const auto Elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Now - Snapshot.submitted_).count();
        if (!History.empty() && (Elapsed <= History.back().elapsed_ || (!IsTerminal(Snapshot.state_) && Elapsed - History.back().elapsed_ < 500))) continue;
        History.push_back({Elapsed, static_cast<qint64>(std::min<uint64_t>(Snapshot.progress_.bytesDone_, INT64_MAX)),
            Snapshot.state_ == JobState::Paused || Snapshot.state_ == JobState::WaitingForUser || Snapshot.deferred_});
        // A bounded recent history avoids UI memory growing with long-running jobs.
        if (History.size() > 600) History.erase(History.begin());
    }
}
void FileJobUi::updateGraph(const JobSnapshot& Snapshot)
{
    const auto Total = static_cast<qint64>(std::min<uint64_t>(Snapshot.progress_.bytesTotal_.value_or(0), INT64_MAX));
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
    for (const auto& [Id, Snapshot] : Snapshots) if (Snapshot.deferred_) Queue.push_back(Snapshot);
    std::sort(Queue.begin(), Queue.end(), [](const auto& A, const auto& B) { return A.queueOrder_ < B.queueOrder_; });
    std::map<std::string, size_t> Positions;
    for (size_t Index = 0; Index < Queue.size(); ++Index) Positions[Queue[Index].id_] = Index + 1;
    int Active = 0, Waiting = 0, Finished = 0, Failed = 0, Queued = 0, Delayed = 0; double TotalRate = 0;
    for (int Row = jobList_->topLevelItemCount() - 1; Row >= 0; --Row)
        if (jobs_->findData(jobList_->topLevelItem(Row)->data(0, Qt::UserRole)) < 0) delete jobList_->takeTopLevelItem(Row);
    std::map<QString, QTreeWidgetItem*> Rows;
    for (int I = 0; I < jobList_->topLevelItemCount(); ++I) Rows[jobList_->topLevelItem(I)->data(0, Qt::UserRole).toString()] = jobList_->topLevelItem(I);
    for (int Row = 0; Row < jobs_->count(); ++Row) {
        const auto Id = jobs_->itemData(Row).toString();
        const auto Found = Snapshots.find(Id.toStdString()); if (Found == Snapshots.end()) continue;
        const auto& Snapshot = Found->second;
        const bool Terminal = IsTerminal(Snapshot.state_);
        Queued += Snapshot.deferred_; TotalRate += measuredRate(Snapshot);
        Active += !Terminal; Waiting += Snapshot.state_ == JobState::WaitingForUser;
        Finished += Snapshot.state_ == JobState::Completed;
        Failed += Snapshot.state_ == JobState::Failed || Snapshot.state_ == JobState::Cancelled;
        QTreeWidgetItem* Item = Rows.contains(Id) ? Rows.at(Id) : nullptr;
        if (!Item) { Item = new QTreeWidgetItem(jobList_); Item->setData(0, Qt::UserRole, Id); }
        Item->setText(0, (Snapshot.deferred_ ? tr("대기 #%1 · ").arg(Positions.at(Snapshot.id_)) : QString{}) + jobs_->itemText(Row));
        Item->setData(0, Qt::UserRole + 2, QVariant::fromValue<qulonglong>(Snapshot.queueOrder_));
        Item->setText(1, Snapshot.cancelRequested_ && !Terminal ? tr("취소 후 정리 중") : StateText(Snapshot.state_));
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
        Item->setText(2, tr("%1 / %2 B").arg(Snapshot.progress_.bytesDone_).arg(Snapshot.progress_.bytesTotal_ ? QString::number(*Snapshot.progress_.bytesTotal_) : tr("미상")));
        Item->setText(3, Snapshot.state_ == JobState::Running && !Snapshot.pauseRequested_ ? fm::ui::TransferGraph::formatRate(measuredRate(Snapshot)) : QStringLiteral("—"));
        Item->setText(4, QString::fromStdString(Snapshot.progress_.currentItem_));
        const auto Endpoint = endpoints_.contains(Snapshot.id_) ? endpoints_.at(Snapshot.id_) : QString{};
        Item->setToolTip(0, jobs_->itemText(Row) + "\n" + Endpoint + "\n" + Id + "\n" + (Snapshot.submittedUtcMs_ ? QLocale().toString(QDateTime::fromMSecsSinceEpoch(Snapshot.submittedUtcMs_), QLocale::ShortFormat) : tr("이전 버전: 접수 시각 미상")));
        const int Filter = filter_->currentIndex();
        const bool Match = Filter == 0 || (Filter == 1 && !Terminal) || (Filter == 2 && Snapshot.state_ == JobState::WaitingForUser)
            || (Filter == 3 && Snapshot.state_ == JobState::Completed) || (Filter == 4 && (Snapshot.state_ == JobState::Failed || Snapshot.state_ == JobState::Cancelled));
        Item->setHidden(!Match || !(Item->text(0) + Item->text(4) + Endpoint).contains(search_->text(), Qt::CaseInsensitive));
        if (Id.toStdString() == selectedId()) jobList_->setCurrentItem(Item);
    }
    summary_->setToolTip(tr("속도는 최근 2초의 실제 처리 바이트입니다. 10초 이상 바이트 변화가 없으면 지연으로 표시하며, 실패를 뜻하지 않습니다. 그래프는 최근 600개 표본을 보관합니다."));
    summary_->setText(tr("미완료 %1 · 대기열 %2 · 응답 필요 %3 · 완료 %4 · 실패·취소 %5\n전체 속도 %6 · 10초 이상 진행 지연 %7").arg(Active).arg(Queued).arg(Waiting).arg(Finished).arg(Failed).arg(fm::ui::TransferGraph::formatRate(TotalRate)).arg(Delayed));
    std::map<QTreeWidgetItem*, bool> Hidden;
    for (int Index = 0; Index < jobList_->topLevelItemCount(); ++Index) Hidden[jobList_->topLevelItem(Index)] = jobList_->topLevelItem(Index)->isHidden();
    std::vector<QTreeWidgetItem*> Ordered;
    while (jobList_->topLevelItemCount()) Ordered.push_back(jobList_->takeTopLevelItem(0));
    std::stable_sort(Ordered.begin(), Ordered.end(), [](auto* A, auto* B) { return A->data(0, Qt::UserRole + 2).toULongLong() < B->data(0, Qt::UserRole + 2).toULongLong(); });
    for (auto* Item : Ordered) { jobList_->addTopLevelItem(Item); Item->setHidden(Hidden.at(Item)); }
    for (auto* Item : Ordered) if (Item->data(0, Qt::UserRole).toString().toStdString() == selectedId()) jobList_->setCurrentItem(Item);
    manager_->findChild<QPushButton*>("hc.jobs.startQueue")->setEnabled(Queued > 0);
    if (indicator_) indicator_->setText(tr("파일 작업 %1 · 응답 필요 %2").arg(Active).arg(Waiting));
    const bool VisibleSelection = jobList_->currentItem() && !jobList_->currentItem()->isHidden();
    if (!VisibleSelection) { managerOpen_->setEnabled(false); managerPause_->setEnabled(false); managerCancel_->setEnabled(false); managerStart_->setEnabled(false); managerRetry_->setEnabled(false); queueUp_->setEnabled(false); queueDown_->setEnabled(false); }
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
    const auto Replied = scheduler_.Reply(selectedId(), {Current.Value().interaction_->id_, Choice, renamed_->text().toStdString()});
    if (!Replied.HasValue()) status_->setText(tr("응답을 적용할 수 없습니다: %1").arg(QString::fromStdString(Replied.Failure().code_)));
    else { renamed_->clear(); updateSelection(); }
}

bool FileJobUi::eventFilter(QObject* Object, QEvent* Event)
{
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
