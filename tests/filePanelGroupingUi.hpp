#pragma once
#include "HyperCommander/UIs/cmpPanel.hpp"
#include "HyperCommander/UIs/itemSelectionController.hpp"
#include "UIs/dlgMain.hpp"
#include "columnMgr.hpp"
#include "colorSchemeMgr.hpp"
#include "commandUiBridge.hpp"
#include "commandMgr.hpp"
#include "fileJobUi.hpp"
#include "qtitanColumnAdapter.hpp"
#include "HyperCommander/UIs/Components/thumbnailModel.hpp"
#include <fmstyle/ThemeManager.h>
#include <QContextMenuEvent>
#include <QCheckBox>
#include <QAbstractNativeEventFilter>
#include <atomic>
#include <thread>
#include <QtitanGrid.h>
#include <QtTest>

namespace hc::tests
{
inline QUuid GroupColumnId(CGridView* View, const QString& Field)
{
    for (const auto& Column : View->ColumnProfile().columns_) if (Column.fieldId_ == Field) return Column.id_;
    return {};
}
inline QModelIndex GroupSource(QModelIndex Index)
{
    while (const auto* Proxy = qobject_cast<const QAbstractProxyModel*>(Index.model())) Index = Proxy->mapToSource(Index);
    return Index;
}
inline void FilePanelGroupingUi(QMainUI* Main, CmpPanel* Panel)
{
    QTemporaryDir Directory; QVERIFY(Directory.isValid());
    const auto Saved = Main->CaptureWorkspace(); const auto Settings = hc::settings::AppSettings().Object();
    auto* Colors = TyStColorSchemeMgr::GetInstance(); const auto OriginalScheme = Colors->GetColorScheme(Colors->GetCurrentColorScheme());
    const auto OriginalTheme = fm::style::ThemeManager::instance().scheme();
    const auto RestoreScheme = qScopeGuard([&] { fm::style::ThemeManager::instance().setScheme(OriginalTheme); emit Colors->sigColorSchemeChanged(OriginalScheme); });
    const auto Restore = qScopeGuard([&] { hc::settings::AppSettings().Replace(Settings); hc::settings::AppSettings().Commit(); Main->RestoreWorkspace(Saved); });
    const auto Source = Directory.filePath("source"); QVERIFY(QDir().mkpath(Source)); QVERIFY(QDir(Source).mkdir("folder"));
    for (int I = 0; I < 60; ++I) {
        QFile File(QDir(Source).filePath(QString("group%1.%2").arg(I, 2, 10, QChar('0')).arg(I % 2 ? "txt" : "pdf")));
        QVERIFY(File.open(QIODevice::ReadWrite)); File.write(QByteArray(I + 1, 'x'));
        const int Days = I % 3 == 0 ? 0 : I % 3 == 1 ? 1 : 10;
        QVERIFY(File.setFileTime(QDateTime(QDate::currentDate().addDays(-Days), QTime(12, 0)), QFileDevice::FileModificationTime));
    }
    Panel->ChangeCurrentPath(Source, {});
    auto* View = static_cast<CGridView*>(Panel->GetFocusView(Panel->CurrentTabIndex()));
    auto* Model = static_cast<CFSModel*>(View->BaseModel());
    QTRY_VERIFY(!Model->IsLoading()); QTRY_COMPARE(Model->rowCount(), 62);
    auto Profile = hc::columns::ReferenceProfile();
    const auto SizeId = std::find_if(Profile.columns_.begin(), Profile.columns_.end(), [](const auto& Column) { return Column.fieldId_ == "file.size"; })->id_;
    Profile.sorts_ = {{SizeId, Qt::DescendingOrder}, {Profile.primaryNameId_, Qt::AscendingOrder}};
    QVERIFY(View->ApplyColumnProfile(Profile)); QVERIFY(View->SetListMode("one-line"));
    Main->raise(); Main->activateWindow(); QVERIFY(QTest::qWaitForWindowActive(Main)); emit Panel->sig_NotifyPanelActivated(); View->EnsureKeyboardFocusOnView();
    QTest::qWait(150);
    auto* Native = View->BaseView(); auto* Grid = View->findChild<Qtitan::Grid*>(); QVERIFY(Grid);
    const auto ExtensionId = GroupColumnId(View, "file.extension"); QVERIFY(!ExtensionId.isNull());
    QRect Header;
    for (const auto& Hit : Grid->hitInfoAll()) if (Hit.info() == Qtitan::GridHitInfo::Column) {
        const auto* Column = static_cast<Qtitan::GridColumn*>(Native->getColumn(Hit.columnIndex()));
        if (Column && QUuid(View->ColumnWidths()->ColumnId(Column->dataBinding()->column())) == ExtensionId) Header = Hit.rect();
    }
    QVERIFY(!Header.isEmpty()); bool MenuWorked = false; bool ChooseColumns = true; bool ColumnsMenuWorked = false;
    const auto MenuTrace = QObject::connect(Native, &Qtitan::GridViewBase::contextMenu, Main, [&](Qtitan::ContextMenuEventArgs* Args) {
        qInfo() << "Grouping menu hit:" << int(Args->hitInfo().info()) << Args->contextMenu()->objectName();
    });
    const auto DisconnectMenuTrace = qScopeGuard([&] { QObject::disconnect(MenuTrace); });
    QTimer MenuTimer; MenuTimer.setInterval(30);
    QObject::connect(&MenuTimer, &QTimer::timeout, Main, [&] {
        auto* Menu = qobject_cast<QMenu*>(QApplication::activePopupWidget()); if (!Menu) return;
        qInfo() << "Grouping popup:" << Menu->objectName() << Menu->actions().size();
        if (ChooseColumns) {
            if (auto* Action = Menu->findChild<QAction*>("hc.columns.visibility")) { ColumnsMenuWorked = true; Action->trigger(); }
        } else if (auto* Action = Menu->findChild<QAction*>("hc.grouping.value")) {
            MenuWorked = Menu->findChild<QAction*>("hc.columns.visibility") != nullptr;
            Action->trigger();
        }
        Menu->close(); MenuTimer.stop();
    });
    const auto OpenHeaderMenu = [&] {
        MenuTimer.start();
        QTest::mouseClick(Grid->viewport(), Qt::RightButton, Qt::NoModifier, Header.center());
        QContextMenuEvent HeaderMenu(QContextMenuEvent::Mouse, Header.center(), Grid->viewport()->mapToGlobal(Header.center()));
        QApplication::sendEvent(Grid->viewport(), &HeaderMenu); MenuTimer.stop();
    };
    OpenHeaderMenu(); QVERIFY(ColumnsMenuWorked);
    auto* ColumnsPopup = Grid->findChild<QWidget*>("qtn_columns_customization"); QVERIFY(ColumnsPopup);
    QTRY_VERIFY(ColumnsPopup->isVisible());
    const auto Boxes = ColumnsPopup->findChildren<QCheckBox*>(); QCOMPARE(Boxes.size(), Profile.columns_.size());
    auto* Box = Boxes.back(); const bool WasChecked = Box->isChecked();
    QTest::mouseClick(Box, Qt::LeftButton); QCOMPARE(Box->isChecked(), !WasChecked);
    QTest::mouseClick(Box, Qt::LeftButton); QCOMPARE(Box->isChecked(), WasChecked);
    ColumnsPopup->close(); ChooseColumns = false; QTest::qWait(100);
    OpenHeaderMenu(); QVERIFY(MenuWorked);
    QTRY_VERIFY(View->GroupingActive()); QTRY_COMPARE(Native->modelController()->groupRows().size(), 3);
    QCOMPARE(Native->getRow(0).type(), Qtitan::GridRow::DataRow); QCOMPARE(Native->getRow(0).modelIndex(0).data(Qt::EditRole).toString(), QString(".."));
    QTest::qWait(80); QSet<int> ParentPositions;
    for (const auto& Hit : Grid->hitInfoAll()) if (Hit.info() == Qtitan::GridHitInfo::Cell
        && Hit.modelIndex().column() == 0 && Hit.modelIndex().data(Qt::EditRole).toString() == "..") ParentPositions.insert(Hit.rect().y());
    QCOMPARE(ParentPositions.size(), 1);
    const auto CheckSorted = [&] {
        QHash<QString, quint64> Previous;
        for (int Row = 0; Row < Native->getRowCount(); ++Row) {
            const auto Item = Native->getRow(Row); if (Item.type() != Qtitan::GridRow::DataRow) continue;
            const auto SourceIndex = GroupSource(Item.modelIndex(0)); const auto File = Model->GetFileInfo(SourceIndex);
            if (!File || File->Name_ == ".." || (File->Attributes_ & FILE_ATTRIBUTE_DIRECTORY)) continue;
            const auto Key = Item.groupRow().groupValue().toString(); const auto Size = quint64(File->Size_);
            if (Previous.contains(Key) && Size > Previous[Key]) return false;
            Previous[Key] = Size;
        }
        return true;
    };
    QTRY_VERIFY(CheckSorted());
    QStringList Selected;
    for (const auto& Index : View->SelectionCandidates()) if (Model->GetName(Index).endsWith(".txt") && Selected.size() < 2)
        Selected.append(hc::workspace::SelectionKey(*Model->GetFileInfo(Index)->storageEntry_));
    QCOMPARE(Selected.size(), 2); View->RestoreSelection(Selected, Selected.front());
    const auto SelectedGroup = Native->focusedRow().modelGroup()->_groupValue;
    int GroupIndex = -1;
    for (int I = 0; I < Native->modelController()->groupRows().size(); ++I)
        if (Native->modelController()->groupRows()[I]._groupValue == SelectedGroup) GroupIndex = I;
    QVERIFY(GroupIndex >= 0); QVERIFY(Native->modelController()->setGroupExpanded(GroupIndex, false));
    QTRY_VERIFY(View->IsGroupFocused()); QCOMPARE(View->CaptureSelection().size(), 2);
    QVERIFY(Native->modelController()->setGroupExpanded(GroupIndex, true));
    QTRY_COMPARE(View->CaptureFocus(), Selected.front()); QTRY_COMPARE(Native->selection()->selectedRowIndexes().size(), 2);
    View->SetGroupsExpanded(false);
    QCOMPARE(View->CaptureSelection().size(), 2); QCOMPARE(View->SelectedSourceIndexes().size(), 2); QVERIFY(View->IsGroupFocused());
    auto* Selection = hc::ui::ItemSelectionController::For(View); Selection->Execute("hc.selection.all");
    QTRY_COMPARE(View->CaptureSelection().size(), 61); Selection->Execute("hc.selection.clear"); QTRY_COMPARE(View->CaptureSelection().size(), 0);
    View->RestoreSelection(Selected, Selected.front());
    const auto Out = Directory.filePath("copied"); QVERIFY(QDir().mkpath(Out));
    auto* Bridge = TyStCommandMgr::GetInstance()->Bridge();
    auto Copy = Bridge->Dispatch("hc.file.copy", {{"destination", Out}}, "grouping-test");
    QTRY_VERIFY(Copy.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready); const auto Copied = Copy.get();
    QVERIFY2(Copied.HasValue(), Copied.HasValue() ? "" : Copied.Failure().code_.c_str());
    QTRY_COMPARE(Bridge->FileJobs()->Scheduler().Snapshot(Copied.Value().jobId_).Value().state_, hc::operations::JobState::Completed);
    QCOMPARE(QDir(Out).entryList(QDir::Files).size(), 2);
    for (const auto& Mode : {"one-line", "two-line", "auto"}) {
        QVERIFY(View->SetListMode(Mode)); QTest::qWait(150); QVERIFY(View->GroupingActive());
        QCOMPARE(View->CaptureSelection().size(), 2); QCOMPARE(Native->modelController()->groupRows().size(), 3);
        for (const auto& Group : Native->modelController()->groupRows()) QVERIFY(!Group.expanded);
        const auto Scroll = View->CaptureScrollState(); View->RestoreScrollState(Scroll); QCOMPARE(View->CaptureScrollState()["anchor"], Scroll["anchor"]);
    }
    View->SetGroupsExpanded(true); QVERIFY(CheckSorted());
    auto State = View->Grouping(); State.order_ = Qt::DescendingOrder; QVERIFY(View->SetGrouping(State)); QTRY_VERIFY(CheckSorted());
    const auto Encoded = View->CaptureWorkspaceView(); QVERIFY(View->SetGrouping({})); View->RestoreWorkspaceView(Encoded); QTRY_VERIFY(View->GroupingActive());
    QVERIFY(View->Grouping() == State);
    auto Future = Encoded; Future["groupingV1"] = QJsonObject{{"version", 200}, {"extension", "preserve"}};
    View->RestoreWorkspaceView(Future); QTRY_VERIFY(!View->GroupingActive()); QCOMPARE(View->CaptureWorkspaceView()["groupingV1"], Future["groupingV1"]);
    View->RestoreWorkspaceView(Encoded); QTRY_VERIFY(View->GroupingActive());
    const auto DateId = GroupColumnId(View, "file.modified"); QVERIFY(!DateId.isNull());
    QVERIFY(View->SetGrouping({true, DateId, "relativeDate", Qt::DescendingOrder})); QTRY_VERIFY(Native->modelController()->groupRows().size() >= 3);
    // Exercise held Space through the focused viewport, including group headers.
    View->RestoreSelection({}, "@group:" + Native->modelController()->groupRows().front()._groupValue.toString());
    View->EnsureKeyboardFocusOnView(); Native->setFocusedRowIndex(1, Qt::ControlModifier);
    QSet<QString> ExpectedMarks;
    for (int Step = 0; Step < Native->getRowCount() - 2; ++Step) {
        const auto BeforeRow = Native->focusedRow(); const int BeforePosition = BeforeRow.rowIndex();
        if (BeforeRow.type() == Qtitan::GridRow::DataRow) {
            const auto Entry = Model->GetFileInfo(GroupSource(BeforeRow.modelIndex(0)));
            if (Entry && Entry->Name_ != "..") {
                const auto Key = hc::workspace::SelectionKey(*Entry->storageEntry_);
                if (!ExpectedMarks.remove(Key)) ExpectedMarks.insert(Key);
            }
        }
        QKeyEvent Press(QEvent::KeyPress, Qt::Key_Space, Qt::NoModifier, " ", Step != 0, 1);
        QApplication::sendEvent(Grid->viewport(), &Press); QTest::qWait(2);
        const auto Marks = View->CaptureSelection(); const QSet<QString> ActualMarks(Marks.begin(), Marks.end());
        if (ActualMarks != ExpectedMarks) qWarning() << "Held Space mismatch at" << Step << BeforePosition << "focus now" << Native->focusedRowIndex()
            << "missing" << (ExpectedMarks - ActualMarks) << "unexpected" << (ActualMarks - ExpectedMarks);
        QCOMPARE(ActualMarks, ExpectedMarks);
        QCOMPARE(Native->focusedRowIndex(), BeforePosition + 1);
        QSet<QString> PaintedMarks;
        for (int Position = 0; Position < Native->getRowCount(); ++Position) {
            const auto Row = Native->getRow(Position);
            if (Row.type() == Qtitan::GridRow::DataRow && Native->modelController()->isRowSelected(Row)) {
                const auto Entry = Model->GetFileInfo(GroupSource(Row.modelIndex(0)));
                if (Entry && Entry->Name_ != "..") PaintedMarks.insert(hc::workspace::SelectionKey(*Entry->storageEntry_));
            }
        }
        QCOMPARE(PaintedMarks, ExpectedMarks);
    }
    QKeyEvent Release(QEvent::KeyRelease, Qt::Key_Space, Qt::NoModifier, " "); QApplication::sendEvent(Grid->viewport(), &Release);
    const auto Capture = qEnvironmentVariable("HC_UI_ARTIFACT_DIR");
    for (const bool Dark : {true, false}) {
        fm::style::ThemeManager::instance().setScheme(Dark ? fm::style::ThemeManager::Scheme::Dark : fm::style::ThemeManager::Scheme::Light);
        auto CursorScheme = Colors->GetColorScheme(Dark ? QStringLiteral("기본 다크") : QStringLiteral("기본 라이트"));
        CursorScheme.IsInverseCursor = true; emit Colors->sigColorSchemeChanged(CursorScheme);
        View->SetPanelActive(true); View->EnsureKeyboardFocusOnView();
        for (const auto* Mode : {"one-line", "two-line"}) {
            QVERIFY(View->SetListMode(Mode)); QTest::qWait(150);
            QSet<int> SourceRows;
            for (int Row = 0; Row < Native->getRowCount(); ++Row) {
                const auto Item = Native->getRow(Row); if (Item.type() != Qtitan::GridRow::DataRow) continue;
                const auto Index = GroupSource(Item.modelIndex(0)); QVERIFY(Index.isValid());
                if (SourceRows.contains(Index.row())) qWarning() << "Duplicate source" << Mode << Row << Index.row() << Model->GetName(Index)
                    << "native/source/pinned" << Native->getRowCount() << Model->rowCount() << Native->modelController()->getPinnedGridRowAtBeginningCount();
                QVERIFY2(!SourceRows.contains(Index.row()), "Grouped native records duplicated a source row"); SourceRows.insert(Index.row());
            }
            QCOMPARE(SourceRows.size(), Model->rowCount());
            for (int Row = 0; Row < qMin(4, Native->getRowCount()); ++Row) {
                const auto Item = Native->getRow(Row);
                qInfo() << "Grouping visual row" << Mode << Row << int(Item.type()) << Item.modelIndex(0).data(Qt::EditRole).toString();
            }
            for (const auto& Hit : Grid->hitInfoAll()) if (Hit.info() == Qtitan::GridHitInfo::Cell && Hit.modelIndex().column() == 0
                && Hit.modelIndex().data(Qt::EditRole).toString() == "..")
                qInfo() << "Grouping parent hit" << Mode << Hit.row().rowIndex() << Hit.rect();
            const auto GroupPosition = Native->modelController()->groupRows().front().rowIndex;
            Native->setFocusedRowIndex(GroupPosition, Qt::ControlModifier); Native->verticalScrollBar()->setValue(0); QTest::qWait(80);
            QRect GroupRect;
            for (const auto& Hit : Grid->hitInfoAll()) if (Hit.info() == Qtitan::GridHitInfo::GroupRow && Hit.row().rowIndex() == GroupPosition) GroupRect = Hit.rect();
            GroupRect = GroupRect.intersected(Grid->viewport()->rect()); QVERIFY(!GroupRect.isEmpty());
            QCOMPARE(Native->focusedRowIndex(), GroupPosition); QVERIFY(View->IsPanelActive()); QVERIFY(View->ListAppearance().invertCursor);
            const auto Shot = Grid->viewport()->grab().toImage(); const auto Dpr = Shot.devicePixelRatio();
            const auto Background = Shot.pixelColor(qRound((GroupRect.right() - 20) * Dpr), qRound(GroupRect.center().y() * Dpr));
            QCOMPARE(Background, View->RecordColors()[fm::style::Token::InvCur]);
            int TextPixels = 0; const auto Foreground = View->RecordColors()[fm::style::Token::OnInvCur];
            const auto TextRect = GroupRect.adjusted(35, 4, 0, -4).intersected(QRect(GroupRect.left(), GroupRect.top(), 260, GroupRect.height()));
            for (int Y = qRound(TextRect.top() * Dpr); Y <= qRound(TextRect.bottom() * Dpr); ++Y)
                for (int X = qRound(TextRect.left() * Dpr); X <= qRound(TextRect.right() * Dpr); ++X)
                    if (Shot.pixelColor(X, Y) == Foreground) ++TextPixels;
            QVERIFY2(TextPixels > 5, "Focused group title must remain readable against its cursor background");
            if (!Capture.isEmpty()) QVERIFY(View->grab().save(QDir(Capture).filePath(QString("grouping-%1-%2.png").arg(Dark ? "dark" : "light", Mode))));
        }
    }
    QVERIFY(View->SetGrouping({})); QTRY_COMPARE(Native->getRowCount(), Model->rowCount());
    QCOMPARE(Native->modelController()->getPinnedGridRowAtBeginningCount(), 0);
    const auto Images = Directory.filePath("images"); QVERIFY(QDir().mkpath(Images));
    QImage Image(4, 4, QImage::Format_RGB32); Image.fill(Qt::green);
    QVERIFY(Image.save(QDir(Images).filePath("a.png"))); QVERIFY(Image.save(QDir(Images).filePath("b.png")));
    Panel->ChangeCurrentPath(Images, {}); QTRY_VERIFY(!Model->IsLoading()); QTRY_COMPARE(Model->rowCount(), 3);
    QVERIFY(View->SetListMode("one-line"));
    QVERIFY(View->SetGrouping({true, GroupColumnId(View, "file.extension"), "value", Qt::AscendingOrder})); View->SetGroupsExpanded(false);
    ThumbnailModel* Tiles = nullptr; for (auto* Child : View->children()) if (auto* Candidate = dynamic_cast<ThumbnailModel*>(Child)) Tiles = Candidate; QVERIFY(Tiles); Tiles->Rebuild(); QCOMPARE(Tiles->TargetPercent(), 100.0);
    auto General = hc::settings::AppSettings().Read(hc::settings::Section::General).toObject();
    auto Thumbnail = General["thumbnails"].toObject(); Thumbnail["autoSwitch"] = true; Thumbnail["autoPercent"] = 70;
    General["thumbnails"] = Thumbnail; hc::settings::AppSettings().Put(hc::settings::Section::General, General);
    QVERIFY(View->SetListMode("auto")); View->ApplyDisplaySettings(); QTRY_VERIFY(View->IsThumbnailMode());
    QTRY_VERIFY(!View->GroupingActive()); QVERIFY(View->Grouping().enabled_);
    QVERIFY(View->SetListMode("one-line")); QTRY_VERIFY(View->GroupingActive());
    for (const auto& Group : Native->modelController()->groupRows()) QVERIFY(!Group.expanded);
}
inline void FilePanelGroupingSearchUi(QMainUI* Main, CmpPanel* Panel)
{
    QTemporaryDir Directory; QVERIFY(Directory.isValid());
    const auto Saved = Main->CaptureWorkspace();
    const auto Restore = qScopeGuard([&] { qunsetenv("HC_EVERYTHING_TEST_INPUT"); qunsetenv("HC_EVERYTHING_TEST_MODE"); Main->RestoreWorkspace(Saved); });
    for (const auto* Name : {"origin", "a", "b"}) QVERIFY(QDir(Directory.path()).mkdir(Name));
    const auto Origin = Directory.filePath("origin"); { QFile File(QDir(Origin).filePath("anchor.txt")); QVERIFY(File.open(QIODevice::WriteOnly)); File.write("anchor"); }
    QJsonArray Paths;
    for (int I = 0; I < 160; ++I) {
        const auto Path = Directory.filePath(QString("%1/group-stream%2.txt").arg(I % 2 ? "a" : "b").arg(I / 2));
        QFile File(Path); QVERIFY(File.open(QIODevice::WriteOnly)); File.write("stream"); Paths.append(Path);
    }
    QFile Fixture(Directory.filePath("fixture.json")); QVERIFY(Fixture.open(QIODevice::WriteOnly)); Fixture.write(QJsonDocument(Paths).toJson()); Fixture.close();
    qputenv("HC_EVERYTHING_TEST_INPUT", Fixture.fileName().toUtf8()); qputenv("HC_EVERYTHING_TEST_MODE", "slow");
    Panel->ChangeCurrentPath(Origin, {}); auto* View = static_cast<CGridView*>(Panel->GetFocusView(Panel->CurrentTabIndex())); auto* Model = static_cast<CFSModel*>(View->BaseModel());
    QTRY_VERIFY(!Model->IsLoading()); QVERIFY(View->ApplyColumnProfile(hc::columns::ReferenceProfile()));
    const hc::columns::GroupingState Original{true, GroupColumnId(View, "file.extension"), "value", Qt::AscendingOrder};
    QVERIFY(View->SetGrouping(Original)); View->SetGroupsExpanded(false);
    Model->setProperty("hc.search.helper", QCoreApplication::applicationFilePath()); Panel->StartEverythingSearch("group-stream", "contains", 50);
    const auto LocationId = GroupColumnId(View, "file.parentPath"); QVERIFY(!LocationId.isNull());
    QVERIFY(View->SetGrouping({true, LocationId, "value", Qt::AscendingOrder}));
    QTRY_VERIFY_WITH_TIMEOUT(Model->rowCount() > 0, 6000); QVERIFY(Model->rowCount() < 160);
    const auto Key = hc::workspace::SelectionKey(*Model->GetFileInfo(Model->index(0, 0))->storageEntry_);
    View->RestoreSelection({Key}, Key); View->SetGroupsExpanded(false); QSignalSpy Resets(Model, &QAbstractItemModel::modelReset);
    QTRY_COMPARE_WITH_TIMEOUT(Model->rowCount(), 160, 15000); QCOMPARE(Resets.size(), 0);
    QTRY_COMPARE(View->BaseView()->modelController()->groupRows().size(), 2); QCOMPARE(View->CaptureSelection(), QStringList{Key});
    for (const auto& Group : View->BaseView()->modelController()->groupRows()) QVERIFY(!Group.expanded);
    QCOMPARE(View->BaseView()->getRowCount(), 2); QCOMPARE(View->SelectedSourceIndexes().size(), 1);
    Panel->StopEverythingSearch(true); QTRY_VERIFY(!Model->IsLoading() && !Model->IsSearchResults()); QTRY_VERIFY(View->Grouping() == Original);
    QTRY_COMPARE(Model->rowCount(), 2); QTRY_VERIFY(View->GroupingActive());
    for (const auto& Group : View->BaseView()->modelController()->groupRows()) QVERIFY(!Group.expanded);
}
inline void FilePanelGroupingLargeUi(QMainUI* Main, CmpPanel* Panel)
{
    QTemporaryDir Directory; QVERIFY(Directory.isValid()); const auto Saved = Main->CaptureWorkspace();
    const auto Restore = qScopeGuard([&] { Main->RestoreWorkspace(Saved); });
    for (int I = 0; I < 4105; ++I) { QFile File(Directory.filePath(QString("large%1.e%2").arg(I).arg(I % 20))); QVERIFY(File.open(QIODevice::WriteOnly)); }
    Panel->ChangeCurrentPath(Directory.path(), {}); auto* View = static_cast<CGridView*>(Panel->GetFocusView(Panel->CurrentTabIndex())); auto* Model = static_cast<CFSModel*>(View->BaseModel());
    QTRY_VERIFY_WITH_TIMEOUT(!Model->IsLoading(), 20000); QTRY_COMPARE(Model->rowCount(), 4106); QVERIFY(Model->UsesIndexedOrder());
    QVERIFY(View->ApplyColumnProfile(hc::columns::ReferenceProfile()));
    QTest::qWait(120); QTRY_VERIFY_WITH_TIMEOUT(!Model->IsLoading() && !Model->IsPageLoading(), 15000);
    QElapsedTimer Timer; Timer.start(); QVERIFY(View->SetGrouping({true, GroupColumnId(View, "file.extension"), "value", Qt::AscendingOrder}));
    QTRY_COMPARE(View->BaseView()->modelController()->groupRows().size(), 20);
    QCOMPARE(View->BaseView()->getRowCount(), 4126); qInfo() << "Grouping 4105 files elapsed ms:" << Timer.elapsed();
    View->SetGroupsExpanded(false); QCOMPARE(View->BaseView()->getRowCount(), 21);
    hc::ui::ItemSelectionController::For(View)->Execute("hc.selection.all");
    auto Reordered = View->Grouping(); Reordered.order_ = Qt::DescendingOrder; QVERIFY(View->SetGrouping(Reordered));
    QTRY_COMPARE_WITH_TIMEOUT(View->CaptureSelection().size(), 4105, 15000);
    QCOMPARE(View->SelectedSourceIndexes().size(), 4105);
    static_cast<Qtitan::GridColumn*>(View->BaseView()->getColumnByModelColumn(0))->setSortOrder(Qtitan::SortDescending);
    QTest::qWait(120); QTRY_VERIFY(!Model->ColumnSorts().isEmpty() && Model->ColumnSorts().front().order_ == Qt::DescendingOrder);
    QVERIFY(View->SetGrouping({})); QTRY_VERIFY_WITH_TIMEOUT(!Model->IsPageLoading(), 15000);
    QTRY_COMPARE(View->BaseView()->getRowCount(), 4106);
    QCOMPARE(Model->GetName(GroupSource(View->BaseView()->getRow(1).modelIndex(0))), QString("large4104.e4"));
    QTRY_COMPARE(View->CaptureSelection().size(), 4105);
}
inline void FilePanelGroupingReentryUi(QMainUI* Main, CmpPanel* Panel)
{
    QTemporaryDir Directory; QVERIFY(Directory.isValid());
    const auto Saved = Main->CaptureWorkspace();
    const auto Restore = qScopeGuard([&] { Main->RestoreWorkspace(Saved); });
    for (const auto* Name : {"a.txt", "b.txt", "c.txt"}) {
        QFile File(Directory.filePath(Name)); QVERIFY(File.open(QIODevice::WriteOnly)); File.write("file");
    }
    Panel->ChangeCurrentPath(Directory.path(), {});
    auto* View = static_cast<CGridView*>(Panel->GetFocusView(Panel->CurrentTabIndex())); auto* Model = static_cast<CFSModel*>(View->BaseModel());
    QTRY_VERIFY(!Model->IsLoading()); QTRY_COMPARE(Model->rowCount(), 4); Model->SetWatchEnabled(false);
    QVERIFY(View->ApplyColumnProfile(hc::columns::DefaultProfile())); QVERIFY(View->SetListMode("one-line"));
    QVERIFY(View->SetGrouping({true, GroupColumnId(View, "file.created"), "relativeDate", Qt::DescendingOrder}));
    Main->raise(); Main->activateWindow(); QVERIFY(QTest::qWaitForWindowActive(Main)); emit Panel->sig_NotifyPanelActivated();
    View->EnsureKeyboardFocusOnView(); QTest::qWait(200);
    auto* Grid = View->findChild<Qtitan::Grid*>(); QVERIFY(Grid); auto* Native = View->BaseView();
    QTRY_COMPARE(Native->getRowCount(), 5); View->RestoreSelection({}, {}); Grid->setFocus(); Native->setFocusedRowIndex(2, Qt::ControlModifier);
    QSet<QString> Expected;
    for (const auto& Index : View->SelectionCandidates()) {
        const auto Item = Model->GetFileInfo(Index); if (Item && Item->storageEntry_ && Item->Name_ != "c.txt") Expected.insert(hc::workspace::SelectionKey(*Item->storageEntry_));
    }
    QCOMPARE(Expected.size(), 2); bool Injected = false;
    const auto FocusTrace = QObject::connect(Native, &Qtitan::GridViewBase::focusRowChanged, Main, [&](int Before, int After) { qInfo() << "Reentry focus" << Before << After << View->CaptureSelection().size(); });
    const auto DisconnectFocus = qScopeGuard([&] { QObject::disconnect(FocusTrace); });
    const auto Reenter = QObject::connect(View, &CGridView::sigSelectionIdentityChanged, Main, [&] {
        qInfo() << "Reentry marks" << View->CaptureSelection().size() << "focus" << Native->focusedRowIndex() << "injected" << Injected;
        if (Injected || View->CaptureSelection().size() != 1) return;
        Injected = true; QKeyEvent Repeat(QEvent::KeyPress, Qt::Key_Space, Qt::NoModifier, " ", true, 1);
        QApplication::sendEvent(Grid, &Repeat);
    });
    const auto Disconnect = qScopeGuard([&] { QObject::disconnect(Reenter); });
    QTest::keyClick(Grid, Qt::Key_Space); QTRY_VERIFY(Injected);
    const auto SelectionKeys = [&] { const auto Marks = View->CaptureSelection(); return QSet<QString>(Marks.begin(), Marks.end()); };
    QTRY_COMPARE(SelectionKeys(), Expected); QTRY_COMPARE(Native->focusedRowIndex(), 4);
}

inline void FilePanelGroupingInputUi(QMainUI* Main, CmpPanel* Panel)
{
    QTemporaryDir Directory; QVERIFY(Directory.isValid());
    const auto Saved = Main->CaptureWorkspace(); auto& Settings = hc::settings::AppSettings(); const auto Original = Settings.Object();
    const auto Restore = qScopeGuard([&] { Settings.Replace(Original); Settings.Commit(); Main->RestoreWorkspace(Saved); });
    auto Options = Settings.Read(hc::settings::Section::Options).toObject();
    Options["folderSizeMode"] = "manual"; Options["folderSizeUseEverything"] = false; Options["folderSizeOnSpace"] = true;
    QVERIFY(Settings.Put(hc::settings::Section::Options, Options)); Settings.Commit();
    for (int I = 0; I < 24; ++I) {
        const auto Folder = QString("folder%1").arg(I, 2, 10, QChar('0')); QVERIFY(QDir(Directory.path()).mkpath(Folder + "/inner"));
        QFile File(Directory.filePath(Folder + "/inner/data.bin")); QVERIFY(File.open(QIODevice::WriteOnly)); File.write(QByteArray(2048 + I, 'x'));
    }
    for (int I = 0; I < 30; ++I) {
        QFile File(Directory.filePath(QString("file%1.txt").arg(I, 2, 10, QChar('0')))); QVERIFY(File.open(QIODevice::ReadWrite)); File.write("file");
        QVERIFY(File.setFileTime(QDateTime(QDate::currentDate().addDays(-1 - I % 3), QTime(12, 0)), QFileDevice::FileModificationTime));
        QVERIFY(File.setFileTime(QDateTime(QDate::currentDate().addDays(-1 - I % 3), QTime(12, 0)), QFileDevice::FileBirthTime));
    }
    Panel->ChangeCurrentPath(Directory.path(), {});
    auto* View = static_cast<CGridView*>(Panel->GetFocusView(Panel->CurrentTabIndex())); auto* Model = static_cast<CFSModel*>(View->BaseModel());
    QTRY_VERIFY(!Model->IsLoading()); QTRY_COMPARE(Model->rowCount(), 55); Model->SetWatchEnabled(false);
    QVERIFY(View->ApplyColumnProfile(hc::columns::ReferenceProfile()));
    QVERIFY(View->SetGrouping({true, GroupColumnId(View, "file.modified"), "relativeDate", Qt::DescendingOrder})); View->SetGroupsExpanded(true);
    Main->raise(); Main->activateWindow(); QVERIFY(QTest::qWaitForWindowActive(Main)); emit Panel->sig_NotifyPanelActivated();
    auto* Grid = View->findChild<Qtitan::Grid*>(); QVERIFY(Grid); auto* Native = View->BaseView();
    for (auto* Target : {static_cast<QWidget*>(Grid), Grid->viewport()}) {
        View->RestoreSelection({}, {}); Target->setFocus(); Native->setFocusedRowIndex(2, Qt::ControlModifier);
        QSet<QString> Expected;
        for (int Step = 0; Step < Native->getRowCount() - 3; ++Step) {
            const auto Row = Native->focusedRow(); const auto Position = Row.rowIndex();
            if (Row.type() == Qtitan::GridRow::DataRow) {
                const auto Item = Model->GetFileInfo(GroupSource(Row.modelIndex(0))); QVERIFY(Item && Item->storageEntry_);
                const auto Key = hc::workspace::SelectionKey(*Item->storageEntry_); if (!Expected.remove(Key)) Expected.insert(Key);
            }
            QKeyEvent Press(QEvent::KeyPress, Qt::Key_Space, Qt::NoModifier, " ", Step != 0, 1); QApplication::sendEvent(Target, &Press);
            QTest::qWait(20);
            const auto Marks = View->CaptureSelection(); const QSet<QString> Actual(Marks.begin(), Marks.end());
            if (Actual != Expected) qWarning() << "Space folder refresh" << Step << "position" << Position << "viewport" << (Target == Grid->viewport())
                << "missing" << (Expected - Actual) << "extra" << (Actual - Expected);
            QCOMPARE(Actual, Expected); QCOMPARE(Native->focusedRowIndex(), Position + 1);
            QSet<QString> Painted;
            for (int I = 0; I < Native->getRowCount(); ++I) {
                const auto Candidate = Native->getRow(I);
                if (Candidate.type() == Qtitan::GridRow::DataRow && Native->modelController()->isRowSelected(Candidate)) {
                    const auto Item = Model->GetFileInfo(GroupSource(Candidate.modelIndex(0))); QVERIFY(Item && Item->storageEntry_);
                    Painted.insert(hc::workspace::SelectionKey(*Item->storageEntry_));
                }
            }
            QCOMPARE(Painted, Expected);
        }
        QKeyEvent Release(QEvent::KeyRelease, Qt::Key_Space, Qt::NoModifier, " "); QApplication::sendEvent(Target, &Release);
        if (Target == Grid) QTRY_COMPARE_WITH_TIMEOUT(Model->PendingDirectorySizes(), 0, 10000);
    }
    struct NativeInputTrace final : QObject {
        CGridView* view_{}; bool active_ = false; bool held_ = false; int presses_ = 0; int characters_ = 0;
        bool eventFilter(QObject*, QEvent* Event) override {
            if (active_ && Event->type() == QEvent::KeyRelease) {
                const auto* Key = static_cast<QKeyEvent*>(Event); if (Key->key() == Qt::Key_Space && !Key->isAutoRepeat()) held_ = false;
            }
            if (active_ && Event->type() == QEvent::KeyPress) {
                const auto* Key = static_cast<QKeyEvent*>(Event); ++presses_; characters_ += Key->count();
                qInfo() << "Native Space key" << Key->key() << "count" << Key->count() << "repeat" << Key->isAutoRepeat()
                    << "native" << Key->nativeVirtualKey() << "focus" << view_->BaseView()->focusedRowIndex()
                    << view_->CaptureFocus() << "marks" << view_->CaptureSelection().size();
            }
            return false;
        }
    } Trace;
    Trace.view_ = View; Grid->installEventFilter(&Trace); Grid->viewport()->installEventFilter(&Trace);
    struct NativeMessages final : QAbstractNativeEventFilter {
        NativeInputTrace* trace_{}; int messages_ = 0; int repeats_ = 0;
        bool nativeEventFilter(const QByteArray&, void* Message, qintptr*) override {
            const auto* Msg = static_cast<MSG*>(Message);
            if (trace_->active_ && Msg->message == WM_KEYDOWN && Msg->wParam == VK_SPACE) {
                ++messages_; repeats_ += LOWORD(Msg->lParam);
                qInfo() << "Space Windows message" << LOWORD(Msg->lParam) << "previous" << bool(Msg->lParam & (LPARAM(1) << 30));
            }
            return false;
        }
    } Messages;
    Messages.trace_ = &Trace; qApp->installNativeEventFilter(&Messages);
    const auto RemoveMessages = qScopeGuard([&] { qApp->removeNativeEventFilter(&Messages); });
    const auto FocusTrace = QObject::connect(Native, &Qtitan::GridViewBase::focusRowChanged, Main, [&](int Before, int After) {
        if (Trace.active_) qInfo() << "Native Space focus" << Before << After << View->CaptureFocus();
    });
    const auto DisconnectTrace = qScopeGuard([&] { QObject::disconnect(FocusTrace); });
    int Resets = 0, ResetsWhileHeld = 0;
    const auto ResetTrace = QObject::connect(Model, &QAbstractItemModel::modelReset, Main, [&] {
        if (Trace.active_) { ++Resets; if (Trace.held_) ++ResetsWhileHeld; qInfo() << "Space model reset" << Resets << "marks" << View->CaptureSelection().size(); }
    });
    const auto DisconnectReset = qScopeGuard([&] { QObject::disconnect(ResetTrace); });
    for (const bool Watch : {false, true}) for (const int Interval : {30}) {
        QVERIFY(View->ApplyColumnProfile(hc::columns::DefaultProfile()));
        QVERIFY(View->SetGrouping({true, GroupColumnId(View, "file.created"), "relativeDate", Qt::DescendingOrder})); View->SetGroupsExpanded(true);
        QTRY_VERIFY(!Model->IsLoading()); QTest::qWait(80);
        Model->SetWatchEnabled(Watch); View->RestoreSelection({}, {});
        const auto Window = reinterpret_cast<HWND>(Main->winId());
        const auto ForegroundThread = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
        const bool Attached = ForegroundThread && ForegroundThread != GetCurrentThreadId() && AttachThreadInput(GetCurrentThreadId(), ForegroundThread, TRUE);
        SetForegroundWindow(Window); if (Attached) AttachThreadInput(GetCurrentThreadId(), ForegroundThread, FALSE);
        View->EnsureKeyboardFocusOnView(); Native->setFocusedRowIndex(1, Qt::ControlModifier);
        QTRY_VERIFY(GetForegroundWindow() == Window); QTest::qWait(80);
        QSet<QString> Expected;
        for (const auto& Index : View->SelectionCandidates()) {
            const auto Item = Model->GetFileInfo(Index); if (Item && Item->storageEntry_ && Item->Name_ != "..") Expected.insert(hc::workspace::SelectionKey(*Item->storageEntry_));
        }
        QCOMPARE(Expected.size(), 54);
        QStringList NativeOrder;
        for (int Position = 1; Position < Native->getRowCount(); ++Position) {
            const auto Row = Native->getRow(Position); QString Key;
            if (Row.type() == Qtitan::GridRow::DataRow) {
                const auto Item = Model->GetFileInfo(GroupSource(Row.modelIndex(0))); QVERIFY(Item && Item->storageEntry_);
                Key = hc::workspace::SelectionKey(*Item->storageEntry_);
            }
            NativeOrder.append(Key);
        }
        const int Count = Native->getRowCount() - 1; std::atomic<bool> Finished{false}, FocusLost{false}; std::atomic<int> Sent{0};
        Trace.presses_ = Trace.characters_ = 0; Messages.messages_ = Messages.repeats_ = Resets = ResetsWhileHeld = 0; Trace.active_ = Trace.held_ = true;
        qInfo() << "Native Space phase watch/interval/count" << Watch << Interval << Count;
        const auto TouchPath = Directory.filePath("file29.txt").toStdWString();
        std::jthread Worker([&](std::stop_token Token) {
            INPUT Press{}; Press.type = INPUT_KEYBOARD; Press.ki.wVk = VK_SPACE;
            for (int I = 0; I < Count && !Token.stop_requested(); ++I) {
                if (GetForegroundWindow() != Window) { FocusLost = true; break; }
                if (SendInput(1, &Press, sizeof(INPUT)) != 1) break;
                ++Sent;
                if (Watch && I % 6 == 0) {
                    const auto Handle = CreateFileW(TouchPath.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
                    if (Handle != INVALID_HANDLE_VALUE) { FILETIME Now{}; GetSystemTimeAsFileTime(&Now); SetFileTime(Handle, nullptr, nullptr, &Now); CloseHandle(Handle); }
                }
                if (Interval) std::this_thread::sleep_for(std::chrono::milliseconds(Interval));
            }
            Press.ki.dwFlags = KEYEVENTF_KEYUP; SendInput(1, &Press, sizeof(INPUT)); Finished = true;
        });
        const auto ReleaseKey = qScopeGuard([&] { Worker.request_stop(); if (Worker.joinable()) Worker.join(); });
        QTRY_VERIFY_WITH_TIMEOUT(Finished.load(), 10000); Worker.join(); QTest::qWait(300);
        if (Watch) QTRY_VERIFY(Resets > 0);
        QTRY_VERIFY(!Model->IsLoading()); Trace.active_ = false;
        QVERIFY(!FocusLost); QCOMPARE(Sent.load(), Count);
        QCOMPARE(Messages.repeats_, Count); QCOMPARE(Trace.presses_, Messages.messages_);
        QVERIFY(Trace.presses_ > 0 && Trace.presses_ <= NativeOrder.size());
        // Windows may merge native repeats while the UI is busy. Each delivered
        // command must select the next file without leaving a hole in the prefix.
        Expected.clear();
        for (int I = 0; I < Trace.presses_; ++I) if (!NativeOrder[I].isEmpty()) Expected.insert(NativeOrder[I]);
        const auto Marks = View->CaptureSelection(); const QSet<QString> Actual(Marks.begin(), Marks.end());
        qInfo() << "Native Space phase result" << "events/characters" << Trace.presses_ << Trace.characters_ << "focus" << Native->focusedRowIndex()
            << "actual/expected" << Actual.size() << Expected.size() << "messages/repeats/resets" << Messages.messages_ << Messages.repeats_ << Resets;
        QCOMPARE(Actual, Expected); QCOMPARE(ResetsWhileHeld, 0);
        QSet<QString> Painted;
        for (int Position = 0; Position < Native->getRowCount(); ++Position) {
            const auto Row = Native->getRow(Position);
            if (Row.type() == Qtitan::GridRow::DataRow && Native->modelController()->isRowSelected(Row)) {
                const auto Item = Model->GetFileInfo(GroupSource(Row.modelIndex(0))); QVERIFY(Item && Item->storageEntry_);
                Painted.insert(hc::workspace::SelectionKey(*Item->storageEntry_));
            }
        }
        QCOMPARE(Painted, Expected);
    }
}
}
