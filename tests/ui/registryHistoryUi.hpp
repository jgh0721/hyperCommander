#pragma once

#include "HyperCommander/UIs/Components/cmpRegistryDetailView.hpp"
#include "HyperCommander/UIs/registryEditors.hpp"
#include <fmwidgets/ValueInputs.h>
#include <fmwidgets/Button.h>
#include <QtitanGrid.h>

inline void RegistryHistorySmoke(QMainUI* Main)
{
    using namespace hc::registry;
    auto* Bridge = TyStCommandMgr::GetInstance()->Bridge(); QVERIFY(Bridge);
    auto* Left = Main->findChild<CmpPanel*>("cmpLeftPanel"); auto* Right = Main->findChild<CmpPanel*>("cmpRightPanel");
    QVERIFY(Left); QVERIFY(Right);
    const auto Saved = Main->CaptureWorkspace();
    WinRegistryBackend Backend;
    const auto Base = RegistryLocation::Parse("HKCU\\Software\\HyperCommanderTests").value(); QCOMPARE(Backend.Create(Base), quint32(0));
    const auto Key = Base.Child("RegistryEditor-task_4ceba737038e-" + QUuid::createUuid().toString(QUuid::WithoutBraces));
    QCOMPARE(Backend.Create(Key), quint32(0));
    QPointer<CRegistryDetailView> Detail;
    const auto Cleanup = qScopeGuard([&] {
        if (Detail) { Detail->Editor()->Discard(); Right->CloseQuickView(Right->CurrentTabIndex()); }
        Left->HideSystemView(); Main->RestoreWorkspace(Saved); Backend.DeleteKey(Key);
    });
    const RegistryValue Original{"history", REG_SZ, RegistryValueCodec::EncodeText(REG_SZ, "before").value};
    QCOMPARE(Backend.Write(Key, Original), quint32(0));
    Main->show(); Main->activateWindow(); QTRY_VERIFY(Main->isActiveWindow());
    emit Left->sig_NotifyPanelActivated(); QVERIFY(Bridge->ExecuteCommand("hc.navigate.systemRegistry"));
    QTRY_VERIFY(Left->RegistryView()); auto* View = Left->RegistryView();
    QVERIFY(View->Navigate(Key.Path(), QString("history")));
    QTRY_COMPARE(View->CurrentSource().data(RegistryTreeModel::ValueNameRole).toString(), QString("history"));
    QVERIFY(Bridge->ExecuteCommand("hc.registry.edit"));
    QTRY_VERIFY(qobject_cast<CRegistryDetailView*>(Right->GetFocusView(Right->CurrentTabIndex())));
    Detail = qobject_cast<CRegistryDetailView*>(Right->GetFocusView(Right->CurrentTabIndex()));
    auto* Text = Detail->Editor()->findChild<fm::ui::TextField*>("hc.registry.text"); QVERIFY(Text);
    Text->setText("after"); Detail->Editor()->findChild<fm::ui::Button*>("hc.registry.save")->click();
    QTRY_COMPARE(RegistryValueCodec::DecodeText(Backend.Read(Key, "history").value).text, QString("after"));
    QTRY_VERIFY(!Detail->Editor()->IsModified());
    emit Left->sig_NotifyPanelActivated(); QTRY_VERIFY(Bridge->Action("hc.registry.undo")->isEnabled());
    QVERIFY(Bridge->ExecuteCommand("hc.registry.undo")); QTRY_VERIFY(Backend.Read(Key, "history").value == Original);
    QTRY_VERIFY(Bridge->Action("hc.registry.redo")->isEnabled());
    QVERIFY(Bridge->ExecuteCommand("hc.registry.redo"));
    QTRY_COMPARE(RegistryValueCodec::DecodeText(Backend.Read(Key, "history").value).text, QString("after"));
    QTRY_VERIFY(Bridge->Action("hc.registry.undo")->isEnabled());
    QVERIFY(Bridge->ExecuteCommand("hc.registry.history")); QTRY_VERIFY(View->HistoryShown());
    QTRY_VERIFY(!View->History()->Loading()); QVERIFY(View->History()->rowCount() >= 3);
    auto* Grid = View->findChild<Qtitan::TreeGrid*>("hc.registry.historyGrid"); QVERIFY(Grid);
    Grid->view<Qtitan::GridTreeView>()->setFocusedRowIndex(0);
    QTRY_VERIFY(View->CurrentHistory()); QVERIFY(!View->CanExecuteCommand("hc.registry.delete"));
    auto* Rows = Grid->view<Qtitan::GridTreeView>(); const auto RootIndex = Rows->focusedRow().modelIndex(0);
    const auto BeforeExpand = Rows->getRowCount(); Rows->expand(RootIndex);
    QTRY_VERIFY(Rows->getRowCount() > BeforeExpand);
    QVERIFY(Bridge->ExecuteCommand("hc.registry.describe")); QTRY_VERIFY(!Detail->Editor()->isEnabled());
    const auto State = View->CaptureState(); QVERIFY(State["showHistory"].toBool());
    QVERIFY(Bridge->ExecuteCommand("hc.registry.browse")); QVERIFY(!View->HistoryShown());
    View->RestoreState(State); QTRY_VERIFY(View->HistoryShown());
    View->SetFilter("unlikely-history-filter-82c4"); QTRY_COMPARE(Grid->view<Qtitan::GridTreeView>()->getRowCount(), 0);
    View->SetFilter({}); QTRY_VERIFY(Grid->view<Qtitan::GridTreeView>()->getRowCount() >= 3);
    View->ShowHistory(false);
}