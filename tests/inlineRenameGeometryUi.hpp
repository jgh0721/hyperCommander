#pragma once
#include "HyperCommander/UIs/Components/cmpView.hpp"
#include "HyperCommander/UIs/cmpPanel.hpp"
#include "UIs/dlgMain.hpp"
#include "builtInFsModel.hpp"
#include "commandMgr.hpp"
#include "qtitanColumnAdapter.hpp"
#include <QtitanGrid.h>
#include <QtTest>

namespace hc::tests
{
inline void InlineRenameGeometryUi(QMainUI* Main, CmpPanel* Panel)
{
    auto& Settings = hc::settings::AppSettings(); const auto Original = Settings.Object();
    const auto Workspace = Main->CaptureWorkspace(); auto* Commands = TyStCommandMgr::GetInstance();
    const auto Restore = qScopeGuard([&] {
        for (auto* View : Main->findChildren<CGridView*>()) View->CancelRename();
        Settings.Replace(Original); Settings.Commit(); Commands->Refresh(); Main->RestoreWorkspace(Workspace);
    });
    QTemporaryDir Directory; QVERIFY(Directory.isValid());
    for (int I = 0; I < 180; ++I) {
        QFile File(Directory.filePath(QString("rename-%1.txt").arg(I, 3, 10, QChar('0'))));
        QVERIFY(File.open(QIODevice::WriteOnly)); File.write("rename geometry");
    }
    auto Options = Settings.Read(hc::settings::Section::Options).toObject(); Options["renameInDialog"] = false;
    QVERIFY(Settings.Put(hc::settings::Section::Options, Options)); Commands->Refresh();
    Panel->ChangeCurrentPath(Directory.path(), {});
    auto* View = static_cast<CGridView*>(Panel->GetFocusView(Panel->CurrentTabIndex()));
    auto* Model = static_cast<CFSModel*>(View->BaseModel());
    QTRY_VERIFY(!Model->IsLoading()); QTRY_COMPARE(Model->rowCount(), 181); Model->SetWatchEnabled(false);
    auto* Grid = View->findChild<Qtitan::Grid*>(); QVERIFY(Grid); auto* Native = View->BaseView();
    Main->raise(); Main->activateWindow(); QVERIFY(QTest::qWaitForWindowActive(Main));
    emit Panel->sig_NotifyPanelActivated();
    const QDir Evidence(QDir::current().absoluteFilePath("rename-geometry")); QVERIFY(QDir().mkpath(Evidence.path()));
    for (const bool Grouped : {false, true}) for (const auto Layout : {
        hc::columns::LayoutMode::Details, hc::columns::LayoutMode::NameFirst, hc::columns::LayoutMode::NameLast}) {
        auto Profile = hc::columns::DefaultProfile(); Profile.mode_ = Layout;
        QVERIFY(View->ApplyColumnProfile(Profile));
        QVERIFY(View->SetListMode(Layout == hc::columns::LayoutMode::Details ? "one-line" : "two-line"));
        auto Presentation = View->CaptureWorkspaceView();
        Presentation["listPresentation"] = QJsonObject{{"version", 1},
            {"mode", Layout == hc::columns::LayoutMode::Details ? "one-line" : "two-line"},
            {"nameBelow", Layout == hc::columns::LayoutMode::NameLast}};
        View->RestoreWorkspaceView(Presentation); QTest::qWait(150);
        QCOMPARE(View->RecordGeometry().nameBelow, Layout == hc::columns::LayoutMode::NameLast);
        QUuid GroupColumn;
        for (const auto& Column : View->ColumnProfile().columns_) if (Column.fieldId_ == "file.created") GroupColumn = Column.id_;
        QVERIFY(!GroupColumn.isNull());
        QVERIFY(View->SetGrouping(Grouped ? hc::columns::GroupingState{true, GroupColumn, "relativeDate", Qt::DescendingOrder} : hc::columns::GroupingState{}));
        View->SetGroupsExpanded(true); QTest::qWait(150);
        QCOMPARE(View->IsTwoLine(), Layout != hc::columns::LayoutMode::Details);
        QCOMPARE(View->RecordGeometry().nameBelow, Layout == hc::columns::LayoutMode::NameLast);
        QModelIndex Target;
        for (int I = 0; I < Model->rowCount(); ++I) if (Model->GetName(Model->index(I, 0)) == "rename-100.txt") Target = Model->index(I, 0);
        QVERIFY(Target.isValid());
        Qtitan::GridRow Row;
        for (int Position = 0; Position < Native->getRowCount(); ++Position) {
            const auto Candidate = Native->getRow(Position); if (Candidate.type() != Qtitan::GridRow::DataRow) continue;
            auto Source = Candidate.modelIndex(0);
            while (const auto* Proxy = qobject_cast<const QAbstractProxyModel*>(Source.model())) Source = Proxy->mapToSource(Source);
            if (Source == Target) { Row = Candidate; break; }
        }
        QVERIFY(Row.isValid());
        View->EnsureKeyboardFocusOnView(); Native->setFocusedRowIndex(Row.rowIndex(), Qt::ControlModifier);
        Native->scrollToRow(Row, true);
        auto* Scroll = Native->verticalScrollBar(); QVERIFY(Scroll->maximum() > 0);
        Scroll->setValue(Scroll->value() - Grid->viewport()->height() / 2); QTest::qWait(100);
        const auto NameCell = [&] {
            for (const auto& Hit : Grid->hitInfoAll())
                if (Hit.info() == Qtitan::GridHitInfo::Cell && Hit.row().rowIndex() == Row.rowIndex() && Hit.modelIndex().column() == 0) return Hit.rect();
            return QRect{};
        };
        const auto Before = NameCell(); QVERIFY(Before.isValid());
        QVERIFY(Before.top() > Grid->viewport()->height() / 4);
        QVERIFY(Before.bottom() < Grid->viewport()->height() - 10);
        const auto Position = Scroll->value(); const auto Focus = View->CaptureFocus();
        QTest::keyClick(Grid, Qt::Key_F2);
        QLineEdit* Input = nullptr;
        QTRY_VERIFY((Input = View->findChild<QLineEdit*>("hc.file.renameEditor")) && Input->isVisible());
        const auto Label = QString("%1-%2").arg(Grouped ? "grouped" : "flat").arg(static_cast<int>(Layout));
        QVERIFY(Grid->grab().save(Evidence.filePath(Label + ".png")));
        QCOMPARE(Scroll->value(), Position); QCOMPARE(NameCell(), Before); QCOMPARE(View->CaptureFocus(), Focus);
        const auto IconRight = Before.left() + 8 + View->RecordGeometry().sideMargin + View->ListIconSize() - 1;
        QVERIFY2(Input->geometry().left() > IconRight, "Rename editor must leave the full icon slot visible");
        QVERIFY(Input->geometry().right() <= Before.right());
        QVERIFY(Before.contains(Input->geometry()));
        QCOMPARE(Input->text(), QString("rename-100.txt")); QVERIFY(Input->hasFocus());
        QTest::qWait(100); QCOMPARE(Scroll->value(), Position); QCOMPARE(NameCell(), Before);
        // Completing an unchanged name must preserve the same viewport as opening it.
        QTest::keyClick(Input, Qt::Key_Return); QTRY_VERIFY(!View->IsEditing());
        QTest::qWait(80); QCOMPARE(Scroll->value(), Position); QCOMPARE(NameCell(), Before);
        QVERIFY(QFileInfo::exists(Directory.filePath("rename-100.txt")));
        qInfo() << "Rename geometry preserved" << Label << "cell" << Before << "scroll" << Position;
    }
}
}
