#include "stdafx.h"
#include "qtitanColumnAdapter.hpp"
#include "column_Defs.hpp"
#include "cmnTypeDefs.hpp"
#include <QtitanGrid.h>
#include <QtnGridViewGraphics.h>

using namespace hc::view;

namespace {
const QStringList LayoutModes{"details", "name-first", "name-last"};
QJsonObject EncodePolicies(const QMap<QString, ColumnWidthPolicy>& Policies)
{
    QJsonObject Result;
    for (auto It = Policies.begin(); It != Policies.end(); ++It) Result[It.key()] = EncodeWidth(It.value());
    return Result;
}
}

QtitanColumnAdapter::QtitanColumnAdapter(QWidget* Owner, Qtitan::Grid* Grid, Qtitan::GridBandedTableView* View, bool Transient)
    : QObject(Owner), owner_(Owner), grid_(Grid), view_(View), transient_(Transient)
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
    const auto Defaults = hc::columns::DefaultProfile().columns_;
    const auto Reference = hc::columns::ReferenceProfile().columns_;
    // Workspace capture records visual order. Reordering the same default
    // definitions must not lose readable floors after restart.
    const auto Matches = [&](const auto& Preset) {
        return definition_.columns_.size() == Preset.size() &&
            std::all_of(definition_.columns_.begin(), definition_.columns_.end(), [&](const auto& Column) { return Preset.contains(Column); });
    };
    referencePresentation_ = Matches(Reference);
    defaultPresentation_ = Matches(Defaults) || referencePresentation_;
    layoutWidths_ = {}; unsupportedLayout_ = {}; baseProfile_.clear(); workspaceOwned_ = false;
    layoutMode_ = hc::columns::ModeName(definition_.mode_);
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
        if (referencePresentation_) {
            // The reference uses minute timestamps and a separate extension.
            // Keep these floors in pixels; persisted percentage bounds use percent.
            if (Field == "file.modified") readableFloors_[Id] = 164;
            else if (Field == "file.extension") readableFloors_[Id] = 56;
            else if (Field == "file.size") readableFloors_[Id] = 80;
            else if (Field == "file.attributes") readableFloors_[Id] = 48;
        }
        defaults_[Id] = Definition.widths_[0];
    }
    singleLineDefaults_ = defaults_;
    viewId_ = hc::columns::IdText(definition_.id_);
    baseProfile_ = hc::columns::SettingsPrefix + PaneKey() + "/" + viewId_;
    activateLayout(layoutMode_);
}

void QtitanColumnAdapter::Reload()
{
    if (ids_.isEmpty()) return;
    if (workspaceOwned_) { Request(); return; }
    baseProfile_ = hc::columns::SettingsPrefix + PaneKey() + "/" + viewId_;
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
    settings_ = transient_ ? hc::view::WidthSettings{defaults_, true} : LoadWidthPolicies(hc::settings::AppSettings(), profile_, defaults_);
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
    if (layoutObserver_) layoutObserver_(Mode);
    return true;
}

QJsonObject QtitanColumnAdapter::WorkspaceLayout() const
{
    if (!unsupportedLayout_.isEmpty()) return unsupportedLayout_;
    auto Profiles = layoutWidths_;
    Profiles[layoutMode_] = EncodePolicies(settings_.columns_);
    return {{"version", 1}, {"definition", hc::columns::EncodeProfile(Profile())}, {"mode", layoutMode_}, {"profiles", Profiles}};
}

void QtitanColumnAdapter::RestoreWorkspaceLayout(const QJsonObject& View)
{
    if (baseProfile_.isEmpty()) Reload();
    layoutWidths_ = {}; unsupportedLayout_ = {};
    const auto Layout = View.value(hc::columns::WorkspaceKey).toObject();
    if (!Layout.isEmpty()) {
        bool Valid = Layout.value("version") == QJsonValue(1) && LayoutModes.contains(Layout.value("mode").toString()) && Layout.value("profiles").isObject();
        const auto Definition = hc::columns::DecodeProfile(Layout.value("definition").toObject());
        if (!Definition || !hc::columns::IsSupportedProfile(*Definition) || Definition->id_ != definition_.id_) Valid = false;
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
    Width -= static_cast<Qtitan::GridViewBase*>(view_)->layout()->getRowGroupIndent();
    return std::max(0, Width);
}

QtitanColumnAdapter::ViewportAnchor QtitanColumnAdapter::CaptureViewport() const
{
    ViewportAnchor Result;
    if (captureViewport_) { Result.state_ = captureViewport_(); return Result; }
    Result.horizontal_ = view_->horizontalScrollBar()->value();
    if (view_->getRowCount() <= 0) return Result;
    auto* Layout = static_cast<Qtitan::GridViewBase*>(view_)->layout(); Layout->getModelHeight();
    const int Scroll = view_->verticalScrollBar()->value();
    int Low = 0, High = view_->getRowCount();
    // Public row positions include the vendor's actual metrics and row padding.
    while (Low + 1 < High) {
        const int Middle = (Low + High) / 2;
        if (Layout->getRowScrollPos(Middle) <= Scroll) Low = Middle; else High = Middle;
    }
    Result.entry_ = view_->getRow(Low).modelIndex(0);
    Result.offset_ = Scroll - Layout->getRowScrollPos(Low);
    return Result;
}
void QtitanColumnAdapter::RestoreViewport(const ViewportAnchor& Anchor)
{
    if (restoreViewport_ && !Anchor.state_.isEmpty()) { restoreViewport_(Anchor.state_); return; }
    if (!Anchor.entry_.isValid()) return;
    auto* Layout = static_cast<Qtitan::GridViewBase*>(view_)->layout(); Layout->updateLayout(); Layout->getModelHeight();
    const auto Row = view_->getRow(Anchor.entry_);
    if (!Row.isValid()) return;
    const int Offset = std::clamp(Anchor.offset_, 0, std::max(0, Layout->calculateGridRowLength(Row, false) - 1));
    view_->verticalScrollBar()->setValue(Layout->getRowScrollPos(Row.rowIndex()) + Offset);
    view_->horizontalScrollBar()->setValue(Anchor.horizontal_);
}

int QtitanColumnAdapter::OneLineNameWidth(int Available) const
{
    return fittedWidths(Available, true).value(ids_.value(nameColumn_), {Available, 1, Available}).width_;
}

void QtitanColumnAdapter::Request()
{
    if (!applying_ && !timer_.isActive()) timer_.start();
}

int QtitanColumnAdapter::contentWidth(const QString& Id) const
{
    const auto Definition = std::find_if(definition_.columns_.begin(), definition_.columns_.end(), [&](const auto& Column) {
        return hc::columns::IdText(Column.id_) == Id;
    });
    if (Definition == definition_.columns_.end() || !Definition->template_.isEmpty()) return 0;
    const auto& Field = Definition->fieldId_;
    QStringList Samples;
    if (Field == "file.created" || Field == "file.modified" || Field == "file.accessed")
        Samples.append(hc::columns::Format(hc::columns::CellValue::Ready(QDateTime(QDate(2000, 12, 28), QTime(23, 59, 59))), Definition->format_));
    else if (Field == "file.attributes") Samples.append("-----");
    else if (Field == "file.size") {
        // Include four-digit values around unit boundaries, not just 9.09 TiB.
        for (quint64 Value : {9999999999999ULL, 1048575ULL, 1073741823ULL, 1099511627775ULL})
            Samples.append(hc::columns::Format(hc::columns::CellValue::Ready(Value), Definition->format_));
        Samples.append(QObject::tr("<폴더>")); Samples.append(QObject::tr("<링크>"));
    } else return 0;
    const QFontMetrics Metrics(grid_->font());
    int TextWidth = 0;
    for (const auto& Sample : Samples) TextWidth = std::max(TextWidth, Metrics.horizontalAdvance(Sample));
    // These direct built-in metadata fields are already in the listing. Honor
    // provider display strings too, without evaluating templates or plugin fields.
    // Bound the work on large listings and include the focused area when scrolled.
    if (const auto* Model = view_->model()) {
        const int Column = ids_.key(Id, -1);
        const int Count = std::min(128, Model->rowCount());
        for (int Sample = 0; Column >= 0 && Sample < Count; ++Sample) {
            const int Row = int(qint64(Sample) * Model->rowCount() / Count);
            TextWidth = std::max(TextWidth, Metrics.horizontalAdvance(Model->index(Row, Column).data().toString()));
        }
        const int Focus = view_->focusedRowIndex();
        for (int Row = std::max(0, Focus - 32); Column >= 0 && Row < std::min(view_->getRowCount(), Focus + 64); ++Row)
            TextWidth = std::max(TextWidth, Metrics.horizontalAdvance(view_->getRow(Row).modelIndex(Column).data().toString()));
    }
    return std::max(Metrics.horizontalAdvance(Definition->title_) + 28, TextWidth + textPadding_);
}

std::optional<ColumnWidthResult> QtitanColumnAdapter::resolveWidth(const QString& Id, int Available,
    const ColumnWidthPolicy& Policy, const ColumnWidthPolicy& Default, bool MetadataRow) const
{
    auto Result = Resolve(Policy, Available);
    if (Result && Policy.mode_ == WidthMode::Flexible) {
        const bool DefaultWidth = Policy == Default;
        if (const int Content = contentWidth(Id); Content > 0) {
            // The separate metadata line has its own space budget. Give content
            // its natural width before fitting, even if an older ratio was tiny.
            if (DefaultWidth || (Policy.unit_ == WidthUnit::Percent && !Policy.minimum_ && !Policy.maximum_)
                || (MetadataRow && !Policy.maximum_)) {
                Result->minimum_ = std::clamp(Content, Result->minimum_, Result->maximum_);
                Result->width_ = Result->minimum_;
            }
        } else if (defaultPresentation_ && DefaultWidth) {
            Result->minimum_ = std::max(Result->minimum_, readableFloors_.value(Id));
            Result->width_ = std::max(Result->width_, Result->minimum_);
            Result->maximum_ = std::max(Result->maximum_, Result->minimum_);
        }
    }
    return Result;
}

QMap<QString, ColumnWidthResult> QtitanColumnAdapter::fittedWidths(int Available, bool OneLine) const
{
    const bool Details = OneLine || layoutMode_ == "details";
    const auto Defaults = OneLine ? defaultsForLayout("details") : defaults_;
    QMap<QString, ColumnWidthResult> Result;
    for (int Row = 0; Row < (Details ? 1 : 2); ++Row) {
        QStringList Ids;
        std::vector<ColumnWidthAllocation> Columns;
        int Primary = -1;
        for (auto It = ids_.begin(); It != ids_.end(); ++It) {
            const auto* Column = view_->getColumnByModelColumn(It.key());
            if (!Column) continue;
            const auto& Id = It.value();
            auto Policy = settings_.columns_.value(Id);
            if (OneLine && layoutMode_ != "details") {
                Policy = Defaults.value(Id);
                if (const auto Saved = DecodeWidth(layoutWidths_.value("details").toObject().value(Id).toObject())) Policy = *Saved;
            }
            if (Policy.hidden_ || (!Details && (It.key() == nameColumn_ ? 0 : 1) != Row)) continue;
            auto Width = resolveWidth(Id, Available, Policy, Defaults.value(Id), !Details && It.key() != nameColumn_);
            if (!Width) continue;
            if (!Details && It.key() != nameColumn_ && Column->visualIndex() == 1 && Policy.mode_ == WidthMode::Flexible) {
                Width->width_ = std::min(Width->maximum_, Width->width_ + leadingIndent_);
                Width->minimum_ = std::min(Width->width_, Width->minimum_ + leadingIndent_);
            }
            if (It.key() == nameColumn_) {
                Primary = int(Columns.size());
                if (!Details) Width = ColumnWidthResult{Available, 1, Available};
            }
            Ids.append(Id);
            Columns.push_back({*Width, (Details || It.key() != nameColumn_) && Policy.mode_ == WidthMode::Fixed});
        }
        const auto Fitted = FitColumnWidths(Columns, Available, Primary);
        for (int Index = 0; Index < Ids.size(); ++Index) Result[Ids[Index]] = Fitted[Index];
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
    if (defaultPresentation_ && !referencePresentation_ && layoutMode_ != "details") {
        int Position = 1;
        for (const auto* Field : {"file.type", "file.size", "file.modified", "file.attributes"})
            for (const auto& Definition : definition_.columns_) if (Definition.fieldId_ == Field) {
                const auto ModelColumn = ids_.key(hc::columns::IdText(Definition.id_), -1);
                if (auto* Column = view_->getColumnByModelColumn(ModelColumn)) Column->setVisualIndex(Position++);
            }
    }
    // Reordering metadata changes which cell shares the icon's horizontal slot.
    // Size both the header and body after that order is established.
    for (auto It = ids_.begin(); It != ids_.end(); ++It)
        if (auto* Column = static_cast<Qtitan::GridTableColumn*>(view_->getColumnByModelColumn(It.key())))
            Column->setCaptionIndent(layoutMode_ != "details" && (It.key() == nameColumn_ || Column->visualIndex() == 1) ? leadingIndent_ : 0);
    const auto Widths = fittedWidths(Available);
    for (auto It = ids_.begin(); It != ids_.end(); ++It)
    {
        auto* Column = view_->getColumnByModelColumn(It.key());
        if (!Column || !settings_.columns_.contains(It.value())) continue;
        auto* Banded = static_cast<Qtitan::GridBandedTableColumn*>(Column);
        const int Row = layoutMode_ == "details" ? 0 : (It.key() == nameColumn_ ? (layoutMode_ == "name-first" ? 0 : 1) : (layoutMode_ == "name-first" ? 1 : 0));
        if (Banded->rowIndex() != Row) Banded->setRowIndex(Row);
        const auto& Policy = settings_.columns_[It.value()];
        if (Column->isVisible() == Policy.hidden_) Column->setVisible(!Policy.hidden_);
        const auto Width = Widths.constFind(It.value());
        if (Width == Widths.end()) continue;
        Column->setAutoWidth(false);
        Column->setHorSizingEnabled(Policy.mode_ == WidthMode::Flexible && (layoutMode_ == "details" || It.key() != nameColumn_));
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
        const auto Width = Widths.constFind(It.value());
        if (Width == Widths.end()) continue;
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

bool QtitanColumnAdapter::save() { return transient_ || workspaceOwned_ || (!profile_.isEmpty() && SaveWidths(hc::settings::AppSettings(), profile_, settings_)); }

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


QString QtitanColumnAdapter::PaneKey() const
{
    for (auto* Parent = owner_.data(); Parent; Parent = Parent->parentWidget())
        if (Parent->objectName() == "cmpLeftPanel" || Parent->objectName() == "cmpRightPanel") return Parent->objectName();
    return "default";
}

hc::columns::ColumnProfile QtitanColumnAdapter::Profile() const
{
    auto Result = definition_;
    Result.mode_ = hc::columns::ParseMode(layoutMode_).value_or(hc::columns::LayoutMode::Details);
    for (auto& Column : Result.columns_) {
        const auto Id = hc::columns::IdText(Column.id_);
        for (int Mode = 0; Mode < 3; ++Mode) {
            const auto Name = hc::columns::ModeName(static_cast<hc::columns::LayoutMode>(Mode));
            const auto Policy = DecodeWidth(layoutWidths_[Name].toObject()[Id].toObject());
            if (Policy) Column.widths_[Mode] = *Policy;
        }
        Column.widths_[int(Result.mode_)] = settings_.columns_.value(Id, Column.widths_[int(Result.mode_)]);
    }
    return Result;
}

void QtitanColumnAdapter::UseProfileWidths()
{
    for (int Mode = 0; Mode < 3; ++Mode) {
        QJsonObject Values;
        for (const auto& Column : definition_.columns_) Values[hc::columns::IdText(Column.id_)] = EncodeWidth(Column.widths_[Mode]);
        layoutWidths_[hc::columns::ModeName(static_cast<hc::columns::LayoutMode>(Mode))] = Values;
    }
    workspaceOwned_ = true;
    activateLayout(hc::columns::ModeName(definition_.mode_));
}
