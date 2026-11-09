#pragma once
#include "qtitanRegression.hpp"
#include <QSortFilterProxyModel>

namespace hc::tests
{
inline void GridGroupingSummaryExperiment()
{
    constexpr auto RawRole = Qt::ItemDataRole(Qt::UserRole + 20);
    QStandardItemModel Model;
    for (int I = 1; I <= 3; ++I) {
        auto* Group = new QStandardItem("caption");
        Group->setData("key", Qt::UserRole + 10);
        Group->setData(0, Qt::UserRole + 11);
        Group->setData("caption", Qt::UserRole + 12);
        auto* Size = new QStandardItem("formatted size");
        Size->setData(I * 100, RawRole);
        Model.appendRow({Group, Size});
    }
    Qtitan::Grid Grid; Grid.setViewType(Qtitan::Grid::BandedTableView);
    auto* View = Grid.view<Qtitan::GridBandedTableView>();
    View->options().setAutoCreateColumns(false); View->setModel(&Model); View->addBand("files");
    auto* Group = View->addColumn(0, "group"); Group->setBandIndex(0);
    auto* Size = View->addColumn(1, "size"); Size->setBandIndex(0); Size->setWidth(560);
    Group->dataBinding()->setGroupingRoles(Qt::UserRole + 10, Qt::UserRole + 11, Qt::UserRole + 12);
    Size->dataBinding()->setSummaryRole(RawRole); Size->setGroupSummary(Group, Qtitan::GridSummary::Sum);
    QSignalSpy Changed(View, SIGNAL(groupingChanged()));
    Group->setGroupIndex(0); Group->setVisible(false);
    View->options().setGroupSummaryPlace(Qtitan::GridViewOptions::SummaryRow);
    View->modelController()->refresh(); View->expandAll();
    QCOMPARE(View->modelController()->groupRows().size(), 1);
    QVERIFY(!Group->isVisible()); QVERIFY(Changed.count() > 0);
    auto* Row = View->getRow(0).modelGroup();
    const auto& Summary = View->modelController()->summary()->getGroupSummaryValue(Row, Size->dataBinding());
    QCOMPARE(Summary.role, RawRole); QCOMPARE(Summary.value.toLongLong(), 600);
    QCOMPARE(View->getRow(0).groupText().toString(), QString("caption"));
    Grid.resize(640, 300); Grid.show(); QVERIFY(QTest::qWaitForWindowExposed(&Grid));
    QVERIFY(!View->groupCaptionProvider());
    const auto Legacy = Grid.viewport()->grab().toImage(); QVERIFY(!Legacy.isNull());
    int CaptionCalls = 0;
    View->setGroupCaptionProvider([&](const Qtitan::GridRow&) { ++CaptionCalls; return QString("CUSTOM SUMMARY 600"); });
    const auto Custom = Grid.viewport()->grab().toImage(); QVERIFY(CaptionCalls > 0); QVERIFY(Custom != Legacy);
    View->setGroupCaptionProvider({}); QVERIFY(!View->groupCaptionProvider());
    QCOMPARE(Grid.viewport()->grab().toImage(), Legacy);
    qInfo("P4 experiment: raw Sum=600, hidden group column, SummaryRow, groupingChanged, caption provider and unchanged legacy fallback passed.");
}
inline void GridNestedGroupingRegression()
{
    QStandardItemModel Model;
    const auto Append = [&](const QString& Parent, const QString& Child, int Id) {
        Model.appendRow({new QStandardItem(Parent), new QStandardItem(Child), new QStandardItem(QString::number(Id))});
    };
    Append("a", "same", 1); Append("a", "other", 2); Append("b", "same", 3);
    Qtitan::Grid Grid; Grid.setViewType(Qtitan::Grid::BandedTableView);
    auto* View = Grid.view<Qtitan::GridBandedTableView>(); View->options().setAutoCreateColumns(false);
    View->setModel(&Model); View->addBand("files");
    for (int I = 0; I < 3; ++I) View->addColumn(I, QString::number(I))->setBandIndex(0);
    View->beginUpdate(); static_cast<Qtitan::GridColumn*>(View->getColumnByModelColumn(0))->setGroupIndex(0); static_cast<Qtitan::GridColumn*>(View->getColumnByModelColumn(1))->setGroupIndex(1); View->endUpdate();
    auto* Controller = View->modelController(); Controller->setIncrementalSortThreshold(1.0); View->expandAll();
    QCOMPARE(Controller->groupRows().size(), 5);
    QVERIFY(Controller->setGroupExpanded(1, false));
    Append("b", "other", 4);
    const auto Inserted = GridGroupSnapshot(View); Controller->refresh(); QCOMPARE(GridGroupSnapshot(View), Inserted);
    Model.setData(Model.index(0, 0), "b");
    const auto Moved = GridGroupSnapshot(View); Controller->refresh(); QCOMPARE(GridGroupSnapshot(View), Moved);
    Model.removeRow(1);
    const auto Removed = GridGroupSnapshot(View); Controller->refresh(); QCOMPARE(GridGroupSnapshot(View), Removed);
}
inline void GridIndependentGroupingRegression()
{
    constexpr int KeyRole = Qt::UserRole + 10, OrderRole = Qt::UserRole + 11, TextRole = Qt::UserRole + 12;
    struct SnapshotModel : QStandardItemModel { void ResetSnapshot() { beginResetModel(); endResetModel(); } } Model;
    QSortFilterProxyModel Proxy; Proxy.setSourceModel(&Model);
    const auto Append = [&](int Group, int Rank) {
        auto* Name = new QStandardItem(QString("file%1").arg(Rank)); Name->setData(Rank, Qt::UserRole);
        auto* Date = new QStandardItem(QString("2026-10-20 12:%1").arg(Rank)); Date->setData(Rank, Qt::UserRole);
        Date->setData(QString("g%1").arg(Group), KeyRole);
        Date->setData(Group < 0 ? QVariant{} : QVariant(Group), OrderRole);
        Date->setData(Group < 0 ? QString("missing") : QString("caption%1").arg(Group), TextRole);
        Model.appendRow({Name, Date});
    };
    Append(2, 2); Append(1, 4); Append(2, 1); Append(-1, 0); Append(1, 3);
    Qtitan::Grid Grid; Grid.setViewType(Qtitan::Grid::BandedTableView);
    auto* View = Grid.view<Qtitan::GridBandedTableView>();
    View->options().setAutoCreateColumns(false); View->options().setColumnHidingOnGroupingEnabled(false);
    View->options().setGroupKeepLevelExpanded(0); View->options().setRowAutoHeight(false); View->options().setGroupsHeader(false);
    View->setModel(&Proxy); View->addBand("files");
    auto* Name = View->addColumn(0, "name"); Name->setBandIndex(0); Name->dataBinding()->setSortRole(Qt::UserRole);
    auto* Date = View->addColumn(1, "date"); Date->setBandIndex(0); Date->setRowIndex(1);
    Date->dataBinding()->setSortRole(Qt::UserRole); Date->dataBinding()->setGroupingRoles(KeyRole, OrderRole, TextRole);
    View->beginUpdate(); Name->setSortOrder(Qtitan::SortAscending); Date->setGroupIndex(0); View->endUpdate();
    auto* Controller = View->modelController(); Controller->setIncrementalSortThreshold(1.0); View->expandAll();
    const auto Files = [&] {
        QList<int> Values;
        for (int Row = 0; Row < View->getRowCount(); ++Row) {
            const auto Item = View->getRow(Row);
            if (Item.type() == Qtitan::GridRow::DataRow) Values.append(Item.modelIndex(0).data(Qt::UserRole).toInt());
        }
        return Values;
    };
    QCOMPARE(Controller->groupRows().size(), 3); QCOMPARE(Files(), QList<int>({3, 4, 1, 2, 0}));
    QCOMPARE(View->getRow(0).groupValue().toString(), QString("g1"));
    QCOMPARE(View->getRow(0).groupText().toString(), QString("caption1"));
    QCOMPARE(Model.index(0, 1).data().toString(), QString("2026-10-20 12:2"));
    Date->setSortOrder(Qtitan::SortDescending);
    QCOMPARE(Controller->sortRules()->groupRule(0)->order, Qtitan::SortAscending);
    QCOMPARE(Date->sortOrder(), Qtitan::SortDescending);
    Date->setSortOrder(Qtitan::SortAscending); QCOMPARE(Date->sortOrder(), Qtitan::SortAscending);
    Date->setSortOrder(Qtitan::SortDescending); QCOMPARE(Date->sortOrder(), Qtitan::SortDescending);
    Name->setSortOrder(Qtitan::SortDescending); QCOMPARE(Files(), QList<int>({4, 3, 2, 1, 0}));
    Controller->setGroupOrder(Date->dataBinding(), 0, Qtitan::SortDescending);
    QCOMPARE(Files(), QList<int>({2, 1, 4, 3, 0}));
    QCOMPARE(Date->sortOrder(), Qtitan::SortDescending);
    Date->setSortOrder(Qtitan::SortAscending); QCOMPARE(Date->sortOrder(), Qtitan::SortAscending);
    QCOMPARE(Controller->sortRules()->groupRule(0)->order, Qtitan::SortDescending);
    auto* First = (&Controller->groupRows().front()); QVERIFY(Controller->setGroupExpanded(0, false));
    const auto Collapsed = First->_groupValue; Append(2, 9);
    QCOMPARE((&Controller->groupRows().front())->_groupValue, Collapsed); QVERIFY(!(&Controller->groupRows().front())->expanded);
    const auto Incremental = GridGroupSnapshot(View); Controller->refresh(); QCOMPARE(GridGroupSnapshot(View), Incremental);
    Model.setData(Model.index(1, 1), QString("same caption"), TextRole);
    Model.setData(Model.index(0, 1), QString("same caption"), TextRole);
    QCOMPARE(Controller->groupRows().size(), 3);
    Model.setData(Model.index(1, 1), "g2", KeyRole); Model.setData(Model.index(1, 1), 2, OrderRole);
    const auto Moved = GridGroupSnapshot(View); Controller->refresh(); QCOMPARE(GridGroupSnapshot(View), Moved);
    Model.removeRow(1); const auto Removed = GridGroupSnapshot(View); Controller->refresh(); QCOMPARE(GridGroupSnapshot(View), Removed);
    View->expandAll(); Grid.resize(700, 600); Grid.show(); QVERIFY(QTest::qWaitForWindowExposed(&Grid));
    struct Painter final : Qtitan::GridRecordPainter {
        int calls_ = 0; bool twoLines_ = false;
        void paintBackground(QPainter& Paint, const Record& Row, QWidget*) override {
            ++calls_; twoLines_ |= Row.lines.size() == 2; Paint.fillRect(Row.rect, QColor(12, 177, 98));
        }
    } Paint;
    View->setRecordPainter(&Paint); const auto Image = Grid.viewport()->grab().toImage();
    QVERIFY(!Image.isNull()); QVERIFY(Paint.calls_ > 0); QVERIFY(Paint.twoLines_); View->setRecordPainter(nullptr);
    View->options().setScrollRowStyle(Qtitan::ScrollByPixel);
    Controller->setIndexPinnedPosition(Proxy.index(0, 0), Qtitan::AtBeginning); Controller->refresh();
    QTest::qWait(80); QSet<int> PinnedPositions;
    for (const auto& Hit : Grid.hitInfoAll())
        if (Hit.info() == Qtitan::GridHitInfo::Cell && Hit.modelIndex() == Proxy.index(0, 0)) PinnedPositions.insert(Hit.rect().y());
    QCOMPARE(PinnedPositions.size(), 1);
    Model.ResetSnapshot(); Controller->refresh();
    QCOMPARE(Proxy.rowCount(), Model.rowCount()); QVERIFY(View->getRowCount() > 0);
}
}
