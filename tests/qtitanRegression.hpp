#pragma once

#include <QAbstractTableModel>
#include <QElapsedTimer>
#include <QPersistentModelIndex>
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

inline Qtitan::GridTableView* SetupGrid(Qtitan::Grid& Grid, GridCountingModel& Model)
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
