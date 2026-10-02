#include "stdafx.h"
#include "qtitanColumnAdapter.hpp"
#include "column_Defs.hpp"
#include "cmnTypeDefs.hpp"
#include <QtitanGrid.h>

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
    defaultPresentation_ = definition_.columns_ == hc::columns::DefaultProfile().columns_;
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
    if (Result && defaultPresentation_ && Policy == defaults_.value(Id)) {
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
