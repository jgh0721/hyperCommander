#include "fileToolsDialogs.hpp"
#include <QtConcurrent>
#include <fmwidgets/Button.h>
#include <fmwidgets/RenameComparisonView.h>
#include <fmwidgets/WrapLayout.h>

namespace hc::ui {
namespace {
QString RenameStateText(filetools::RenameState State, bool Active) {
    using filetools::RenameState;
    if (Active) return QObject::tr("대기·진행 중");
    switch (State) {
    case RenameState::Completed: return QObject::tr("완료 · 되돌리기 가능");
    case RenameState::Restored: return QObject::tr("원래 이름 복구됨");
    default: return QObject::tr("중단 · 확인 필요");
    }
}
}
RenameHistoryDialog::RenameHistoryDialog(filetools::RenameJobController* Jobs, QWidget* Parent)
    : DialogShell(tr("이름 변경 이력"), Parent), jobs_(Jobs), jobOwner_(Jobs ? Jobs->RenameOwner() : nullptr) {
    setObjectName("hc.renameHistory"); resize(1020, 760);
    SetSummary(tr("재시작 후에도 변경 전후 이름을 확인할 수 있습니다. 중단 작업은 선택하여 이어 실행하거나 복구하세요."));
    list_ = new QTreeWidget(this); list_->setObjectName("hc.renameHistory.list"); list_->setAccessibleName(tr("이름 변경 이력 목록"));
    list_->setHeaderLabels({tr("시간"), tr("작업"), tr("상태"), tr("항목"), tr("폴더")}); list_->setRootIsDecorated(false);
    list_->setSelectionMode(QAbstractItemView::SingleSelection); list_->setUniformRowHeights(true); list_->setMinimumHeight(180);
    list_->setColumnWidth(0, 170); list_->setColumnWidth(1, 90); list_->setColumnWidth(2, 190); BodyLayout()->addWidget(list_, 1);
    comparison_ = new fm::ui::RenameComparisonView(this); comparison_->setObjectName("hc.renameHistory.preview"); comparison_->SetMode(2); BodyLayout()->addWidget(comparison_, 1);
    auto* Note = new QLabel(tr("선택한 계획의 첫 256개를 표시합니다. 실행 전에 파일 ID·크기·수정 시각을 다시 검사하며, 다른 파일은 덮어쓰지 않습니다. 완료된 실행 취소 기록을 다시 되돌리면 다시 실행됩니다."), this);
    Note->setWordWrap(true); BodyLayout()->addWidget(Note);
    queued_ = new QCheckBox(tr("대기열에 추가(&Q)"), this); queued_->setObjectName("hc.renameHistory.queued"); BodyLayout()->addWidget(queued_);
    auto* Actions = new fm::ui::WrapLayout;
    const auto Button = [this, Actions](const QString& Text, const char* Name) { auto* B = new fm::ui::Button(Text, this); B->setObjectName(Name); B->setAutoDefault(false); Actions->addWidget(B); return B; };
    undo_ = Button(tr("실행 취소(&U)"), "hc.renameHistory.undo"); resume_ = Button(tr("이어 실행(&R)"), "hc.renameHistory.resume");
    restore_ = Button(tr("원래 이름 복구(&O)"), "hc.renameHistory.restore"); refresh_ = Button(tr("새로 고침(&F)"), "hc.renameHistory.refresh");
    BodyLayout()->addLayout(Actions); AcceptButton()->hide(); CancelButton()->setText(tr("닫기"));
    connect(list_, &QTreeWidget::currentItemChanged, this, [this] { select(); });
    connect(refresh_, &QPushButton::clicked, this, [this] { reload(); });
    connect(undo_, &QPushButton::clicked, this, [this] { submit(0); }); connect(resume_, &QPushButton::clicked, this, [this] { submit(1); }); connect(restore_, &QPushButton::clicked, this, [this] { submit(2); });
    reload();
}
bool RenameHistoryDialog::active(const QString& Id) const {
    if (!jobOwner_) return false;
    const auto Key = "rename-record=" + Id.toStdString();
    for (const auto& Snapshot : jobs_->RenameScheduler().Snapshots())
        if (!operations::IsTerminal(Snapshot.state_) && Snapshot.metadata_.options_ == Key) return true;
    return false;
}
void RenameHistoryDialog::reload() {
    if (loading_) return; loading_ = true; refresh_->setEnabled(false); undo_->setEnabled(false); resume_->setEnabled(false); restore_->setEnabled(false);
    const auto Selected = list_->currentItem() ? list_->currentItem()->data(0, Qt::UserRole).toString() : QString();
    SetMessage(tr("이름 변경 이력을 읽고 있습니다…"));
    using Result = storage::Result<QList<filetools::RenameRecord>>; auto* Watcher = new QFutureWatcher<Result>(this);
    connect(Watcher, &QFutureWatcher<Result>::finished, this, [this, Watcher, Selected] {
        const auto Result = Watcher->result(); Watcher->deleteLater(); loading_ = false; refresh_->setEnabled(true);
        if (!Result.HasValue()) { SetMessage(QString::fromStdString(Result.Failure().code_), true); return; }
        records_ = Result.Value(); QSignalBlocker Block(list_); list_->clear(); QTreeWidgetItem* Selection = nullptr;
        for (const auto& Record : records_) {
            auto* Item = new QTreeWidgetItem(list_, {QDateTime::fromMSecsSinceEpoch(Record.createdUtcMs_).toString("yyyy-MM-dd HH:mm:ss"),
                Record.kind_ == "undo" ? tr("실행 취소") : Record.kind_ == "redo" ? tr("다시 실행") : tr("이름 변경"), RenameStateText(Record.state_, active(Record.id_)),
                QString::number(Record.count_), QDir::toNativeSeparators(Record.folder_)});
            Item->setData(0, Qt::UserRole, Record.id_); if (Record.id_ == Selected) Selection = Item;
        }
        list_->setCurrentItem(Selection ? Selection : list_->topLevelItem(0)); select();
    });
    Watcher->setFuture(QtConcurrent::run([] { return filetools::ReadRenameHistory(filetools::RenameJournalDirectory()); }));
}
void RenameHistoryDialog::select() {
    undo_->setEnabled(false); resume_->setEnabled(false); restore_->setEnabled(false); comparison_->SetRows({}); const auto Generation = ++generation_;
    auto* Item = list_->currentItem(); if (!Item || loading_) { if (!loading_) SetMessage(tr("이름 변경 기록이 없습니다.")); return; }
    const auto Id = Item->data(0, Qt::UserRole).toString(); SetMessage(tr("변경 계획을 읽고 있습니다…"));
    using Result = storage::Result<filetools::RenameRecord>; auto* Watcher = new QFutureWatcher<Result>(this);
    connect(Watcher, &QFutureWatcher<Result>::finished, this, [this, Watcher, Generation] {
        const auto Result = Watcher->result(); Watcher->deleteLater(); if (Generation != generation_ || loading_) return;
        if (!Result.HasValue()) { SetMessage(QString::fromStdString(Result.Failure().code_), true); return; }
        const auto& Record = Result.Value(); const bool Active = active(Record.id_); QList<fm::ui::RenameComparisonView::Row> Rows;
        for (int I = 0; I < qMin(256, Record.count_); ++I) Rows.append({QFileInfo(Record.rows_[I].source_.path_).fileName(), QFileInfo(Record.rows_[I].target_).fileName(), RenameStateText(Record.state_, Active), false});
        comparison_->SetRows(Rows);
        undo_->setText(Record.kind_ == "undo" ? tr("다시 실행(&U)") : tr("실행 취소(&U)"));
        bool Superseded = false, ChildActive = false;
        for (const auto& Later : records_) if (Later.parentId_ == Record.id_) {
            Superseded = Superseded || Later.state_ == filetools::RenameState::Completed;
            ChildActive = ChildActive || active(Later.id_);
        }
        undo_->setEnabled(jobOwner_ && !Active && !ChildActive && !Superseded && Record.state_ == filetools::RenameState::Completed);
        const bool Recoverable = jobOwner_ && !Active && Record.state_ != filetools::RenameState::Completed && Record.state_ != filetools::RenameState::Restored;
        resume_->setEnabled(Recoverable); restore_->setEnabled(Recoverable);
        SetMessage(Active || ChildActive ? tr("작업 관리자에서 대기 또는 실행 중입니다.") : Superseded ? tr("이미 되돌린 기록입니다. 최신 실행 취소·다시 실행 기록을 선택하세요.")
            : !Record.error_.isEmpty() ? Record.error_ : tr("%1개 · %2").arg(Record.count_).arg(RenameStateText(Record.state_, false)), !Record.error_.isEmpty());
    });
    Watcher->setFuture(QtConcurrent::run([Id] { return filetools::ReadRenameRecord(filetools::RenameJournalDirectory(), Id); }));
}
void RenameHistoryDialog::submit(int Action) {
    auto* Item = list_->currentItem(); if (!jobOwner_ || loading_ || !Item) return;
    const auto Id = Item->data(0, Qt::UserRole).toString(); if (active(Id)) { select(); return; }
    loading_ = true; ++generation_; undo_->setEnabled(false); resume_->setEnabled(false); restore_->setEnabled(false); refresh_->setEnabled(false);
    SetMessage(tr("복원 계획을 저장하고 작업을 접수하고 있습니다…"));
    using Result = storage::Result<operations::PreparedJob>; auto* Watcher = new QFutureWatcher<Result>(this);
    connect(Watcher, &QFutureWatcher<Result>::finished, this, [this, Watcher, Action] {
        auto Prepared = Watcher->future().takeResult(); Watcher->deleteLater(); loading_ = false; refresh_->setEnabled(true);
        if (!Prepared.HasValue() || !jobOwner_) { SetMessage(Prepared.HasValue() ? tr("작업 관리자가 종료되었습니다.") : QString::fromStdString(Prepared.Failure().code_), true); return; }
        Prepared.Value().request_.deferred_ = queued_->isChecked();
        auto Started = jobs_->SubmitRename(std::move(Prepared.Value()), Action == 0 ? tr("이름 변경 실행 취소·다시 실행") : Action == 1 ? tr("중단된 이름 변경 이어 실행") : tr("중단된 이름 변경 원래 이름 복구"));
        if (!Started.HasValue()) { SetMessage(QString::fromStdString(Started.Failure().code_), true); return; }
        const auto Handle = Started.Value(); auto* Timer = new QTimer(this);
        connect(Timer, &QTimer::timeout, this, [this, Timer, Handle] {
            if (Handle.completion_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
            Timer->stop(); Timer->deleteLater(); reload();
        }); Timer->start(150); reload();
    });
    Watcher->setFuture(QtConcurrent::run([Id, Action] { const auto Directory = filetools::RenameJournalDirectory(); return Action == 0 ? filetools::PrepareRenameUndo(Directory, Id) : filetools::PrepareRenameRecovery(Directory, Id, Action == 2); }));
}
}
