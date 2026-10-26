#include "stdafx.h"
#include "saveRecoveryDialog.hpp"
#include "runtime/saveRecovery.hpp"
#include "UniqueLibs/qtDialogFrame.hpp"
#include <QtConcurrent/QtConcurrentRun>
#include <QTreeWidget>
#include <QHeaderView>
#include <QFormLayout>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QFileDialog>
#include <QFileInfo>
#include <QDir>
#include <QCloseEvent>
#include <QSignalBlocker>
#include <QSettings>

namespace hc::compare
{
    SaveRecoveryDialog::SaveRecoveryDialog(Request Context, QWidget* Parent) : QDialog(Parent), context_(std::move(Context))
    {
        setObjectName("saveRecoveryDialog"); setWindowTitle(tr("저장 복구")); resize(920, 680);
        setWindowModality(Qt::ApplicationModal);
        auto* layout = new QVBoxLayout(this);
        auto* description = new QLabel(tr("기록을 선택해 경로를 확인한 뒤 복구를 실행하세요. 대상 파일이 사라진 경우에만 확인된 백업을 복원합니다.\n현재 대상 파일과 임시 파일, 복구 기록은 보존합니다. 저장하지 않은 편집 내용은 이 기록에 포함되지 않습니다."), this);
        description->setWordWrap(true); layout->addWidget(description);
        auto* commands = new QHBoxLayout();
        const auto button = [this](const QString& Text, const char* Name) {
            auto* b = new QPushButton(Text, this); b->setObjectName(Name); b->setAutoDefault(false); return b;
        };
        refresh_ = button(tr("목록 새로 고침"), "recoveryRefresh"); browse_ = button(tr("기록 열기…"), "recoveryBrowse");
        preview_ = button(tr("기록 다시 확인"), "recoveryPreview");
        commands->addWidget(refresh_); commands->addWidget(browse_); commands->addWidget(preview_); commands->addStretch(); layout->addLayout(commands);
        journals_ = new QTreeWidget(this); journals_->setObjectName("recoveryJournals"); journals_->setAccessibleName(tr("저장 복구 기록 목록"));
        journals_->setHeaderLabels({tr("기록"), tr("경로")}); journals_->setRootIsDecorated(false); journals_->setUniformRowHeights(true);
        journals_->setSelectionMode(QAbstractItemView::SingleSelection); journals_->setColumnWidth(0, 300); layout->addWidget(journals_, 1);
        auto* form = new QFormLayout();
        const auto field = [this, form](const QString& Label, const char* Name) {
            auto* f = new QLineEdit(this); f->setReadOnly(true); f->setObjectName(Name); f->setAccessibleName(Label); form->addRow(Label, f); return f;
        };
        destination_ = field(tr("대상 파일"), "recoveryDestination"); backup_ = field(tr("백업 파일"), "recoveryBackup"); stage_ = field(tr("임시 파일"), "recoveryStage");
        phase_ = new QLabel(this); phase_->setTextFormat(Qt::PlainText); form->addRow(tr("마지막 저장 단계"), phase_); layout->addLayout(form);
        state_ = new QLabel(this); state_->setObjectName("recoveryStatus"); state_->setTextFormat(Qt::PlainText); state_->setWordWrap(true); state_->setTextInteractionFlags(Qt::TextSelectableByMouse);
        layout->addWidget(state_);
        details_ = new QPlainTextEdit(this); details_->setObjectName("recoveryDetails"); details_->setAccessibleName(tr("복구 결과와 보존 파일")); details_->setReadOnly(true); layout->addWidget(details_, 1);
        auto* actions = new QHBoxLayout(); actions->addStretch();
        recover_ = button(tr("상태 확인 및 누락 파일 복원"), "recoveryApply"); close_ = button(tr("닫기"), "recoveryClose"); actions->addWidget(recover_); actions->addWidget(close_); layout->addLayout(actions);
        // The description becomes the header subtitle; recover · close move to the footer band.
        description->hide();
        auto* footer = hc::ui::ApplyDialogFrame(this, {{}, description->text(), fm::ui::glyph::Undo});
        hc::ui::MoveToFooter(footer, recover_, true); hc::ui::MoveToFooter(footer, close_);
        connect(refresh_, &QPushButton::clicked, this, &SaveRecoveryDialog::Refresh);
        connect(browse_, &QPushButton::clicked, this, [this] {
            const auto path = QFileDialog::getOpenFileName(this, tr("저장 복구 기록 열기"), {}, tr("HyperCompare 복구 기록 (*.hcsave)")); if (!path.isEmpty()) OpenJournal(path);
        });
        connect(preview_, &QPushButton::clicked, this, [this] { if (!selectedPath_.isEmpty()) OpenJournal(selectedPath_); });
        connect(journals_, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem* Item) {
            if (!Busy() && Item) OpenJournal(Item->data(0, Qt::UserRole).toString());
        });
        connect(recover_, &QPushButton::clicked, this, &SaveRecoveryDialog::Recover);
        connect(close_, &QPushButton::clicked, this, &SaveRecoveryDialog::reject);
        connect(&watcher_, &QFutureWatcher<Result>::finished, this, [this] {
            active_ = false; last_ = watcher_.result();
            if (task_ == Task::List)
            {
                const QSignalBlocker block(journals_); journals_->clear();
                for (const auto& value : last_.recovery_)
                {
                    const auto path = value.toString(); auto* item = new QTreeWidgetItem(journals_, {QFileInfo(path).fileName(), path});
                    item->setData(0, Qt::UserRole, path); item->setToolTip(0, path); item->setToolTip(1, path);
                }
                journals_->sortItems(1, Qt::AscendingOrder);
                state_->setText(last_.error_.isEmpty() ? (last_.recovery_.isEmpty() ? tr("저장 복구 기록이 없습니다.") : tr("기록 %1개 — 검토할 기록을 선택하세요.").arg(last_.recovery_.size())) : tr("목록 조회 중단: %1").arg(last_.error_));
                details_->setPlainText(tr("기록 폴더: %1").arg(last_.saveRecovery_["directory"].toString()));
            }
            else
            {
                if (task_ == Task::Preview && last_.error_.isEmpty() && last_.outcome_ == "RecoveryPreview")
                { previewPath_ = selectedPath_; previewHash_ = last_.saveRecovery_["journalSha256"].toString().toLatin1(); }
                showResult();
            }
            updateControls(); if (activityChanged_) activityChanged_(); if (closing_) close();
        });
        QSettings settings; QFont listFont = font(), textFont("Cascadia Mono", 11);
        listFont.fromString(settings.value("fonts/list", listFont.toString()).toString());
        textFont.fromString(settings.value("fonts/text", textFont.toString()).toString());
        journals_->setFont(listFont); details_->setFont(textFont);
        Refresh();
    }
    SaveRecoveryDialog::~SaveRecoveryDialog() { canceled_->store(true); watcher_.waitForFinished(); }
    void SaveRecoveryDialog::clearPreview()
    {
        previewPath_.clear(); previewHash_.clear(); destination_->clear(); backup_->clear(); stage_->clear(); phase_->clear(); details_->clear();
    }
    void SaveRecoveryDialog::Refresh()
    {
        if (Busy()) return; selectedPath_.clear(); clearPreview(); run(Task::List);
    }
    void SaveRecoveryDialog::OpenJournal(const QString& Path)
    {
        if (Busy() || Path.isEmpty()) return;
        selectedPath_ = QDir::cleanPath(QFileInfo(Path).absoluteFilePath()); clearPreview();
        { const QSignalBlocker block(journals_); QTreeWidgetItem* selected = nullptr;
            for (int i = 0; i < journals_->topLevelItemCount(); ++i)
                if (journals_->topLevelItem(i)->data(0, Qt::UserRole).toString() == selectedPath_) selected = journals_->topLevelItem(i);
            if (!selected)
            {
                selected = new QTreeWidgetItem(journals_, {QFileInfo(selectedPath_).fileName(), selectedPath_}); selected->setData(0, Qt::UserRole, selectedPath_);
                selected->setToolTip(0, selectedPath_); selected->setToolTip(1, selectedPath_);
            }
            journals_->setCurrentItem(selected); journals_->scrollToItem(selected);
        }
        run(Task::Preview, selectedPath_);
    }
    void SaveRecoveryDialog::Recover()
    {
        if (Busy() || previewHash_.size() != 64 || previewPath_ != selectedPath_) return;
        run(Task::Recover, previewPath_);
    }
    void SaveRecoveryDialog::run(Task Kind, const QString& Path)
    {
        task_ = Kind; active_ = true; canceled_->store(false);
        auto hash = Kind == Task::Recover ? previewHash_ : QByteArray{}; previewHash_.clear();
        Request request; request.mode_ = "recover"; request.journal_ = Path; request.dryRun_ = Kind != Task::Recover;
        request.timeoutMs_ = context_.timeoutMs_; request.protectedInputs_ = context_.protectedInputs_;
        state_->setText(Kind == Task::Recover ? tr("파일 상태 확인 및 복원 중…") : tr("기록 확인 중…"));
        updateControls(); if (activityChanged_) activityChanged_();
        watcher_.setFuture(QtConcurrent::run([request, hash, canceled = canceled_] {
            Result result; Budget limits; limits.canceled_ = canceled.get(); limits.deadline_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(request.timeoutMs_);
            try { RecoverSavedFile(request, result, limits, hash); }
            catch (const std::exception& error) { result.error_ = QString::fromUtf8(error.what()); result.exitCode_ = 2; result.completion_ = result.error_ == "Canceled" ? "Canceled" : "Failed"; }
            return result;
        }));
    }
    void SaveRecoveryDialog::showResult()
    {
        const auto& info = last_.saveRecovery_;
        for (const auto& pair : {std::pair{destination_, "destination"}, {backup_, "backup"}, {stage_, "stage"}})
        { pair.first->setText(info[pair.second].toString()); pair.first->setCursorPosition(0); }
        const QMap<QString, QString> phases{{"writing", tr("임시 파일 기록")}, {"prepared", tr("게시 준비")}, {"backedUp", tr("원본 백업")}, {"published", tr("결과 게시")}, {"finished", tr("저장 완료")}, {"aborted", tr("저장 중단")}};
        phase_->setText(phases.value(info["phase"].toString(), info["phase"].toString()));
        if (!last_.error_.isEmpty())
            state_->setText(last_.error_ == "SaveRecoveryPreviewChanged" ? tr("확인 후 복구 기록이 변경되었습니다. ‘기록 다시 확인’으로 경로를 검토하세요.") : tr("복구 처리 중단: %1").arg(last_.error_));
        else if (task_ == Task::Preview) state_->setText(tr("기록 확인 완료. 실제 대상·백업·임시 파일의 식별자는 복구 실행 시 검사합니다."));
        else if (info["identitiesVerified"].toBool())
            state_->setText(last_.completion_ != "Complete" ? tr("복구 검사가 일부 완료되었습니다. 아래 오류와 보존 파일을 확인하세요.") :
                last_.commit_ == "Committed" ? tr("복구 검사 완료 — 이미 저장된 결과 파일을 유지했습니다.") :
                info["destinationPresent"].toBool() ? tr("복구 검사 완료 — 대상 파일이 존재합니다. 아래 보존 경로를 검토하세요.") : tr("대상 파일이 없습니다. 보존된 임시 파일을 별도로 검토하세요."));
        QStringList lines{tr("기록: %1").arg(selectedPath_)};
        if (!last_.error_.isEmpty()) lines << tr("오류: %1").arg(last_.error_);
        for (const auto& warning : last_.warnings_) lines << tr("알림: %1").arg(warning.toString());
        if (task_ == Task::Recover) { lines << tr("보존된 경로:"); for (const auto& path : last_.recovery_) lines << path.toString(); }
        details_->setPlainText(lines.join('\n'));
    }
    void SaveRecoveryDialog::updateControls()
    {
        journals_->setEnabled(!Busy()); refresh_->setEnabled(!Busy()); browse_->setEnabled(!Busy());
        preview_->setEnabled(!Busy() && !selectedPath_.isEmpty()); recover_->setEnabled(!Busy() && previewHash_.size() == 64 && previewPath_ == selectedPath_);
        close_->setText(Busy() ? tr("취소 요청") : tr("닫기"));
    }
    void SaveRecoveryDialog::Cancel()
    { if (Busy()) { canceled_->store(true); state_->setText(tr("취소 요청 중… 진행 중인 파일 작업이 끝날 때까지 기다립니다.")); } }
    void SaveRecoveryDialog::reject() { if (Busy()) { Cancel(); return; } QDialog::reject(); }
    void SaveRecoveryDialog::closeEvent(QCloseEvent* Event)
    { if (Busy()) { closing_ = true; Cancel(); Event->ignore(); return; } QDialog::closeEvent(Event); }
}
