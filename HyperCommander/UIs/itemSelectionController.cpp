#include "stdafx.h"
#include "itemSelectionController.hpp"
#include "UniqueLibs/qtMessageDialog.hpp"

/// UIs
#include "Components/cmpView.hpp"
#include "dialogShell.hpp"
#include "fileColorConditionRow.hpp"

/// 내부 라이브러리
#include "UniqueLibs/itemSelection.hpp"
#include "UniqueLibs/columnPluginFields.hpp"
#include "commonLibs/columns/columnQueryEngine.hpp"
#include "commonLibs/Qt/elidedLabel.hpp"
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
        auto* SizeRow = new QHBoxLayout; SizeRow->addWidget(sizeOp_); SizeRow->addWidget(size_, 1); AdvancedLayout->addRow(Text("크기 (폴더는 계산된 크기)"), SizeRow);
        useDate_ = new QCheckBox(Text("기준일 이후 (포함)")); useDate_->setObjectName("hc.selection.useDate");
        date_ = new QDateEdit(QDate::currentDate()); date_->setObjectName("hc.selection.date"); date_->setCalendarPopup(true);
        auto* DateRow = new QHBoxLayout; DateRow->addWidget(useDate_); DateRow->addWidget(date_, 1); AdvancedLayout->addRow(Text("수정일"), DateRow);
        // Design 8 E: the file color condition table, column fields included. Its result joins the
        // terms above with the same AND/OR choice.
        conditions_ = new QTableWidget(0, 4); conditions_->setObjectName("hc.selection.conditions"); conditions_->verticalHeader()->hide();
        conditions_->setHorizontalHeaderLabels({Text("항목"), Text("비교 방법"), Text("값"), Text("삭제")});
        conditions_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch); conditions_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Fixed);
        conditions_->setColumnWidth(0, 150); conditions_->setColumnWidth(3, 40); conditions_->setEditTriggers(QAbstractItemView::NoEditTriggers);
        conditions_->setSelectionBehavior(QAbstractItemView::SelectRows); conditions_->setAccessibleName(Text("추가 조건"));
        auto* AddCondition = new QPushButton(Text("조건 추가")); AddCondition->setObjectName("hc.selection.addCondition"); AddCondition->setAutoDefault(false);
        AdvancedLayout->addRow(Text("추가 조건"), conditions_); AdvancedLayout->addRow(QString{}, AddCondition);
        connect(AddCondition, &QPushButton::clicked, this, [this] {
            if (rows_.size() >= 32) return;
            appendCondition(settings::FileColorRules::NormalizeCondition({{"type", "extension"}, {"value", "txt"}}, 1)); schedule();
        });
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
        preview_ = new hc::qt::ElidedLabel; static_cast<hc::qt::ElidedLabel*>(preview_)->SetLineCount(4); preview_->setObjectName("hc.selection.preview"); preview_->setTextFormat(Qt::PlainText); preview_->setWordWrap(true);
        preview_->setTextInteractionFlags(Qt::TextSelectableByMouse); preview_->setAccessibleName(Text("선택 결과 미리보기")); BodyLayout()->addWidget(preview_);
        // Field value checks: "조회 시작" past the large-scan gate, "중지" while reading (design 7.3).
        auto* QueryRow = new QHBoxLayout; QueryRow->addStretch(); BodyLayout()->addLayout(QueryRow);
        query_ = new fm::ui::Button(Text("중지")); query_->setObjectName("hc.selection.query"); query_->setAutoDefault(false); query_->hide(); QueryRow->addWidget(query_);
        connect(query_, &QPushButton::clicked, this, [this] { if (queryAction_) queryAction_(); });
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
            bool Ok = false; const auto Name = hc::ui::InputDialog::getText(this, Text("선택 조건 저장"), Text("조건 이름"), QLineEdit::Normal, {}, &Ok).trimmed();
            if (!Ok || Name.isEmpty()) return;
            auto Items = settings_["presets"].toArray(); int Existing = -1;
            for (int I = 0; I < Items.size(); ++I) if (Items[I].toObject()["name"].toString() == Name) Existing = I;
            const QJsonObject Item{{"name", Name.left(80)}, {"criteria", Read().Save()}};
            if (Existing >= 0) {
                if (hc::ui::MessageDialog::question(this, Text("선택 조건 저장"), Text("같은 이름의 조건을 교체하시겠습니까?")) != QMessageBox::Yes) return;
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
        C.groupId_ = group_->currentData().toString(); if (const auto G = selectedGroup()) { C.group_ = G->rule_; C.groupVersion_ = G->version_; }
        C.caseSensitive_ = case_->isChecked(); C.advanced_ = tabs_->currentIndex() == 1; C.all_ = combine_->currentIndex() == 0;
        C.size_ = size_->text(); C.minimumSize_ = sizeOp_->currentIndex() == 0;
        for (auto* Row : rows_) C.conditions_.append(Row->Record());
        if (useDate_->isChecked()) C.modified_ = date_->date(); return C;
    }
    // A field check is waiting for the user (Button "조회 시작") or running (Button "중지").
    void Querying(const QString& Summary, const QString& Button) {
        if (preview_->text() != Summary) { preview_->setText(Summary); preview_->setToolTip(Summary); }
        query_->setText(Button); query_->show(); SetAcceptEnabled(false); apply_->setEnabled(false);
    }
    QJsonObject Settings() const { auto Result = settings_; Result["last"] = Read().Save(); return Result; }
    void Pending(bool Background) {
        // An explicit foreground scan supersedes the queued input refresh.
        if (!Background) debounce_.stop();
        if (!Background) { query_->hide(); queryAction_ = {}; }
        // A refresh invalidates row indexes, not the last confirmed result.
        // Keep its text and actions steady until a complete replacement is ready.
        if (!hasResult_ && preview_->text().isEmpty()) preview_->setText(Text("일치 항목 확인 중…"));
        if (!Background) { SetAcceptEnabled(false); apply_->setEnabled(false); }
    }
    void Result(const QString& Summary, bool CanApply, bool CanUndo, const QString& Error = {}) {
        query_->hide();
        hasResult_ = Error.isEmpty() && !Summary.isEmpty();
        if (preview_->text() != Summary) { preview_->setText(Summary); preview_->setToolTip(Summary); }
        if (error_ != Error) { error_ = Error; SetMessage(Error, !Error.isEmpty()); }
        SetAcceptEnabled(CanApply); apply_->setEnabled(CanApply); undo_->setEnabled(CanUndo);
        const bool Remove = operation_->currentIndex() == 1;
        SetTitle(Remove ? Text("항목 선택 해제") : Text("항목 선택")); AcceptButton()->setText(Remove ? Text("해제 후 닫기(&D)") : Text("선택 후 닫기(&S)"));
    }
    std::function<void(const Criteria&)> changed_;
    std::function<void()> invalidated_;
    std::function<void(bool)> applyAction_;
    std::function<void()> undoAction_, queryAction_;
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
        qDeleteAll(rows_); rows_.clear(); conditions_->setRowCount(0);
        for (const auto Value : C.conditions_) appendCondition(Value.toObject());
        auto GroupIndex = group_->findData(C.groupId_);
        if (GroupIndex < 0 && !C.groupId_.isEmpty()) { group_->addItem(Text("사용할 수 없는 그룹"), C.groupId_); GroupIndex = group_->count() - 1; }
        group_->setCurrentIndex(qMax(0, GroupIndex)); const auto G = selectedGroup();
        pattern_->setReadOnly(G.has_value()); syntax_->setEnabled(!G); loading_ = false; schedule();
    }
    void schedule() {
        if (loading_) return;
        tabs_->setFixedHeight(tabs_->tabBar()->sizeHint().height() + tabs_->currentWidget()->sizeHint().height() + 8);
        if (invalidated_) invalidated_(); Pending(false); debounce_.start();
    }
    void refreshPresets() {
        presets_->clear(); presets_->addItem(Text("저장한 조건 선택…"));
        for (const auto& I : settings_["presets"].toArray()) presets_->addItem(I.toObject()["name"].toString());
    }
    void appendCondition(const QJsonObject& Record) {
        auto* Row = new FileColorConditionRow(conditions_, Record, 4, catalog_); rows_.append(Row);
        Row->onEdited_ = [this] { if (!loading_) schedule(); };
        Row->onRemove_ = [this, Row] {
            if (Row->Row() < 0) return; conditions_->removeRow(Row->Row()); rows_.removeAll(Row); Row->deleteLater(); updateConditionHeight(); schedule();
        };
        updateConditionHeight();
    }
    void updateConditionHeight() {
        int Height = conditions_->horizontalHeader()->sizeHint().height() + 2 * conditions_->frameWidth();
        if (!conditions_->rowCount()) Height += conditions_->verticalHeader()->defaultSectionSize();
        for (int I = 0; I < qMin(conditions_->rowCount(), 4); ++I) Height += conditions_->rowHeight(I);
        conditions_->setFixedHeight(Height);
    }
    QList<Group> groups_; QJsonObject settings_; bool loading_ = false, hasResult_ = false; QString error_; QTimer debounce_;
    QTableWidget* conditions_ = nullptr; QList<FileColorConditionRow*> rows_; fm::ui::Button* query_ = nullptr;
    QVector<hc::columns::FieldDefinition> catalog_ = hc::columns::PluginFields::Instance().Fields();
    QComboBox *operation_, *group_, *syntax_, *combine_, *sizeOp_, *scope_, *presets_;
    QLineEdit *pattern_, *exclude_, *size_; QCheckBox *case_, *useDate_; QDateEdit* date_; QTabWidget* tabs_;
    QLabel* preview_; fm::ui::Button *apply_, *undo_;
};

struct RowInput {
    settings::FileRuleInput rule_; QString key_; int row_ = -1; bool selected_ = false, sizeKnown_ = true;
    FieldValueMap fields_; QString path_; QStringList needed_; bool done_ = false;
};
struct Scan {
    Criteria criteria_; Matcher matcher_; quint64 generation_ = 0, selection_ = 0;
    qint64 referenceTime_ = QDateTime::currentMSecsSinceEpoch();
    int offset_ = 0, rows_ = 0, matched_ = 0, added_ = 0, removed_ = 0, total_ = 0;
    QBitArray before_, after_; QStringList beforeKeys_, names_; bool ready_ = false;
    QString command_, location_; QSet<QString> restoreKeys_;
    bool grouped_ = false; int sourceRows_ = 0;
    QModelIndexList entries_; QStringList afterKeys_;
    std::shared_ptr<settings::FileColorRules::EvaluationContext> ruleContext_ = std::make_shared<settings::FileColorRules::EvaluationContext>();
    // Design 7.2: rows the list values could not decide, and their field check.
    QStringList required_; QList<RowInput> unknown_; QMultiHash<QString, int> byPath_;
    std::shared_ptr<hc::columns::QueryEngine> engine_;
    int next_ = 0, resolved_ = 0, unavailable_ = 0, unresolved_ = 0;
    bool confirmed_ = false, querying_ = false;
};
}

struct ItemSelectionController::State {
    ItemSelectionController* owner_; QPointer<CGridView> view_; QPointer<CFsModelT> model_; QPointer<SelectionDialog> dialog_;
    quint64 generation_ = 0, selection_ = 0; bool applying_ = false;
    QTimer refresh_; std::optional<bool> pendingApply_;
    std::shared_ptr<Scan> scan_; QStringList previous_, remembered_;
    QString historyLocation_, dialogLocation_; bool hasPrevious_ = false, hasRemembered_ = false;
    QString location() const { return model_ ? model_->GetCurrentPathWithRoot() : QString{}; }
    QString locationKey() const {
        const auto* Native = qobject_cast<CFSModel*>(model_.data());
        return Native ? workspace::LocationKey(Native->WorkspaceLocation()) : location();
    }
    bool loading() const {
        if (!view_ || !model_ || view_->BaseModel() != model_) return true;
        const auto* Native = qobject_cast<CFSModel*>(model_.data());
        return Native ? !Native->CanUseSnapshot() || Native->IsPageLoading() : model_->IsLoading();
    }
    bool valid(const std::shared_ptr<Scan>& Scan) const {
        return scan_ == Scan && view_ && !loading() && Scan->generation_ == generation_ && Scan->selection_ == selection_
            && Scan->grouped_ == view_->GroupingActive()
            && (Scan->grouped_ ? Scan->sourceRows_ == model_->rowCount() : Scan->rows_ == view_->BaseView()->getRowCount())
            && Scan->location_ == locationKey();
    }
    void message(const QString& Text) {
        if (auto* Window = qobject_cast<QMainWindow*>(view_ ? view_->window() : nullptr)) Window->statusBar()->showMessage(Text, 8000);
    }
    void invalidate() {
        ++generation_; scan_.reset();
        if (dialog_) {
            if (dialogLocation_ != locationKey()) {
                refresh_.stop(); pendingApply_.reset();
                dialog_->Result({}, false, false, Text("경로가 변경되었습니다. 창을 닫고 다시 실행하십시오."));
            } else queueRefresh();
        }
        if (historyLocation_ != locationKey()) { previous_.clear(); remembered_.clear(); hasPrevious_ = hasRemembered_ = false; historyLocation_ = locationKey(); }
    }
    void queueRefresh() {
        // Do not restart the timer on every event: a busy download folder must
        // still publish complete results, at most once per refresh interval.
        if (dialog_ && !refresh_.isActive()) refresh_.start();
    }
    void selectionChanged() {
        if (applying_) return;
        ++selection_; scan_.reset(); queueRefresh();
    }
    void requestApply(bool Close) {
        if (scan_ && scan_->ready_ && valid(scan_)) { apply(Close); return; }
        if (!dialog_) return;
        // The visible result can outlive its row indexes. Never apply those old
        // indexes; defer the click until the current snapshot has been evaluated.
        pendingApply_ = Close; start(dialog_->Read());
    }
    void start(Criteria Criteria, const QString& Command = {}, bool Background = false) {
        refresh_.stop(); scan_.reset();
        if (dialog_ && dialogLocation_ != locationKey()) {
            dialog_->Result({}, false, false, Text("경로가 변경되었습니다. 창을 닫고 다시 실행하십시오.")); return;
        }
        const auto* Native = qobject_cast<CFSModel*>(model_.data());
        const bool WaitingForSizes = Native && Criteria.advanced_ && !Criteria.size_.trimmed().isEmpty()
            && Criteria.scope_ != Scope::Files && Native->PendingDirectorySizes() > 0;
        if (loading() || WaitingForSizes) {
            if (dialog_) { dialog_->Pending(Background); queueRefresh(); }
            else message(Text("파일 목록을 읽는 중에는 선택을 변경할 수 없습니다."));
            return;
        }
        auto Work = std::make_shared<Scan>(); Work->criteria_ = Criteria; Work->command_ = Command;
        Work->generation_ = generation_; Work->selection_ = selection_; Work->rows_ = view_->BaseView()->getRowCount(); Work->location_ = locationKey();
        Work->grouped_ = view_->GroupingActive(); Work->sourceRows_ = model_->rowCount();
        if (Work->grouped_) { Work->entries_ = view_->SelectionCandidates(); Work->rows_ = Work->entries_.size(); }
        Work->before_.resize(Work->rows_); Work->after_.resize(Work->rows_);
        if (Command == "restore") Work->restoreKeys_ = QSet<QString>(previous_.begin(), previous_.end());
        if (Command == "recall") Work->restoreKeys_ = QSet<QString>(remembered_.begin(), remembered_.end());
        scan_ = Work; if (dialog_) dialog_->Pending(Background);
        using Prepared = QPair<Matcher, QString>;
        auto* Watcher = new QFutureWatcher<Prepared>(owner_);
        QObject::connect(Watcher, &QFutureWatcher<Prepared>::finished, owner_, [this, Work, Watcher] {
            auto Prepared = Watcher->future().takeResult(); Watcher->deleteLater(); if (!valid(Work)) return;
            if (!Prepared.second.isEmpty()) { if (dialog_) dialog_->Result({}, false, hasPrevious_, Prepared.second); else message(Prepared.second); return; }
            Work->matcher_ = std::move(Prepared.first); Work->required_ = Work->matcher_.RequiredFields(); step(Work);
        });
        Watcher->setFuture(QtConcurrent::run([Criteria] { Matcher Prepared; const auto Error = Prepared.Compile(Criteria); return QPair<Matcher, QString>{std::move(Prepared), Error}; }));
    }
    void step(const std::shared_ptr<Scan>& Work) {
        if (!valid(Work)) return;
        QList<RowInput> Batch; QElapsedTimer Budget; Budget.start(); auto* Grid = view_->BaseView();
        while (Work->offset_ < Work->rows_ && Batch.size() < 128 && Budget.elapsed() < 8) {
            const int At = Work->offset_++;
            const auto Row = Work->grouped_ ? Qtitan::GridRow{} : Grid->getRow(At);
            auto Index = Work->grouped_ ? Work->entries_[At] : Row.modelIndex(0); if (!Index.isValid()) continue;
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
            if (Item->Attributes_ & FILE_ATTRIBUTE_DIRECTORY) {
                Input.sizeKnown_ = Item->directorySize_.has_value();
                Input.rule_.size_ = Item->directorySize_.value_or(0);
            }
            if (Item->Reserved0_) Input.rule_.reparseTag_ = Item->Reserved0_;
            // Values already known (list values, cached field values) decide most rows here.
            if (const auto* Native = qobject_cast<CFSModel*>(model_.data()); Native && !Work->required_.isEmpty()) {
                Input.path_ = Native->FieldQueryPath(Item);
                for (const auto& Field : Work->required_) Input.fields_.insert(Field, Native->RuleFieldValue(Item, Field));
            }
            Input.row_ = At; Input.selected_ = Work->grouped_ ? view_->IsSourceSelected(Index) : Grid->modelController()->isRowSelected(Row); Batch.append(std::move(Input));
        }
        using BatchResult = QPair<QList<Verdict>, QString>;
        auto* Watcher = new QFutureWatcher<BatchResult>(owner_);
        QObject::connect(Watcher, &QFutureWatcherBase::finished, owner_, [this, Work, Watcher, Batch] {
            const auto Result = Watcher->result(); Watcher->deleteLater(); if (!valid(Work)) return;
            if (!Result.second.isEmpty()) { Work->ready_ = false; if (dialog_) dialog_->Result({}, false, hasPrevious_, Result.second); else message(Result.second); return; }
            const auto& Verdicts = Result.first;
            for (qsizetype I = 0; I < Batch.size(); ++I) {
                if (Verdicts[I].truth_ == hc::columns::Truth::Unknown) {
                    auto Row = Batch[I]; Row.needed_ = Verdicts[I].needed_; Work->unknown_.append(std::move(Row)); continue;
                }
                account(Work, Batch[I], Verdicts[I].truth_ == hc::columns::Truth::True);
            }
            if (Work->offset_ < Work->rows_) { QTimer::singleShot(0, owner_, [this, Work] { step(Work); }); return; }
            if (!Work->unknown_.isEmpty()) { beginQuery(Work); return; }
            finish(Work);
        });
        Watcher->setFuture(QtConcurrent::run([Matcher = Work->matcher_, Batch, Context = Work->ruleContext_] {
            BatchResult Result; Result.first.reserve(Batch.size());
            for (const auto& Input : Batch) {
                Result.first.append(Matcher.Evaluate(Input.rule_, Input.fields_, Context.get(), Input.sizeKnown_));
                if (!Result.first.back().error_.isEmpty()) { Result.second = Result.first.back().error_; break; }
            }
            return Result;
        }));
    }
    void account(const std::shared_ptr<Scan>& Work, const RowInput& Input, bool Match) {
        if (Work->command_ == "invertAll") Match = !Input.selected_;
        if (Work->command_ == "restore" || Work->command_ == "recall") Match = Work->restoreKeys_.contains(Input.key_);
        const bool After = Matcher::Selected(Work->criteria_.operation_, Input.selected_, Match);
        Work->before_.setBit(Input.row_, Input.selected_); Work->after_.setBit(Input.row_, After);
        if (Input.selected_) Work->beforeKeys_.append(Input.key_);
        if (After) Work->afterKeys_.append(Input.key_);
        Work->matched_ += Match; Work->added_ += After && !Input.selected_; Work->removed_ += !After && Input.selected_; Work->total_ += After;
        if (Match && Work->names_.size() < 12) Work->names_.append(Input.rule_.name_);
    }
    void finish(const std::shared_ptr<Scan>& Work) {
        Work->ready_ = true; Work->querying_ = false;
        // The engine may be the caller (its change callback); release it after the stack unwinds.
        if (Work->engine_) QTimer::singleShot(0, owner_, [Engine = std::exchange(Work->engine_, {})] {});
        if (Work->command_ == "remember") { remembered_ = Work->beforeKeys_; hasRemembered_ = true; historyLocation_ = locationKey(); message(Text("현재 선택을 기억하였습니다.")); scan_.reset(); return; }
        if (!Work->command_.isEmpty()) { apply(false); return; }
        updatePreview();
        if (pendingApply_ && dialog_ && Work->criteria_.Save() == dialog_->Read().Save()) {
            const bool Close = *pendingApply_; pendingApply_.reset(); apply(Close);
        }
    }
    // Design 7.3: past the threshold the check waits for "조회 시작"; it never starts on its own.
    void beginQuery(const std::shared_ptr<Scan>& Work) {
        if (!dialog_) { stopQuery(Work); return; }
        bool Plugin = false;
        for (const auto& Row : Work->unknown_) for (const auto& Field : Row.needed_) Plugin |= hc::columns::IsPluginField(Field);
        if (!Work->confirmed_ && Work->unknown_.size() > (Plugin ? 200 : 2000)) {
            const std::weak_ptr<Scan> Weak = Work;
            dialog_->queryAction_ = [this, Weak] { if (const auto W = Weak.lock(); W && valid(W)) { W->confirmed_ = true; beginQuery(W); } };
            dialog_->Querying(Text("%1개 항목의 필드 값을 조회해야 합니다.\n이름·크기 등 다른 조건으로 걸러진 뒤의 개수입니다. %2디스크에 따라 몇 분 걸릴 수 있으며, 조회 중에도 중지할 수 있습니다.")
                .arg(QLocale().toString(qsizetype(Work->unknown_.size())), Plugin ? Text("플러그인 필드는 파일 내용을 읽습니다. ") : QString{}), Text("조회 시작"));
            return;
        }
        Work->confirmed_ = true; Work->querying_ = true;
        for (int Index = 0; Index < Work->unknown_.size(); ++Index) Work->byPath_.insert(Work->unknown_[Index].path_, Index);
        Work->engine_ = std::make_shared<hc::columns::QueryEngine>(hc::columns::PluginFields::Instance().Loader(true));
        const std::weak_ptr<Scan> Weak = Work;
        Work->engine_->Changed = [this, Weak](const QSet<QString>& Paths) {
            const auto W = Weak.lock(); if (!W) return;
            resolve(W, Paths);
            QTimer::singleShot(0, owner_, [W] {}); // Never drop the last reference inside the engine's callback.
        };
        dialog_->queryAction_ = [this, Weak] { if (const auto W = Weak.lock(); W && valid(W)) stopQuery(W); };
        pump(Work); progress(Work);
        if (Work->resolved_ == Work->unknown_.size()) finish(Work);
    }
    void pump(const std::shared_ptr<Scan>& Work) {
        // Bounded in flight, well under the engine's own 4096 budget.
        while (Work->engine_ && Work->next_ < Work->unknown_.size() && Work->engine_->Pending() < 512) request(Work, Work->next_++);
    }
    void request(const std::shared_ptr<Scan>& Work, int Index) {
        auto& Row = Work->unknown_[Index]; bool Waiting = false;
        for (const auto& Field : Row.needed_) {
            if (Row.fields_.value(Field, hc::columns::CellValue::Missing(hc::columns::ValueState::Pending)).State() != hc::columns::ValueState::Pending) continue;
            const auto Value = Work->engine_->Read(Row.path_, Field, true, false);
            if (Value.State() == hc::columns::ValueState::Pending) Waiting = true;
            else Row.fields_[Field] = Value.State() == hc::columns::ValueState::OnDemand ? hc::columns::CellValue::Missing(hc::columns::ValueState::Unsupported, "on-demand") : Value;
        }
        if (!Waiting) reevaluate(Work, Index);
    }
    void resolve(const std::shared_ptr<Scan>& Work, const QSet<QString>& Paths) {
        if (!valid(Work) || !Work->querying_) return;
        for (const auto& Path : Paths) for (const int Index : Work->byPath_.values(Path)) {
            auto& Row = Work->unknown_[Index]; if (Row.done_) continue; bool Waiting = false;
            for (const auto& Field : Row.needed_) {
                if (Row.fields_.value(Field, hc::columns::CellValue::Missing(hc::columns::ValueState::Pending)).State() != hc::columns::ValueState::Pending) continue;
                const auto Value = Work->engine_->Read(Path, Field, false);
                if (Value.State() == hc::columns::ValueState::Pending) { Waiting = true; continue; }
                Row.fields_[Field] = Value.State() == hc::columns::ValueState::OnDemand ? hc::columns::CellValue::Missing(hc::columns::ValueState::Unsupported, "on-demand") : Value;
            }
            if (!Waiting) reevaluate(Work, Index);
        }
        pump(Work); progress(Work);
        if (Work->querying_ && Work->resolved_ == Work->unknown_.size()) finish(Work);
    }
    void reevaluate(const std::shared_ptr<Scan>& Work, int Index) {
        auto& Row = Work->unknown_[Index]; if (Row.done_) return;
        const auto Outcome = Work->matcher_.Evaluate(Row.rule_, Row.fields_, Work->ruleContext_.get(), Row.sizeKnown_);
        // A slower tier may be needed once the cheaper values are in.
        if (Outcome.truth_ == hc::columns::Truth::Unknown && Outcome.error_.isEmpty()) {
            bool Fresh = false; for (const auto& Field : Outcome.needed_) Fresh |= Row.fields_.value(Field, hc::columns::CellValue::Missing(hc::columns::ValueState::Pending)).State() == hc::columns::ValueState::Pending && !Row.needed_.contains(Field);
            if (Fresh) { Row.needed_ = Outcome.needed_; request(Work, Index); return; }
        }
        Row.done_ = true; ++Work->resolved_;
        for (const auto& Value : std::as_const(Row.fields_)) if (Value.State() == hc::columns::ValueState::Error || Value.State() == hc::columns::ValueState::Unsupported) { ++Work->unavailable_; break; }
        account(Work, Row, Outcome.truth_ == hc::columns::Truth::True);
        if (auto* Native = qobject_cast<CFSModel*>(model_.data())) {
            hc::columns::FieldValues Shared; for (auto It = Row.fields_.cbegin(); It != Row.fields_.cend(); ++It) Shared.insert(It.key(), It.value());
            Native->ShareRuleFieldValues(Row.path_, Shared);
        }
    }
    // Stopped rows count as not matching; the summary says how many (design 7.3).
    void stopQuery(const std::shared_ptr<Scan>& Work) {
        for (auto& Row : Work->unknown_) if (!Row.done_) { Row.done_ = true; ++Work->unresolved_; account(Work, Row, false); }
        Work->resolved_ = Work->unknown_.size(); finish(Work);
    }
    void progress(const std::shared_ptr<Scan>& Work) {
        if (!dialog_ || !Work->querying_) return;
        dialog_->Querying(Text("필드 값 확인 중 %1 / %2\n일치 %3개 · 확인 불가 %4개")
            .arg(QLocale().toString(Work->resolved_), QLocale().toString(qsizetype(Work->unknown_.size())))
            .arg(Work->matched_).arg(Work->unavailable_), Text("중지"));
    }
    void updatePreview() {
        const auto Work = scan_; if (!dialog_ || !Work || !valid(Work) || !Work->ready_) return;
        if (Work->criteria_.Save() != dialog_->Read().Save()) return;
        QString Summary = Text("일치 %1개 · 현재 선택 %2개\n%3\n추가 %4개 · 해제 %5개 → 적용 후 선택 %6개")
            .arg(Work->matched_).arg(Work->beforeKeys_.size()).arg(Work->names_.join(" · ")).arg(Work->added_).arg(Work->removed_).arg(Work->total_);
        if (!Work->total_ && !Work->beforeKeys_.isEmpty()) Summary += "\n" + Text("현재 선택이 모두 해제됩니다.");
        if (Work->unresolved_) Summary += "\n" + Text("중지됨 · 미확인 %1개는 일치하지 않음으로 처리했습니다.").arg(Work->unresolved_);
        if (Work->unavailable_) Summary += "\n" + Text("필드 값을 확인할 수 없는 %1개는 일치하지 않음으로 처리했습니다.").arg(Work->unavailable_);
        dialog_->Result(Summary, Work->added_ + Work->removed_ > 0, hasPrevious_);
    }
    void apply(bool Close) {
        auto Work = scan_; if (!Work || !Work->ready_ || !valid(Work)) return;
        // A changed input invalidates the preview before its debounce expires.
        if (Work->command_.isEmpty() && dialog_ && Work->criteria_.Save() != dialog_->Read().Save()) return;
        if (!Work->added_ && !Work->removed_) {
            message(Text("선택 변경 없음"));
            // A deferred accept can become a no-op when the matching entry
            // disappears during refresh. The requested close still completes.
            if (Close && dialog_) dialog_->done(QDialog::Accepted);
            return;
        }
        auto* Grid = view_->BaseView(); applying_ = true;
        if (Work->grouped_) view_->RestoreSelection(Work->afterKeys_, view_->CaptureFocus());
        else {
        Grid->beginUpdate(); Grid->deselectAll();
        for (int At = 0; At < Work->after_.size();) {
            if (!Work->after_.testBit(At)) { ++At; continue; }
            const int First = At; while (At < Work->after_.size() && Work->after_.testBit(At)) ++At;
            // IgnoreSelection preserves commander-style cursor input, but its
            // selectRowRange helper truncates ranges to one row. Use the
            // explicit range API without changing the user's input policy.
            const auto* FirstColumn = Grid->getFirstColumn(); const auto* LastColumn = Grid->getLastColumn();
            Grid->selectRange(First, At - 1, FirstColumn ? FirstColumn->index() : -1, LastColumn ? LastColumn->index() : -1, Qtitan::Select);
        }
        Grid->endUpdate();
        }
        view_->BaseModel()->setProperty("hc.selectAllEntries", false); applying_ = false; ++selection_;
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
            Dialog.invalidated_ = [this] { refresh_.stop(); scan_.reset(); pendingApply_.reset(); };
            Dialog.applyAction_ = [this](bool Close) { requestApply(Close); };
            Dialog.undoAction_ = [this] { if (hasPrevious_) { Criteria C; C.scope_ = Scope::Both; C.operation_ = Operation::Replace; start(C, "restore"); } };
            const QPointer<ItemSelectionController> Guard(owner_);
            QObject::connect(owner_, &QObject::destroyed, &Dialog, &QDialog::reject);
            start(Dialog.Read()); Dialog.exec();
            if (!Guard) return;
            refresh_.stop(); pendingApply_.reset(); scan_.reset(); dialog_ = nullptr;
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
            if (view_->IsGroupFocused()) { message(Text("파일 항목에 커서를 둔 뒤 실행하십시오.")); return; }
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
    state_->refresh_.setSingleShot(true); state_->refresh_.setInterval(750);
    connect(&state_->refresh_, &QTimer::timeout, this, [this] { if (state_->dialog_) state_->start(state_->dialog_->Read(), {}, true); });
    connect(Model, &CFsModelT::sigRefreshCompleted, this, [this] { state_->queueRefresh(); });
    if (auto* Native = qobject_cast<CFSModel*>(Model)) {
        connect(Native, &CFSModel::DirectorySizesChanged, this, [this] { state_->invalidate(); });
        connect(Native, &CFSModel::MetadataChanged, this, [this] { state_->invalidate(); });
    }
    connect(Model, &QObject::destroyed, this, [this] {
        state_->scan_.reset(); if (state_->dialog_) state_->dialog_->reject();
    });
    connect(Model, &QAbstractItemModel::modelAboutToBeReset, this, [this] { state_->invalidate(); });
    connect(Model, &QAbstractItemModel::layoutAboutToBeChanged, this, [this] { state_->invalidate(); });
    connect(Model, &QAbstractItemModel::rowsAboutToBeInserted, this, [this] { state_->invalidate(); });
    connect(Model, &QAbstractItemModel::rowsAboutToBeRemoved, this, [this] { state_->invalidate(); });
    const auto ViewOrderChanged = [this] {
        // Grouped scans retain source indexes and stable keys; native row layout
        // changes do not invalidate their candidates. Source/proxy mutations do.
        if (state_->scan_ && state_->scan_->grouped_ && state_->view_->GroupingActive()) return;
        state_->invalidate();
    };
    connect(View->BaseView(), &Qtitan::GridViewBase::sortingChanged, this, ViewOrderChanged);
    connect(View->BaseView(), &Qtitan::GridViewBase::groupingChanged, this, ViewOrderChanged);
    connect(View, &CGridView::sigSelectionIdentityChanged, this, [this] {
        state_->selectionChanged();
    });
    if (auto* Visible = View->BaseView()->model(); Visible != Model) {
        connect(Visible, &QAbstractItemModel::modelReset, this, [this] { state_->invalidate(); });
        connect(Visible, &QAbstractItemModel::layoutChanged, this, [this] { state_->invalidate(); });
        connect(Visible, &QAbstractItemModel::rowsInserted, this, [this] { state_->invalidate(); });
        connect(Visible, &QAbstractItemModel::rowsRemoved, this, [this] { state_->invalidate(); });
    }
    connect(View->BaseView(), &Qtitan::GridViewBase::selectionChanged, this, [this] {
        if (!state_->view_->GroupingActive()) state_->selectionChanged();
    });
}
ItemSelectionController::~ItemSelectionController() = default;
void ItemSelectionController::Execute(const QString& Command) { state_->execute(Command.section('.', 2)); }
void ItemSelectionController::RememberPrevious(const QStringList& Keys)
{
    if (Keys.isEmpty()) return;
    state_->previous_ = Keys; state_->hasPrevious_ = true; state_->historyLocation_ = state_->locationKey();
}
} // namespace hc::ui
