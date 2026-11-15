#pragma once
/// UIs
#include "HyperCommander/UIs/Components/cmpRegistryDetailView.hpp"
#include "HyperCommander/UIs/registryLinkPane.hpp"
/// 고유 라이브러리
#include "UniqueLibs/registry/registryLinks.hpp"
/// 외부 라이브러리
#include <fmwidgets/Button.h>
#include <QSysInfo>

inline void RegistryLinksSmoke(QMainUI* Main)
{
    using namespace hc::registry;
    if (QSysInfo::machineHostName().compare("iMonAGR-Win11", Qt::CaseInsensitive) != 0) QSKIP("Owned VM Classes fixture only");
    WinRegistryBackend Backend;
    const auto Physical = RegistryLinkResolver::PhysicalClasses(Backend, Hive::CurrentUser, hc::registry::View::Native64);
    QVERIFY(Physical); const auto Classes = Physical.value;
    QVERIFY(Backend.Enumerate(Classes));
    const auto Token = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const auto Extension = ".HyperCommanderTest-" + Token, Program = "HyperCommanderTest." + Token;
    const auto Ext = Classes.Child(Extension), Prog = Classes.Child(Program);
    QVERIFY(!Backend.Enumerate(Ext)); QVERIFY(!Backend.Enumerate(Prog));
    const auto Saved = Main->CaptureWorkspace();
    auto* Left = Main->findChild<CmpPanel*>("cmpLeftPanel"); auto* Right = Main->findChild<CmpPanel*>("cmpRightPanel"); QVERIFY(Left); QVERIFY(Right);
    const RegistryValue Association{{}, REG_SZ, RegistryValueCodec::EncodeText(REG_SZ, Program).value};
    const RegistryValue Title{{}, REG_SZ, RegistryValueCodec::EncodeText(REG_SZ, "HyperCommanderTest document").value};
    const auto Cleanup = qScopeGuard([&] {
        Right->CloseQuickView(Right->CurrentTabIndex()); Left->HideSystemView(); Main->RestoreWorkspace(Saved);
        for (const auto& Pair : QList<QPair<RegistryLocation, RegistryValue>>{{Ext, Association}, {Prog, Title}}) {
            const auto Current = Backend.Enumerate(Pair.first);
            if (!Current) continue;
            if (!Current.value.keys.isEmpty() || Current.value.values.size() > 1
                || (!Current.value.values.isEmpty() && Current.value.values.first() != Pair.second)) {
                QTest::qFail("External change to owned Classes fixture; preserve", __FILE__, __LINE__); continue;
            }
            if (!Current.value.values.isEmpty()) Backend.DeleteValue(Pair.first, {});
            Backend.DeleteKey(Pair.first);
            if (Backend.Enumerate(Pair.first)) QTest::qFail("Owned Classes fixture remains", __FILE__, __LINE__);
        }
    });
    QCOMPARE(Backend.Create(Ext), quint32(0)); QCOMPARE(Backend.Create(Prog), quint32(0));
    QCOMPARE(Backend.Write(Ext, Association), quint32(0)); QCOMPARE(Backend.Write(Prog, Title), quint32(0));
    Main->show(); Main->activateWindow(); QTRY_VERIFY(Main->isActiveWindow()); emit Left->sig_NotifyPanelActivated();
    auto* Bridge = TyStCommandMgr::GetInstance()->Bridge(); QVERIFY(Bridge->ExecuteCommand("hc.navigate.systemRegistry"));
    QTRY_VERIFY(Left->RegistryView()); auto* View = Left->RegistryView();
    View->Navigate("HKCR\\" + Extension, QString());
    QTRY_COMPARE(View->PathText(), RegistryLocation(Hive::ClassesRoot, Extension).Path());
    QTRY_COMPARE(View->CurrentSource().data(RegistryTreeModel::ValueNameRole).toString(), QString());
    QVERIFY(Bridge->ExecuteCommand("hc.registry.describe"));
    QTRY_VERIFY(qobject_cast<CRegistryDetailView*>(Right->GetFocusView(Right->CurrentTabIndex())));
    auto* Detail = qobject_cast<CRegistryDetailView*>(Right->GetFocusView(Right->CurrentTabIndex()));
    Detail->ShowLinks(); auto* Links = Detail->findChild<RegistryLinks*>(); QVERIFY(Links);
    QTRY_VERIFY_WITH_TIMEOUT(!Links->Loading(), 10000);
    const auto HasForward = [&] {
        for (const auto& Link : Links->Result().forward) if (Link.target && Link.target->key == Program && !Link.error) return true;
        return false;
    };
    QTRY_VERIFY(HasForward()); // Forward links must not wait for the full index.
    QTRY_VERIFY_WITH_TIMEOUT(Links->FastIndexComplete(), 90000);
    const auto Artifacts = qEnvironmentVariable("HC_UI_ARTIFACT_DIR");
    if (!Artifacts.isEmpty()) QVERIFY(Main->grab().save(QDir(Artifacts).filePath("registry-links.png")));
    auto* Pause = Detail->findChild<fm::ui::Button*>("hc.registry.indexPause"); QVERIFY(Pause);
    if (Links->IndexLoading()) { Pause->click(); QVERIFY(!Links->IndexLoading()); }
    Detail->ShowDescription(); QVERIFY(!Links->IndexLoading());
    Detail->ShowLinks(); QTRY_VERIFY(!Links->Loading());
    QVERIFY(Bridge->ExecuteCommand("hc.registry.follow"));
    QTRY_COMPARE(View->PathText(), RegistryLocation(Hive::ClassesRoot, Program).Path());
}