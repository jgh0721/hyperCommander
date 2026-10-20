#include "stdafx.h"
#include "itemSelectionController.hpp"

/// UIs
#include "Components/cmpView.hpp"
#include "dialogShell.hpp"

/// 내부 라이브러리
#include "UniqueLibs/itemSelection.hpp"
#include "UniqueLibs/builtInFsModel.hpp"
#include "UniqueLibs/qtWorkspaceRestore.hpp"
#include "UniqueLibs/settings/appSettings.hpp"

/// 외부 라이브러리
#include <QtitanGrid.h>
#include <fmwidgets/Button.h>

/// 프레임워크
#include <QtConcurrent/QtConcurrentRun>
#include <QFutureWatcher>
#include <QInputDialog>
#include <QStatusBar>
#include <QTabWidget>
#include <QDateEdit>
#include <QFormLayout>
#include <QElapsedTimer>

namespace hc::ui {
namespace {
using namespace hc::selection;
QString Text(const char* Source) { return QCoreApplication::translate("ItemSelection", Source); }

class SelectionDialog final : public DialogShell {
public:
    SelectionDialog(const QList<Group>& Groups, const QJsonObject& Settings, bool Remove, QWidget* Parent)
        : DialogShell(Remove ? Text("항목 선택 해제") : Text("항목 선택"), Parent), groups_(Groups), settings_(Settings) {
        setObjectName("hc.selection.dialog"); EnableCompactLayout(760);
        auto* Form = new QFormLayout; Form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        operation_ = combo("hc.selection.operation", {Text("일치 항목을 현재 선택에 추가"), Text("일치 항목을 현재 선택에서 해제"),
            Text("현재 목록의 선택을 일치 항목으로 교체"), Text("현재 선택 중 일치 항목만 유지")});
        Form->addRow(Text("동작"), operation_);
        group_ = new QComboBox; group_->setObjectName("hc.selection.group"); group_->addItem(Text("직접 입력"), "");
        for (const auto& G : groups_) group_->addItem(G.name_, G.id_);
        group_->setToolTip(groups_.isEmpty() ? Text("설정 → 파일 그룹에 사용 중인 그룹이 없습니다.") : Text("설정 → 파일 그룹에 저장된 그룹의 조건을 사용합니다."));
        Form->addRow(Text("파일 그룹"), group_);
        pattern_ = new QLineEdit; pattern_->setObjectName("hc.selection.pattern"); pattern_->setMaxLength(4096);
        syntax_ = combo("hc.selection.syntax", {Text("와일드카드"), Text("문자열 포함"), Text("정규식")});
        auto* NameRow = new QHBoxLayout; NameRow->addWidget(pattern_, 1); NameRow->addWidget(syntax_);
        Form->addRow(Text("포함할 이름"), NameRow);
        exclude_ = new QLineEdit; exclude_->setObjectName("hc.selection.exclude"); exclude_->setMaxLength(4096);
        exclude_->setPlaceholderText(Text("예: *_backup.*; *.tmp")); Form->addRow(Text("제외할 이름"), exclude_);
        BodyLayout()->addLayout(Form);
        auto* Hint = new QLabel(Text("와일드카드: * 여러 글자 · ? 한 글자 · ; 패턴 구분. 그룹은 설정 → 파일 그룹의 조건을 사용합니다."));
        Hint->setWordWrap(true); BodyLayout()->addWidget(Hint);
        tabs_ = new QTabWidget; tabs_->setObjectName("hc.selection.mode");
        auto* Simple = new QWidget; auto* SimpleLayout = new QVBoxLayout(Simple);
        auto* SimpleHint = new QLabel(Text("그룹 또는 이름 조건으로 현재 목록의 항목을 선택합니다."));
        SimpleHint->setWordWrap(true); SimpleLayout->addWidget(SimpleHint); tabs_->addTab(Simple, Text("간단"));
        auto* Advanced = new QWidget; auto* AdvancedLayout = new QFormLayout(Advanced);
        combine_ = combo("hc.selection.combine", {Text("모든 조건 일치 (AND)"), Text("하나 이상 일치 (OR)")});
        AdvancedLayout->addRow(Text("조건 결합"), combine_);
        size_ = new QLineEdit; size_->setObjectName("hc.selection.size"); size_->setPlaceholderText(Text("빈 값: 크기 조건 사용 안 함"));
        size_->setMaxLength(24); sizeOp_ = combo("hc.selection.sizeOp", {Text("이상 (MiB)"), Text("이하 (MiB)")});
        auto* SizeRow = new QHBoxLayout; SizeRow->addWidget(sizeOp_); SizeRow->addWidget(size_, 1); AdvancedLayout->addRow(Text("파일 크기"), SizeRow);
        useDate_ = new QCheckBox(Text("기준일 이후 (포함)")); useDate_->setObjectName("hc.selection.useDate");
        date_ = new QDateEdit(QDate::currentDate()); date_->setObjectName("hc.selection.date"); date_->setCalendarPopup(true);
        auto* DateRow = new QHBoxLayout; DateRow->addWidget(useDate_); DateRow->addWidget(date_, 1); AdvancedLayout->addRow(Text("수정일"), DateRow);
        tabs_->addTab(Advanced, Text("고급")); BodyLayout()->addWidget(tabs_);
        auto* Options = new QHBoxLayout;
        scope_ = combo("hc.selection.scope", {Text("파일만"), Text("폴더만"), Text("파일과 폴더")}); Options->addWidget(new QLabel(Text("항목 종류"))); Options->addWidget(scope_);
        case_ = new QCheckBox(Text("대소문자 구분")); case_->setObjectName("hc.selection.case"); Options->addWidget(case_); Options->addStretch(); BodyLayout()->addLayout(Options);
        auto* ScopeHint = new QLabel(Text("현재 목록 전체 · 화면 밖 행 포함 · 하위 폴더 탐색 없음 · .. 제외"));
        ScopeHint->setWordWrap(true); BodyLayout()->addWidget(ScopeHint);
        auto* PresetRow = new QHBoxLayout; presets_ = new QComboBox; presets_->setObjectName("hc.selection.presets");
        auto* Save = new QPushButton(Text("조건 저장…")); Save->setObjectName("hc.selection.savePreset"); Save->setAutoDefault(false);
        auto* Delete = new QPushButton(Text("조건 삭제")); Delete->setObjectName("hc.selection.deletePreset"); Delete->setAutoDefault(false);
        PresetRow->addWidget(presets_, 1); PresetRow->addWidget(Save); PresetRow->addWidget(Delete); BodyLayout()->addLayout(PresetRow);
        preview_ = new QLabel; preview_->setObjectName("hc.selection.preview"); preview_->setTextFormat(Qt::PlainText); preview_->setWordWrap(true);
        preview_->setTextInteractionFlags(Qt::TextSelectableByMouse); preview_->setAccessibleName(Text("선택 결과 미리보기")); BodyLayout()->addWidget(preview_);
        auto* Footer = qobject_cast<QHBoxLayout*>(AcceptButton()->parentWidget()->layout());
        apply_ = new fm::ui::Button(Text("적용(&A)")); apply_->setObjectName("hc.selection.apply"); apply_->setAutoDefault(false);
        undo_ = new fm::ui::Button(Text("이전 선택 복원")); undo_->setObjectName("hc.selection.undo"); undo_->setAutoDefault(false);
        Footer->insertWidget(0, undo_); Footer->insertWidget(Footer->count() - 1, apply_);
        debounce_.setSingleShot(true); debounce_.setInterval(180);
        connect(&debounce_, &QTimer::timeout, this, [this] { if (changed_) changed_(Read()); });
        for (auto* Edit : {pattern_, exclude_, size_}) connect(Edit, &QLineEdit::textChanged, this, [this] { schedule(); });
        connect(pattern_, &QLineEdit::textEdited, this, [this] { group_->setCurrentIndex(0); });
        for (auto* Combo : {operation_, syntax_, combine_, sizeOp_, scope_}) connect(Combo, &QComboBox::currentIndexChanged, this, [this] { schedule(); });
        connect(syntax_, &QComboBox::currentIndexChanged, this, [this] { if (!loading_ && group_->currentIndex()) group_->setCurrentIndex(0); });
        connect(group_, &QComboBox::currentIndexChanged, this, [this] {
            if (loading_) return;
            const auto G = selectedGroup(); loading_ = true;
            if (G) {
                pattern_->setText("*"); syntax_->setCurrentIndex(0); scope_->setCurrentIndex(2);
            }
            pattern_->setReadOnly(G.has_value()); syntax_->setEnabled(!G); loading_ = false; schedule();
        });
        connect(tabs_, &QTabWidget::currentChanged, this, [this] { schedule(); });
        for (auto* Check : {case_, useDate_}) connect(Check, &QCheckBox::toggled, this, [this] { schedule(); });
        connect(date_, &QDateEdit::dateChanged, this, [this] { schedule(); });
        connect(apply_, &QPushButton::clicked, this, [this] { if (applyAction_) applyAction_(false); });
        connect(undo_, &QPushButton::clicked, this, [this] { if (undoAction_) undoAction_(); });
        connect(presets_, &QComboBox::activated, this, [this](int Index) {
            if (Index > 0) Load(settings_["presets"].toArray().at(Index - 1).toObject()["criteria"].toObject());
        });
        connect(Save, &QPushButton::clicked, this, [this] {
            bool Ok = false; const auto Name = QInputDialog::getText(this, Text("선택 조건 저장"), Text("조건 이름"), QLineEdit::Normal, {}, &Ok).trimmed();
            if (!Ok || Name.isEmpty()) return;
            auto Items = settings_["presets"].toArray(); int Existing = -1;
            for (int I = 0; I < Items.size(); ++I) if (Items[I].toObject()["name"].toString() == Name) Existing = I;
            const QJsonObject Item{{"name", Name.left(80)}, {"criteria", Read().Save()}};
            if (Existing >= 0) {
                if (QMessageBox::question(this, Text("선택 조건 저장"), Text("같은 이름의 조건을 교체하시겠습니까?")) != QMessageBox::Yes) return;
                Items[Existing] = Item;
            } else if (Items.size() < 64) Items.append(Item);
            else { SetMessage(Text("저장한 조건은 최대 64개입니다."), true); return; }
            settings_["presets"] = Items; refreshPresets();
        });
        connect(Delete, &QPushButton::clicked, this, [this] {
            const int Index = presets_->currentIndex() - 1; if (Index < 0) return;
            auto Items = settings_["presets"].toArray(); Items.removeAt(Index); settings_["presets"] = Items; refreshPresets();
        });
        refreshPresets(); Load(Settings["last"].toObject()); operation_->setCurrentIndex(Remove ? 1 : 0);
        SetInitialInput(pattern_); FitToContents();
    }
    Criteria Read() const {
        Criteria C; C.operation_ = Operation(operation_->currentIndex()); C.syntax_ = Syntax(syntax_->currentIndex());
        C.scope_ = Scope(scope_->currentIndex()); C.pattern_ = pattern_->text(); C.exclude_ = exclude_->text();
        C.groupId_ = group_->currentData().toString(); if (const auto G = selectedGroup()) C.group_ = G->rule_;
        C.caseSensitive_ = case_->isChecked(); C.advanced_ = tabs_->currentIndex() == 1; C.all_ = combine_->currentIndex() == 0;
        C.size_ = size_->text(); C.minimumSize_ = sizeOp_->currentIndex() == 0;
        if (useDate_->isChecked()) C.modified_ = date_->date(); return C;
    }
    QJsonObject Settings() const { auto Result = settings_; Result["last"] = Read().Save(); return Result; }
    void Result(const QString& Summary, bool CanApply, bool CanUndo, const QString& Error = {}) {
        preview_->setText(Summary); SetMessage(Error, !Error.isEmpty()); SetAcceptEnabled(CanApply); apply_->setEnabled(CanApply); undo_->setEnabled(CanUndo);
        const bool Remove = operation_->currentIndex() == 1;
        SetTitle(Remove ? Text("항목 선택 해제") : Text("항목 선택")); AcceptButton()->setText(Remove ? Text("해제 후 닫기(&D)") : Text("선택 후 닫기(&S)"));
        if (isVisible()) FitToContents();
    }
    std::function<void(const Criteria&)> changed_;
    std::function<void()> invalidated_;
    std::function<void(bool)> applyAction_;
    std::function<void()> undoAction_;
protected:
    void accept() override { if (AcceptButton()->isEnabled() && applyAction_) applyAction_(true); }
private:
    QComboBox* combo(const char* Name, const QStringList& Values) {
        auto* Combo = new QComboBox; Combo->setObjectName(Name); Combo->addItems(Values); Combo->setMinimumContentsLength(8);
        Combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon); return Combo;
    }
    std::optional<Group> selectedGroup() const {
        for (const auto& G : groups_) if (G.id_ == group_->currentData().toString()) return G; return {};
    }
    void Load(const QJsonObject& Value) {
        loading_ = true; const auto C = Criteria::Load(Value);
        operation_->setCurrentIndex(int(C.operation_)); pattern_->setText(C.pattern_); exclude_->setText(C.exclude_);
        syntax_->setCurrentIndex(int(C.syntax_)); scope_->setCurrentIndex(int(C.scope_)); case_->setChecked(C.caseSensitive_);
        tabs_->setCurrentIndex(C.advanced_ ? 1 : 0); combine_->setCurrentIndex(C.all_ ? 0 : 1); size_->setText(C.size_);
        sizeOp_->setCurrentIndex(C.minimumSize_ ? 0 : 1); useDate_->setChecked(C.modified_.isValid());
        if (C.modified_.isValid()) date_->setDate(C.modified_);
        auto GroupIndex = group_->findData(C.groupId_);
        if (GroupIndex < 0 && !C.groupId_.isEmpty()) { group_->addItem(Text("사용할 수 없는 그룹"), C.groupId_); GroupIndex = group_->count() - 1; }
        group_->setCurrentIndex(qMax(0, GroupIndex)); const auto G = selectedGroup();
        pattern_->setReadOnly(G.has_value()); syntax_->setEnabled(!G); loading_ = false; schedule();
    }
    void schedule() {
        if (loading_) return;
        tabs_->setFixedHeight(tabs_->tabBar()->sizeHint().height() + tabs_->currentWidget()->sizeHint().height() + 8);
        if (invalidated_) invalidated_(); Result(Text("일치 항목 확인 중…"), false, false); debounce_.start();
    }
    void refreshPresets() {
        presets_->clear(); presets_->addItem(Text("저장한 조건 선택…"));
        for (const auto& I : settings_["presets"].toArray()) presets_->addItem(I.toObject()["name"].toString());
    }
    QList<Group> groups_; QJsonObject settings_; bool loading_ = false; QTimer debounce_;
    QComboBox *operation_, *group_, *syntax_, *combine_, *sizeOp_, *scope_, *presets_;
    QLineEdit *pattern_, *exclude_, *size_; QCheckBox *case_, *useDate_; QDateEdit* date_; QTabWidget* tabs_;
    QLabel* preview_; fm::ui::Button *apply_, *undo_;
};

struct RowInput { settings::FileRuleInput rule_; QString key_; int row_ = -1; bool selected_ = false; };
struct Scan {
    Criteria criteria_; Matcher matcher_; quint64 generation_ = 0, selection_ = 0;
    qint64 referenceTime_ = QDateTime::currentMSecsSinceEpoch();
    int offset_ = 0, rows_ = 0, matched_ = 0, added_ = 0, removed_ = 0, total_ = 0;
    QBitArray before_, after_; QStringList beforeKeys_, names_; bool ready_ = false;
    QString command_, location_; QSet<QString> restoreKeys_;
};
}

struct ItemSelectionController::State {
    ItemSelectionController* owner_; QPointer<CGridView> view_; QPointer<CFsModelT> model_; QPointer<SelectionDialog> dialog_;
    quint64 generation_ = 0, selection_ = 0; bool applying_ = false;
    std::shared_ptr<Scan> scan_; QStringList previous_, remembered_;
    QString historyLocation_, dialogLocation_; bool hasPrevious_ = false, hasRemembered_ = false;
    QString location() const { return model_ ? model_->GetCurrentPathWithRoot() : QString{}; }
    QString locationKey() const {
        const auto* Native = qobject_cast<CFSModel*>(model_.data());
        return Native ? workspace::LocationKey(Native->WorkspaceLocation()) : location();
    }
    bool loading() const {
        if (!view_ || !model_ || view_->BaseModel() != model_ || model_->IsLoading()) return true;
        const auto* Native = qobject_cast<CFSModel*>(model_.data()); return Native && Native->IsPageLoading();
    }
    bool valid(const std::shared_ptr<Scan>& Scan) const {
        return scan_ == Scan && view_ && !loading() && Scan->generation_ == generation_ && Scan->selection_ == selection_
            && Scan->rows_ == view_->BaseView()->getRowCount() && Scan->location_ == locationKey();
    }
    void message(const QString& Text) {
        if (auto* Window = qobject_cast<QMainWindow*>(view_ ? view_->window() : nullptr)) Window->statusBar()->showMessage(Text, 8000);
    }
    void invalidate() {
        ++generation_; scan_.reset();
        if (dialog_) dialog_->Result(Text("목록이 변경되었습니다. 조건을 변경하여 다시 확인하십시오."), false, false);
        if (historyLocation_ != locationKey()) { previous_.clear(); remembered_.clear(); hasPrevious_ = hasRemembered_ = false; historyLocation_ = locationKey(); }
    }
    void start(Criteria Criteria, const QString& Command = {}) {
        scan_.reset();
        if (dialog_ && dialogLocation_ != locationKey()) {
            dialog_->Result({}, false, false, Text("경로가 변경되었습니다. 창을 닫고 다시 실행하십시오.")); return;
        }
        if (loading()) { message(Text("파일 목록을 읽는 중에는 선택을 변경할 수 없습니다.")); return; }
        auto Work = std::make_shared<Scan>(); Work->criteria_ = Criteria; Work->command_ = Command;
        const auto Error = Work->matcher_.Compile(Criteria);
        if (!Error.isEmpty()) { if (dialog_) dialog_->Result({}, false, hasPrevious_, Error); else message(Error); return; }
        Work->generation_ = generation_; Work->selection_ = selection_; Work->rows_ = view_->BaseView()->getRowCount(); Work->location_ = locationKey();
        Work->before_.resize(Work->rows_); Work->after_.resize(Work->rows_);
        if (Command == "restore") Work->restoreKeys_ = QSet<QString>(previous_.begin(), previous_.end());
        if (Command == "recall") Work->restoreKeys_ = QSet<QString>(remembered_.begin(), remembered_.end());
        scan_ = Work; if (dialog_) dialog_->Result(Text("일치 항목 확인 중…"), false, hasPrevious_);
        QTimer::singleShot(0, owner_, [this, Work] { step(Work); });
    }
    void step(const std::shared_ptr<Scan>& Work) {
        if (!valid(Work)) return;
        QList<RowInput> Batch; QElapsedTimer Budget; Budget.start(); auto* Grid = view_->BaseView();
        while (Work->offset_ < Work->rows_ && Batch.size() < 128 && Budget.elapsed() < 8) {
            const int At = Work->offset_++; const auto Row = Grid->getRow(At);
            auto Index = Row.modelIndex(0); if (!Index.isValid()) continue;
            while (const auto* Proxy = qobject_cast<const QAbstractProxyModel*>(Index.model())) Index = Proxy->mapToSource(Index);
            const auto Item = view_->BaseModel()->GetFileInfo(Index);
            if (!Item || Item->Name_ == "..") continue;
            RowInput Input; Input.rule_ = {Item->Name_, Item->Attributes_, quint64(qMax<qint64>(0, Item->Size_)), false};
            Input.rule_.referenceUtcMs_ = Work->referenceTime_;
            Input.rule_.modifiedUtcMs_ = Item->Modified_.isValid() ? std::optional<qint64>(Item->Modified_.toMSecsSinceEpoch()) : std::nullopt;
            if (Item->storageEntry_) {
                Input.key_ = workspace::SelectionKey(*Item->storageEntry_);
                Input.rule_.size_ = Item->storageEntry_->size_;
                Input.rule_.modifiedUtcMs_ = settings::FileColorRules::MetadataTime(Item->storageEntry_->modifiedUtcMs_);
            } else Input.key_ = "legacy:" + Item->Name_;
            if (Item->Reserved0_) Input.rule_.reparseTag_ = Item->Reserved0_;
            Input.row_ = At; Input.selected_ = Grid->modelController()->isRowSelected(Row); Batch.append(std::move(Input));
        }
        auto* Watcher = new QFutureWatcher<QList<bool>>(owner_);
        QObject::connect(Watcher, &QFutureWatcherBase::finished, owner_, [this, Work, Watcher, Batch] {
            const auto Matches = Watcher->result(); Watcher->deleteLater(); if (!valid(Work)) return;
            for (qsizetype I = 0; I < Batch.size(); ++I) {
                const auto& Input = Batch[I]; bool Match = Matches[I]; bool After;
                if (Work->command_ == "invertAll") Match = !Input.selected_;
                if (Work->command_ == "restore" || Work->command_ == "recall") Match = Work->restoreKeys_.contains(Input.key_);
                After = Matcher::Selected(Work->criteria_.operation_, Input.selected_, Match);
                Work->before_.setBit(Input.row_, Input.selected_); Work->after_.setBit(Input.row_, After);
                if (Input.selected_) Work->beforeKeys_.append(Input.key_);
                Work->matched_ += Match; Work->added_ += After && !Input.selected_; Work->removed_ += !After && Input.selected_; Work->total_ += After;
                if (Match && Work->names_.size() < 12) Work->names_.append(Input.rule_.name_);
            }
            if (Work->offset_ < Work->rows_) { QTimer::singleShot(0, owner_, [this, Work] { step(Work); }); return; }
            Work->ready_ = true;
            if (Work->command_ == "remember") { remembered_ = Work->beforeKeys_; hasRemembered_ = true; historyLocation_ = locationKey(); message(Text("현재 선택을 기억하였습니다.")); scan_.reset(); return; }
            if (!Work->command_.isEmpty()) { apply(false); return; }
            updatePreview();
        });
        Watcher->setFuture(QtConcurrent::run([Matcher = Work->matcher_, Batch] {
            QList<bool> Result; Result.reserve(Batch.size()); for (const auto& Input : Batch) Result.append(Matcher.Matches(Input.rule_)); return Result;
        }));
    }
    void updatePreview() {
        const auto Work = scan_; if (!dialog_ || !Work || !valid(Work) || !Work->ready_) return;
        if (Work->criteria_.Save() != dialog_->Read().Save()) return;
        QString Summary = Text("일치 %1개 · 현재 선택 %2개\n%3\n추가 %4개 · 해제 %5개 → 적용 후 선택 %6개")
            .arg(Work->matched_).arg(Work->beforeKeys_.size()).arg(Work->names_.join(" · ")).arg(Work->added_).arg(Work->removed_).arg(Work->total_);
        if (!Work->total_ && !Work->beforeKeys_.isEmpty()) Summary += "\n" + Text("현재 선택이 모두 해제됩니다.");
        dialog_->Result(Summary, Work->added_ + Work->removed_ > 0, hasPrevious_);
    }
    void apply(bool Close) {
        auto Work = scan_; if (!Work || !Work->ready_ || !valid(Work)) return;
        // A changed input invalidates the preview before its debounce expires.
        if (Work->command_.isEmpty() && dialog_ && Work->criteria_.Save() != dialog_->Read().Save()) return;
        if (!Work->added_ && !Work->removed_) { message(Text("선택 변경 없음")); return; }
        auto* Grid = view_->BaseView(); applying_ = true; Grid->beginUpdate(); Grid->deselectAll();
        for (int At = 0; At < Work->after_.size();) {
            if (!Work->after_.testBit(At)) { ++At; continue; }
            const int First = At; while (At < Work->after_.size() && Work->after_.testBit(At)) ++At;
            // IgnoreSelection preserves commander-style cursor input, but its
            // selectRowRange helper truncates ranges to one row. Use the
            // explicit range API without changing the user's input policy.
            const auto* FirstColumn = Grid->getFirstColumn(); const auto* LastColumn = Grid->getLastColumn();
            Grid->selectRange(First, At - 1, FirstColumn ? FirstColumn->index() : -1, LastColumn ? LastColumn->index() : -1, Qtitan::Select);
        }
        Grid->endUpdate(); view_->BaseModel()->setProperty("hc.selectAllEntries", false); applying_ = false; ++selection_;
        const bool Restore = Work->command_ == "restore";
        previous_ = Work->beforeKeys_; hasPrevious_ = !Restore; historyLocation_ = locationKey();
        emit view_->sigThumbnailSelectionChanged(); view_->SyncThumbnailCursor();
        message(Text("선택 변경: 추가 %1개 · 해제 %2개 · 최종 %3개").arg(Work->added_).arg(Work->removed_).arg(Work->total_));
        if (Work->command_ == "restore" || Work->command_ == "recall") {
            const auto Missing = Work->restoreKeys_.size() - Work->total_;
            if (Missing > 0) message(Text("선택을 복원하였습니다. 현재 목록에 없는 %1개는 제외하였습니다.").arg(Missing));
        }
        scan_.reset();
        if (dialog_) { if (Close) dialog_->done(QDialog::Accepted); else start(dialog_->Read()); }
    }
    void execute(QString Command) {
        if (view_ && view_->IsEditing()) { message(Text("이름 편집을 마친 뒤 선택을 변경하십시오.")); return; }
        if (loading()) { message(Text("파일 목록을 읽는 중에는 선택을 변경할 수 없습니다.")); return; }
        if (historyLocation_ != locationKey()) { previous_.clear(); remembered_.clear(); hasPrevious_ = hasRemembered_ = false; historyLocation_ = locationKey(); }
        if (dialog_) { dialog_->raise(); dialog_->activateWindow(); return; }
        if (Command == "select" || Command == "deselect") {
            auto& Settings = settings::AppSettings();
            const auto Config = Settings.Read(settings::Section::General).toObject()["selectionDialog"].toObject();
            const auto Rules = Settings.Read(settings::Section::Options).toObject()["fileColorRules"].toObject();
            SelectionDialog Dialog(Groups(Rules), Config, Command == "deselect", view_->window()); dialog_ = &Dialog; dialogLocation_ = locationKey();
            Dialog.SetSummary(location());
            Dialog.changed_ = [this](const Criteria& C) { start(C); };
            Dialog.invalidated_ = [this] { scan_.reset(); };
            Dialog.applyAction_ = [this](bool Close) { apply(Close); };
            Dialog.undoAction_ = [this] { if (hasPrevious_) { Criteria C; C.scope_ = Scope::Both; C.operation_ = Operation::Replace; start(C, "restore"); } };
            const QPointer<ItemSelectionController> Guard(owner_);
            QObject::connect(owner_, &QObject::destroyed, &Dialog, &QDialog::reject);
            start(Dialog.Read()); Dialog.exec();
            if (!Guard) return;
            scan_.reset(); dialog_ = nullptr;
            auto General = Settings.Read(settings::Section::General).toObject(); General["selectionDialog"] = Dialog.Settings();
            if (!Settings.Put(settings::Section::General, General)) message(Text("선택 조건 설정을 저장할 수 없습니다."));
            if (view_ && model_) view_->EnsureKeyboardFocusOnView(); return;
        }
        if ((Command == "restore" && !hasPrevious_) || (Command == "recall" && !hasRemembered_)) { message(Text("이 위치에 복원할 선택이 없습니다.")); return; }
        Criteria C; C.scope_ = Scope::Both;
        if (Command == "all") C.operation_ = Operation::Replace;
        else if (Command == "clear") C.operation_ = Operation::Remove;
        else if (Command == "invertAll" || Command == "restore" || Command == "recall") C.operation_ = Operation::Replace;
        else if (Command == "sameExtension" || Command == "clearExtension") {
            const auto Focus = view_->BaseView()->focusedRow(); auto Index = Focus.isValid() ? Focus.modelIndex(0) : QModelIndex{};
            while (const auto* Proxy = qobject_cast<const QAbstractProxyModel*>(Index.model())) Index = Proxy->mapToSource(Index);
            const auto Item = Index.isValid() ? view_->BaseModel()->GetFileInfo(Index) : nsHC::TySpFileSource{};
            if (!Item || Item->Name_ == ".." || (Item->Attributes_ & 0x10)) { message(Text("파일 항목에 커서를 둔 뒤 실행하십시오.")); return; }
            C.scope_ = Scope::Files; C.syntax_ = Syntax::Regex; const auto Ext = QFileInfo(Item->Name_).suffix();
            C.pattern_ = Ext.isEmpty() ? "^[^.]*$" : "\\." + QRegularExpression::escape(Ext) + "$";
            C.operation_ = Command == "sameExtension" ? Operation::Add : Operation::Remove;
        }
        start(C, Command);
    }
};

ItemSelectionController* ItemSelectionController::For(CGridView* View) {
    if (!View) return nullptr;
    if (auto* Existing = View->findChild<QObject*>("hc.selection.controller", Qt::FindDirectChildrenOnly)) return static_cast<ItemSelectionController*>(Existing);
    return new ItemSelectionController(View);
}
ItemSelectionController::ItemSelectionController(CGridView* View) : QObject(View), state_(std::make_unique<State>()) {
    setObjectName("hc.selection.controller"); state_->owner_ = this; state_->view_ = View;
    auto* Model = View->BaseModel(); state_->model_ = Model;
    connect(Model, &QObject::destroyed, this, [this] {
        state_->scan_.reset(); if (state_->dialog_) state_->dialog_->reject();
    });
    connect(Model, &QAbstractItemModel::modelAboutToBeReset, this, [this] { state_->invalidate(); });
    connect(Model, &QAbstractItemModel::layoutAboutToBeChanged, this, [this] { state_->invalidate(); });
    connect(Model, &QAbstractItemModel::rowsAboutToBeInserted, this, [this] { state_->invalidate(); });
    connect(Model, &QAbstractItemModel::rowsAboutToBeRemoved, this, [this] { state_->invalidate(); });
    connect(View->BaseView(), &Qtitan::GridViewBase::sortingChanged, this, [this] { state_->invalidate(); });
    connect(View->BaseView(), &Qtitan::GridViewBase::selectionChanged, this, [this] {
        if (state_->applying_) return; ++state_->selection_; state_->scan_.reset();
        if (state_->dialog_) state_->start(state_->dialog_->Read());
    });
}
ItemSelectionController::~ItemSelectionController() = default;
void ItemSelectionController::Execute(const QString& Command) { state_->execute(Command.section('.', 2)); }
} // namespace hc::ui
