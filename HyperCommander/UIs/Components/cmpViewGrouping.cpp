#include "stdafx.h"
#include "cmpView.hpp"
#include "UniqueLibs/productDefaults.hpp"

#include "builtInFsModel.hpp"
#include "qtitanColumnAdapter.hpp"
#include "qtitanGrouping.hpp"
#include "../columnProfileSync.hpp"
#include "UniqueLibs/qtMessageDialog.hpp"
#include "qtWorkspaceState.hpp"
#include "thumbnailModel.hpp"
#include <QtitanGrid.h>
#include <QtnGridViewGraphics.h>

bool CGridView::GroupingActive() const { return view_ && view_->groupColumnCount() > 0; }
QString CGridView::ColumnSourceText() const
{
    const auto Profile = ColumnProfile();
    const auto Source = flatProfile_ ? tr("평면 · %1").arg(flatSource_) : manualColumns_ ? tr("고정") : automaticColumnRule_ >= 0 ? tr("자동 · %1번 조건").arg(automaticColumnRule_ + 1) : tr("자동");
    QStringList Groups;
    for (const auto& Level : groupingLevels_) for (const auto& Definition : Profile.columns_) if (Definition.id_ == Level.columnId_) {
        const auto Mode = Level.mode_ == "relativeDate" ? tr("상대 날짜") : Level.mode_ == "date" ? tr("날짜") : Level.mode_ == "month" ? tr("월") : tr("값");
        Groups.append(tr("%1(%2)").arg(Definition.title_, Mode)); break;
    }
    return tr("열 세트: %1 (%2) · 그룹: %3 (%4)").arg(Profile.title_, Source,
        Groups.isEmpty() ? tr("없음") : Groups.join(" → "), groupingBySet_.contains(Profile.id_) ? tr("이 탭에서 변경") : tr("세트 기본값"));
}
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
    connect(view_, &Qtitan::GridViewBase::groupingChanged, this, [this] {
        if (groupingProjection_ || groupingUpdateDepth_ || restoringPresentation_) return;
        QVector<hc::columns::GroupingState> Levels;
        for (int Index = 0; Index < view_->groupColumnCount(); ++Index) {
            auto* Column = view_->groupColumnByIndex(Index);
            const QUuid Id(Column->property("hc.columnId").toString());
            const auto Profile = ColumnProfile();
            const auto It = std::find_if(Profile.columns_.begin(), Profile.columns_.end(), [&](const auto& Item) { return Item.id_ == Id; });
            if (It == Profile.columns_.end() || !hc::columns::SupportsGrouping(*It) || Index >= 3) { groupingTimer_.start(); return; }
            auto Level = hc::columns::GroupingState{true, Id, hc::columns::IsGroupingDate(It->fieldId_) ? "relativeDate" : "value", Qt::AscendingOrder};
            for (const auto& Saved : groupingLevels_) if (Saved.columnId_ == Id) Level = Saved;
            if (auto* Rule = view_->modelController()->sortRules()->groupRule(Index)) Level.order_ = Rule->order == Qtitan::SortDescending ? Qt::DescendingOrder : Qt::AscendingOrder;
            Levels.append(Level);
        }
        QTimer::singleShot(0, this, [this, Levels] { if (!groupingProjection_ && Levels != groupingLevels_) SetGroupingLevels(Levels); });
    });
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
        if (Focus.isValid() && Focus.type() == Qtitan::GridRow::DataRow && Focus.modelGroup() && Focus.modelGroup()->firstRecord >= Group->firstRecord && Focus.modelGroup()->lastRecord <= Group->lastRecord)
            groupCollapseFocus_ = CaptureFocus();
        if (!groupCollapseFocus_.isEmpty()) groupLastFocus_[hc::columns::NativeGroupKey(view_->modelController(), Group)] = groupCollapseFocus_;
        if (groupExpansion_.size() >= 4096 && !groupExpansion_.contains(hc::columns::NativeGroupKey(view_->modelController(), Group))) groupExpansion_.remove(groupExpansion_.begin().key());
        if (groupLastFocus_.size() > 4096) groupLastFocus_.erase(groupLastFocus_.begin());
        groupExpansion_[hc::columns::NativeGroupKey(view_->modelController(), Group)] = false;
    });
    connect(view_, &Qtitan::GridViewBase::groupCollapsed, this, [this](Qtitan::GridModelGroup* Group) {
        if (!userGroupCollapse_) return;
        userGroupCollapse_ = false; groupingProjection_ = false;
        groupInteractionFocus_ = groupCollapseFocus_.isEmpty() ? QString{} : "@group:" + hc::columns::NativeGroupKey(view_->modelController(), Group);
        groupCollapseFocus_.clear(); groupInteractionTimer_.start(0);
    });
    connect(view_, &Qtitan::GridViewBase::groupExpanded, this, [this](Qtitan::GridModelGroup* Group) {
        if (groupingProjection_) return;
        const auto Key = hc::columns::NativeGroupKey(view_->modelController(), Group);
        if (groupExpansion_.size() >= 4096 && !groupExpansion_.contains(Key)) groupExpansion_.remove(groupExpansion_.begin().key());
        groupExpansion_[Key] = true;
        groupInteractionFocus_ = groupLastFocus_.value(Key);
        groupInteractionTimer_.start(0);
    });
}
bool CGridView::SetGrouping(const hc::columns::GroupingState& State)
{
    return SetGroupingLevels(State.enabled_ ? QVector<hc::columns::GroupingState>{State} : QVector<hc::columns::GroupingState>{});
}
bool CGridView::SetGroupingLevels(const QVector<hc::columns::GroupingState>& Levels)
{
    auto Check = ColumnProfile(); Check.grouping_ = hc::columns::ProfileGrouping{}; Check.grouping_->levels_ = Levels;
    if (!hc::columns::ValidateGrouping(Check).isEmpty()) return false;
    const auto Selected = CaptureSelection(); const auto Focus = CaptureFocus(); const auto Scroll = CaptureScrollState();
    if (groupingLevels_ != Levels) { groupExpansion_ = {}; groupLastFocus_.clear(); groupsExpandedByDefault_ = expandPolicy_ != hc::columns::ProfileGrouping::Expand::None; }
    groupingLevels_ = Levels; grouping_ = Levels.isEmpty() ? hc::columns::GroupingState{} : Levels.front();
    unsupportedGrouping_ = {}; rememberGrouping(); applyGrouping();
    RestoreSelection(Selected, Focus); RestoreScrollState(Scroll);
    requestPresentation(); emit sigViewSettingsChanged();
    return true;
}
void CGridView::rememberGrouping()
{
    const auto Profile = ColumnProfile(); const auto Default = hc::columns::DefaultGroupingLevels(Profile);
    groupingBySetOrder_.removeAll(Profile.id_);
    if (groupingLevels_ == Default) groupingBySet_.remove(Profile.id_);
    else {
        groupingBySet_[Profile.id_] = groupingLevels_; groupingBySetOrder_.prepend(Profile.id_);
        while (groupingBySetOrder_.size() > 16) groupingBySet_.remove(groupingBySetOrder_.takeLast());
    }
}
void CGridView::applySetGrouping(const hc::columns::ColumnProfile& Profile)
{
    auto Next = hc::columns::DefaultGroupingLevels(Profile);
    if (groupingBySet_.contains(Profile.id_)) {
        const auto Saved = groupingBySet_[Profile.id_];
        auto Check = Profile; Check.grouping_ = hc::columns::ProfileGrouping{};
        Check.grouping_->levels_ = Saved;
        if (hc::columns::ValidateGrouping(Check).isEmpty()) {
            Next = Saved; groupingBySetOrder_.removeAll(Profile.id_); groupingBySetOrder_.prepend(Profile.id_);
        } else { groupingBySet_.remove(Profile.id_); groupingBySetOrder_.removeAll(Profile.id_); }
    }
    const auto Options = Profile.grouping_ && hc::columns::ValidateGrouping(Profile).isEmpty()
        ? *Profile.grouping_ : hc::columns::ProfileGrouping{};
    const bool Changed = groupingLevels_ != Next || expandPolicy_ != Options.expand_;
    expandPolicy_ = Options.expand_; hideGroupedColumn_ = Options.hideGroupedColumn_;
    if (Changed) {
        groupExpansion_ = {}; groupLastFocus_.clear();
        groupsExpandedByDefault_ = expandPolicy_ != hc::columns::ProfileGrouping::Expand::None;
    }
    groupingLevels_ = Next; grouping_ = Next.isEmpty() ? hc::columns::GroupingState{} : Next.front(); unsupportedGrouping_ = {};
}
void CGridView::focusFirstEntry()
{
    QScopedValueRollback Guard(groupingProjection_, true);
    const auto Focus = [this](const QModelIndex& Source, int Position) {
        const auto Key = sourceSelectionKey(Source);
        if (groupingUpdateDepth_ && !Key.isEmpty()) queryFocus_ = Key;
        view_->setFocusedRowIndex(Position, Qt::ControlModifier);
    };
    // Respect the visible sort/group order before opening a collapsed group.
    // '..' sorts first, and grouping pins it above the group headers.
    for (int Position = 0; Position < view_->getRowCount(); ++Position) {
        const auto Row = view_->getRow(Position);
        if (Row.type() != Qtitan::GridRow::DataRow) continue;
        Focus(proxy_ ? proxy_->mapToSource(Row.modelIndex(0)) : Row.modelIndex(0), Position); return;
    }
    for (const auto& Source : SelectionCandidates()) {
        const auto Position = view_->modelController()->ensureVisible(proxy_ ? proxy_->mapFromSource(Source) : Source);
        if (Position >= 0) { Focus(Source, Position); return; }
    }
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
    if (groupingContext_ != Context) { groupExpansion_ = {}; groupLastFocus_.clear(); groupsExpandedByDefault_ = expandPolicy_ != hc::columns::ProfileGrouping::Expand::None; groupingContext_ = Context; }
    QVector<Qtitan::GridColumn*> Targets; QVector<int> GroupColumns;
    const auto Profile = ColumnProfile();
    for (const auto& Level : groupingLevels_) for (int Column = 0; Column < model_->columnCount(); ++Column)
        if (QUuid(columnAdapter_->ColumnId(Column)) == Level.columnId_) {
            Targets.append(static_cast<Qtitan::GridColumn*>(view_->getColumnByModelColumn(Column))); GroupColumns.append(Column); break;
        }
    const bool Active = Model && !groupingLevels_.isEmpty() && unsupportedGrouping_.isEmpty()
        && Targets.size() == groupingLevels_.size() && !Targets.contains(nullptr) && !IsThumbnailMode();
    const auto Effective = Active ? groupingLevels_ : QVector<hc::columns::GroupingState>{};
    view_->beginUpdate();
    // Pin relocation searches the currently sorted records. Move the parent
    // before changing group keys or sort ranks inside the deferred update.
    for (int Row = 0; Row < model_->rowCount(); ++Row) if (model_->GetName(model_->index(Row, 0)) == "..") {
        const auto Index = proxy_ ? proxy_->mapFromSource(model_->index(Row, 0)) : model_->index(Row, 0);
        if (Index.isValid()) view_->modelController()->setIndexPinnedPosition(Index, Active ? Qtitan::AtBeginning : Qtitan::AtNone);
        break;
    }
    if (Model) Model->SetGroupingLevels(Effective);
    const auto Options = Profile.grouping_ && hc::columns::ValidateGrouping(Profile).isEmpty() ? *Profile.grouping_ : hc::columns::ProfileGrouping{};
    view_->options().setColumnHidingOnGroupingEnabled(hideGroupedColumn_);
    view_->options().setGroupsHeader(Options.groupsHeader_);
    bool Same = Active && view_->groupColumnCount() == Targets.size();
    for (int Level = 0; Same && Level < Targets.size(); ++Level) Same = view_->groupColumnByIndex(Level) == Targets[Level];
    if (!Same) {
        view_->clearGrouping();
        for (int Column = 0; Column < model_->columnCount(); ++Column)
            if (auto* Item = static_cast<Qtitan::GridColumn*>(view_->getColumnByModelColumn(Column))) Item->dataBinding()->setGroupingRoles();
    }
    if (Active) {
        view_->options().setGroupKeepLevelExpanded(0);
        for (int Level = 0; Level < Targets.size(); ++Level) hc::columns::ApplyNativeGroup(view_, Targets[Level], Effective[Level], Level);
        groupedSelection_ = QSet<QString>(Selected.begin(), Selected.end());
    }
    hc::columns::ConfigureGroupCaptions(view_, Active ? GroupColumns : QVector<int>{}, Options);
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
        const auto Key = hc::columns::NativeGroupKey(view_->modelController(), Group);
        const bool Expanded = groupExpansion_.value(Key).toBool(expandPolicy_ == hc::columns::ProfileGrouping::Expand::First && groupExpansion_.isEmpty() ? Index == 0 : groupsExpandedByDefault_);
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
    groupsExpandedByDefault_ = State.value("defaultExpanded").toBool(hc::defaults::DefaultValue("workspace/defaultExpanded").toBool());
    groupExpansion_ = {}; const auto Values = State.value("groups").toObject();
    for (auto It = Values.begin(); It != Values.end() && groupExpansion_.size() < 4096; ++It)
        if (It.value().isBool()) groupExpansion_.insert(It.key(), It.value());
    applyGroupExpansion(); projectGroupedSelection();
}
void CGridView::SetGroupsExpanded(bool Expanded)
{
    if (!GroupingActive()) return;
    groupsExpandedByDefault_ = Expanded; groupExpansion_ = {};
    for (const auto& Group : view_->modelController()->groupRows()) {
        if (groupExpansion_.size() >= 4096) break;
        groupExpansion_[hc::columns::NativeGroupKey(view_->modelController(), &Group)] = Expanded;
    }
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
    auto* Default = Menu->addAction(tr("세트 기본 그룹으로 되돌리기")); Default->setObjectName("hc.grouping.profileDefault");
    Default->setEnabled(groupingBySet_.contains(Profile.id_));
    connect(Default, &QAction::triggered, this, [this] {
        const auto Profile = ColumnProfile(); groupingBySet_.remove(Profile.id_); groupingBySetOrder_.removeAll(Profile.id_);
        applySetGrouping(Profile); applyGrouping(); emit sigViewSettingsChanged();
    });
    auto* Save = Menu->addAction(tr("현재 그룹을 ‘%1’ 기본값으로 저장…").arg(Profile.title_));
    Save->setObjectName("hc.grouping.saveAsProfileDefault");
    Save->setEnabled(groupingBySet_.contains(Profile.id_) && columnAdapter_->Writable() && !hc::settings::AppSettings().Editing());
    connect(Save, &QAction::triggered, this, [this] {
        using namespace hc::columns;
        auto& Settings = hc::settings::AppSettings();
        if (Settings.Editing() || !columnAdapter_->Writable()) return;
        const auto Current = ColumnProfile(); const auto Previous = Settings.CommittedObject();
        auto Columns = Previous["columns"].toObject(); auto Library = WithBuiltInProfiles(Columns["ColumnsV2/Profiles"].toObject());
        auto Updated = DecodeProfile(Library[IdText(Current.id_)].toObject());
        if (!Updated) return;
        auto Group = Updated->grouping_.value_or(ProfileGrouping{});
        Group.levels_ = groupingLevels_;
        Updated->grouping_ = Group;
        const auto Error = ValidateGrouping(*Updated);
        if (!Error.isEmpty()) { hc::ui::MessageDialog::warning(this, tr("그룹 기본값 저장"), Error); return; }
        QStringList Titles;
        for (const auto& Level : groupingLevels_) for (const auto& Column : Current.columns_) if (Column.id_ == Level.columnId_) { Titles.append(Column.title_); break; }
        const auto Title = Titles.isEmpty() ? tr("없음") : Titles.join(" → ");
        int Count = 0; for (auto* View : window()->findChildren<CGridView*>()) if (View->ColumnProfile().id_ == Current.id_) ++Count;
        if (hc::ui::MessageDialog::question(this, tr("그룹 기본값 저장"),
            tr("‘%1’ 세트의 기본 그룹을 ‘%2’로 바꿉니다. 이 세트를 쓰는 열린 탭 %3개와 새 탭에 적용됩니다.").arg(Current.title_, Title).arg(Count),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) return;
        Library[IdText(Updated->id_)] = EncodeProfile(*Updated); Columns["ColumnsV2/Profiles"] = Library;
        if (!Settings.Put(hc::settings::Section::Columns, Columns) || !Settings.Commit()) {
            Settings.Replace(Previous); hc::ui::MessageDialog::warning(this, tr("그룹 기본값 저장 실패"), tr("설정을 저장하지 못했습니다. 기존 설정을 유지합니다.")); return;
        }
        QVector<ViewSnapshot> Snapshots;
        if (!SyncOpenViews(window(), {{Updated->id_, *Updated}}, {}, Snapshots)) {
            RestoreViews(Snapshots);
            const bool Restored = Settings.Replace(Previous) && Settings.Commit();
            hc::ui::MessageDialog::warning(this, tr("그룹 기본값 적용 실패"), Restored
                ? tr("기존 설정과 탭 상태를 복구했습니다.") : tr("설정 복구에 실패했습니다. 디스크 설정을 확인하세요."));
        }
    });
    for (const auto Order : {Qt::AscendingOrder, Qt::DescendingOrder}) {
        auto* Action = Menu->addAction(Order == Qt::AscendingOrder ? tr("그룹 오름차순") : tr("그룹 내림차순"));
        Action->setObjectName(Order == Qt::AscendingOrder ? "hc.grouping.ascending" : "hc.grouping.descending");
        Action->setEnabled(grouping_.enabled_); Action->setCheckable(true); Action->setChecked(grouping_.enabled_ && grouping_.order_ == Order);
        connect(Action, &QAction::triggered, this, [this, Order] { auto Levels = groupingLevels_; if (!Levels.isEmpty()) { Levels.front().order_ = Order; SetGroupingLevels(Levels); } });
    }
    Menu->addSeparator();
    for (const bool Expanded : {true, false}) {
        auto* Action = Menu->addAction(Expanded ? tr("모두 펼치기") : tr("모두 접기")); Action->setEnabled(GroupingActive());
        Action->setObjectName(Expanded ? "hc.grouping.expandAll" : "hc.grouping.collapseAll");
        connect(Action, &QAction::triggered, this, [this, Expanded] { SetGroupsExpanded(Expanded); });
    }
    if (IsThumbnailMode()) { auto* Hint = Menu->addAction(tr("그룹 제목은 상세 목록에서 표시됩니다")); Hint->setEnabled(false); }
}
