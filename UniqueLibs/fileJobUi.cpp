#include "stdafx.h"
#include "fileJobUi.hpp"

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
    : QObject(Main), scheduler_(std::move(Events)), main_(Main)
{
    Main->installEventFilter(this);
    dialog_ = new QDialog(Main);
    dialog_->setObjectName("hc.fileJobs");
    dialog_->setWindowTitle(tr("파일 작업"));
    dialog_->resize(680, 440);
    auto* Layout = new QVBoxLayout(dialog_);
    jobs_ = new QComboBox(dialog_); jobs_->setObjectName("hc.jobs.selection");
    Layout->addWidget(jobs_);
    status_ = new QLabel(dialog_); status_->setWordWrap(true);
    status_->setTextFormat(Qt::PlainText);
    Layout->addWidget(status_);
    progress_ = new QProgressBar(dialog_); Layout->addWidget(progress_);
    details_ = new QPlainTextEdit(dialog_); details_->setReadOnly(true);
    details_->setObjectName("hc.jobs.details"); Layout->addWidget(details_);
    renamed_ = new QLineEdit(dialog_); renamed_->setPlaceholderText(tr("새 이름"));
    renamed_->setAccessibleName(tr("충돌한 항목의 새 이름"));
    renamed_->setObjectName("hc.jobs.newName"); Layout->addWidget(renamed_);
    auto* Buttons = new QHBoxLayout; Layout->addLayout(Buttons);
    const std::vector<std::pair<InteractionChoice, QString>> Choices{
        {InteractionChoice::Confirm, tr("영구 삭제")}, {InteractionChoice::Replace, tr("덮어쓰기")},
        {InteractionChoice::Skip, tr("건너뛰기")}, {InteractionChoice::Rename, tr("이름 변경")}
    };
    for (const auto& [Choice, Text] : Choices)
    {
        auto* Button = new QPushButton(Text, dialog_);
        Button->setObjectName("hc.jobs.choice." + QString::number(static_cast<int>(Choice)));
        choices_[Choice] = Button; Buttons->addWidget(Button);
        connect(Button, &QPushButton::clicked, this, [this, Choice] { answer(Choice); });
    }
    Buttons->addStretch();
    pause_ = new QPushButton(tr("일시 정지"), dialog_); pause_->setObjectName("hc.jobs.pause");
    cancel_ = new QPushButton(tr("취소"), dialog_); cancel_->setObjectName("hc.jobs.cancel");
    Buttons->addWidget(pause_); Buttons->addWidget(cancel_);
    auto* Hide = new QPushButton(tr("닫기"), dialog_); Buttons->addWidget(Hide);
    auto* Clear = new QPushButton(tr("완료 이력 지우기"), dialog_); Layout->addWidget(Clear);
    connect(Clear, &QPushButton::clicked, this, [this] {
        for (const auto& [Id, Result] : results_)
        {
            scheduler_.Forget(Id);
            const auto Row = jobs_->findData(QString::fromStdString(Id));
            if (Row >= 0) jobs_->removeItem(Row);
        }
        results_.clear(); updateSelection();
    });
    connect(Hide, &QPushButton::clicked, dialog_, &QDialog::hide);
    connect(pause_, &QPushButton::clicked, this, [this] {
        const auto Current = scheduler_.Snapshot(selectedId());
        if (Current.HasValue()) scheduler_.Pause(selectedId(), !Current.Value().pauseRequested_);
        updateSelection();
    });
    connect(cancel_, &QPushButton::clicked, this, [this] { scheduler_.Cancel(selectedId()); updateSelection(); });
    connect(jobs_, &QComboBox::currentIndexChanged, this, [this] { renamed_->clear(); updateSelection(); });
    timer_ = new QTimer(this);
    connect(timer_, &QTimer::timeout, this, [this] { updateJobs(); });
    timer_->start(100);
    updateSelection();
}

FileJobUi::~FileJobUi() { timer_->stop(); scheduler_.Shutdown(); }

Result<JobHandle> FileJobUi::Submit(FileOperationRequest Request, OperationContext Context)
{
    if (closing_) return Error{ErrorCategory::Cancelled, "window-closing"};
    const auto Label = Request.sources_.empty() ? Request.destination_.root_ : Request.sources_.front().entry_.name_;
    const auto Operation = Request.operation_;
    return track(SubmitFileOperation(scheduler_, std::move(Request), std::move(Context)), Operation, Label);
}

Result<JobHandle> FileJobUi::Submit(ArchiveOperationRequest Request, OperationContext Context)
{
    if (closing_) return Error{ErrorCategory::Cancelled, "window-closing"};
    const auto Label = Request.sources_.empty() ? Request.destination_.nesting_.empty() ? Request.destination_.root_ : Request.destination_.nesting_.back().entry_ : Request.sources_.front().entry_.name_;
    const auto Operation = Request.operation_;
    return track(SubmitArchiveOperation(scheduler_, std::move(Request), std::move(Context)), Operation, Label);
}

Result<JobHandle> FileJobUi::Submit(MoveOperationRequest Request, OperationContext Context)
{
    if (closing_) return Error{ErrorCategory::Cancelled, "window-closing"};
    const auto Label = Request.sources_.empty() ? Request.destination_.root_ : Request.sources_.front().name_;
    return track(SubmitMoveOperation(scheduler_, std::move(Request), std::move(Context)), hc::operations::FileOperation::Move, Label);
}

Result<JobHandle> FileJobUi::track(Result<JobHandle> Submitted, hc::operations::FileOperation Operation, const std::string& Label)
{
    const auto OperationLabel = Operation == hc::operations::FileOperation::Copy ? tr("복사")
        : Operation == hc::operations::FileOperation::Move ? tr("이동") : Operation == hc::operations::FileOperation::Delete ? tr("삭제")
        : Operation == hc::operations::FileOperation::Rename ? tr("이름 변경") : tr("새 폴더");
    if (!Submitted.HasValue()) return Submitted.Failure();
    const auto& Handle = Submitted.Value();
    pending_.emplace(Handle.id_, Handle);
    jobs_->addItem(OperationLabel + " · " + QString::fromStdString(Label), QString::fromStdString(Handle.id_));
    jobs_->setItemData(jobs_->count() - 1, static_cast<int>(Operation), Qt::UserRole + 1);
    jobs_->setCurrentIndex(jobs_->count() - 1);
    Show();
    return Handle;
}

void FileJobUi::Show() { dialog_->show(); dialog_->raise(); updateSelection(); }
std::string FileJobUi::selectedId() const { return jobs_->currentData().toString().toStdString(); }

void FileJobUi::updateJobs()
{
    for (auto It = pending_.begin(); It != pending_.end();)
    {
        if (It->second.completion_.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) { ++It; continue; }
        const auto Stored = results_.insert_or_assign(It->first, It->second.completion_.get()).first;
        It = pending_.erase(It);
        if (onFinished_) onFinished_(Stored->second);
    }
    // Snapshot polling recovers even when a bounded event subscriber drops progress.
    if (dialog_->isVisible()) updateSelection();
    if (closing_ && pending_.empty() && main_) { timer_->stop(); main_->close(); }
}

void FileJobUi::updateSelection()
{
    const auto Current = scheduler_.Snapshot(selectedId());
    const bool Valid = Current.HasValue();
    const auto Snapshot = Valid ? Current.Value() : JobSnapshot{};
    const bool Active = Valid && !IsTerminal(Snapshot.state_);
    pause_->setEnabled(Active && !Snapshot.cancelRequested_ && Snapshot.state_ != JobState::Committing);
    pause_->setText(Snapshot.pauseRequested_ ? tr("계속") : tr("일시 정지"));
    cancel_->setEnabled(Active && !Snapshot.cancelRequested_);
    for (const auto& [Choice, Button] : choices_)
        Button->setVisible(Active && !Snapshot.cancelRequested_ && Snapshot.interaction_
            && std::find(Snapshot.interaction_->choices_.begin(), Snapshot.interaction_->choices_.end(), Choice) != Snapshot.interaction_->choices_.end());
    renamed_->setVisible(choices_.at(InteractionChoice::Rename)->isVisibleTo(dialog_));
    if (!Valid) { status_->setText(tr("파일 작업이 없습니다.")); details_->clear(); progress_->setRange(0, 1); progress_->setValue(0); progress_->setFormat({}); return; }
    status_->setText(tr("%1 · 완료 %2 / 건너뜀 %3 / 실패 %4\n%5")
        .arg(Snapshot.cancelRequested_ && Active ? tr("취소 후 정리 중") : StateText(Snapshot.state_))
        .arg(Snapshot.completedItems_).arg(Snapshot.skippedItems_).arg(Snapshot.failedItems_)
        .arg(QString::fromStdString(Snapshot.progress_.currentItem_)));
    const auto Operation = static_cast<hc::operations::FileOperation>(jobs_->currentData(Qt::UserRole + 1).toInt());
    const bool ByteProgress = Operation == hc::operations::FileOperation::Copy || Operation == hc::operations::FileOperation::Move;
    const auto Total = ByteProgress ? Snapshot.progress_.bytesTotal_.value_or(0) : 0;
    if (Active && Total == 0) { progress_->setRange(0, 0); }
    else
    {
        progress_->setRange(0, 1000);
        progress_->setValue(Total ? static_cast<int>(std::min(1000.0, 1000.0 * Snapshot.progress_.bytesDone_ / Total)) : Snapshot.state_ == JobState::Completed ? 1000 : 0);
        progress_->setFormat(ByteProgress ? tr("%1 / %2 바이트 · %3 바이트/초").arg(Snapshot.progress_.bytesDone_).arg(Total).arg(Snapshot.bytesPerSecond_, 0, 'f', 0)
            : tr("%1개 항목 처리").arg(Snapshot.completedItems_ + Snapshot.skippedItems_));
    }
    QString Details;
    if (Snapshot.interaction_)
        Details = Snapshot.interaction_->kind_ == InteractionKind::DeleteConfirmation
            ? tr("선택한 항목과 하위 항목을 영구 삭제합니다. 휴지통에서 복원할 수 없습니다.")
            : tr("목적지에 같은 이름의 항목이 있습니다. 처리 방법을 선택하세요.");
    if (Snapshot.error_) Details += "\n" + QString::fromStdString(Snapshot.error_->code_);
    if (const auto It = results_.find(Snapshot.id_); It != results_.end())
    {
        std::size_t Shown = 0;
        for (const auto& Item : It->second.items_)
        {
            if (++Shown > 200) { Details += tr("\n표시 한도: 처음 200개 항목. 전체 결과는 작업 이력에 보관됩니다."); break; }
            Details += "\n" + DisplayPath(Item.item_) + " — " + (Item.status_ == ItemOutcome::Status::Completed ? tr("완료")
                : Item.status_ == ItemOutcome::Status::Skipped ? tr("건너뜀") : tr("실패"));
            if (Item.error_) Details += ": " + QString::fromStdString(Item.error_->code_);
            if (Item.destinationCommitted_ && !Item.sourceRemoved_) Details += tr(" (목적지 저장됨, 원본 유지)");
        }
        for (const auto& Error : It->second.cleanupErrors_) Details += tr("\n정리 실패: %1").arg(QString::fromStdString(Error.code_));
    }
    Details = Details.trimmed();
    if (details_->toPlainText() != Details) details_->setPlainText(Details);
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
