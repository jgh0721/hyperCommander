#include "stdafx.h"
#include "directoryMergeDialog.hpp"
#include "runtime/requestFile.hpp"
#include "directoryView.hpp"
#include <QSettings>
#include "runtime/directoryPlan.hpp"
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QStyledItemDelegate>
#include <QTableView>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QtConcurrentRun>

namespace hc::compare
{
    namespace
    {
        QString choiceLabel(const QString& Choice)
        {
            if (Choice == "keep") return QObject::tr("대상 유지");
            if (Choice == "unresolved") return QObject::tr("미결정");
            if (Choice == "merge") return QObject::tr("자동 텍스트 병합");
            if (Choice == "omit") return QObject::tr("제외 — 대상 존재 시 삭제");
            return Choice.toUpper();
        }
        class ChoiceDelegate final : public QStyledItemDelegate
        {
        public:
            explicit ChoiceDelegate(QObject* Parent) : QStyledItemDelegate(Parent) {}
            QWidget* createEditor(QWidget* Parent, const QStyleOptionViewItem&, const QModelIndex& Index) const override
            {
                auto* combo = new QComboBox(Parent); combo->setAccessibleName(QObject::tr("병합 원본 선택"));
                for (const auto& choice : Index.data(Qt::UserRole).toStringList()) combo->addItem(choiceLabel(choice), choice);
                return combo;
            }
            void setEditorData(QWidget* Editor, const QModelIndex& Index) const override
            { auto* combo = static_cast<QComboBox*>(Editor); combo->setCurrentIndex(combo->findData(Index.data(Qt::EditRole))); }
            void setModelData(QWidget* Editor, QAbstractItemModel* Model, const QModelIndex& Index) const override
            { Model->setData(Index, static_cast<QComboBox*>(Editor)->currentData(), Qt::EditRole); }
        };
        Budget budget(const Request& Request, const std::shared_ptr<std::atomic_bool>& Canceled)
        { Budget result; result.canceled_ = Canceled.get(); result.deadline_ = Request.timeoutMs_ == 0 ? std::chrono::steady_clock::time_point::max() : std::chrono::steady_clock::now() + std::chrono::milliseconds(Request.timeoutMs_); return result; }
    }
    void DirectoryPlanModel::SetPlan(QJsonObject Plan)
    {
        beginResetModel(); plan_ = std::move(Plan); actions_ = plan_["actions"].toArray(); choices_.clear(); unresolved_ = 0;
        for (auto& paths : paths_) paths.clear();
        const auto roots = plan_["roots"].toArray();
        for (qsizetype i = 0; i < std::min<qsizetype>(3, roots.size()); ++i)
            for (const auto& entry : roots[i].toObject()["entries"].toArray()) paths_[std::size_t(i)].insert(entry.toObject()["path"].toString().toCaseFolded());
        choices_.reserve(actions_.size());
        for (const auto& action : actions_) { const auto choice = action.toObject()["choice"].toString(); choices_.push_back(choice); unresolved_ += choice == "unresolved"; }
        endResetModel(); if (changed_) changed_();
    }
    QJsonObject DirectoryPlanModel::Plan() const
    {
        auto plan = plan_; QJsonArray actions;
        for (qsizetype row = 0; row < actions_.size(); ++row) { auto action = actions_[row].toObject(); action["choice"] = choices_[row]; actions.append(action); }
        plan["actions"] = actions; return plan;
    }
    bool DirectoryPlanModel::Matches(const QString& OutputPath, bool Mirror, bool Overwrite, storage::CopyOptions Copy) const
    {
        return HasPlan() && Output().compare(QDir::cleanPath(QDir::fromNativeSeparators(OutputPath)), Qt::CaseInsensitive) == 0 &&
            plan_["mirror"].toBool() == Mirror && plan_["overwrite"].toBool() == Overwrite &&
            plan_["copyPolicy"].toObject()["metadata"].toBool() == Copy.metadata_ && plan_["copyPolicy"].toObject()["acl"].toBool() == Copy.acl_ && plan_["copyPolicy"].toObject()["streams"].toBool() == Copy.streams_;
    }
    int DirectoryPlanModel::rowCount(const QModelIndex& Parent) const { return Parent.isValid() ? 0 : int(actions_.size()); }
    int DirectoryPlanModel::columnCount(const QModelIndex& Parent) const { return Parent.isValid() ? 0 : 3; }
    QVariant DirectoryPlanModel::data(const QModelIndex& Index, int Role) const
    {
        if (!Index.isValid() || Index.row() < 0 || Index.row() >= rowCount() || Index.column() < 0 || Index.column() >= 3) return {};
        const auto action = actions_[Index.row()].toObject();
        if (Role == Qt::UserRole)
        {
            if (plan_["transfer"] == true)
            {
                QStringList choices{"keep"}; if (paths_[0].contains(action["path"].toString().toCaseFolded())) choices.append("left"); return choices;
            }
            QStringList choices{"unresolved", "omit"}; const QStringList names{"left", "right", "base"};
            for (std::size_t i = 0; i < paths_.size(); ++i) if (paths_[i].contains(action["path"].toString().toCaseFolded())) choices.push_back(names[qsizetype(i)]);
            if (action["choice"] == "merge") choices.push_back("merge");
            return choices;
        }
        if (Role == Qt::EditRole && Index.column() == 1) return choices_[Index.row()];
        if (Role != Qt::DisplayRole && Role != Qt::AccessibleTextRole) return {};
        if (Index.column() == 0) return action["path"].toString();
        if (Index.column() == 1) return choiceLabel(choices_[Index.row()]);
        return DirectoryStatusName(action["reason"].toString());
    }
    QVariant DirectoryPlanModel::headerData(int Section, Qt::Orientation Orientation, int Role) const
    {
        if (Role != Qt::DisplayRole) return {};
        if (Orientation == Qt::Vertical) return Section + 1;
        return Section >= 0 && Section < 3 ? QStringList{tr("경로"), tr("선택"), tr("비교 상태")}[Section] : QVariant{};
    }
    Qt::ItemFlags DirectoryPlanModel::flags(const QModelIndex& Index) const
    { return QAbstractTableModel::flags(Index) | (Index.isValid() && Index.column() == 1 ? Qt::ItemIsEditable : Qt::NoItemFlags); }
    bool DirectoryPlanModel::setData(const QModelIndex& Index, const QVariant& Value, int Role)
    {
        if (Role != Qt::EditRole || Index.column() != 1 || !Index.isValid() || Index.row() >= rowCount() || !data(Index, Qt::UserRole).toStringList().contains(Value.toString())) return false;
        auto& old = choices_[Index.row()]; if (old == Value.toString()) return true;
        unresolved_ -= old == "unresolved"; old = Value.toString(); unresolved_ += old == "unresolved";
        emit dataChanged(Index, Index, {Qt::DisplayRole, Qt::EditRole}); if (changed_) changed_(); return true;
    }
    DirectoryMergeDialog::DirectoryMergeDialog(Request Input, Result Comparison, QWidget* Parent)
        : QDialog(Parent), input_(std::move(Input)), comparison_(std::move(Comparison))
    {
        if (!input_.file3_.isEmpty()) { const std::array roots{input_.file1_, input_.file2_, input_.file3_}; for (int i = 0; i < 3; ++i) if (i != input_.directoryPair_[0] && i != input_.directoryPair_[1]) input_.protectedInputs_.append(roots[i]); }
        setObjectName("directoryMergeDialog"); setWindowTitle(tr("디렉터리 병합 계획")); resize(1000, 700);
        auto* box = new QVBoxLayout(this); auto* introduction = new QLabel(tr("출력 디렉터리를 지정하고 계획을 검토하십시오. 입력은 보존하며, ‘계획 실행’에서 출력 파일을 변경합니다."), this);
        introduction->setObjectName("directoryPlanIntroduction"); introduction->setWordWrap(true); box->addWidget(introduction);
        const std::array roots{input_.file1_, input_.file2_, input_.file3_}; const auto selected = input_.file3_.isEmpty() ? std::array{0, 1} : input_.directoryPair_;
        auto* inputs = new QLabel(tr("LEFT  %1\nRIGHT  %2%3").arg(roots[selected[0]], roots[selected[1]], input_.base_.isEmpty() ? QString{} : "\nBASE  " + input_.base_), this);
        inputs->setTextFormat(Qt::PlainText); inputs->setWordWrap(true); inputs->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard); box->addWidget(inputs);
        auto* destination = new QHBoxLayout(); output_ = new QLineEdit(this); output_->setObjectName("directoryOutput"); output_->setPlaceholderText(tr("출력 디렉터리")); output_->setAccessibleName(tr("병합 출력 디렉터리"));
        auto* browse = new QPushButton(tr("찾아보기…"), this); browse->setObjectName("directoryOutputBrowse"); destination->addWidget(output_, 1); destination->addWidget(browse); box->addLayout(destination);
        connect(browse, &QPushButton::clicked, this, [this] { if (Busy()) return; const auto path = QFileDialog::getExistingDirectory(this, tr("출력 디렉터리")); if (!path.isEmpty()) output_->setText(path); });
        auto* options = new QHBoxLayout(); overwrite_ = new QCheckBox(tr("기존 출력 파일 교체 허용"), this); mirror_ = new QCheckBox(tr("출력에만 있는 항목 제거 계획 포함"), this);
        options->addWidget(overwrite_); options->addWidget(mirror_); options->addStretch(); prepare_ = new QPushButton(tr("계획 만들기"), this); options->addWidget(prepare_); box->addLayout(options);
        connect(prepare_, &QPushButton::clicked, this, [this] {
            if (model_->HasPlan() && QMessageBox::question(this, tr("계획 다시 만들기"), tr("현재 파일별 선택을 버리고 새 계획을 만드시겠습니까?")) != QMessageBox::Yes) return;
            if (transfer_) PrepareTransfer(transferSource_, transferPath_); else Prepare(output_->text(), mirror_->isChecked(), overwrite_->isChecked());
        });
        auto* copyRow = new QHBoxLayout(); metadata_ = new QCheckBox(tr("파일 수정 시각·특성"), this); acl_ = new QCheckBox(tr("파일 DACL"), this); streams_ = new QCheckBox(tr("파일 추가 데이터 스트림(ADS)"), this);
        metadata_->setObjectName("directoryCopyMetadata"); acl_->setObjectName("directoryCopyAcl"); streams_->setObjectName("directoryCopyStreams");
        metadata_->setChecked(input_.directoryCopy_.metadata_); acl_->setChecked(input_.directoryCopy_.acl_); streams_->setChecked(input_.directoryCopy_.streams_);
        metadata_->setToolTip(tr("선택한 원본 파일의 수정 시각과 읽기 전용·숨김·시스템·보관 특성을 복사합니다. 폴더에는 적용하지 않습니다."));
        acl_->setToolTip(tr("원본 파일 DACL을 복사하며 대상 폴더의 상속 규칙이 적용됩니다. 적용 여부가 기록되지 않은 중단 작업은 자동 재개하지 않고 복구 검토를 요구합니다."));
        streams_->setToolTip(tr("원본 파일의 추가 데이터 스트림을 복사하고 내용을 검증합니다. 지원되지 않는 저장소에서는 실행을 중단합니다."));
        copyRow->addWidget(metadata_); copyRow->addWidget(acl_); copyRow->addWidget(streams_); copyRow->addStretch(); box->addLayout(copyRow);
        model_ = new DirectoryPlanModel(this); model_->setObjectName("directoryPlanModel"); table_ = new QTableView(this); table_->setObjectName("directoryPlanTable"); table_->setModel(model_);
        QFont list = font(); list.fromString(QSettings{}.value("fonts/list", list.toString()).toString()); table_->setFont(list);
        table_->setItemDelegateForColumn(1, new ChoiceDelegate(table_)); table_->setSelectionBehavior(QAbstractItemView::SelectRows); table_->setSelectionMode(QAbstractItemView::ExtendedSelection);
        table_->setAlternatingRowColors(true); table_->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
        table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch); table_->setColumnWidth(1, 250); table_->setColumnWidth(2, 180); box->addWidget(table_, 1);
        auto* choices = new QHBoxLayout();
        for (const auto& role : {QString("left"), QString("right"), QString("base"), QString("omit")})
        {
            if (role == "base" && input_.base_.isEmpty()) continue;
            auto* button = new QPushButton(role == "omit" ? tr("선택 항목 제외") : tr("선택 항목 %1").arg(role.toUpper()), this);
            button->setToolTip(tr("선택한 행에 적용합니다. 해당 원본이 없는 행은 변경하지 않습니다.")); choices->addWidget(button);
            connect(button, &QPushButton::clicked, this, [this, role] { if (Busy()) return; for (const auto& index : table_->selectionModel()->selectedRows()) model_->setData(model_->index(index.row(), 1), role); });
        }
        choices->addStretch(); box->addLayout(choices);
        state_ = new QLabel(this); state_->setObjectName("directoryPlanState"); state_->setTextFormat(Qt::PlainText); state_->setWordWrap(true); state_->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard); box->addWidget(state_);
        auto* buttons = new QHBoxLayout(); save_ = new QPushButton(tr("계획 저장…"), this); apply_ = new QPushButton(tr("계획 실행…"), this); cancel_ = new QPushButton(tr("닫기"), this);
        buttons->addWidget(save_); buttons->addWidget(apply_); buttons->addStretch(); buttons->addWidget(cancel_); box->addLayout(buttons);
        connect(save_, &QPushButton::clicked, this, [this] { const auto path = QFileDialog::getSaveFileName(this, tr("새 병합 계획 저장"), {}, "HyperCompare directory plan (*.json)"); if (!path.isEmpty()) SavePlan(path); });
        connect(apply_, &QPushButton::clicked, this, [this] {
            if (QMessageBox::question(this, tr("디렉터리 병합 실행"), tr("검토한 계획을 출력 디렉터리에 적용합니다. 교체와 제외 항목의 삭제가 포함될 수 있습니다. 실행하시겠습니까?")) == QMessageBox::Yes)
                ApplyPlan(true, QFileInfo::exists(planPath_ + ".journal"));
        });
        connect(cancel_, &QPushButton::clicked, this, [this] { reject(); });
        model_->changed_ = [this] { planPath_.clear(); if (statusChanged_) statusChanged_(3); state_->setText(tr("미결정 %1개 — 선택 열은 더블 클릭 또는 F2로 편집합니다.\n계획 출력: %2").arg(model_->Unresolved()).arg(model_->Output())); updateControls(); };
        const auto optionsChanged = [this] {
            if (statusChanged_) statusChanged_(3);
            updateControls();
            if (!Busy() && model_->HasPlan() && !model_->Matches(output_->text(), mirror_->isChecked(), overwrite_->isChecked(), copyOptions()))
                state_->setText(tr("출력 설정이 변경되었습니다. ‘계획 만들기’로 다시 검토하십시오."));
        };
        connect(metadata_, &QCheckBox::toggled, this, optionsChanged); connect(acl_, &QCheckBox::toggled, this, optionsChanged); connect(streams_, &QCheckBox::toggled, this, optionsChanged);
        connect(output_, &QLineEdit::textChanged, this, optionsChanged);
        connect(mirror_, &QCheckBox::toggled, this, optionsChanged); connect(overwrite_, &QCheckBox::toggled, this, optionsChanged);
        connect(&watcher_, &QFutureWatcher<Result>::finished, this, [this] {
            active_ = false; last_ = watcher_.result();
            if (last_.error_.isEmpty())
            {
                if (kind_ == "prepare") { model_->SetPlan(last_.directoryPlan_); output_->setText(model_->Output()); }
                if (kind_ == "save") { planPath_ = last_.planWritten_; state_->setText(tr("계획 저장 완료: %1").arg(planPath_)); }
                if (kind_ == "apply") state_->setText(tr("적용 완료 — %1").arg(planPath_));
            }
            else state_->setText(tr("%1 · 적용 상태: %2\n%3").arg(last_.error_, last_.commit_, last_.recovery_.isEmpty() ? QString{} : QString::fromUtf8(QJsonDocument(last_.recovery_).toJson())));
            updateControls(); if (statusChanged_) statusChanged_(kind_ == "apply" || !last_.error_.isEmpty() ? last_.exitCode_ : 3); if (closing_) close();
        });
        updateControls();
    }
    DirectoryMergeDialog::~DirectoryMergeDialog() { canceled_->store(true); watcher_.waitForFinished(); }
    void DirectoryMergeDialog::run(QString Kind, std::function<Result()> Work)
    {
        if (Busy()) return; kind_ = std::move(Kind); canceled_->store(false); active_ = true; if (statusChanged_) statusChanged_(3); state_->setText(tr("처리 중…")); updateControls();
        watcher_.setFuture(QtConcurrent::run([work = std::move(Work)] {
            try { return work(); } catch (const Failure& error) { Result result; result.error_ = QString::fromStdString(error.code_); return result; }
            catch (const std::exception& error) { Result result; result.error_ = QString::fromUtf8(error.what()); return result; }
        }));
    }
    void DirectoryMergeDialog::Prepare(const QString& Output, bool Mirror, bool Overwrite)
    {
        if (Busy()) return; model_->SetPlan({}); auto input = input_; input.output_ = Output; input.mirror_ = Mirror; input.overwrite_ = Overwrite; input.directoryCopy_ = copyOptions();
        output_->setText(Output); mirror_->setChecked(Mirror); overwrite_->setChecked(Overwrite);
        run("prepare", [input, comparison = comparison_, cancel = canceled_] {
            Result result; result.directoryPlan_ = MakeDirectoryPlan(input, comparison, budget(input, cancel)); result.exitCode_ = 0; result.completion_ = "Complete"; return result;
        });
    }
    void DirectoryMergeDialog::PrepareTransfer(int Source, const QString& Path)
    {
        if (Busy() || Source < 0 || Source > 1) return;
        const std::array roots{input_.file1_, input_.file2_, input_.file3_}; const auto selected = input_.file3_.isEmpty() ? std::array{0, 1} : input_.directoryPair_;
        transfer_ = true; transferSource_ = Source; transferPath_ = Path; model_->SetPlan({}); output_->setText(roots[selected[1 - Source]]); mirror_->setChecked(false); overwrite_->setChecked(true);
        findChild<QLabel*>("directoryPlanIntroduction")->setText(tr("선택 항목을 반대편 폴더로 복사합니다. LEFT는 복사 원본이며, ‘대상 유지’ 항목은 변경하지 않습니다. 하위 폴더에는 현재 포함/제외 필터가 적용됩니다. 계획을 검토·저장한 뒤 실행하십시오.\n복사 원본: %1").arg(roots[selected[Source]]));
        findChild<QPushButton*>("directoryOutputBrowse")->setEnabled(false);
        auto input = input_; input.directoryCopy_ = copyOptions();
        run("prepare", [input, comparison = comparison_, Source, Path, cancel = canceled_] {
            Result result; result.directoryPlan_ = MakeDirectoryTransferPlan(input, comparison, Source, Path, budget(input, cancel)); result.exitCode_ = 0; result.completion_ = "Complete"; return result;
        });
    }
    void DirectoryMergeDialog::SavePlan(const QString& Path)
    {
        if (Busy() || !model_->Matches(output_->text(), mirror_->isChecked(), overwrite_->isChecked(), copyOptions())) return;
        const auto plan = model_->Plan(); run("save", [input = input_, plan, Path, cancel = canceled_] {
            CheckProtectedInputs(input, {Path}, budget(input, cancel));
            Result result; SaveDirectoryPlan(plan, Path, result, budget(input, cancel)); result.exitCode_ = 0; result.completion_ = "Complete"; return result;
        });
    }
    void DirectoryMergeDialog::ApplyPlan(bool AllowDelete, bool Resume)
    {
        if (Busy() || planPath_.isEmpty() || model_->Unresolved() || !model_->Matches(output_->text(), mirror_->isChecked(), overwrite_->isChecked(), copyOptions())) return;
        Request request; request.mode_ = "apply"; request.headless_ = true; request.applyPlan_ = planPath_; request.allowDelete_ = AllowDelete; request.resume_ = Resume;
        request.protectedInputs_ = input_.protectedInputs_; request.requestId_ = input_.requestId_;
        request.timeoutMs_ = input_.timeoutMs_; request.applyPlanDigest_ = QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(model_->Plan()).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex());
        run("apply", [request, cancel = canceled_] { return Execute(request, cancel, true); });
    }
    storage::CopyOptions DirectoryMergeDialog::copyOptions() const
    { return {metadata_->isChecked(), true, acl_->isChecked(), streams_->isChecked()}; }
    void DirectoryMergeDialog::updateControls()
    {
        metadata_->setEnabled(!Busy()); acl_->setEnabled(!Busy()); streams_->setEnabled(!Busy());
        prepare_->setEnabled(!Busy()); output_->setEnabled(!Busy() && !transfer_); mirror_->setEnabled(!Busy() && !transfer_); overwrite_->setEnabled(!Busy() && !transfer_); table_->setEnabled(!Busy());
        const bool current = model_->Matches(output_->text(), mirror_->isChecked(), overwrite_->isChecked(), copyOptions());
        save_->setEnabled(!Busy() && current); apply_->setEnabled(!Busy() && current && !planPath_.isEmpty() && !model_->Unresolved());
        cancel_->setText(Busy() ? tr("취소") : tr("닫기"));
    }
    void DirectoryMergeDialog::reject() { if (Busy()) { canceled_->store(true); state_->setText(tr("취소 요청 중…")); return; } QDialog::reject(); }
    void DirectoryMergeDialog::closeEvent(QCloseEvent* Event)
    { if (Busy()) { canceled_->store(true); closing_ = true; Event->ignore(); return; } QDialog::closeEvent(Event); }
}
