#pragma once

#include <QAbstractTableModel>
#include <QElapsedTimer>
#include <QPersistentModelIndex>
#include <QStandardItemModel>
#include "../externalLibs/QtitanDataGrid/src/src/grid/QtnGridTreeView.h"
#include "../externalLibs/QtitanDataGrid/src/src/grid/QtnGridFilter.h"
#include <QtTest>
#include <QtitanGrid.h>
#include "../externalLibs/QtitanDataGrid/src/src/grid/QtnGridViewGraphics.h"
#include "../externalLibs/QtitanDataGrid/src/src/grid/QtnGridEditors.h"

namespace hc::tests {
class GridCountingModel : public QAbstractTableModel
{
public:
    explicit GridCountingModel(int Rows) { for (int Row = 0; Row < Rows; ++Row) keys_.append(Rows - Row); }
    int rowCount(const QModelIndex& Parent = {}) const override { return Parent.isValid() ? 0 : keys_.size(); }
    int columnCount(const QModelIndex& Parent = {}) const override { return Parent.isValid() ? 0 : 3; }
    QVariant data(const QModelIndex& Index, int Role = Qt::DisplayRole) const override
    {
        if (!Index.isValid()) return {};
        if (Role == Qt::UserRole) { ++sortReads_; return Index.column() == 1 ? keys_[Index.row()] / 10 : keys_[Index.row()]; }
        if (Role == Qt::DisplayRole) { if (Index.column() == 1) return keys_[Index.row()] / 10; return QString("file%1").arg(keys_[Index.row()]); }
        if (Role == Qt::ForegroundRole) return Index.column() == 0 ? QColor(Qt::red) : QColor(Qt::green);
        if (Role == Qt::BackgroundRole) return Index.column() == 0 ? QColor(Qt::black) : QColor(Qt::blue);
        if (Role == Qt::FontRole) { QFont Font; Font.setBold(Index.column() == 1); return Font; }
        if (Role == Qt::UserRole + 1) return 0.5;
        if (Role == Qt::DecorationRole) { QPixmap Pixmap(16, 16); Pixmap.fill(Qt::white); return QIcon(Pixmap); }
        return {};
    }
    void multiData(const QModelIndex& Index, QModelRoleDataSpan Roles) const override
    {
        ++multiReads_;
        for (auto& Role : Roles) Role.setData(data(Index, Role.role()));
    }
    void Change(int Row, int Key)
    {
        keys_[Row] = Key;
        emit dataChanged(index(Row, 0), index(Row, 2), {Qt::DisplayRole, Qt::UserRole});
    }
    void Touch(int First, int Last) { emit dataChanged(index(First, 0), index(Last, 2), {Qt::ForegroundRole}); }
    void Insert(int Count)
    {
        const int Start = keys_.size(); beginInsertRows({}, Start, Start + Count - 1);
        for (int Row = 0; Row < Count; ++Row) keys_.append(-Row - 1);
        endInsertRows();
    }
    void Remove(int Row) { beginRemoveRows({}, Row, Row); keys_.removeAt(Row); endRemoveRows(); }
    void Reset(int Count)
    {
        beginResetModel(); keys_.clear(); for (int Row = 0; Row < Count; ++Row) keys_.append(Row); endResetModel();
    }
    mutable int multiReads_ = 0;
    mutable int sortReads_ = 0;
private:
    QVector<int> keys_;
};

inline Qtitan::GridTableView* SetupGrid(Qtitan::Grid& Grid, QAbstractItemModel& Model)
{
    Grid.setViewType(Qtitan::Grid::TableView);
    auto* View = Grid.view<Qtitan::GridTableView>();
    View->options().setAutoCreateColumns(false);
    View->options().setRowAutoHeight(false);
    View->options().setGroupsHeader(false);
    View->setModel(&Model);
    for (int Column = 0; Column < 3; ++Column) View->addColumn(Column, QString::number(Column))->setWidth(120);
    Grid.resize(640, 400);
    return View;
}

class GridIncrementalModel : public QAbstractTableModel
{
public:
    struct Record { int key_; int caption_; bool visible_ = true; };
    explicit GridIncrementalModel(int Count)
    {
        for (int Row = 0; Row < Count; ++Row) records_.append({Row, Row / 100});
    }
    int rowCount(const QModelIndex& Parent = {}) const override { return Parent.isValid() ? 0 : records_.size(); }
    int columnCount(const QModelIndex& Parent = {}) const override { return Parent.isValid() ? 0 : 3; }
    QVariant data(const QModelIndex& Index, int Role = Qt::DisplayRole) const override
    {
        if (!Index.isValid()) return {};
        const auto& Item = records_.at(Index.row());
        if (Role == Qt::UserRole + 5) return Item.visible_;
        if (Role != Qt::UserRole && Role != Qt::DisplayRole) return {};
        if (Role == Qt::UserRole) ++sortReads_;
        if (Role == Qt::DisplayRole && Index.column() != 0) ++groupReads_;
        if (Index.column() == 0) return Item.key_;
        if (Index.column() == 1) return Role == Qt::DisplayRole ? Item.caption_ : Item.key_ / 100;
        return (Item.key_ / 10) % 10;
    }
    void ChangeRange(const QMap<int, int>& Keys, int First, int Last)
    {
        for (auto It = Keys.constBegin(); It != Keys.constEnd(); ++It)
            records_[It.key()] = {It.value(), It.value() / 100};
        emit dataChanged(index(First, 0), index(Last, 2), {Qt::DisplayRole, Qt::UserRole});
    }
    void ChangeCaption(int Row, int Caption)
    {
        records_[Row].caption_ = Caption;
        emit dataChanged(index(Row, 1), index(Row, 1), {Qt::DisplayRole});
    }
    void SetVisible(int Row, bool Visible)
    {
        records_[Row].visible_ = Visible;
        emit dataChanged(index(Row, 0), index(Row, 0), {Qt::UserRole + 5});
    }
    void Insert(int Row, int Key)
    {
        beginInsertRows({}, Row, Row); records_.insert(Row, {Key, Key / 100}); endInsertRows();
    }
    void Remove(int Row, int Count = 1)
    {
        beginRemoveRows({}, Row, Row + Count - 1); records_.remove(Row, Count); endRemoveRows();
    }
    mutable int sortReads_ = 0;
    mutable int groupReads_ = 0;
private:
    QVector<Record> records_;
};

inline QStringList GridGroupSnapshot(Qtitan::GridTableView* View)
{
    QStringList Result;
    for (int Row = 0; Row < View->getRowCount(); ++Row)
    {
        const auto Item = View->getRow(Row);
        if (Item.type() == Qtitan::GridRow::DataRow)
        {
            QStringList Path;
            for (auto Index = Item.modelIndex(); Index.isValid(); Index = Index.parent()) Path.prepend(QString::number(Index.row()));
            Result.append("row:" + Path.join('/'));
        }
        else
        {
            const auto* Group = Item.modelGroup();
            Result.append(QString("group:%1:%2:%3:%4:%5:%6:%7")
                .arg(Group->depth).arg(Group->_groupValue.toString()).arg(Group->firstRecord)
                .arg(Group->lastRecord).arg(Group->index).arg(Group->parentIndex).arg(Group->last));
        }
    }
    return Result;
}

inline void GridIncrementalTreeRegression()
{
    QStandardItemModel Model;
    const auto MakeRow = [](int Key, int Group) {
        auto* First = new QStandardItem(); First->setData(Key, Qt::DisplayRole);
        auto* Second = new QStandardItem(); Second->setData(Group, Qt::DisplayRole);
        return QList<QStandardItem*>({First, Second});
    };
    for (int Row = 0; Row < 30; ++Row)
    {
        auto Items = MakeRow(Row, Row / 10);
        for (int Child = 0; Child < 3; ++Child) Items.first()->appendRow(MakeRow(Child, 0));
        Items.first()->child(0)->appendRow(MakeRow(0, 0));
        Model.appendRow(Items);
    }
    Qtitan::TreeGrid Grid;
    Grid.setViewType(Qtitan::TreeGrid::TreeView);
    auto* View = Grid.view<Qtitan::GridTreeView>();
    View->options().setAutoCreateColumns(false);
    View->options().setRowAutoHeight(false);
    View->setModel(&Model);
    auto* KeyColumn = static_cast<Qtitan::GridTableColumn*>(View->addColumn(0, "Key"));
    auto* GroupColumn = static_cast<Qtitan::GridTableColumn*>(View->addColumn(1, "Group"));
    KeyColumn->setSortOrder(Qtitan::SortAscending);
    GroupColumn->setGroupIndex(0);
    auto* Controller = View->modelController();
    Controller->expandToLevel(3);
    View->expandAll();
    QCOMPARE(Controller->getRecordCount(), 150);
    const auto CheckTree = [&] {
        View->expandAll();
        const auto Before = GridGroupSnapshot(View);
        Controller->refresh(); View->expandAll();
        return Before == GridGroupSnapshot(View);
    };
    // Relocating an expanded root must restore its complete visible subtree.
    Model.setData(Model.index(0, 0), 80);
    QCOMPARE(Controller->getRecordCount(), 150);
    QVERIFY(CheckTree());
    Model.setData(Model.index(0, 1), 7);
    QCOMPARE(Controller->getRecordCount(), 150);
    QVERIFY(CheckTree());
    auto* Parent = Model.item(0);
    Parent->insertRow(0, MakeRow(9, 0));
    QCOMPARE(Controller->getRecordCount(), 151);
    QVERIFY(CheckTree());
    Parent->removeRow(1); // An expanded child and its grandchild disappear together.
    QCOMPARE(Controller->getRecordCount(), 149);
    QVERIFY(CheckTree());
    Model.insertRow(0, MakeRow(90, 9));
    QCOMPARE(Controller->getRecordCount(), 150);
    QVERIFY(CheckTree());
    Model.removeRow(1);
    QCOMPARE(Controller->getRecordCount(), 146);
    QVERIFY(CheckTree());
}


inline void GridIncrementalRegression()
{
    GridIncrementalModel Model(2000);
    Qtitan::Grid Grid;
    auto* View = SetupGrid(Grid, Model);
    auto* Controller = View->modelController();
    for (int Column = 0; Column < 3; ++Column)
        static_cast<Qtitan::GridTableColumn*>(View->getColumnByModelColumn(Column))->dataBinding()->setSortRole(Qt::UserRole);
    auto* KeyColumn = static_cast<Qtitan::GridTableColumn*>(View->getColumnByModelColumn(0));
    auto* ParentColumn = static_cast<Qtitan::GridTableColumn*>(View->getColumnByModelColumn(1));
    auto* ChildColumn = static_cast<Qtitan::GridTableColumn*>(View->getColumnByModelColumn(2));
    KeyColumn->setSortOrder(Qtitan::SortAscending);
    QSignalSpy Sorted(Controller, &Qtitan::GridModelController::sortCompleted);

    // A range spanning the entire model has only two changed keys. The unchanged
    // rows must not trigger the 30% full-sort fallback or enter binary insertion.
    Model.sortReads_ = 0;
    Model.ChangeRange({{10, 1801}, {1800, 10}}, 0, 1999);
    QCOMPARE(Sorted.count(), 0);
    QVERIFY(Model.sortReads_ < 2300);
    int Previous = std::numeric_limits<int>::min();
    for (int Row = 0; Row < View->getRowCount(); ++Row)
    {
        const int Key = View->getRow(Row).modelIndex().data(Qt::UserRole).toInt();
        QVERIFY(Previous <= Key); Previous = Key;
    }
    // Structural shifts must retain snapshots for surviving rows.
    Model.Insert(0, -1);
    Model.Remove(300, 2);
    Sorted.clear(); Model.sortReads_ = 0;
    Model.ChangeRange({}, 0, Model.rowCount() - 1);
    QCOMPARE(Sorted.count(), 0);
    QVERIFY(Model.sortReads_ < Model.rowCount() + 100);

    ParentColumn->setGroupIndex(0);
    ChildColumn->setGroupIndex(1);
    View->expandAll();
    // Compare incremental intervals, ancestry, last-child flags and row order
    // against the full regroup path, without relying on a visual-only assertion.
    const auto MatchesRebuild = [&] {
        View->expandAll();
        const auto Incremental = GridGroupSnapshot(View);
        Controller->refresh(); View->expandAll();
        return Incremental == GridGroupSnapshot(View);
    };
    Model.groupReads_ = 0;
    Model.Insert(100, 505);
    qInfo() << "Qtitan two-level group insert display reads:" << Model.groupReads_;
    QVERIFY(Model.groupReads_ < 300);
    QVERIFY(MatchesRebuild());
    Model.ChangeRange({{12, 7000}, {1500, 11}, {1800, 501}}, 0, Model.rowCount() - 1);
    QVERIFY(MatchesRebuild());
    Model.Remove(12); // Delete the only member of the new parent/child path.
    QVERIFY(MatchesRebuild());
    // Equal sortRole with a changed displayRole splits a group, then rejoins it.
    Model.ChangeCaption(50, 9000); QVERIFY(MatchesRebuild());
    Model.ChangeCaption(50, Model.index(50, 1).data(Qt::UserRole).toInt()); QVERIFY(MatchesRebuild());
    // Removing a separator must merge equal neighbours at every group depth.
    Model.ChangeCaption(51, 9001); QVERIFY(MatchesRebuild());
    Model.Remove(51); QVERIFY(MatchesRebuild());

    const auto FindGroup = [&](int Parent, int Child) {
        for (int Row = 0; Row < View->getRowCount(); ++Row)
        {
            const auto Item = View->getRow(Row);
            if (Item.type() != Qtitan::GridRow::GroupRow || Item.modelGroup()->depth != 1) continue;
            if (Item.groupValue().toInt() == Child && Item.parentGroupRow().groupValue().toInt() == Parent) return Item;
        }
        return Qtitan::GridRow();
    };
    auto Collapsed = FindGroup(5, 0);
    QVERIFY(Collapsed.isValid()); QVERIFY(Collapsed.setGroupExpanded(false));
    QVERIFY(FindGroup(6, 0).isGroupExpanded());
    Model.Insert(25, 508);
    QVERIFY(!FindGroup(5, 0).isGroupExpanded());
    QVERIFY(FindGroup(6, 0).isGroupExpanded());
    Model.Insert(26, 608);
    QVERIFY(!FindGroup(5, 0).isGroupExpanded());
    QVERIFY(FindGroup(6, 0).isGroupExpanded());

    // Preserve model identity for selected, focused, and pinned rows as records move.
    View->expandAll();
    View->options().setSelectionPolicy(Qtitan::GridViewOptions::MultiRowSelection);
    const QPersistentModelIndex Selected(Model.index(400, 0));
    View->selectRow(View->getRow(Selected).rowIndex());
    View->setFocusedRowIndex(View->getRow(Selected).rowIndex());
    Model.ChangeRange({{Selected.row(), 850}}, 0, Model.rowCount() - 1);
    QVERIFY(View->selection()->selectedRowIndexes().contains(Selected));
    QCOMPARE(Controller->focusedRow().modelIndex(), QModelIndex(Selected));
    View->getRow(Selected).setPinnedPosition(Qtitan::AtBeginning);
    Model.Insert(0, 2100); QVERIFY(MatchesRebuild());
    QCOMPARE(Controller->getPinnedGridRowAtBeginningCount(), 1);
    View->getRow(Selected).setPinnedPosition(Qtitan::AtEnd);
    Model.Remove(0); QVERIFY(MatchesRebuild());
    QCOMPARE(Controller->getPinnedGridRowAtEndCount(), 1);
    View->getRow(Selected).setPinnedPosition(Qtitan::AtNone);

    // A filter-only edit must remove/restore a row even with identical sort keys.
    auto* Filter = Controller->filter();
    auto* Condition = new Qtitan::GridFilterGroupCondition(Filter);
    Condition->addValue(0, static_cast<Qt::ItemDataRole>(Qt::UserRole + 5), true);
    Filter->setCondition(Condition); Filter->setActive(true);
    Model.SetVisible(200, false);
    QCOMPARE(Controller->getRecordCount(), Model.rowCount() - 1);
    QVERIFY(MatchesRebuild());
    Model.SetVisible(200, true);
    QCOMPARE(Controller->getRecordCount(), Model.rowCount());
    QVERIFY(MatchesRebuild());
    Filter->setActive(false);

    // Coalesced operations and direction changes must invalidate only when needed.
    Controller->beginUpdate();
    Model.Insert(20, 7100); Model.ChangeRange({{22, 999}}, 22, 22); Model.Remove(30, 3);
    Controller->endUpdate(); QVERIFY(MatchesRebuild());
    ParentColumn->setSortOrder(Qtitan::SortDescending);
    ChildColumn->setSortOrder(Qtitan::SortDescending);
    KeyColumn->setSortOrder(Qtitan::SortDescending);
    Model.Insert(3, 9999); Model.ChangeRange({{5, 123}}, 5, 5);
    QVERIFY(MatchesRebuild());

    Controller->setIncrementalSortThreshold(0.01);
    QMap<int, int> Bulk;
    const int Count = qCeil(Model.rowCount() * Controller->incrementalSortThreshold());
    for (int Row = 0; Row < Count; ++Row) Bulk.insert(Row, 20000 + Row);
    Sorted.clear();
    Model.ChangeRange(Bulk, 0, Model.rowCount() - 1);
    QCOMPARE(Sorted.count(), 1);
    QVERIFY(MatchesRebuild());
    Model.Remove(0, Model.rowCount());
    QCOMPARE(View->getRowCount(), 0);
    Model.Insert(0, 1); QVERIFY(MatchesRebuild());
}


inline void GridCacheAndSortRegression()
{
    QCOMPARE(QByteArray(Qtitan::getGridVersion()), QByteArray("9.3.0"));
    GridCountingModel Model(100000);
    Qtitan::Grid Grid;
    auto* View = SetupGrid(Grid, Model);
    auto* Controller = View->modelController();
    auto* Column = static_cast<Qtitan::GridTableColumn*>(View->getColumnByModelColumn(0));
    Column->dataBinding()->setSortRole(Qt::UserRole);
    Model.sortReads_ = 0;
    QElapsedTimer Timer; Timer.start();
    Column->setSortOrder(Qtitan::SortAscending);
    qInfo() << "Qtitan 100000 rows sort ms/read count:" << Timer.elapsed() << Model.sortReads_;
    QVERIFY(Model.sortReads_ <= 100128);
    QCOMPARE(View->getRow(0).modelIndex().data(Qt::UserRole).toInt(), 1);
    QCOMPARE(View->getRow(99999).modelIndex().data(Qt::UserRole).toInt(), 100000);
    QSignalSpy Sorted(Controller, &Qtitan::GridModelController::sortCompleted);
    Model.sortReads_ = 0;
    Model.Touch(2, 2);
    QCOMPARE(Sorted.count(), 0);
    QVERIFY(Model.sortReads_ < 20);
    Model.sortReads_ = 0;
    Model.Change(2, -100);
    QCOMPARE(Sorted.count(), 0);
    QCOMPARE(View->getRow(0).modelIndex().row(), 2);
    QVERIFY(Model.sortReads_ < 200);
    Model.sortReads_ = 0;
    Model.Insert(1);
    QCOMPARE(Sorted.count(), 0);
    QVERIFY(Model.sortReads_ < 200);
    QCOMPARE(View->getRow(1).modelIndex().data(Qt::UserRole).toInt(), -1);

    // Cache keys must include column and role. Unsupported roles must erase old values.
    Controller->beginUpdate(); // Isolate cache reads from layout repaint requests.
    Controller->clearRoleCache(); Model.multiReads_ = 0;
    QModelRoleData Roles[] = {QModelRoleData(Qt::ForegroundRole), QModelRoleData(Qt::FontRole), QModelRoleData(Qt::UserRole + 100)};
    Roles[2].setData("stale");
    Qtitan::GridModelDataBinding::ModelRoleData Span(Roles);
    QVERIFY(Controller->getModelMultiValue(Model.index(0, 0), Span));
    QCOMPARE(Roles[0].data().value<QColor>(), QColor(Qt::red));
    QVERIFY(!Roles[1].data().value<QFont>().bold());
    QVERIFY(!Roles[2].data().isValid());
    QVERIFY(Controller->getModelMultiValue(Model.index(0, 0), Span));
    QCOMPARE(Model.multiReads_, 1);
    QVERIFY(Controller->getModelMultiValue(Model.index(0, 1), Span));
    QCOMPARE(Roles[0].data().value<QColor>(), QColor(Qt::green));
    QVERIFY(Roles[1].data().value<QFont>().bold());
    QCOMPARE(Model.multiReads_, 2);
    Model.Touch(0, 0);
    QVERIFY(Controller->getModelMultiValue(Model.index(0, 1), Span));
    QCOMPARE(Model.multiReads_, 3);
    Model.Remove(0);
    QVERIFY(Controller->getModelMultiValue(Model.index(0, 1), Span));
    QCOMPARE(Model.multiReads_, 4);
    Controller->endUpdate();

    Model.Reset(100);
    Controller->setIncrementalSortThreshold(0.3);
    Model.Insert(100);
    int Previous = std::numeric_limits<int>::min();
    for (int Row = 0; Row < View->getRowCount(); ++Row)
    {
        const int Value = View->getRow(Row).modelIndex().data(Qt::UserRole).toInt();
        QVERIFY(Previous <= Value); Previous = Value;
    }
    Model.Reset(0);
    QCOMPARE(View->getRowCount(), 0);

    Model.Reset(30);
    auto* GroupColumn = static_cast<Qtitan::GridTableColumn*>(View->getColumnByModelColumn(1));
    GroupColumn->dataBinding()->setSortRole(Qt::UserRole);
    GroupColumn->setGroupIndex(0);
    View->expandAll();
    const auto CheckGroups = [&] {
        int Count = 0;
        int PreviousKey = std::numeric_limits<int>::min();
        for (int Row = 0; Row < View->getRowCount(); ++Row)
        {
            const auto Item = View->getRow(Row);
            if (Item.type() != Qtitan::GridRow::DataRow) continue;
            const auto Index = Item.modelIndex();
            const int Key = Index.data(Qt::UserRole).toInt();
            if (Key < PreviousKey) return false;
            if (Item.groupRow().groupValue().toInt() != Key / 10) return false;
            PreviousKey = Key; ++Count;
        }
        return Count == Model.rowCount();
    };
    QVERIFY(CheckGroups());
    Model.Change(2, 92); View->expandAll(); QVERIFY(CheckGroups());
    Model.Insert(1); View->expandAll(); QVERIFY(CheckGroups());
    Model.Insert(40); View->expandAll(); QVERIFY(CheckGroups());
    GroupColumn->setGroupIndex(-1);
    GridIncrementalRegression();
    GridIncrementalTreeRegression();
}

inline void GridFileManagerRegression()
{
    GridCountingModel Model(100);
    Qtitan::Grid Grid;
    auto* View = SetupGrid(Grid, Model);
    View->options().setSelectionPolicy(Qtitan::GridViewOptions::MultiRowSelection);
    View->setFileManagerMode(true);
    QVERIFY(View->isFileManagerMode());
    Grid.show(); Grid.activateWindow(); Grid.setFocus(); Grid.viewport()->setFocus();
    QTest::qWait(100);
    View->setFocusedRowIndex(2);
    QTest::keyClick(Grid.viewport(), Qt::Key_Space);
    QCOMPARE(View->modelController()->focusedRowIndex(), 3);
    QCOMPARE(View->selection()->selectedRowIndexes(), QModelIndexList({Model.index(2, 0)}));
    QTest::keyClick(Grid.viewport(), Qt::Key_Down, Qt::ShiftModifier);
    QCOMPARE(View->modelController()->focusedRowIndex(), 4);
    QCOMPARE(View->selection()->selectedRowIndexes(), QModelIndexList({Model.index(2, 0)}));
    QTest::keyClick(Grid.viewport(), Qt::Key_End);
    QCOMPARE(View->modelController()->focusedRowIndex(), 99);
    QTest::keyClick(Grid.viewport(), Qt::Key_Down);
    QCOMPARE(View->modelController()->focusedRowIndex(), 99);
    QTest::keyClick(Grid.viewport(), Qt::Key_Home);
    QCOMPARE(View->modelController()->focusedRowIndex(), 0);
    QTest::keyClick(Grid.viewport(), Qt::Key_PageDown);
    QVERIFY(View->modelController()->focusedRowIndex() > 0);
    QTest::keyClick(Grid.viewport(), Qt::Key_PageUp);
    QCOMPARE(View->selection()->selectedRowIndexes(), QModelIndexList({Model.index(2, 0)}));
    const auto ColumnBefore = View->modelController()->focusedColumnIndex();
    QTest::keyClick(Grid.viewport(), Qt::Key_Right);
    QCOMPARE(View->modelController()->focusedColumnIndex(), ColumnBefore);
    View->setFocusedRowIndex(2);
    QTest::keyClick(Grid.viewport(), Qt::Key_Space);
    QVERIFY(View->selection()->selectedRowIndexes().isEmpty());

    View->selectRow(2);
    View->setCutRows({2});
    const QPersistentModelIndex Cut = View->getRow(2).modelIndex();
    auto* Column = static_cast<Qtitan::GridTableColumn*>(View->getColumnByModelColumn(0));
    Column->dataBinding()->setSortRole(Qt::UserRole);
    Column->setSortOrder(Qtitan::SortAscending);
    const int Moved = View->getRow(Cut).rowIndex();
    QVERIFY(View->isRowCut(Moved));
    QVERIFY(View->selection()->selectedRowIndexes().contains(Cut));
    View->setFocusedRowIndex(Moved);
    auto& Options = View->fileManagerOptions();
    Options.cursorColor = QColor(12, 34, 56); Options.cursorTextColor = Qt::yellow;
    Options.setInvertedCursor(true); Options.setInvertedSelection(true);
    QVERIFY(!Options.invertedCursor()); QVERIFY(Options.invertedSelection());
    Options.setInvertedSelection(false);
    Qtitan::GridEditorStyleOption Paint;
    qtn_init_editor_option(View, &Paint, Column->dataBinding(), View->getRow(Moved), 0,
        false, true, true, true, false, QRect(0, 0, 100, 20), Qt::AlignLeft);
    QCOMPARE(Paint.backgroundBrush.color(), Options.cursorColor);
    QCOMPARE(Paint.palette.color(QPalette::Text).rgb(), QColor(Qt::yellow).rgb());
    QVERIFY(Paint.palette.color(QPalette::Text).alphaF() < 0.5);
    QVERIFY(!(Paint.state & QStyle::State_HasFocus));
    View->clearCutRows(); QVERIFY(!View->isRowCut(Moved));
    View->options().setModelDecorationOpacityRole(Qt::UserRole + 1);
    qtn_init_editor_option(View, &Paint, Column->dataBinding(), View->getRow(Moved), 0,
        false, true, true, true, false, QRect(0, 0, 100, 20), Qt::AlignLeft);
    const int HiddenAlpha = Paint.icon.pixmap(QSize(16, 16)).toImage().pixelColor(8, 8).alpha();
    QVERIFY(HiddenAlpha >= 125 && HiddenAlpha <= 130);
    View->setCutRows({Moved});
    qtn_init_editor_option(View, &Paint, Column->dataBinding(), View->getRow(Moved), 0,
        false, true, true, true, false, QRect(0, 0, 100, 20), Qt::AlignLeft);
    const int CutAlpha = Paint.icon.pixmap(QSize(16, 16)).toImage().pixelColor(8, 8).alpha();
    QVERIFY(CutAlpha >= 48 && CutAlpha <= 53);
    View->clearCutRows();
    Options.backgroundColor = Qt::black;
    Options.alternateBackgroundColor = Qt::darkGray;
    View->options().setAlternatingRowColors(true);
    View->setFocusedRowIndex(0);
    Grid.viewport()->repaint();
    QRect CursorCell;
    for (const auto& Hit : Grid.hitInfoAll())
        if (Hit.info() == Qtitan::GridHitInfo::Cell && Hit.row().rowIndex() == 0) CursorCell = Hit.rect();
    QVERIFY(!CursorCell.isEmpty());
    const auto Pixels = Grid.viewport()->grab().toImage();
    const qreal Dpr = Pixels.devicePixelRatio();
    // Verify the actual renderer fills the cursor past the last column, without cell frames.
    QCOMPARE(Pixels.pixelColor(qRound(500 * Dpr), qRound(CursorCell.center().y() * Dpr)), Options.cursorColor);
    Options.setInvertedSelection(true);
    qtn_init_editor_option(View, &Paint, Column->dataBinding(), View->getRow(2), 0,
        false, true, true, false, false, QRect(0, 0, 100, 20), Qt::AlignLeft);
    QCOMPARE(Paint.backgroundBrush.color(), QColor(Qt::red));
    QCOMPARE(Paint.palette.color(QPalette::Text), QColor(Qt::black));
    Options.setInvertedSelection(false);
    Options.selectedColor = Qt::magenta; Options.selectedTextColor = Qt::cyan;
    qtn_init_editor_option(View, &Paint, Column->dataBinding(), View->getRow(2), 0,
        false, true, true, false, false, QRect(0, 0, 100, 20), Qt::AlignLeft);
    QCOMPARE(Paint.backgroundBrush.color(), QColor(Qt::magenta));
    QCOMPARE(Paint.palette.color(QPalette::Text), QColor(Qt::cyan));
    qtn_init_editor_option(View, &Paint, Column->dataBinding(), View->getRow(1), 0,
        false, false, true, false, false, QRect(0, 0, 100, 20), Qt::AlignLeft);
    QCOMPARE(Paint.backgroundBrush.color(), Options.alternateBackgroundColor);
    Options.setInvertedCursor(true); QVERIFY(!Options.invertedSelection());
    View->setCutRows({Moved}); Model.Remove(Cut.row());
    for (int Row = 0; Row < View->getRowCount(); ++Row) QVERIFY(!View->isRowCut(Row));
    View->deselectAll(); Model.Reset(0);
    QTest::keyClick(Grid.viewport(), Qt::Key_Space);
    QVERIFY(View->selection()->selectedRowIndexes().isEmpty());
    View->setFileManagerMode(false);
    QCOMPARE(View->options().selectionPolicy(), Qtitan::GridViewOptions::MultiRowSelection);

    // Natural filename sorting survives a second header sort without an explicit numeric flag.
    Model.Reset(20);
    Column->dataBinding()->setSortRole(Qt::DisplayRole);
    Column->setSortOrder(Qtitan::SortAscending, true);
    QCOMPARE(View->getRow(2).modelIndex().data().toString(), QString("file2"));
    Column->setSortOrder(Qtitan::SortDescending);
    QCOMPARE(View->getRow(0).modelIndex().data().toString(), QString("file19"));
}
}
