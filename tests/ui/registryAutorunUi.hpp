#pragma once
/// UIs
#include "HyperCommander/UIs/Components/cmpRegistryView.hpp"
#include "HyperCommander/UIs/Components/cmpRegistryDetailView.hpp"
/// 외부 라이브러리
#include <QtitanGrid.h>
#include <fmwidgets/WrapCheckBox.h>
#include <QMessageBox>
#include <QSysInfo>
#include <QTimer>

inline void RegistryAutorunSmoke(QMainUI* Main)
{
    using namespace hc::registry;
    if (QSysInfo::machineHostName().compare("iMonAGR-Win11", Qt::CaseInsensitive) != 0) QSKIP("Owned VM autorun fixture only");
    auto* Bridge = TyStCommandMgr::GetInstance()->Bridge(); QVERIFY(Bridge);
    auto* Left = Main->findChild<CmpPanel*>("cmpLeftPanel"); auto* Right = Main->findChild<CmpPanel*>("cmpRightPanel"); QVERIFY(Left); QVERIFY(Right);
    const auto Saved = Main->CaptureWorkspace(); WinRegistryBackend Backend;
    const RegistryLocation Run{Hive::CurrentUser, "Software\\Microsoft\\Windows\\CurrentVersion\\Run"};
    const auto Approval = StartupApprovedCodec::LocationFor(Run).value();
    const auto Name = "HyperCommanderTestRegistryP6-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
    const RegistryValue Command{Name, REG_SZ, RegistryValueCodec::EncodeText(REG_SZ, "\"C:\\Windows\\System32\\cmd.exe\" /c exit 0").value};
    const auto OriginalRun = Backend.Read(Run, Name), OriginalApproval = Backend.Read(Approval, Name);
    QVERIFY(OriginalRun.error == ERROR_FILE_NOT_FOUND || OriginalRun.error == ERROR_PATH_NOT_FOUND);
    QVERIFY(OriginalApproval.error == ERROR_FILE_NOT_FOUND || OriginalApproval.error == ERROR_PATH_NOT_FOUND);
    const bool RunExisted = bool(Backend.Enumerate(Run)), ApprovalExisted = bool(Backend.Enumerate(Approval));
    QCOMPARE(Backend.Create(Run), quint32(0));
    const auto Cleanup = qScopeGuard([&] {
        if (auto* Detail = qobject_cast<CRegistryDetailView*>(Right->GetFocusView(Right->CurrentTabIndex()))) { Detail->Editor()->Discard(); Right->CloseQuickView(Right->CurrentTabIndex()); }
        Left->HideSystemView(); Main->RestoreWorkspace(Saved);
        const auto Current = Backend.Read(Run, Name);
        if (Current && Current.value != Command) { QTest::qFail("Owned Run value changed externally; preserve fixture", __FILE__, __LINE__); return; }
        if (Current) Backend.DeleteValue(Run, Name);
        const auto Approved = Backend.Read(Approval, Name);
        if (Approved && StartupApprovedCodec::Decode(Approved.value).state == StartupApproval::State::Unknown) { QTest::qFail("Unexpected approval bytes; preserve fixture", __FILE__, __LINE__); return; }
        if (Approved) Backend.DeleteValue(Approval, Name);
        if (!RunExisted) Backend.DeleteKey(Run); if (!ApprovalExisted) Backend.DeleteKey(Approval);
        if (Backend.Read(Run, Name) || Backend.Read(Approval, Name)) QTest::qFail("Owned autorun fixture remains", __FILE__, __LINE__);
    });
    QCOMPARE(Backend.Write(Run, Command), quint32(0));
    Main->show(); Main->activateWindow(); QTRY_VERIFY(Main->isActiveWindow()); emit Left->sig_NotifyPanelActivated();
    QVERIFY(Bridge->ExecuteCommand("hc.navigate.systemRegistry")); QTRY_VERIFY(Left->RegistryView()); auto* View = Left->RegistryView();
    View->SetFilter(Name); QVERIFY(Bridge->ExecuteCommand("hc.registry.autoruns")); QVERIFY(View->AutorunsShown());
    QTRY_VERIFY_WITH_TIMEOUT(!View->Autoruns()->Loading(), 120000);
    auto* Grid = View->findChild<Qtitan::TreeGrid*>("hc.registry.autorunGrid"); QVERIFY(Grid); auto* Rows = Grid->view<Qtitan::GridTreeView>();
    const auto Select = [&] {
        for (int Row = 0; Row < Rows->getRowCount(); ++Row) {
            const auto Index = Rows->getRow(Row).modelIndex(0);
            if (Index.data(RegistryTreeModel::ValueNameRole).toString() == Name && Index.data(RegistryResultModel::PayloadRole).toMap()["view"].toInt() == int(hc::registry::View::Native64)) { Rows->setFocusedRowIndex(Row); return true; }
        }
        return false;
    };
    QTRY_VERIFY(Select()); QTRY_VERIFY(View->CurrentAutorun()); QVERIFY(View->CurrentAutorun()->canToggle);
    auto* HideMicrosoft = View->findChild<QCheckBox*>("hc.registry.hideMicrosoft"); QVERIFY(HideMicrosoft); QVERIFY(HideMicrosoft->isVisible());
    if (View->CurrentAutorun()->microsoft && !View->CurrentAutorun()->risk) {
        HideMicrosoft->setChecked(true); QTRY_VERIFY(!Select());
        HideMicrosoft->setChecked(false); QTRY_VERIFY(Select());
    }
    const auto Toggle = [&] {
        QTimer Answer, Timeout; bool Accepted = false;
        QObject::connect(&Answer, &QTimer::timeout, [&] { if (auto* Box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) if (Box->standardButtons().testFlag(QMessageBox::Yes)) { Accepted = true; Answer.stop(); Box->button(QMessageBox::Yes)->click(); } });
        QObject::connect(&Timeout, &QTimer::timeout, [&] { if (auto* Box = qobject_cast<QDialog*>(QApplication::activeModalWidget())) Box->reject(); });
        Answer.start(25); Timeout.setSingleShot(true); Timeout.start(10000);
        const bool Invoked = Bridge->ExecuteCommand("hc.registry.toggleAutorun"); Answer.stop(); Timeout.stop(); return Invoked && Accepted;
    };
    QVERIFY(Toggle()); QTRY_VERIFY(Backend.Read(Approval, Name));
    QTRY_COMPARE(StartupApprovedCodec::Decode(Backend.Read(Approval, Name).value).state, StartupApproval::State::Disabled);
    QCOMPARE(Backend.Read(Run, Name).value, Command);
    QTRY_VERIFY_WITH_TIMEOUT(!View->Autoruns()->Loading(), 120000); QTRY_VERIFY(Select());
    QTRY_VERIFY(View->CurrentAutorun() && !View->CurrentAutorun()->enabled);
    QVERIFY(Toggle()); QTRY_COMPARE(StartupApprovedCodec::Decode(Backend.Read(Approval, Name).value).state, StartupApproval::State::Enabled);
    QCOMPARE(Backend.Read(Run, Name).value, Command);
    QTRY_VERIFY_WITH_TIMEOUT(!View->Autoruns()->Loading(), 120000); QTRY_VERIFY(Select());
    QTRY_VERIFY(View->CurrentAutorun() && View->CurrentAutorun()->enabled);
    // A direct external approval write must refresh the active view without F5.
    QCOMPARE(Backend.Write(Approval, StartupApprovedCodec::Encode(Name, false, 123)), quint32(0));
    QTRY_VERIFY_WITH_TIMEOUT(View->CurrentAutorun() && !View->CurrentAutorun()->enabled, 30000);
    QCOMPARE(Backend.Write(Approval, StartupApprovedCodec::Encode(Name, true, 0)), quint32(0));
    QTRY_VERIFY_WITH_TIMEOUT(View->CurrentAutorun() && View->CurrentAutorun()->enabled, 30000);
    const auto Artifacts = qEnvironmentVariable("HC_UI_ARTIFACT_DIR"); if (!Artifacts.isEmpty()) QVERIFY(Main->grab().save(QDir(Artifacts).filePath("registry-autoruns.png")));
    const auto State = View->CaptureState(); QVERIFY(State["showAutoruns"].toBool());
    QVERIFY(Bridge->ExecuteCommand("hc.registry.open")); QTRY_COMPARE(View->PathText(), Run.Path());
    QTRY_COMPARE(View->CurrentSource().data(RegistryTreeModel::ValueNameRole).toString(), Name);
}
