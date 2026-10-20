#include "stdafx.h"
#include "cmpView.hpp"

#include "builtInFsModel.hpp"
#include "qtitanColumnAdapter.hpp"
#include "qtWorkspaceState.hpp"
#include "thumbnailModel.hpp"
#include <QtitanGrid.h>
#include <QtnGridViewGraphics.h>

bool CGridView::GroupingActive() const { return view_ && view_->groupColumnCount() > 0; }
bool CGridView::IsGroupFocused() const
{
    const auto Row = view_->focusedRow();
    return Row.isValid() && Row.type() == Qtitan::GridRow::GroupRow;
}
QString CGridView::sourceSelectionKey(QModelIndex Index) const
{
    while (const auto* Proxy = qobject_cast<const QAbstractProxyModel*>(Index.model())) Index = Proxy->mapToSource(Index);
    if (!Index.isValid() || Index.model() != model_) return {};
    const auto Item = model_->GetFileInfo(Index);
    return Item && Item->storageEntry_ && Item->Name_ != ".." ? hc::workspace::SelectionKey(*Item->storageEntry_) : QString{};
}
QModelIndexList CGridView::SelectionCandidates() const
{
    QModelIndexList Result;
    if (!model_) return Result;
    for (int Row = 0; Row < model_->rowCount(); ++Row) {
        const auto Index = model_->index(Row, 0);
        if (proxy_ && !proxy_->mapFromSource(Index).isValid()) continue;
        Result.append(Index);
    }
    return Result;
}
QModelIndexList CGridView::SelectedSourceIndexes() const
{
    QModelIndexList Result;
    if (!GroupingActive()) {
        for (auto Index : view_->selection()->selectedRowIndexes()) {
            while (const auto* Proxy = qobject_cast<const QAbstractProxyModel*>(Index.model())) Index = Proxy->mapToSource(Index);
            if (Index.isValid() && Index.model() == model_) Result.append(Index);
        }
    } else for (const auto& Index : SelectionCandidates())
        if (groupedSelection_.contains(sourceSelectionKey(Index))) Result.append(Index);
    return Result;
}
bool CGridView::IsSourceSelected(const QModelIndex& Index) const
{
    if (GroupingActive()) return groupedSelection_.contains(sourceSelectionKey(Index));
    const auto Visible = proxy_ ? proxy_->mapFromSource(Index) : Index;
    const auto Row = view_->getRow(Visible);
    return Row.isValid() && Row.type() == Qtitan::GridRow::DataRow && view_->modelController()->isRowSelected(Row);
}
void CGridView::projectGroupedSelection()
{
    if (!GroupingActive()) return;
    QScopedValueRollback Guard(groupingProjection_, true);
    view_->beginUpdate(); view_->deselectAll();
    const auto* First = view_->getFirstColumn(); const auto* Last = view_->getLastColumn();
    int Start = -1;
    for (int Position = 0; Position <= view_->getRowCount(); ++Position) {
        const auto Row = Position < view_->getRowCount() ? view_->getRow(Position) : Qtitan::GridRow{};
        const bool Selected = Row.isValid() && Row.type() == Qtitan::GridRow::DataRow
            && groupedSelection_.contains(sourceSelectionKey(Row.modelIndex(0)));
        if (Selected && Start < 0) Start = Position;
        if (!Selected && Start >= 0) {
            view_->selectRange(Start, Position - 1, First ? First->index() : -1, Last ? Last->index() : -1, Qtitan::Select);
            Start = -1;
        }
    }
    view_->endUpdate(); selectionSummaryTimer_.start(0);
}
void CGridView::initializeGrouping()
{
    groupingTimer_.setSingleShot(true);
    connect(&groupingTimer_, &QTimer::timeout, this, &CGridView::applyGrouping);
    groupingDateTimer_.setInterval(30000);
    connect(&groupingDateTimer_, &QTimer::timeout, this, [this] {
        if (isVisible()) if (auto* Model = qobject_cast<CFSModel*>(model_)) Model->RefreshGroupingDate();
    });
    groupingDateTimer_.start();
    // Native completion signals precede row remapping in the controller update guard.
    groupInteractionTimer_.setSingleShot(true);
    connect(&groupInteractionTimer_, &QTimer::timeout, this, [this] {
        groupInteractionPending_ = false;
        const auto Focus = groupInteractionFocus_.isEmpty() ? CaptureFocus() : groupInteractionFocus_;
        groupInteractionFocus_.clear(); RestoreSelection(CaptureSelection(), Focus);
        emit sigViewSettingsChanged();
    });
    connect(view_, &Qtitan::GridViewBase::groupExpanding, this, [this](Qtitan::GridModelGroup*, bool&) {
        if (!groupingProjection_) groupInteractionPending_ = true;
    });
    connect(view_, &Qtitan::GridViewBase::groupCollapsing, this, [this](Qtitan::GridModelGroup* Group, bool&) {
        if (groupingProjection_) return;
        userGroupCollapse_ = true; groupingProjection_ = true; groupInteractionPending_ = true;
        const auto Focus = view_->focusedRow(); groupCollapseFocus_.clear();
        if (Focus.isValid() && Focus.type() == Qtitan::GridRow::DataRow && Focus.modelGroup() == Group)
            groupCollapseFocus_ = CaptureFocus();
        if (!groupCollapseFocus_.isEmpty()) groupLastFocus_[Group->_groupValue.toString()] = groupCollapseFocus_;
        if (groupExpansion_.size() >= 4096 && !groupExpansion_.contains(Group->_groupValue.toString())) groupExpansion_.remove(groupExpansion_.begin().key());
        if (groupLastFocus_.size() > 4096) groupLastFocus_.erase(groupLastFocus_.begin());
        groupExpansion_[Group->_groupValue.toString()] = false;
    });
    connect(view_, &Qtitan::GridViewBase::groupCollapsed, this, [this](Qtitan::GridModelGroup* Group) {
        if (!userGroupCollapse_) return;
        userGroupCollapse_ = false; groupingProjection_ = false;
        groupInteractionFocus_ = groupCollapseFocus_.isEmpty() ? QString{} : "@group:" + Group->_groupValue.toString();
        groupCollapseFocus_.clear(); groupInteractionTimer_.start(0);
    });
    connect(view_, &Qtitan::GridViewBase::groupExpanded, this, [this](Qtitan::GridModelGroup* Group) {
        if (groupingProjection_) return;
        const auto Key = Group->_groupValue.toString();
        if (groupExpansion_.size() >= 4096 && !groupExpansion_.contains(Key)) groupExpansion_.remove(groupExpansion_.begin().key());
        groupExpansion_[Key] = true;
        groupInteractionFocus_ = groupLastFocus_.value(Key);
        groupInteractionTimer_.start(0);
    });
}
bool CGridView::SetGrouping(const hc::columns::GroupingState& State)
{
    if (State.enabled_) {
        const auto Profile = ColumnProfile();
        const auto It = std::find_if(Profile.columns_.begin(), Profile.columns_.end(), [&](const auto& Column) { return Column.id_ == State.columnId_; });
        if (It == Profile.columns_.end() || !hc::columns::SupportsGrouping(*It) || !hc::columns::SupportsGroupingMode(It->fieldId_, State.mode_)) return false;
    }
    const auto Selected = CaptureSelection(); const auto Focus = CaptureFocus(); const auto Scroll = CaptureScrollState();
    if (grouping_.columnId_ != State.columnId_ || grouping_.mode_ != State.mode_) { groupExpansion_ = {}; groupLastFocus_.clear(); groupsExpandedByDefault_ = true; }
    grouping_ = State; unsupportedGrouping_ = {}; applyGrouping();
    RestoreSelection(Selected, Focus); RestoreScrollState(Scroll);
    requestPresentation(); emit sigViewSettingsChanged();
    return true;
}
void CGridView::applyGrouping()
{
    if (!model_ || groupingUpdateDepth_ || groupingProjection_) return;
    groupingTimer_.stop();
    groupingReset_ = false; groupInteractionTimer_.stop(); groupInteractionPending_ = false; groupInteractionFocus_.clear();
    const auto Selected = CaptureSelection(); const auto Focus = CaptureFocus();
    QScopedValueRollback Guard(groupingProjection_, true);
    auto* Model = qobject_cast<CFSModel*>(model_);
    const auto Context = Model ? (Model->IsSearchResults() ? "search:" + Model->EverythingQuery()
        : hc::workspace::LocationKey(Model->WorkspaceLocation())) : QString{};
    if (groupingContext_ != Context) { groupExpansion_ = {}; groupLastFocus_.clear(); groupsExpandedByDefault_ = true; groupingContext_ = Context; }
    Qtitan::GridColumn* Target = nullptr;
    const auto Profile = ColumnProfile();
    for (const auto& Definition : Profile.columns_)
        if (Definition.id_ == grouping_.columnId_ && hc::columns::SupportsGrouping(Definition)
            && hc::columns::SupportsGroupingMode(Definition.fieldId_, grouping_.mode_))
            for (int Column = 0; Column < model_->columnCount(); ++Column)
                if (QUuid(columnAdapter_->ColumnId(Column)) == Definition.id_) Target = static_cast<Qtitan::GridColumn*>(view_->getColumnByModelColumn(Column));
    const bool Active = Model && grouping_.enabled_ && unsupportedGrouping_.isEmpty() && Target && !IsThumbnailMode();
    auto Effective = grouping_; Effective.enabled_ = Active;
    view_->beginUpdate();
    // Pin relocation searches the currently sorted records. Move the parent
    // before changing group keys or sort ranks inside the deferred update.
    for (int Row = 0; Row < model_->rowCount(); ++Row) if (model_->GetName(model_->index(Row, 0)) == "..") {
        const auto Index = proxy_ ? proxy_->mapFromSource(model_->index(Row, 0)) : model_->index(Row, 0);
        if (Index.isValid()) view_->modelController()->setIndexPinnedPosition(Index, Active ? Qtitan::AtBeginning : Qtitan::AtNone);
        break;
    }
    if (Model) Model->SetGrouping(Effective);
    if (!Active || view_->groupColumnCount() != 1 || view_->groupColumnByIndex(0) != Target) {
        view_->clearGrouping();
        for (int Column = 0; Column < model_->columnCount(); ++Column)
            if (auto* Item = static_cast<Qtitan::GridColumn*>(view_->getColumnByModelColumn(Column))) Item->dataBinding()->setGroupingRoles();
        if (Active) {
            Target->dataBinding()->setGroupingRoles(CFSModel::GroupKeyRole, CFSModel::GroupOrderRole, CFSModel::GroupTextRole);
            view_->options().setColumnHidingOnGroupingEnabled(false);
            view_->options().setGroupKeepLevelExpanded(0);
            Target->setGroupIndex(0);
        }
    }
    if (Active) {
        view_->modelController()->setGroupOrder(Target->dataBinding(), 0,
            grouping_.order_ == Qt::AscendingOrder ? Qtitan::SortAscending : Qtitan::SortDescending, false, true);
        groupedSelection_ = QSet<QString>(Selected.begin(), Selected.end());
    }
    // Configuration changes can invalidate the previous comparison order used
    // by native pin relocation. Rebuild once from source identities here;
    // incremental search publication continues through ColumnValuesChanged.
    view_->modelController()->refresh();
    view_->endUpdate();
    if (Active) applyGroupExpansion();
    RestoreSelection(Selected, Focus);
    columnAdapter_->Request();
    if (IsThumbnailMode()) thumbnailModel_->Schedule();
}
void CGridView::applyGroupExpansion()
{
    if (!GroupingActive()) return;
    QScopedValueRollback Guard(groupingProjection_, true);
    // Expansion invalidates visible row positions, but group storage remains stable.
    view_->beginUpdate();
    const int Count = view_->modelController()->groupRows().size();
    for (int Index = 0; Index < Count; ++Index) {
        const auto* Group = &view_->modelController()->groupRows()[Index];
        const auto Key = Group->_groupValue.toString();
        const bool Expanded = groupExpansion_.value(Key).toBool(groupsExpandedByDefault_);
        if (Expanded != Group->expanded) {
            if (Expanded) view_->modelController()->setGroupExpanded(Index, true);
            else view_->modelController()->setGroupExpanded(Index, false);
        }
    }
    view_->endUpdate();
}
void CGridView::RestoreGroupExpansion(const QJsonObject& State)
{
    applyGrouping();
    groupsExpandedByDefault_ = State.value("defaultExpanded").toBool(true);
    groupExpansion_ = {}; const auto Values = State.value("groups").toObject();
    for (auto It = Values.begin(); It != Values.end() && groupExpansion_.size() < 4096; ++It)
        if (It.value().isBool()) groupExpansion_.insert(It.key(), It.value());
    applyGroupExpansion(); projectGroupedSelection();
}
void CGridView::SetGroupsExpanded(bool Expanded)
{
    if (!GroupingActive()) return;
    groupsExpandedByDefault_ = Expanded; groupExpansion_ = {};
    const auto Focus = CaptureFocus(); applyGroupExpansion(); RestoreSelection(CaptureSelection(), Focus);
    emit sigViewSettingsChanged();
}
void CGridView::showGroupingMenu(Qtitan::ContextMenuEventArgs* Args)
{
    const auto Hit = Args->hitInfo();
    auto* Menu = Args->contextMenu();
    // Default grid actions are disconnected by the file panel. Expose its supported commands.
    Menu->clear();
    if (Hit.info() == Qtitan::GridHitInfo::Column) {
        auto* Columns = Menu->addAction(tr("컬럼 표시/숨기기...")); Columns->setObjectName("hc.columns.visibility");
        connect(Columns, &QAction::triggered, this, [this] {
            const auto Position = QCursor::pos();
            QTimer::singleShot(0, this, [this, Position] { view_->showQuickColumnsCustomization(Position); });
        });
        Menu->addSeparator();
    }
    Menu->setObjectName("hc.grouping.menu");
    Qtitan::GridColumn* Column = Hit.info() == Qtitan::GridHitInfo::Column
        ? qobject_cast<Qtitan::GridColumn*>(view_->getColumn(Hit.columnIndex())) : nullptr;
    const auto Id = Column ? QUuid(columnAdapter_->ColumnId(Column->dataBinding()->column())) : QUuid{};
    const auto Profile = ColumnProfile();
    const auto It = std::find_if(Profile.columns_.begin(), Profile.columns_.end(), [&](const auto& Item) { return Item.id_ == Id; });
    if (It != Profile.columns_.end()) {
        if (hc::columns::SupportsGrouping(*It)) {
            auto* GroupMenu = Menu->addMenu(tr("이 컬럼으로 그룹화")); GroupMenu->setObjectName("hc.grouping.byColumn");
            const auto Modes = hc::columns::IsGroupingDate(It->fieldId_) ? QStringList{"relativeDate", "date", "month"} : QStringList{"value"};
            for (const auto& Mode : Modes) {
                const auto Text = Mode == "relativeDate" ? tr("상대 날짜별") : Mode == "date" ? tr("날짜별") : Mode == "month" ? tr("월별") : tr("값별");
                auto* Action = GroupMenu->addAction(Text); Action->setObjectName("hc.grouping." + Mode);
                connect(Action, &QAction::triggered, this, [this, Id, Mode] {
                    SetGrouping({true, Id, Mode, Mode == "value" ? Qt::AscendingOrder : Qt::DescendingOrder});
                });
            }
        } else { auto* Action = Menu->addAction(tr("이 컬럼은 그룹화를 지원하지 않습니다")); Action->setEnabled(false); }
    }
    auto* Clear = Menu->addAction(tr("그룹화 해제")); Clear->setObjectName("hc.grouping.clear"); Clear->setEnabled(grouping_.enabled_ || !unsupportedGrouping_.isEmpty());
    connect(Clear, &QAction::triggered, this, [this] { SetGrouping({}); });
    for (const auto Order : {Qt::AscendingOrder, Qt::DescendingOrder}) {
        auto* Action = Menu->addAction(Order == Qt::AscendingOrder ? tr("그룹 오름차순") : tr("그룹 내림차순"));
        Action->setObjectName(Order == Qt::AscendingOrder ? "hc.grouping.ascending" : "hc.grouping.descending");
        Action->setEnabled(grouping_.enabled_); Action->setCheckable(true); Action->setChecked(grouping_.enabled_ && grouping_.order_ == Order);
        connect(Action, &QAction::triggered, this, [this, Order] { auto State = grouping_; State.order_ = Order; SetGrouping(State); });
    }
    Menu->addSeparator();
    for (const bool Expanded : {true, false}) {
        auto* Action = Menu->addAction(Expanded ? tr("모두 펼치기") : tr("모두 접기")); Action->setEnabled(GroupingActive());
        Action->setObjectName(Expanded ? "hc.grouping.expandAll" : "hc.grouping.collapseAll");
        connect(Action, &QAction::triggered, this, [this, Expanded] { SetGroupsExpanded(Expanded); });
    }
    if (IsThumbnailMode()) { auto* Hint = Menu->addAction(tr("그룹 제목은 상세 목록에서 표시됩니다")); Hint->setEnabled(false); }
}
