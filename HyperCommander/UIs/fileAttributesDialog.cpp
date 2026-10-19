#include "stdafx.h"
#include "fileAttributesDialog.hpp"
#include "UniqueLibs/settings/appSettings.hpp"
#include <QtConcurrent/QtConcurrentRun>
#include <QGridLayout>
#include <QMessageBox>
#include <QPushButton>
#include <QTimeZone>
#include <fmwidgets/Button.h>
#include <QTabWidget>

namespace hc::ui
{
    FileAttributesDialog::FileAttributesDialog(std::vector<storage::EntrySnapshot> Entries,
        std::shared_ptr<storage::IStorageProvider> Reader, std::shared_ptr<const storage::ISelectionManifest> Manifest, QWidget* Parent)
        : DialogShell(tr("속성 변경"), Parent), roots_(std::move(Entries)), reader_(std::move(Reader)), manifest_(std::move(Manifest)),
          state_(std::make_shared<State>()), watcher_(new QFutureWatcher<void>(this)), timer_(new QTimer(this))
    {
        setObjectName("hc.fileAttributes"); resize(680, 700);
        targetPath_ = new QLabel(this); targetPath_->setObjectName("hc.attributes.targetPath");
        targetPath_->setWordWrap(true); targetPath_->setTextFormat(Qt::PlainText);
        targetPath_->setTextInteractionFlags(Qt::TextSelectableByMouse); BodyLayout()->addWidget(targetPath_);
        updateTargetSummary(roots_);
        showDetails_ = new QPushButton(tr("대상 목록 펼치기"), this); showDetails_->setAutoDefault(false);
        showDetails_->setCheckable(true); showDetails_->setObjectName("hc.attributes.showDetails"); BodyLayout()->addWidget(showDetails_);
        details_ = new QPlainTextEdit(this); details_->setReadOnly(true); details_->setObjectName("hc.attributes.details");
        details_->setAccessibleName(tr("대상 목록 및 처리 결과")); details_->setFixedHeight(120); details_->hide(); BodyLayout()->addWidget(details_);
        connect(showDetails_, &QPushButton::toggled, this, [this](bool Expanded) {
            details_->setVisible(Expanded);
            showDetails_->setText(result_ ? (Expanded ? tr("처리 결과 접기") : tr("처리 결과 펼치기"))
                : (Expanded ? tr("대상 목록 접기") : tr("대상 목록 펼치기")));
        });
        recursive_ = new QCheckBox(tr("선택한 폴더의 하위 파일·폴더 포함"), this); recursive_->setObjectName("hc.attributes.recursive");
        recursive_->setEnabled(manifest_ || std::any_of(roots_.begin(), roots_.end(), [](const auto& E) { return E.directory_; }));
        BodyLayout()->addWidget(recursive_);
        scope_ = new QLabel(this); scope_->setWordWrap(true); scope_->setTextFormat(Qt::PlainText); BodyLayout()->addWidget(scope_);
        auto* Tabs = new QTabWidget(this); Tabs->setObjectName("hc.attributes.tabs"); BodyLayout()->addWidget(Tabs);
        auto* AttributesPage = new QWidget(Tabs); auto* AttributesLayout = new QVBoxLayout(AttributesPage);
        auto* Grid = new QGridLayout;
        Grid->addWidget(new QLabel(tr("속성"), this), 0, 0); Grid->addWidget(new QLabel(tr("현재 상태"), this), 0, 1); Grid->addWidget(new QLabel(tr("변경 지시"), this), 0, 2);
        const QStringList Names{tr("R 읽기 전용"), tr("H 숨김"), tr("S 시스템"), tr("A 기록")};
        for (int I = 0; I < 4; ++I) {
            auto* Label = new QLabel(Names[I], this); values_[I] = new QComboBox(this); values_[I]->setObjectName(QString("hc.attributes.value%1").arg(I));
            values_[I]->addItems({tr("유지"), tr("설정"), tr("해제")}); Label->setBuddy(values_[I]);
            values_[I]->setAccessibleName(Names[I]); current_[I] = new QLabel(tr("확인 중"), this);
            Grid->addWidget(Label, I + 1, 0); Grid->addWidget(current_[I], I + 1, 1); Grid->addWidget(values_[I], I + 1, 2);
            connect(values_[I], &QComboBox::currentIndexChanged, this, [this] { update(); });
        }
        AttributesLayout->addLayout(Grid);
        auto* Hint = new QLabel(tr("유지는 항목별 값을 보존합니다. 폴더의 읽기 전용 속성은 하위 파일의 쓰기 권한을 제한하지 않습니다."), this);
        Hint->setWordWrap(true); AttributesLayout->addWidget(Hint); AttributesLayout->addStretch(); Tabs->addTab(AttributesPage,tr("파일 속성"));
        auto* DatesPage = new QWidget(Tabs); auto* DatesLayout = new QVBoxLayout(DatesPage);
        changeTime_ = new QCheckBox(tr("수정 시각 변경"), this); changeTime_->setObjectName("hc.attributes.changeTime"); DatesLayout->addWidget(changeTime_);
        auto* Time = new QHBoxLayout; timestamp_ = new QDateTimeEdit(QDateTime::currentDateTime(), this); timestamp_->setCalendarPopup(true);
        timestamp_->setDisplayFormat("yyyy-MM-dd HH:mm:ss"); timestamp_->setAccessibleName(tr("수정 시각"));
        now_ = new QPushButton(tr("현재 시각"), this); now_->setAutoDefault(false); Time->addWidget(timestamp_, 1); Time->addWidget(now_); DatesLayout->addLayout(Time);
        auto* TimeNote = new QLabel(tr("시간대: %1 · 생성·접근 시각 유지").arg(QString::fromUtf8(QTimeZone::systemTimeZoneId())), this);
        TimeNote->setWordWrap(true); DatesLayout->addWidget(TimeNote); DatesLayout->addStretch(); Tabs->addTab(DatesPage,tr("날짜·시간"));
        summary_ = new QLabel(this); summary_->setWordWrap(true); summary_->setTextFormat(Qt::PlainText); summary_->setObjectName("hc.attributes.summary"); BodyLayout()->addWidget(summary_);
        retry_ = new QPushButton(tr("실패 항목만 다시 확인"), this); retry_->setAutoDefault(false); retry_->hide(); BodyLayout()->addWidget(retry_);
        connect(retry_, &QPushButton::clicked, this, [this] {
            roots_.clear(); manifest_.reset();
            for (const auto& Item : state_->outcome_.items_) if (Item.retry_) roots_.push_back(Item.entry_);
            recursive_->setChecked(false); recursive_->setEnabled(false); collect();
        });
        connect(now_, &QPushButton::clicked, this, [this] { timestamp_->setDateTime(QDateTime::fromSecsSinceEpoch(QDateTime::currentSecsSinceEpoch())); });
        connect(changeTime_, &QCheckBox::toggled, this, [this] { update(); });
        connect(timestamp_, &QDateTimeEdit::dateTimeChanged, this, [this] { update(); });
        connect(recursive_, &QCheckBox::toggled, this, [this] { if (!busy_) { ready_ = false; result_ = false; update(); } });
        connect(watcher_, &QFutureWatcher<void>::finished, this, [this] { complete(); });
        connect(timer_, &QTimer::timeout, this, [this] { poll(); }); timer_->start(100);
        EnableCompactLayout(720);
        connect(showDetails_, &QPushButton::toggled, this, [this] { FitToContents(); });
        collect(); FitToContents();
    }
    FileAttributesDialog::~FileAttributesDialog() { state_->stop_.request_stop(); }
    void FileAttributesDialog::updateTargetSummary(const std::vector<storage::EntrySnapshot>& Entries)
    {
        const auto Count = manifest_ ? manifest_->Count() : Entries.size();
        if (Entries.empty()) {
            SetSummary(tr("%1개 항목 선택").arg(Count)); targetPath_->setText(tr("대상 경로 확인 중…")); return;
        }
        const auto FirstPath = QDir::toNativeSeparators(QString::fromStdString(Entries.front().locator_.root_));
        if (Count == 1) {
            SetSummary(tr("%1 · %2 1개").arg(QFileInfo(FirstPath).fileName(), Entries.front().directory_ ? tr("폴더") : tr("파일")));
            targetPath_->setText(FirstPath);
        } else {
            const auto Folders = std::count_if(Entries.begin(), Entries.end(), [](const auto& E) { return E.directory_; });
            SetSummary(Count == Entries.size() ? tr("%1개 항목 선택 · 파일 %2개, 폴더 %3개").arg(Count).arg(Entries.size() - Folders).arg(Folders)
                : tr("%1개 항목 선택").arg(Count));
            const auto Parent = QFileInfo(FirstPath).absolutePath();
            const bool SameParent = std::all_of(Entries.begin(), Entries.end(), [&](const auto& E) {
                return QFileInfo(QString::fromStdString(E.locator_.root_)).absolutePath().compare(Parent, Qt::CaseInsensitive) == 0;
            });
            targetPath_->setText(SameParent ? QDir::toNativeSeparators(Parent) : tr("여러 위치 · 첫 번째 대상: %1").arg(FirstPath));
        }
        targetPath_->setToolTip(targetPath_->text());
    }
    attributes::Approval FileAttributesDialog::approval(std::shared_ptr<State> StateValue)
    {
        return [StateValue](const storage::EntrySnapshot& Entry, bool Enumerating) {
            if (!StateValue->allowElevation_) return attributes::Decision::Skip;
            { std::lock_guard Lock(StateValue->mutex_); StateValue->answer_.reset(); StateValue->question_ = {{Entry, Enumerating}}; }
            while (!StateValue->stop_.stop_requested()) {
                { std::lock_guard Lock(StateValue->mutex_); if (StateValue->answer_) return *StateValue->answer_; }
                std::this_thread::sleep_for(std::chrono::milliseconds(30));
            }
            return attributes::Decision::Stop;
        };
    }
    void FileAttributesDialog::poll()
    {
        if (!busy_ || asking_) return;
        std::optional<std::pair<storage::EntrySnapshot, bool>> Question;
        { std::lock_guard Lock(state_->mutex_); summary_->setText(state_->progress_); Question = std::exchange(state_->question_, {}); }
        if (!Question) return;
        asking_ = true;
        QMessageBox Box(QMessageBox::Question, tr("관리자 권한 필요"),
            tr("%1\n\n%2\n관리자 도우미를 사용하시겠습니까? 권한·소유자는 변경하지 않습니다. Windows UAC 확인이 표시될 수 있습니다.")
                .arg(QString::fromStdString(Question->first.locator_.root_), Question->second ? tr("하위 항목을 조회할 권한이 없습니다.") : tr("이 항목의 속성을 변경할 권한이 없습니다.")), QMessageBox::NoButton, this);
        Box.setTextFormat(Qt::PlainText);
        auto* Elevate = Box.addButton(tr("관리자로 계속"), QMessageBox::AcceptRole);
        auto* Skip = Box.addButton(tr("건너뛰기"), QMessageBox::RejectRole);
        auto* Stop = Box.addButton(tr("작업 중단"), QMessageBox::DestructiveRole);
        Box.setDefaultButton(Skip); Box.exec();
        const auto Decision = Box.clickedButton() == Elevate ? attributes::Decision::Elevate : Box.clickedButton() == Stop ? attributes::Decision::Stop : attributes::Decision::Skip;
        { std::lock_guard Lock(state_->mutex_); state_->answer_ = Decision; }
        if (Decision == attributes::Decision::Stop) state_->stop_.request_stop();
        asking_ = false;
    }
    storage::MetadataPatch FileAttributesDialog::patch() const
    {
        storage::MetadataPatch Patch; constexpr std::uint32_t Bits[]{1, 2, 4, 32};
        for (int I = 0; I < 4; ++I) { if (values_[I]->currentIndex() == 1) Patch.setMask_ |= Bits[I]; if (values_[I]->currentIndex() == 2) Patch.clearMask_ |= Bits[I]; }
        if (changeTime_->isChecked()) Patch.modifiedUtcMs_ = timestamp_->dateTime().toMSecsSinceEpoch();
        return Patch;
    }
    void FileAttributesDialog::update()
    {
        for (auto* Value : values_) Value->setEnabled(!busy_ && !result_);
        changeTime_->setEnabled(!busy_ && !result_); timestamp_->setEnabled(!busy_ && !result_ && changeTime_->isChecked());
        now_->setEnabled(timestamp_->isEnabled());
        AcceptButton()->setText(result_ ? tr("닫기") : ready_ ? tr("적용") : tr("대상 확인"));
        SetAcceptEnabled(!busy_ && (result_ || !ready_ || (!state_->plan_.blocked_ && !state_->plan_.cancelled_
            && !state_->plan_.entries_.empty() && !patch().Empty() && patch().Valid() && timestamp_->dateTime().isValid())));
        CancelButton()->setText(busy_ ? tr("작업 중단") : tr("취소"));
        if (!busy_ && !result_) {
            QStringList Changes; for (int I = 0; I < 4; ++I) if (values_[I]->currentIndex()) Changes << values_[I]->accessibleName() + " " + values_[I]->currentText();
            if (changeTime_->isChecked()) Changes << tr("수정 시각 %1").arg(timestamp_->text());
            summary_->setText(Changes.isEmpty() ? tr("변경할 속성을 선택하세요.") : Changes.join(" · "));
        }
    }
    void FileAttributesDialog::collect()
    {
        if (busy_) return;
        state_ = std::make_shared<State>(); state_->allowElevation_ = settings::AppSettings().Read(settings::Section::Options).toObject()["allowElevation"].toBool(true);
        SetMessage({});
        busy_ = true; applying_ = result_ = ready_ = false; retry_->hide(); details_->clear(); update();
        showDetails_->setChecked(false); showDetails_->setText(tr("대상 목록 펼치기"));
        if (!manifest_ || !recursive_->isChecked()) updateTargetSummary(roots_);
        state_->progress_ = tr("대상 확인 중 · 실제 파일은 아직 변경하지 않습니다.");
        auto StateValue = state_; auto Reader = reader_; auto Roots = roots_; auto Manifest = manifest_; const bool Recursive = recursive_->isChecked(); recursive_->setEnabled(false);
        watcher_->setFuture(QtConcurrent::run([StateValue, Reader, Roots = std::move(Roots), Manifest, Recursive]() mutable {
            storage::OperationContext Context; Context.stopToken_ = StateValue->stop_.get_token();
            StateValue->plan_ = attributes::Collect(std::move(Roots), Reader, Recursive, Context, approval(StateValue), Manifest);
        }));
    }
    void FileAttributesDialog::accept()
    {
        if (busy_) return;
        if (result_) { DialogShell::accept(); return; }
        if (!ready_) { collect(); return; }
        if (!AcceptButton()->isEnabled()) return;
        busy_ = applying_ = true; state_->stop_ = std::stop_source{}; recursive_->setEnabled(false); update();
        auto StateValue = state_; auto Reader = reader_; const auto Patch = patch();
        watcher_->setFuture(QtConcurrent::run([StateValue, Reader, Patch] {
            storage::OperationContext Context; Context.stopToken_ = StateValue->stop_.get_token();
            auto Writer = storage::MakeNativeWritableStorage(StateValue->plan_.entries_.front().locator_.kind_, Reader);
            StateValue->outcome_ = attributes::Apply(StateValue->plan_, Patch, Writer, permissions::MakeFileElevationService(), Context, approval(StateValue),
                [StateValue](std::size_t Done, const auto& Entry) { std::lock_guard Lock(StateValue->mutex_); StateValue->progress_ = QObject::tr("변경 중 %1 / %2 · %3").arg(Done).arg(StateValue->plan_.entries_.size()).arg(QString::fromStdString(Entry.locator_.root_)); });
        }));
    }
    void FileAttributesDialog::complete()
    {
        try { watcher_->future().waitForFinished(); }
        catch (...) {
            busy_ = false; ready_ = false; result_ = true; changed_ |= applying_;
            SetMessage(tr("작업 중 예외가 발생했습니다. 실제 상태를 확인하세요."), true); update(); return;
        }
        busy_ = false; QStringList Lines;
        for (const auto& Issue : state_->plan_.issues_) Lines << Issue.state_ + " · " + QString::fromStdString(Issue.entry_.locator_.root_) + " · " + Issue.detail_;
        if (applying_) {
            result_ = true; const auto& Result = state_->outcome_; changed_ |= Result.changed_ > 0 || Result.failed_ > 0;
            showDetails_->setText(tr("처리 결과 접기")); showDetails_->setChecked(true);
            summary_->setText(tr("성공 %1 · 변경 없음 %2 · 실패 %3 · 건너뜀 %4 · 미처리 %5\n수집 단계 제외·실패 %6건 (조회 불가 폴더의 하위 개수는 미확인)")
                .arg(Result.changed_).arg(Result.unchanged_).arg(Result.failed_).arg(Result.skipped_).arg(Result.pending_).arg(state_->plan_.issues_.size()));
            for (const auto& Item : Result.items_) Lines << Item.state_ + " · " + QString::fromStdString(Item.entry_.locator_.root_) + " · " + Item.detail_;
            retry_->setVisible(std::any_of(Result.items_.begin(), Result.items_.end(), [](const auto& I) { return I.retry_; }));
            SetMessage(tr("이미 반영한 변경은 유지됩니다. 결과 불명 항목은 실제 상태를 확인한 뒤 새 작업으로 처리하세요."));
        } else {
            if (manifest_ && !recursive_->isChecked()) updateTargetSummary(state_->plan_.entries_);
            ready_ = !state_->plan_.blocked_ && !state_->plan_.cancelled_;
            scope_->setText(tr("대상 %1개 · 수집 제외·실패 %2건 · %3").arg(state_->plan_.entries_.size()).arg(state_->plan_.issues_.size()).arg(recursive_->isChecked() ? tr("하위 항목 포함") : tr("선택 항목만")));
            static constexpr std::uint32_t Bits[]{1, 2, 4, 32};
            for (int I = 0; I < 4; ++I) {
                const auto Count = std::count_if(state_->plan_.entries_.begin(), state_->plan_.entries_.end(), [I](const auto& E) { return (E.attributes_ & Bits[I]) != 0; });
                current_[I]->setText(state_->plan_.entries_.empty() ? tr("대상 없음") : Count == 0 ? tr("모두 해제") : Count == state_->plan_.entries_.size() ? tr("모두 설정") : tr("혼합"));
            }
            for (const auto& Entry : state_->plan_.entries_) Lines << QString::fromStdString(Entry.locator_.root_);
            if (!ready_) SetMessage(tr("대상 수집이 중단되었습니다. 다시 확인하세요."), true);
            else if (!state_->plan_.issues_.empty()) SetMessage(tr("일부 항목 또는 하위 폴더가 제외되었습니다. 목록을 확인한 뒤 적용하세요."));
            recursive_->setEnabled(manifest_ || std::any_of(roots_.begin(), roots_.end(), [](const auto& E) { return E.directory_; }));
        }
        details_->setPlainText(Lines.join('\n')); update();
    }
    void FileAttributesDialog::reject()
    {
        if (busy_) { state_->stop_.request_stop(); SetMessage(tr("중단 요청 중입니다. 이미 반영한 변경은 유지됩니다.")); return; }
        DialogShell::reject();
    }
}
