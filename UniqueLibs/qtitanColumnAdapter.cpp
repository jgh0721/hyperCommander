#include "stdafx.h"
#include "qtitanColumnAdapter.hpp"
#include "column_Defs.hpp"
#include "cmnTypeDefs.hpp"
#include <QtitanGrid.h>
#include <QStyledItemDelegate>
#include <QStyleOptionComboBox>

using namespace hc::view;

namespace {
class CenteredColumnChoice final : public QComboBox
{
public:
    using QComboBox::QComboBox;
protected:
    void initStyleOption(QStyleOptionComboBox* Option) const override
    {
        QComboBox::initStyleOption(Option);
        Option->textAlignment = Qt::AlignCenter;
    }
};
class ColumnWidthEditor final : public QDoubleSpinBox
{
public:
    using QDoubleSpinBox::QDoubleSpinBox;
protected:
    QString textFromValue(double Value) const override
    {
        // Keep fractional percentages from header drags and two-line defaults
        // intact until edited, while presenting a compact whole-number value.
        return locale().toString(Value, 'f', 0);
    }
};
class ColumnNameDelegate final : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;
protected:
    void initStyleOption(QStyleOptionViewItem* Option, const QModelIndex& Index) const override
    {
        QStyledItemDelegate::initStyleOption(Option, Index);
        Option->displayAlignment = Qt::AlignLeft | Qt::AlignVCenter;
        Option->rect.adjust(10, 0, -10, 0);
    }
};
const QStringList LayoutModes{"details", "name-first", "name-last"};
QJsonObject EncodePolicies(const QMap<QString, ColumnWidthPolicy>& Policies)
{
    QJsonObject Result;
    for (auto It = Policies.begin(); It != Policies.end(); ++It) Result[It.key()] = EncodeWidth(It.value());
    return Result;
}
}

QtitanColumnAdapter::QtitanColumnAdapter(QWidget* Owner, Qtitan::Grid* Grid, Qtitan::GridBandedTableView* View)
    : QObject(Owner), owner_(Owner), grid_(Grid), view_(View)
{
    timer_.setSingleShot(true);
    timer_.setInterval(16);
    connect(&timer_, &QTimer::timeout, this, &QtitanColumnAdapter::Apply);
    dragTimer_.setInterval(20);
    connect(&dragTimer_, &QTimer::timeout, this, [this] {
        // Qtitan may consume release in its temporary application drag filter.
        if (!(QApplication::mouseButtons() & Qt::LeftButton)) captureDrag();
    });
    for (auto* Widget : {Owner, Grid->viewport(), static_cast<QWidget*>(Grid), static_cast<QWidget*>(View->verticalScrollBar())}) Widget->installEventFilter(this);
    connect(View->verticalScrollBar(), &QScrollBar::rangeChanged, this, [this] { Request(); });
    connect(View, &Qtitan::GridViewBase::columnsUpdated, this, [this] { if (!applying_) Request(); });
    connect(View, &Qtitan::GridViewBase::columnVisibleChanged, this, [this](Qtitan::GridColumnBase* Column, bool Visible) {
        if (applying_) return;
        const auto Id = ids_.value(Column->index());
        if (settings_.columns_.contains(Id)) { settings_.columns_[Id].hidden_ = !Visible; save(); }
        Request();
    });
    // Exclude the actual scrollbar area instead of an overlay obscuring columns.
    View->options().setScrollBarsTransparent(false);
    View->tableOptions().setColumnAutoWidth(false);
}

void QtitanColumnAdapter::Bind(const ColumnView& Columns)
{
    ids_.clear(); readableFloors_.clear(); defaults_.clear(); applied_.clear(); nameColumn_ = -1;
    definition_ = Columns.profile_;
    for (const auto& Column : Columns.VecColumns)
    {
        const auto& Definition = Column.definition_;
        const auto Id = hc::columns::IdText(Definition.id_);
        ids_[Column.Index] = Id;
        if (Definition.id_ == definition_.primaryNameId_) nameColumn_ = Column.Index;
        const auto& Field = Definition.fieldId_;
        readableFloors_[Id] = Field == "file.name" || Field == "file.stem" ? 200
            : Field == "file.size" ? 88 : Field == "file.created" || Field == "file.modified" ? 190
            : Field == "file.attributes" ? 56 : 80;
        defaults_[Id] = Definition.widths_[0];
    }
    singleLineDefaults_ = defaults_;
    viewId_ = hc::columns::IdText(definition_.id_);
    QTimer::singleShot(0, this, &QtitanColumnAdapter::Reload);
}

void QtitanColumnAdapter::Reload()
{
    if (ids_.isEmpty()) return;
    if (workspaceOwned_) { Request(); return; }
    QString Pane = "default";
    for (auto* Parent = owner_.data(); Parent; Parent = Parent->parentWidget())
        if (Parent->objectName() == "cmpLeftPanel" || Parent->objectName() == "cmpRightPanel") { Pane = Parent->objectName(); break; }
    baseProfile_ = hc::columns::SettingsPrefix + Pane + "/" + viewId_;
    const auto Profile = hc::settings::AppSettings().Read(hc::settings::Section::Columns).toObject()[baseProfile_].toObject();
    const auto Mode = Profile.value("listMode").toString("details");
    activateLayout(LayoutModes.contains(Mode) ? Mode : "details");
    if (!LayoutModes.contains(Mode)) settings_.writable_ = false;
    Request();
}

QString QtitanColumnAdapter::profileForLayout(const QString& Mode) const
{
    return Mode == "details" ? baseProfile_ : baseProfile_ + "/" + Mode;
}

QMap<QString, ColumnWidthPolicy> QtitanColumnAdapter::defaultsForLayout(const QString& Mode) const
{
    QMap<QString, ColumnWidthPolicy> Result;
    const auto Layout = hc::columns::ParseMode(Mode).value_or(hc::columns::LayoutMode::Details);
    for (const auto& Column : definition_.columns_)
        Result[hc::columns::IdText(Column.id_)] = Column.widths_[int(Layout)];
    return Result;
}

void QtitanColumnAdapter::activateLayout(const QString& Mode)
{
    layoutMode_ = Mode;
    defaults_ = defaultsForLayout(Mode);
    profile_ = profileForLayout(Mode);
    settings_ = LoadWidthPolicies(hc::settings::AppSettings(), profile_, defaults_);
    inherited_ = settings_.columns_;
    const auto Overrides = layoutWidths_.value(Mode).toObject();
    for (auto It = Overrides.begin(); It != Overrides.end(); ++It) {
        const auto Policy = It.value().isObject() ? DecodeWidth(It.value().toObject()) : std::nullopt;
        if (Policy && ids_.values().contains(It.key())) settings_.columns_[It.key()] = *Policy;
    }
    if (!unsupportedLayout_.isEmpty()) settings_.writable_ = false;
    applied_.clear(); Request();
}

bool QtitanColumnAdapter::SetLayoutMode(const QString& Mode)
{
    if (!LayoutModes.contains(Mode) || !settings_.writable_ || (Mode != "details" && nameColumn_ < 0)) return false;
    layoutWidths_[layoutMode_] = EncodePolicies(settings_.columns_);
    activateLayout(Mode);
    return true;
}

QJsonObject QtitanColumnAdapter::WorkspaceLayout() const
{
    if (!unsupportedLayout_.isEmpty()) return unsupportedLayout_;
    auto Profiles = layoutWidths_;
    Profiles[layoutMode_] = EncodePolicies(settings_.columns_);
    return {{"version", 1}, {"definition", hc::columns::EncodeProfile(definition_)}, {"mode", layoutMode_}, {"profiles", Profiles}};
}

void QtitanColumnAdapter::RestoreWorkspaceLayout(const QJsonObject& View)
{
    if (baseProfile_.isEmpty()) Reload();
    layoutWidths_ = {}; unsupportedLayout_ = {};
    const auto Layout = View.value(hc::columns::WorkspaceKey).toObject();
    if (!Layout.isEmpty()) {
        bool Valid = Layout.value("version") == QJsonValue(1) && LayoutModes.contains(Layout.value("mode").toString()) && Layout.value("profiles").isObject();
        const auto Definition = hc::columns::DecodeProfile(Layout.value("definition").toObject());
        // Until the profile editor can rebuild model bindings, only the bound
        // definition is executable. Preserve other valid/future profiles intact.
        if (!Definition || Definition->id_ != definition_.id_ || Definition->primaryNameId_ != definition_.primaryNameId_ ||
            Definition->columns_.size() != definition_.columns_.size()) Valid = false;
        else for (int Index = 0; Index < Definition->columns_.size(); ++Index) {
            auto Saved = Definition->columns_[Index];
            Saved.title_ = definition_.columns_[Index].title_; // Translation is not identity.
            if (Saved != definition_.columns_[Index]) Valid = false;
        }
        const auto Profiles = Layout.value("profiles").toObject();
        for (auto It = Profiles.begin(); It != Profiles.end(); ++It) {
            if (!LayoutModes.contains(It.key()) || !It.value().isObject()) { Valid = false; continue; }
            const auto Columns = It.value().toObject();
            for (auto Column = Columns.begin(); Column != Columns.end(); ++Column)
                if (!ids_.values().contains(Column.key()) || !Column.value().isObject() || !DecodeWidth(Column.value().toObject())) Valid = false;
        }
        if (Valid) layoutWidths_ = Profiles;
        else unsupportedLayout_ = Layout;
        activateLayout(Valid ? Layout.value("mode").toString() : "details");
    } else {
        // Breaking transition: old listLayout/widths data is intentionally ignored.
        workspaceOwned_ = false;
        Reload();
    }
    workspaceOwned_ = true; Request();
}

int QtitanColumnAdapter::AvailableWidth() const
{
    if (!owner_ || !owner_->isVisible() || !grid_->viewport()->isVisible()) return 0;
    const auto Rect = grid_->viewport()->contentsRect();
    auto Width = Rect.width();
    const auto Bar = view_->verticalScrollBar();
    if (Bar->isVisible())
    {
        const QRect BarRect(grid_->viewport()->mapFromGlobal(Bar->mapToGlobal(QPoint())), Bar->size());
        Width -= Rect.intersected(BarRect).width();
    }
    // This view disables row selectors, frozen-row buttons and quick customization;
    // those public options reserve no separate row-header strip (see upgrade notes).
    return std::max(0, Width);
}

void QtitanColumnAdapter::Request()
{
    if (!applying_ && !timer_.isActive()) timer_.start();
}

std::optional<ColumnWidthResult> QtitanColumnAdapter::resolveWidth(const QString& Id, int Available) const
{
    const auto Policy = settings_.columns_.value(Id);
    auto Result = Resolve(Policy, Available);
    // Defaults have a readable pixel floor on narrow panes. Explicit user
    // policies retain their own units and bounds (including narrow columns).
    if (Result && definition_.id_ == hc::columns::BuiltInProfileId && Policy == defaults_.value(Id)) {
        Result->minimum_ = std::max(Result->minimum_, readableFloors_.value(Id));
        Result->width_ = std::max(Result->width_, Result->minimum_);
        Result->maximum_ = std::max(Result->maximum_, Result->minimum_);
    }
    return Result;
}

void QtitanColumnAdapter::Apply()
{
    if (applying_ || dragging_ || settings_.columns_.isEmpty()) return;
    const auto Available = AvailableWidth();
    if (Available <= 0) return;
    QElapsedTimer Duration; Duration.start();
    QScopedValueRollback Guard(applying_, true);
    view_->beginUpdate();
    for (auto It = ids_.begin(); It != ids_.end(); ++It)
    {
        auto* Column = view_->getColumnByModelColumn(It.key());
        if (!Column || !settings_.columns_.contains(It.value())) continue;
        auto* Banded = static_cast<Qtitan::GridBandedTableColumn*>(Column);
        const int Row = layoutMode_ == "details" ? 0 : (It.key() == nameColumn_ ? (layoutMode_ == "name-first" ? 0 : 1) : (layoutMode_ == "name-first" ? 1 : 0));
        if (Banded->rowIndex() != Row) Banded->setRowIndex(Row);
        const auto& Policy = settings_.columns_[It.value()];
        const auto Width = resolveWidth(It.value(), Available);
        if (!Width) continue;
        Column->setAutoWidth(false);
        Column->setHorSizingEnabled(Policy.mode_ == WidthMode::Flexible);
        if (Column->isVisible() == Policy.hidden_) Column->setVisible(!Policy.hidden_);
        if (Column->minWidth() != Width->minimum_) Column->setMinWidth(Width->minimum_);
        if (Column->maxWidth() != Width->maximum_) Column->setMaxWidth(Width->maximum_);
    }
    view_->endUpdate();
    // Qtitan caches header bounds until endUpdate. Publish constraints first so
    // an expanding flexible column is not clamped to the previous pixel maximum.
    view_->beginUpdate();
    for (auto It = ids_.begin(); It != ids_.end(); ++It)
    {
        auto* Column = view_->getColumnByModelColumn(It.key());
        if (!Column || !settings_.columns_.contains(It.value())) continue;
        const auto Width = resolveWidth(It.value(), Available);
        if (!Width) continue;
        if (Column->width() != Width->width_) Column->setWidth(Width->width_);
        applied_[It.value()] = Column->width();
    }
    view_->endUpdate();
    lastWidth_ = Available;
    ++applyCount_;
    lastApplyNs_ = Duration.nsecsElapsed();
}

bool QtitanColumnAdapter::SetPolicy(const QString& Id, const ColumnWidthPolicy& Policy, bool Persist)
{
    if (!readableFloors_.contains(Id) || !Validate(Policy).empty() || !settings_.writable_) return false;
    const auto Previous = settings_;
    settings_.columns_[Id] = Policy;
    if (Persist && !save()) { settings_ = Previous; return false; }
    Request();
    return true;
}

bool QtitanColumnAdapter::save() { return workspaceOwned_ || (!profile_.isEmpty() && SaveWidths(hc::settings::AppSettings(), profile_, settings_)); }

void QtitanColumnAdapter::SetWorkspaceOverrides(const QJsonObject& Columns)
{
    if (!workspaceOwned_) { Reload(); inherited_ = settings_.columns_; workspaceOwned_ = true; }
    settings_.columns_ = inherited_;
    for (auto It = Columns.begin(); It != Columns.end(); ++It) {
        const auto Policy = It.value().isObject() ? DecodeWidth(It.value().toObject()) : std::nullopt;
        if (Policy && readableFloors_.contains(It.key())) settings_.columns_[It.key()] = *Policy;
    }
    Request();
}

QJsonObject QtitanColumnAdapter::WorkspaceOverrides() const
{
    QJsonObject Result;
    for (auto It = settings_.columns_.begin(); It != settings_.columns_.end(); ++It)
        if (!inherited_.contains(It.key()) || EncodeWidth(inherited_.value(It.key())) != EncodeWidth(It.value()))
            Result[It.key()] = EncodeWidth(It.value());
    return Result;
}

void QtitanColumnAdapter::captureDrag()
{
    dragTimer_.stop();
    dragging_ = false;
    const auto Available = AvailableWidth();
    if (applying_ || Available <= 0 || Available != lastWidth_) { Request(); return; }
    bool Changed = false;
    for (auto It = ids_.begin(); It != ids_.end(); ++It)
    {
        const auto Column = view_->getColumnByModelColumn(It.key());
        if (!Column || !applied_.contains(It.value()) || Column->width() == applied_[It.value()]) continue;
        auto& Policy = settings_.columns_[It.value()];
        const auto Next = FromDrag(Policy, Column->width(), Available);
        Changed |= Next != Policy;
        Policy = Next;
    }
    if (Changed && !save()) Reload();
    Request();
}

bool QtitanColumnAdapter::eventFilter(QObject* Watched, QEvent* Event)
{
    if (Watched == grid_->viewport())
    {
        if (Event->type() == QEvent::MouseButtonPress && static_cast<QMouseEvent*>(Event)->button() == Qt::LeftButton) { dragging_ = true; dragTimer_.start(); }
        if (Event->type() == QEvent::MouseButtonRelease)
            QTimer::singleShot(0, this, &QtitanColumnAdapter::captureDrag);
        if (Event->type() == QEvent::Paint && AvailableWidth() != lastWidth_) Request();
    }
    switch (Event->type())
    {
    case QEvent::Resize: case QEvent::Show: case QEvent::LayoutRequest:
    case QEvent::DevicePixelRatioChange: case QEvent::ScreenChangeInternal: case QEvent::FontChange:
        Request(); break;
    case QEvent::Hide: dragging_ = false; dragTimer_.stop(); break;
    default: break;
    }
    return QObject::eventFilter(Watched, Event);
}

bool QtitanColumnAdapter::ShowSettings(QWidget* Parent, QJsonObject* Pending)
{
    if (profile_.isEmpty()) Reload();
    QDialog Dialog(Parent ? Parent : owner_.data());
    Dialog.setObjectName("hc.columnWidthsDialog");
    Dialog.setWindowTitle(tr("목록 모양"));
    auto* Layout = new QVBoxLayout(&Dialog);
    const auto Initial = Pending && !Pending->isEmpty() ? *Pending : WorkspaceLayout();
    auto Draft = Initial.value("profiles").toObject();
    QString DraftMode = Initial.value("mode").toString(layoutMode_);
    auto* Form = new QFormLayout;
    auto* ModeChoice = new QComboBox(&Dialog); ModeChoice->setObjectName("hc.listLayout.mode");
    ModeChoice->addItems({tr("한 줄"), tr("두 줄: 이름 위"), tr("두 줄: 이름 아래")});
    ModeChoice->setCurrentIndex(LayoutModes.indexOf(DraftMode));
    ModeChoice->setEnabled(nameColumn_ >= 0);
    Form->addRow(tr("표시 모드(&M)"), ModeChoice);
    auto* Scope = new QComboBox(&Dialog); Scope->setObjectName("hc.listLayout.scope");
    Scope->addItems({tr("현재 탭"), tr("현재 탭 + 이 패널의 새 탭 기본값")});
    Form->addRow(tr("저장 범위(&S)"), Scope); Layout->addLayout(Form);
    auto* Preview = new QLabel(&Dialog); Preview->setObjectName("hc.listLayout.preview");
    Preview->setFrameStyle(QFrame::StyledPanel); Preview->setMargin(10); Layout->addWidget(Preview);
    Preview->setWordWrap(true);
    auto* Note = new QLabel(tr("모드별 컬럼 너비를 따로 보관합니다. 퍼센트는 패널의 표시 영역 기준이며, 두 줄 모드의 각 줄에 별도로 적용됩니다.\n고정 모드는 헤더 드래그를 제한합니다. ‘기본값’은 선택한 모드의 너비와 표시 여부를 초기화합니다."), &Dialog);
    Note->setWordWrap(true); Layout->addWidget(Note);
    auto* Table = new QTableWidget(ids_.size(), 7, &Dialog);
    Table->setProperty("hc.cellControls", true);
    Table->setHorizontalHeaderLabels({tr("컬럼"), tr("단위"), tr("모드"), tr("기준값"), tr("최소값"), tr("최대값"), tr("표시")});
    Table->verticalHeader()->hide();
    Table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    Table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    Table->setObjectName("hc.listLayout.columns");
    Table->setSelectionBehavior(QAbstractItemView::SelectRows);
    Table->setSelectionMode(QAbstractItemView::SingleSelection);
    Table->setItemDelegateForColumn(0, new ColumnNameDelegate(Table));
    Layout->addWidget(Table);
    auto* ColumnActions = new QHBoxLayout;
    auto* Available = new QComboBox(&Dialog); Available->setObjectName("hc.listLayout.availableColumns");
    Available->setAccessibleName(tr("추가할 컬럼"));
    auto* Add = new QPushButton(tr("컬럼 추가(&A)"), &Dialog); Add->setObjectName("hc.listLayout.addColumn");
    auto* Remove = new QPushButton(tr("컬럼 제거(&R)"), &Dialog); Remove->setObjectName("hc.listLayout.removeColumn");
    Remove->setToolTip(tr("선택한 컬럼을 숨깁니다. 컬럼 추가로 다시 표시할 수 있습니다."));
    ColumnActions->addWidget(Available, 1); ColumnActions->addWidget(Add); ColumnActions->addWidget(Remove);
    Layout->addLayout(ColumnActions);
    struct Row { QString id_; QComboBox* unit_; QComboBox* mode_; QDoubleSpinBox* value_; QDoubleSpinBox* min_; QDoubleSpinBox* max_; QCheckBox* visible_; };
    QList<Row> Rows;
    int RowIndex = 0;
    for (auto It = ids_.begin(); It != ids_.end(); ++It, ++RowIndex)
    {
        const auto Column = view_->getColumnByModelColumn(It.key());
        const auto Policy = settings_.columns_.value(It.value());
        auto* Item = new QTableWidgetItem(Column->caption());
        Item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        Item->setTextAlignment(Qt::AlignLeft | Qt::AlignVCenter); Table->setItem(RowIndex, 0, Item);
        auto* Unit = new CenteredColumnChoice(Table); Unit->addItems({tr("픽셀"), tr("퍼센트")}); Unit->setCurrentIndex(static_cast<int>(Policy.unit_));
        auto* Mode = new CenteredColumnChoice(Table); Mode->addItems({tr("가변"), tr("고정")}); Mode->setCurrentIndex(static_cast<int>(Policy.mode_));
        int ChoiceColumn = 1;
        for (auto* Choice : {Unit, Mode}) {
            int TextWidth = 0;
            for (int Index = 0; Index < Choice->count(); ++Index) {
                Choice->setItemData(Index, int(Qt::AlignCenter), Qt::TextAlignmentRole);
                TextWidth = std::max(TextWidth, Choice->fontMetrics().horizontalAdvance(Choice->itemText(Index)));
            }
            // QTableWidget's content sizing does not reliably include a cell
            // widget's themed arrow/padding, especially with large dialog fonts.
            auto* Cell = new QTableWidgetItem;
            Cell->setFlags(Qt::ItemIsEnabled);
            Cell->setSizeHint(QSize(TextWidth + 52, Choice->sizeHint().height()));
            Table->setItem(RowIndex, ChoiceColumn++, Cell);
        }
        const auto Spin = [&](int Position, double Value, bool Optional) {
            auto* Box = new ColumnWidthEditor(Table); Box->setAlignment(Qt::AlignCenter); Box->setDecimals(6); Box->setRange(Optional ? -1 : 0, Policy.unit_ == WidthUnit::Percent ? 100 : 1000000);
            if (Optional) Box->setSpecialValueText(tr("미지정"));
            Box->setValue(Value); Box->setAccessibleName(Column->caption() + " " + Table->horizontalHeaderItem(Position)->text());
            Table->setCellWidget(RowIndex, Position, Box); return Box;
        };
        auto* Value = Spin(3, Policy.value_, false);
        auto* Minimum = Spin(4, Policy.minimum_.value_or(-1), true);
        auto* Maximum = Spin(5, Policy.maximum_.value_or(-1), true);
        auto* Visible = new QCheckBox(Table); Visible->setChecked(!Policy.hidden_);
        Unit->setAccessibleName(Column->caption() + tr(" 단위")); Mode->setAccessibleName(Column->caption() + tr(" 모드")); Visible->setAccessibleName(Column->caption() + tr(" 표시"));
        auto* VisibilityCell = new QWidget(Table);
        auto* VisibilityLayout = new QHBoxLayout(VisibilityCell); VisibilityLayout->setContentsMargins(0, 0, 0, 0);
        VisibilityLayout->addWidget(Visible, 0, Qt::AlignCenter);
        Table->setCellWidget(RowIndex, 1, Unit); Table->setCellWidget(RowIndex, 2, Mode); Table->setCellWidget(RowIndex, 6, VisibilityCell);
        connect(Unit, &QComboBox::currentIndexChanged, &Dialog, [Value, Minimum, Maximum](int Index) { for (auto* Box : {Value, Minimum, Maximum}) Box->setMaximum(Index == 1 ? 100 : 1000000); });
        Rows.push_back({It.value(), Unit, Mode, Value, Minimum, Maximum, Visible});
    }
    const auto UpdatePreview = [&] {
        const QStringList Samples{tr("프로젝트 검토 자료.txt"), "128 KB", "2026-09-27 10:30", "A",
            "2026-09-27 11:00", "txt", tr("프로젝트 검토 자료"), tr("TXT 파일")};
        QString Name; QStringList Details;
        int Index = 0;
        for (auto It = ids_.begin(); It != ids_.end(); ++It, ++Index) {
            if (!Rows[Index].visible_->isChecked()) continue;
            const auto Sample = definition_.id_ == hc::columns::BuiltInProfileId ? Samples.value(It.key()) : Table->item(Index, 0)->text();
            if (It.key() == nameColumn_) Name = Sample;
            else Details.append(Sample);
        }
        if (DraftMode == "details") {
            if (!Name.isEmpty()) Details.prepend(Name);
            Preview->setText(Details.join("     "));
        } else Preview->setText(DraftMode == "name-first" ? Name + "\n" + Details.join("     ") : Details.join("     ") + "\n" + Name);
    };
    for (const auto& Row : Rows) connect(Row.visible_, &QCheckBox::toggled, &Dialog, UpdatePreview);
    const auto RefreshChoices = [&] {
        const auto Selected = Available->currentData();
        Available->clear();
        for (int Index = 0; Index < Rows.size(); ++Index)
            if (Table->isRowHidden(Index)) Available->addItem(Table->item(Index, 0)->text(), Index);
        const auto Previous = Available->findData(Selected);
        if (Previous >= 0) Available->setCurrentIndex(Previous);
        Available->setEnabled(Available->count() > 0); Add->setEnabled(Available->count() > 0);
        Remove->setEnabled(Table->currentRow() >= 0 && !Table->isRowHidden(Table->currentRow()));
    };
    connect(Table, &QTableWidget::currentCellChanged, &Dialog, RefreshChoices);
    connect(Add, &QPushButton::clicked, &Dialog, [&] {
        if (Available->currentIndex() < 0) return;
        const auto Index = Available->currentData().toInt();
        Table->setRowHidden(Index, false); Rows[Index].visible_->setChecked(true);
        Table->setCurrentCell(Index, 0); Table->scrollToItem(Table->item(Index, 0)); RefreshChoices();
    });
    connect(Remove, &QPushButton::clicked, &Dialog, [&] {
        const auto Index = Table->currentRow(); if (Index < 0) return;
        Rows[Index].visible_->setChecked(false); Table->setRowHidden(Index, true);
        Table->setCurrentCell(-1, -1); RefreshChoices();
    });
    auto* Error = new QLabel(&Dialog); Error->setObjectName("hc.columnWidths.error"); Error->setWordWrap(true); Layout->addWidget(Error);
    auto* Buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel | QDialogButtonBox::RestoreDefaults, &Dialog); Layout->addWidget(Buttons);
    Buttons->button(QDialogButtonBox::RestoreDefaults)->setText(tr("기본값"));
    Buttons->button(QDialogButtonBox::Save)->setText(Pending ? tr("확인") : tr("저장"));
    Buttons->button(QDialogButtonBox::Cancel)->setText(tr("취소"));
    if (!settings_.writable_) { Error->setText(tr("지원하지 않거나 손상된 설정이 있습니다. 기존 설정을 보존하기 위해 저장할 수 없습니다.")); Buttons->button(QDialogButtonBox::Save)->setEnabled(false); }
    connect(Buttons, &QDialogButtonBox::rejected, &Dialog, &QDialog::reject);
    const auto ReadRows = [&](WidthSettings& Next) {
        int VisibleCount = 0;
        for (const auto& Row : Rows)
        {
            if ((Row.min_->value() < 0 && Row.min_->value() != -1) || (Row.max_->value() < 0 && Row.max_->value() != -1))
            { Error->setText(tr("최소·최대값은 0 이상 또는 ‘미지정’이어야 합니다.")); return false; }
            ColumnWidthPolicy Policy{static_cast<WidthUnit>(Row.unit_->currentIndex()), static_cast<WidthMode>(Row.mode_->currentIndex()), Row.value_->value()};
            if (Row.min_->value() >= 0) Policy.minimum_ = Row.min_->value();
            if (Row.max_->value() >= 0) Policy.maximum_ = Row.max_->value();
            Policy.hidden_ = !Row.visible_->isChecked();
            if (!Validate(Policy).empty()) { Error->setText(tr("최소값 ≤ 최대값, 고정 모드에서는 최소값 ≤ 기준값 ≤ 최대값이어야 합니다.")); return false; }
            if (!Policy.hidden_) ++VisibleCount;
            if (DraftMode != "details" && Row.id_ == ids_.value(nameColumn_) && Policy.hidden_) {
                Error->setText(tr("두 줄 모드에서는 이름 컬럼을 표시해야 합니다.")); return false;
            }
            Next.columns_[Row.id_] = Policy;
        }
        if (!VisibleCount) { Error->setText(tr("하나 이상의 컬럼을 표시해야 합니다.")); return false; }
        Error->clear(); return true;
    };
    const auto Populate = [&](const QMap<QString, ColumnWidthPolicy>& Policies) {
        int Index = 0;
        for (const auto& Row : Rows) {
            const auto Policy = Policies.value(Row.id_);
            Row.unit_->setCurrentIndex(int(Policy.unit_)); Row.mode_->setCurrentIndex(int(Policy.mode_));
            Row.value_->setValue(Policy.value_); Row.min_->setValue(Policy.minimum_.value_or(-1));
            Row.max_->setValue(Policy.maximum_.value_or(-1)); Row.visible_->setChecked(!Policy.hidden_);
            Table->setRowHidden(Index++, Policy.hidden_);
        }
        Table->setCurrentCell(-1, -1); RefreshChoices();
        UpdatePreview();
    };
    auto InitialPolicies = LoadWidthPolicies(hc::settings::AppSettings(), profileForLayout(DraftMode), defaultsForLayout(DraftMode)).columns_;
    const auto SavedPolicies = Draft.value(DraftMode).toObject();
    for (auto It = SavedPolicies.begin(); It != SavedPolicies.end(); ++It)
        if (const auto Policy = DecodeWidth(It.value().toObject())) InitialPolicies[It.key()] = *Policy;
    Populate(InitialPolicies);
    connect(ModeChoice, &QComboBox::currentIndexChanged, &Dialog, [&](int Index) {
        WidthSettings Previous;
        if (!ReadRows(Previous)) { const QSignalBlocker Block(ModeChoice); ModeChoice->setCurrentIndex(LayoutModes.indexOf(DraftMode)); return; }
        Draft[DraftMode] = EncodePolicies(Previous.columns_);
        DraftMode = LayoutModes[Index];
        auto Next = LoadWidthPolicies(hc::settings::AppSettings(), profileForLayout(DraftMode), defaultsForLayout(DraftMode));
        const auto Saved = Draft.value(DraftMode).toObject();
        for (auto It = Saved.begin(); It != Saved.end(); ++It)
            if (const auto Policy = DecodeWidth(It.value().toObject())) Next.columns_[It.key()] = *Policy;
        Populate(Next.columns_);
        Buttons->button(QDialogButtonBox::Save)->setEnabled(settings_.writable_ && Next.writable_);
        if (!Next.writable_) Error->setText(tr("이 모드의 설정을 읽을 수 없어 저장할 수 없습니다."));
    });
    connect(Buttons->button(QDialogButtonBox::RestoreDefaults), &QPushButton::clicked, &Dialog, [&] {
        Populate(defaultsForLayout(DraftMode)); Error->clear();
    });
    connect(Buttons, &QDialogButtonBox::accepted, &Dialog, [&] {
        WidthSettings Next;
        if (!ReadRows(Next)) return;
        Draft[DraftMode] = EncodePolicies(Next.columns_);
        if (Scope->currentIndex() == 1) {
            auto& Settings = hc::settings::AppSettings();
            auto Profiles = Settings.Read(hc::settings::Section::Columns).toObject();
            if (!LoadWidthPolicies(Settings, baseProfile_, singleLineDefaults_).writable_) {
                Error->setText(tr("이 패널의 기본 설정을 읽을 수 없어 덮어쓰지 않습니다.")); return;
            }
            for (auto It = Draft.begin(); It != Draft.end(); ++It) {
                const auto Key = profileForLayout(It.key());
                const auto Existing = LoadWidthPolicies(Settings, Key, defaultsForLayout(It.key()));
                if (!Existing.writable_) { Error->setText(tr("읽을 수 없는 모드 설정이 있어 기본값을 저장할 수 없습니다.")); return; }
                auto Profile = Profiles[Key].toObject(); Profile["version"] = 1; Profile["columns"] = It.value(); Profiles[Key] = Profile;
            }
            auto Base = Profiles[baseProfile_].toObject();
            if (Base.isEmpty()) Base = {{"version", 1}, {"columns", EncodePolicies(singleLineDefaults_)}};
            Base["listMode"] = DraftMode; Profiles[baseProfile_] = Base;
            if (!Settings.Put(hc::settings::Section::Columns, Profiles)) { Settings.DiscardEdit(); Error->setText(tr("설정을 저장할 수 없습니다.")); return; }
        }
        if (Pending) *Pending = QJsonObject{{"version", 1}, {"definition", hc::columns::EncodeProfile(definition_)}, {"mode", DraftMode}, {"profiles", Draft}};
        else {
            layoutWidths_ = Draft; activateLayout(DraftMode); workspaceOwned_ = true;
            Request();
        }
        Dialog.accept();
    });
    const auto RowHeight = std::max(36, Table->fontMetrics().height() + 18);
    Table->verticalHeader()->setDefaultSectionSize(RowHeight);
    // Before the dialog is polished the header's size hint can still use the
    // previous font. Reserve a full header row and scrollbar to avoid clipping
    // the fourth row at large font sizes.
    Table->setMinimumHeight(RowHeight * 5 + Table->style()->pixelMetric(QStyle::PM_ScrollBarExtent, nullptr, Table) + 8);
    Dialog.resize(980, std::max(570, Dialog.minimumSizeHint().height()));
    return Dialog.exec() == QDialog::Accepted;
}
