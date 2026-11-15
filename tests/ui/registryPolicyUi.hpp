#pragma once
/// UIs
#include "HyperCommander/UIs/Components/cmpRegistryDetailView.hpp"
#include "HyperCommander/UIs/registryPolicyEditor.hpp"
/// 외부 라이브러리
#include <fmwidgets/ValueInputs.h>
#include <fmwidgets/SegmentedControl.h>
#include <QtitanGrid.h>

inline void RegistryPolicySmoke(QMainUI* Main)
{
    using namespace hc::registry;
    auto* Bridge = TyStCommandMgr::GetInstance()->Bridge(); QVERIFY(Bridge);
    auto* Left = Main->findChild<CmpPanel*>("cmpLeftPanel"); auto* Right = Main->findChild<CmpPanel*>("cmpRightPanel"); QVERIFY(Left); QVERIFY(Right);
    const auto Saved = Main->CaptureWorkspace(); WinRegistryBackend Backend;
    const auto Base = RegistryLocation::Parse("HKCU\\Software\\HyperCommanderTests").value(); QCOMPARE(Backend.Create(Base), quint32(0));
    const auto Key = Base.Child("RegistryEditor-task_4ceba737038e-" + QUuid::createUuid().toString(QUuid::WithoutBraces)); QCOMPARE(Backend.Create(Key), quint32(0));
    QPointer<CRegistryDetailView> Detail;
    const auto Cleanup = qScopeGuard([&] {
        if (Detail) { Detail->PolicyEditor()->Discard(); Right->CloseQuickView(Right->CurrentTabIndex()); }
        Left->HideSystemView(); Main->RestoreWorkspace(Saved); Backend.DeleteKey(Key);
    });
    QCOMPARE(Backend.Write(Key, {"On", REG_DWORD, RegistryValueCodec::EncodeNumber(REG_DWORD, 1).value}), quint32(0));
    QCOMPARE(Backend.Write(Key, {"Text", REG_SZ, RegistryValueCodec::EncodeText(REG_SZ, "before").value}), quint32(0));
    Main->show(); Main->activateWindow(); QTRY_VERIFY(Main->isActiveWindow()); emit Left->sig_NotifyPanelActivated();
    QVERIFY(Bridge->ExecuteCommand("hc.navigate.systemRegistry")); QTRY_VERIFY(Left->RegistryView()); auto* View = Left->RegistryView();
    QTRY_VERIFY_WITH_TIMEOUT(!View->Descriptions()->Loading(), 30000);
    QVERIFY(Bridge->ExecuteCommand("hc.registry.policies")); QVERIFY(View->PoliciesShown());
    auto Catalog = std::make_shared<AdmxCatalog>(); AdmxPolicy Policy;
    Policy.id = "HyperCommanderTest:Policy"; Policy.title = QStringLiteral("시험 정책"); Policy.key = Key.key; Policy.scope = "User";
    Policy.definition = *AdmxCatalog::Parse("<policy key=\"" + Key.key.toUtf8() + "\" valueName=\"On\"><elements><text id=\"text\" valueName=\"Text\" required=\"true\"/></elements></policy>");
    Catalog->policies.append(Policy); Catalog->keyIndex[Key.key.toCaseFolded()] = {0}; View->Policies()->Reload(Catalog);
    QTRY_VERIFY(!View->Policies()->Loading());
    View->SetFilter(QStringLiteral("시험 정책"));
    auto* Grid = View->findChild<Qtitan::TreeGrid*>("hc.registry.policyGrid"); QVERIFY(Grid); auto* Rows = Grid->view<Qtitan::GridTreeView>();
    QTRY_COMPARE(Rows->getRowCount(), 2); Rows->setFocusedRowIndex(1);
    QTRY_VERIFY(View->CurrentPolicy()); QCOMPARE(View->CurrentPolicy()->observation.state, PolicyState::Enabled);
    QVERIFY(!View->CanExecuteCommand("hc.registry.delete"));
    QVERIFY(Bridge->ExecuteCommand("hc.registry.open"));
    QTRY_VERIFY(qobject_cast<CRegistryDetailView*>(Right->GetFocusView(Right->CurrentTabIndex())));
    Detail = qobject_cast<CRegistryDetailView*>(Right->GetFocusView(Right->CurrentTabIndex()));
    auto* Editor = Detail->PolicyEditor(); QVERIFY(Editor->isVisible());
    auto* Input = Editor->findChild<fm::ui::TextField*>("hc.policy.text"); QVERIFY(Input); QCOMPARE(Input->text(), QString("before"));
    const auto Artifacts = qEnvironmentVariable("HC_UI_ARTIFACT_DIR");
    if (!Artifacts.isEmpty()) { QDir().mkpath(Artifacts); QVERIFY(Main->grab().save(QDir(Artifacts).filePath("registry-policy.png"))); }
    Input->setText("draft"); QVERIFY(Editor->IsModified()); View->SetFilter("no-match-policy"); QTRY_COMPARE(Rows->getRowCount(), 0);
    QCOMPARE(Input->text(), QString("draft")); // Moving/filtering must not discard the detail draft.
    Editor->Discard(); QVERIFY(!Editor->IsModified());
    View->SetFilter(QStringLiteral("시험 정책")); QTRY_COMPARE(Rows->getRowCount(), 2); Rows->setFocusedRowIndex(1);
    const auto State = View->CaptureState(); QVERIFY(State["showPolicies"].toBool());
    QVERIFY(Bridge->ExecuteCommand("hc.registry.browse")); QVERIFY(!View->PoliciesShown());
    View->RestoreState(State); QTRY_VERIFY(View->PoliciesShown());
    QCOMPARE(RegistryValueCodec::DecodeText(Backend.Read(Key, "Text").value).text, QString("before"));
}
