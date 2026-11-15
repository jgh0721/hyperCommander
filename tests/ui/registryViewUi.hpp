#pragma once

/// UIs
#include "HyperCommander/UIs/Components/cmpRegistryView.hpp"
/// 공통 라이브러리
#include "commonLibs/system/registryAccess.hpp"
/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QTest>
#include <QScopeGuard>
#include <QUuid>
#include <QtitanGrid.h>

inline void RegistryViewSmoke()
{
    using namespace hc::registry;
    WinRegistryBackend Backend;
    const auto Base = RegistryLocation::Parse("HKCU\\Software\\HyperCommanderTests").value();
    QCOMPARE(Backend.Create(Base), quint32(0));
    const auto Key = Base.Child("RegistryEditor-task_4ceba737038e-" + QUuid::createUuid().toString(QUuid::WithoutBraces));
    QCOMPARE(Backend.Create(Key), quint32(0));
    const auto Cleanup = qScopeGuard([&] { Backend.DeleteKey(Key.Child("child")); Backend.DeleteKey(Key); });
    QCOMPARE(Backend.Create(Key.Child("child")), quint32(0));
    QCOMPARE(Backend.Write(Key, {"", REG_BINARY, "default"}), quint32(0));
    QCOMPARE(Backend.Write(Key, {"test", REG_DWORD, QByteArray::fromHex("01000000")}), quint32(0));
    CRegistryView View; View.resize(1000, 650); View.show();
    QTRY_VERIFY(View.isVisible()); QCOMPARE(View.PathText(), QStringLiteral("컴퓨터")); QCOMPARE(View.VisibleRowCount(), 5);
    QVERIFY(View.Navigate(Key.Path())); QTRY_COMPARE(View.PathText(), Key.Path()); QTRY_COMPARE(View.VisibleRowCount(), 3);
    View.SetFilter("test"); QTRY_COMPARE(View.VisibleRowCount(), 1);
    View.SetFilter({}); QTRY_COMPARE(View.VisibleRowCount(), 3);
    QVERIFY(View.Navigate(Key.Path(), QString("test")));
    QTRY_COMPARE(View.CurrentSource().data(RegistryTreeModel::ValueNameRole).toString(), QString("test"));
    View.SetRegistryView(hc::registry::View::Wow32);
    QTRY_COMPARE(View.SelectedRegistryView(), hc::registry::View::Wow32);
    QTRY_COMPARE(View.PathText(), Key.Path()); QTRY_COMPARE(View.VisibleRowCount(), 3);
    View.SetRegistryView(hc::registry::View::Native64);
    QTRY_COMPARE(View.PathText(), Key.Path()); QTRY_COMPARE(View.VisibleRowCount(), 3);
    auto* Known = View.findChild<QPushButton*>("hc.registry.knownLocations"); QVERIFY(Known); QVERIFY(Known->menu());
    Known->menu()->popup(Known->mapToGlobal(QPoint(0, Known->height())));
    QTRY_VERIFY(Known->menu()->isVisible());
    QPointer<QAction> Environment;
    for (auto* Action : Known->menu()->actions())
        if (Action->property("registryPath").toString() == QStringLiteral("HKEY_CURRENT_USER\\Environment")) Environment = Action;
    QVERIFY(Environment);
    // A favorites refresh can rebuild menu actions, so reacquire by stable path.
    const auto HasEnvironment = [&] {
        for (auto* Action : Known->menu()->actions())
            if (Action->property("registryPath").toString() == QStringLiteral("HKEY_CURRENT_USER\\Environment")) return true;
        return false;
    };
    QTRY_VERIFY(HasEnvironment()); Known->menu()->close();
    const auto State = View.CaptureState(); View.SetFlat(false); View.ToggleTree();
    QVERIFY(View.Navigate(Key.Child("child").Path())); QTRY_COMPARE(View.PathText(), Key.Child("child").Path());
    QTRY_COMPARE(View.VisibleRowCount(), 0);
    View.Back(); QTRY_COMPARE(View.PathText(), Key.Path());
    View.RestoreState(State); QTRY_COMPARE(View.PathText(), Key.Path());
    QVERIFY(View.CaptureState()["flat"].toBool()); QVERIFY(View.CaptureState()["treeVisible"].toBool());
    View.SetShown(false); View.SetShown(true); QTRY_COMPARE(View.VisibleRowCount(), 3);
    View.Navigate("HKLM\\Software"); View.CloseView(); View.close();
}

inline void RegistryPanelSmoke(QMainUI* Main)
{
    Main->show(); Main->activateWindow(); QTRY_VERIFY(Main->isActiveWindow());
    auto* Bridge = TyStCommandMgr::GetInstance()->Bridge(); QVERIFY(Bridge);
    auto* Left = Main->findChild<CmpPanel*>("cmpLeftPanel"); QVERIFY(Left);
    QTemporaryDir Directory; QVERIFY(Directory.isValid());
    const auto Saved = Main->CaptureWorkspace();
    const auto Restore = qScopeGuard([&] { Left->HideSystemView(); Main->RestoreWorkspace(Saved); });
    // Earlier file-panel tests may leave their deleted temporary path selected.
    // Give this test a live workspace before exercising global navigation.
    Left->ChangeCurrentPath(QDir::toNativeSeparators(Directory.path()), {});
    auto* FileModel = static_cast<CFSModel*>(static_cast<CGridView*>(Left->GetFocusView(Left->CurrentTabIndex()))->BaseModel());
    QTRY_VERIFY(FileModel && !FileModel->IsLoading());
    emit Left->sig_NotifyPanelActivated();
    auto* Before = Left->GetFocusView(Left->CurrentTabIndex());
    QVERIFY(Bridge->ExecuteCommand("hc.navigate.systemRegistry"));
    QTRY_VERIFY(Left->RegistryView()); auto* View = Left->RegistryView();
    QVERIFY(Left->HasSystemToolView()); QVERIFY(!Left->SystemView());
    // A delayed file failure behind the system view must not open a modal.
    FileModel->ChangeDirectory("registry-test-missing-directory");
    QTRY_VERIFY(!FileModel->IsLoading()); QVERIFY(FileModel->LastNavigationError());
    QVERIFY(!Left->findChild<QMessageBox*>("hc.navigationError"));
    FileModel->DismissNavigationError();
    QCOMPARE(View->PathText(), QStringLiteral("컴퓨터"));
    for (const auto* Id : {"hc.file.rename", "hc.file.delete", "hc.clipboard.copy", "hc.file.attributes"})
        if (auto* Action = Bridge->Action(Id)) QTRY_VERIFY(!Action->isEnabled());
    const auto Keys = hc::ui::DefaultScopeShortcuts(hc::ui::InputScope::RegistryView);
    QCOMPARE(Keys.value(QKeyCombination(Qt::ControlModifier, Qt::Key_Z)), QString("hc.registry.undo"));
    QCOMPARE(Keys.value(QKeyCombination(Qt::ControlModifier, Qt::Key_1)), QString("hc.registry.favorite.1"));
    QCOMPARE(Keys.value(QKeyCombination(Qt::ControlModifier, Qt::Key_0)), QString("hc.registry.favorite.0"));
    QVERIFY(!Keys.contains(QKeyCombination(Qt::ControlModifier, Qt::Key_F)));
    QVERIFY(!Keys.contains(QKeyCombination(Qt::AltModifier, Qt::Key_Left)));
    (void)Bridge->Dispatch("hc.system.registry.open", {{"path", QString("HKCU")}, {"view", 32}}, "test");
    QTRY_COMPARE(View->PathText(), QString("HKEY_CURRENT_USER"));
    QCOMPARE(View->SelectedRegistryView(), hc::registry::View::Wow32);
    const auto Captured = Left->CaptureWorkspace();
    QCOMPARE(Captured.tabs_[Left->CurrentTabIndex()].view_["system"].toObject()["kind"].toString(), QString("registry"));
    QVERIFY(Bridge->ExecuteCommand("hc.navigate.back")); QTRY_COMPARE(View->PathText(), QStringLiteral("컴퓨터"));
    QVERIFY(Bridge->ExecuteCommand("hc.navigate.forward")); QTRY_COMPARE(View->PathText(), QString("HKEY_CURRENT_USER"));
    QVERIFY(Bridge->ExecuteCommand("hc.registry.address"));
    auto* Address = View->findChild<fm::ui::BreadcrumbBar*>("hc.registry.address");
    qInfo() << "registry address focus" << QApplication::focusWidget() << "visible" << Address->isVisible() << "geometry" << Address->geometry() << "modal" << QApplication::activeModalWidget() << "active" << QApplication::activeWindow();
    QTRY_VERIFY2(Address->isAncestorOf(QApplication::focusWidget()),
        qPrintable(QString("focus=%1; addressVisible=%2; editorVisible=%3")
            .arg(QApplication::focusWidget() ? QApplication::focusWidget()->objectName() : "null")
            .arg(Address->isVisible()).arg(Address->findChild<QLineEdit*>() && Address->findChild<QLineEdit*>()->isVisible())));
    Left->HideSystemView(); QCOMPARE(Left->GetFocusView(Left->CurrentTabIndex()), Before);
    QVERIFY(!Left->HasSystemToolView());
}

inline void RegistryAsyncSelectionSmoke()
{
    using namespace hc::registry;
    WinRegistryBackend Backend;
    const auto Base = RegistryLocation::Parse("HKCU\\Software\\HyperCommanderTests").value();
    QCOMPARE(Backend.Create(Base), quint32(0));
    const auto Key = Base.Child("RegistryEditor-task_4ceba737038e-" + QUuid::createUuid().toString(QUuid::WithoutBraces));
    QCOMPARE(Backend.Create(Key), quint32(0));
    const auto Cleanup = qScopeGuard([&] {
        Backend.DeleteKey(Key.Child("A").Child("nested")); Backend.DeleteKey(Key.Child("A"));
        Backend.DeleteKey(Key.Child("B")); Backend.DeleteKey(Key.Child("C")); Backend.DeleteKey(Key);
    });
    QCOMPARE(Backend.Create(Key.Child("A")), quint32(0)); QCOMPARE(Backend.Create(Key.Child("A").Child("nested")), quint32(0));
    QCOMPARE(Backend.Create(Key.Child("B")), quint32(0));
    QCOMPARE(Backend.Write(Key, {"keep", REG_BINARY, "before"}), quint32(0));
    QCOMPARE(Backend.Write(Key, {"other", REG_BINARY, "other"}), quint32(0));
    CRegistryView Widget; Widget.resize(1150, 700); Widget.show(); QVERIFY(Widget.Navigate(Key.Path()));
    QTRY_COMPARE(Widget.PathText(), Key.Path()); QTRY_COMPARE(Widget.VisibleRowCount(), 4);
    Widget.SetFlat(false);
    auto* Grid = Widget.findChild<Qtitan::TreeGrid*>("hc.registry.items"); QVERIFY(Grid);
    auto* View = Grid->view<Qtitan::GridTreeView>(); QVERIFY(View);
    const auto RowNamed = [View](const QString& Name) {
        for (int I = 0; I < View->getRowCount(); ++I) if (View->getRow(I).modelIndex(0).data().toString() == Name) return I;
        return -1;
    };
    QTRY_VERIFY(RowNamed("A") >= 0); View->setFocusedRowIndex(RowNamed("A")); View->expandRow(View->focusedRow());
    QTRY_VERIFY(RowNamed("nested") >= 0);
    View->setFocusedRowIndex(RowNamed("keep")); View->deselectAll(); View->selectRow(RowNamed("keep")); View->selectRow(RowNamed("other"));
    const auto Before = Widget.CaptureState();
    QCOMPARE(Before["items"].toObject()["selected"].toArray().size(), 2);
    QCOMPARE(Backend.Create(Key.Child("C")), quint32(0)); QCOMPARE(Backend.Write(Key, {"keep", REG_BINARY, "after"}), quint32(0));
    Widget.Refresh(); QTRY_VERIFY(RowNamed("C") >= 0);
    QTRY_COMPARE(Widget.CurrentSource().data(RegistryTreeModel::ValueNameRole).toString(), QString("keep"));
    QTRY_COMPARE(Widget.CaptureState()["items"].toObject()["selected"].toArray().size(), 2);
    QVERIFY(RowNamed("nested") >= 0);
    static_cast<Qtitan::GridTableColumn*>(View->getColumnByModelColumn(0))->setSortOrder(Qtitan::SortDescending);
    QTRY_COMPARE(View->getRow(0).modelIndex(0).data().toString(), QString("C"));
    QTRY_COMPARE(Widget.CurrentSource().data(RegistryTreeModel::ValueNameRole).toString(), QString("keep"));
    QCOMPARE(Widget.CaptureState()["items"].toObject()["selected"].toArray().size(), 2);
    const auto State = Widget.CaptureState();
    CRegistryView Restored; Restored.resize(1150, 700); Restored.show(); Restored.RestoreState(State);
    QTRY_COMPARE(Restored.PathText(), Key.Path());
    QTRY_COMPARE(Restored.CurrentSource().data(RegistryTreeModel::ValueNameRole).toString(), QString("keep"));
    auto* RestoredView = Restored.findChild<Qtitan::TreeGrid*>("hc.registry.items")->view<Qtitan::GridTreeView>();
    QTRY_COMPARE(RestoredView->selection()->selectedRowIndexes().size(), 2);
    // Parent watches remove a deleted current key and the address follows the
    // root proxy's surviving ancestor, rather than displaying stale contents.
    QVERIFY(Restored.Navigate(Key.Child("B").Path())); QTRY_COMPARE(Restored.PathText(), Key.Child("B").Path());
    QCOMPARE(Backend.DeleteKey(Key.Child("B")), quint32(0)); Restored.Service()->Invalidate(Key);
    QTRY_COMPARE(Restored.PathText(), Key.Path());
}
