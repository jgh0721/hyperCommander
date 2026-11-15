#pragma once

#include "HyperCommander/UIs/Components/cmpRegistryView.hpp"
#include <fmwidgets/ContentDialog.h>
#include <fmwidgets/ValueInputs.h>
#include <fmwidgets/Menu.h>
#include <QContextMenuEvent>
#include <QTimer>

inline void RegistryTreeSmoke(QMainUI* Main)
{
    using namespace hc::registry;
    auto* Bridge = TyStCommandMgr::GetInstance()->Bridge(); QVERIFY(Bridge);
    auto* Left = Main->findChild<CmpPanel*>("cmpLeftPanel"); QVERIFY(Left);
    const auto Saved = Main->CaptureWorkspace();
    WinRegistryBackend Backend;
    const auto Base = RegistryLocation::Parse("HKCU\\Software\\HyperCommanderTests").value(); QCOMPARE(Backend.Create(Base), quint32(0));
    const auto Key = Base.Child("RegistryEditor-task_4ceba737038e-" + QUuid::createUuid().toString(QUuid::WithoutBraces));
    QCOMPARE(Backend.Create(Key), quint32(0)); QCOMPARE(Backend.Create(Key.Child("child")), quint32(0));
    const auto Cleanup = qScopeGuard([&] {
        Left->HideSystemView(); Main->RestoreWorkspace(Saved);
        Backend.DeleteKey(Key.Child("child")); Backend.DeleteKey(Key.Child("renamed")); Backend.DeleteKey(Key);
    });
    QCOMPARE(Backend.Write(Key, {"one", REG_BINARY, "one"}), quint32(0));
    QCOMPARE(Backend.Write(Key, {"two", REG_BINARY, "two"}), quint32(0));
    Main->show(); Main->activateWindow(); QTRY_VERIFY(Main->isActiveWindow()); emit Left->sig_NotifyPanelActivated();
    QVERIFY(Bridge->ExecuteCommand("hc.navigate.systemRegistry")); QTRY_VERIFY(Left->RegistryView()); auto* View = Left->RegistryView();
    QVERIFY(View->Navigate(Key.Child("child").Path())); QTRY_COMPARE(View->PathText(), Key.Child("child").Path());
    auto* Tree = View->findChild<Qtitan::TreeGrid*>("hc.registry.keys"); QVERIFY(Tree);
    Tree->viewport()->setFocus(); QTRY_VERIFY(Tree->viewport()->hasFocus());
    QTRY_COMPARE(View->CurrentSource().data(RegistryTreeModel::LocationRole).toString(), Key.Child("child").Path());
    QVERIFY(Bridge->ExecuteCommand("hc.registry.rename"));
    QTRY_VERIFY(Tree->findChild<fm::ui::InlineNameEditor*>("hc.registry.renameEditor"));
    auto* Editor = Tree->findChild<fm::ui::InlineNameEditor*>("hc.registry.renameEditor");
    QTRY_VERIFY(Editor->isVisible()); QTRY_VERIFY(Editor->hasFocus());
    Editor->setText("renamed"); QTest::keyClick(Editor, Qt::Key_Return);
    QTRY_VERIFY(Backend.Enumerate(Key.Child("renamed"))); QVERIFY(!Backend.Enumerate(Key.Child("child")));
    QVERIFY(Backend.Read(Key, "one")); QVERIFY(Backend.Read(Key, "two"));
    QVERIFY(View->Navigate(Key.Path(), QString("one")));
    QTRY_COMPARE(View->CurrentSource().data(RegistryTreeModel::ValueNameRole).toString(), QString("one"));
    auto* Grid = View->findChild<Qtitan::TreeGrid*>("hc.registry.items"); QVERIFY(Grid);
    auto* Rows = Grid->view<Qtitan::GridTreeView>(); Grid->viewport()->setFocus(); Rows->deselectAll();
    for (int Row = 0; Row < Rows->getRowCount(); ++Row) {
        const auto Name = Rows->getRow(Row).modelIndex(0).data(RegistryTreeModel::ValueNameRole).toString();
        if (Name == "one" || Name == "two") Rows->selectRow(Row);
    }
    QCOMPARE(View->SelectedSources().size(), 2);
    // The registry write can be visible before the async rename completion
    // reaches the UI; wait for command readiness before requesting another write.
    QTRY_VERIFY(Bridge->Action("hc.registry.delete")->isEnabled());
    QTimer Answer, Deadline; bool Answered = false;
    QObject::connect(&Answer, &QTimer::timeout, [&] {
        if (auto* Dialog = qobject_cast<fm::ui::ContentDialog*>(QApplication::activeModalWidget())) {
            Answer.stop(); Answered = true; Dialog->button(fm::ui::ContentDialog::PrimaryButton)->click();
        }
    });
    QObject::connect(&Deadline, &QTimer::timeout, [&] {
        if (auto* Dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget())) Dialog->reject();
    });
    Answer.start(25); Deadline.setSingleShot(true); Deadline.start(10000);
    QVERIFY(Bridge->ExecuteCommand("hc.registry.delete")); Answer.stop(); Deadline.stop(); QVERIFY(Answered);
    QTRY_VERIFY(!Backend.Read(Key, "one")); QTRY_VERIFY(!Backend.Read(Key, "two"));
    QVERIFY(Backend.Enumerate(Key.Child("renamed")));
    bool MenuFound = false;
    QTimer CloseMenu;
    QObject::connect(&CloseMenu, &QTimer::timeout, [&] {
        if (auto* Menu = qobject_cast<fm::ui::Menu*>(QApplication::activePopupWidget()); Menu && Menu->objectName() == "hc.registry.context") {
            MenuFound = true; CloseMenu.stop(); Menu->close();
        }
    });
    CloseMenu.start(25);
    QContextMenuEvent Event(QContextMenuEvent::Keyboard, Grid->viewport()->rect().center(), Grid->viewport()->mapToGlobal(Grid->viewport()->rect().center()));
    QTest::keyClick(Grid->viewport(), Qt::Key_Menu);
    CloseMenu.stop(); QVERIFY(MenuFound);
}