#pragma once
#include "HyperCommander/UIs/registryImportDialog.hpp"
#include "HyperCommander/UIs/Components/cmpRegistryView.hpp"
#include "commonLibs/system/registryFile.hpp"
#include "commonLibs/system/registryValue.hpp"
#include "UniqueLibs/registry/registryCommands.hpp"
#include <QAbstractProxyModel>
#include <QTimer>
#include <QTemporaryDir>
#include <QCheckBox>
#include <QPushButton>
inline void RegistryImportSmoke(QMainUI* Main)
{
    using namespace hc::registry;
    auto* Bridge = TyStCommandMgr::GetInstance()->Bridge(); QVERIFY(Bridge);
    auto* Panel = Main->findChild<CmpPanel*>("cmpLeftPanel"); QVERIFY(Panel);
    const auto Saved = Main->CaptureWorkspace(); WinRegistryBackend Backend;
    const RegistryLocation Base{Hive::CurrentUser, "Software\\HyperCommanderTests"}; QCOMPARE(Backend.Create(Base), quint32(0));
    const auto Key = Base.Child("RegistryImport-" + QUuid::createUuid().toString(QUuid::WithoutBraces));
    QCOMPARE(Backend.Create(Key), quint32(0));
    const RegistryValue Before{"text", REG_SZ, RegistryValueCodec::EncodeText(REG_SZ, "before").value};
    auto After = Before; After.bytes = RegistryValueCodec::EncodeText(REG_SZ, "after").value;
    QCOMPARE(Backend.Write(Key, Before), quint32(0));
    const auto Cleanup = qScopeGuard([&] {
        Panel->HideSystemView(); Main->RestoreWorkspace(Saved);
        const auto Current = Backend.Read(Key, Before.name);
        if (Current && Current.value != Before && Current.value != After) { QTest::qFail("Unexpected external fixture change", __FILE__, __LINE__); return; }
        if (Current) Backend.DeleteValue(Key, Before.name);
        if (Backend.DeleteKey(Key)) QTest::qFail("Import fixture cleanup failed", __FILE__, __LINE__);
    });
    QTemporaryDir Directory; const auto Path = Directory.filePath("fixture.reg");
    QFile File(Path); QVERIFY(File.open(QIODevice::WriteOnly));
    const auto Bytes = RegFileWriter::Write({{OperationKind::CreateKey, Key}, {OperationKind::SetValue, Key, After}});
    QCOMPARE(File.write(Bytes), Bytes.size()); File.close();
    Panel->HideSystemView(); Panel->ChangeCurrentPath(Directory.path(), {});
    Main->show(); Main->activateWindow(); QTRY_VERIFY(Main->isActiveWindow()); emit Panel->sig_NotifyPanelActivated();
    auto* Grid = qobject_cast<CGridView*>(Panel->GetFocusView(Panel->CurrentTabIndex())); QVERIFY(Grid);
    QModelIndex Visible;
    const auto Find = [&] {
        auto* Rows = Grid->BaseView();
        for (int I = 0; I < Rows->getRowCount(); ++I) {
            auto Index = Rows->getRow(I).modelIndex(0), Source = Index;
            while (const auto* Proxy = qobject_cast<const QAbstractProxyModel*>(Source.model())) Source = Proxy->mapToSource(Source);
            auto* Model = qobject_cast<const CFSModel*>(Source.model()); if (!Model) continue;
            const auto Entry = Model->GetFileInfo(Source);
            if (Entry && Entry->Name_ == "fixture.reg") { Visible = Index; return true; }
        }
        return false;
    };
    QTRY_VERIFY_WITH_TIMEOUT(Find(), 30000);
    bool SawPreview = false, Applied = false; QTimer Answer, Timeout;
    QObject::connect(&Answer, &QTimer::timeout, [&] {
        auto* Dialog = dynamic_cast<RegistryImportDialog*>(QApplication::activeModalWidget()); if (!Dialog) return;
        SawPreview = true; auto* Backup = Dialog->findChild<QCheckBox*>("hc.registry.import.backup"); if (Backup) Backup->setChecked(true);
        if (!Dialog->button(fm::ui::ContentDialog::PrimaryButton)->isEnabled()) return;
        if (Dialog->Preview().plan.steps.size() != 1) return;
        const auto Artifacts = qEnvironmentVariable("HC_UI_ARTIFACT_DIR");
        if (!Artifacts.isEmpty()) Dialog->grab().save(QDir(Artifacts).filePath("registry-import.png"));
        Applied = true; Answer.stop(); Dialog->button(fm::ui::ContentDialog::PrimaryButton)->click();
    });
    QObject::connect(&Timeout, &QTimer::timeout, [&] { if (auto* Dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget())) Dialog->reject(); });
    Answer.start(25); Timeout.setSingleShot(true); Timeout.start(30000);
    Panel->ReturnOnCurrentTab(Visible); Answer.stop(); Timeout.stop();
    QVERIFY(SawPreview); QVERIFY(Applied);
    QTRY_COMPARE_WITH_TIMEOUT(Backend.Read(Key, Before.name).value.bytes, After.bytes, 30000);
    emit Panel->sig_NotifyPanelActivated(); QVERIFY(Bridge->ExecuteCommand("hc.navigate.systemRegistry"));
    QTRY_VERIFY(Panel->RegistryView()); auto* View = Panel->RegistryView(); View->ReloadHistory();
    QTRY_VERIFY(!View->History()->Loading()); QTRY_VERIFY(Bridge->Action("hc.registry.undo")->isEnabled());
    QVERIFY(Bridge->ExecuteCommand("hc.registry.undo"));
    QTRY_COMPARE_WITH_TIMEOUT(Backend.Read(Key, Before.name).value.bytes, Before.bytes, 30000);
    // A reload must replace both the preview bytes and the provider revision
    // guard; it must not keep applying the first materialized snapshot.
    {
        const auto ReloadPath = Directory.filePath("reloaded.reg");
        QFile ReloadFile(ReloadPath); QVERIFY(ReloadFile.open(QIODevice::WriteOnly));
        auto ReloadValue = After; ReloadValue.bytes = RegistryValueCodec::EncodeText(REG_SZ, "reloaded").value;
        ReloadFile.write(RegFileWriter::Write({{OperationKind::SetValue, Key, ReloadValue}})); ReloadFile.close();
        RegistryImportDialog Reload(Path, View->Descriptions(), Main);
        auto Count = std::make_shared<std::atomic_int>(0);
        Reload.loadSource = [Path, ReloadPath, Count](const std::atomic_bool*) {
            const int Revision = ++*Count; RegistryImportSource Source;
            Source.path = Revision == 1 ? Path : ReloadPath;
            Source.verify = [Revision] { return Revision == 2; }; return Source;
        };
        int Stage = 0; QTimer Next;
        QObject::connect(&Next, &QTimer::timeout, [&] {
            if (!Reload.button(fm::ui::ContentDialog::PrimaryButton)->isEnabled()) return;
            if (Stage++ == 0) { Reload.findChild<QPushButton*>("hc.registry.import.reload")->click(); return; }
            Reload.reject();
        });
        Next.start(25); Timeout.start(20000); Reload.exec(); Next.stop(); Timeout.stop();
        QCOMPARE(Count->load(), 2); QCOMPARE(Reload.SourcePath(), ReloadPath);
        QVERIFY(Reload.Source().verify && Reload.Source().verify());
        QCOMPARE(Reload.Preview().operations.last().value.bytes, ReloadValue.bytes);
        QCOMPARE(Backend.Read(Key, Before.name).value.bytes, Before.bytes);
    }
    // A malformed file must remain a preview error and leave the registry alone.
    File.setFileName(Path); QVERIFY(File.open(QIODevice::WriteOnly | QIODevice::Truncate));
    File.write("Windows Registry Editor Version 5.00\r\n[HKEY_CURRENT_USER\\Software\\HyperCommanderTests]\r\n\"broken\"=hex:zz\r\n"); File.close();
    RegistryImportDialog Invalid(Path, View->Descriptions(), Main); bool SawError = false; QTimer RejectInvalid;
    QObject::connect(&RejectInvalid, &QTimer::timeout, [&] {
        if (Invalid.Preview().error.isEmpty()) return;
        SawError = true; QVERIFY(!Invalid.button(fm::ui::ContentDialog::PrimaryButton)->isEnabled()); Invalid.reject();
    });
    RejectInvalid.start(25); Timeout.start(20000); Invalid.exec(); RejectInvalid.stop(); Timeout.stop();
    QVERIFY(SawError); QCOMPARE(Backend.Read(Key, Before.name).value.bytes, Before.bytes);
    QVERIFY(File.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QVERIFY(File.resize(RegFileParser::MaxFileBytes + 1)); File.close();
    RegistryImportDialog Oversized(Path, View->Descriptions(), Main); bool SawSizeError = false; QTimer RejectLarge;
    QObject::connect(&RejectLarge, &QTimer::timeout, [&] {
        if (Oversized.Preview().error.isEmpty()) return;
        SawSizeError = true; QVERIFY(!Oversized.button(fm::ui::ContentDialog::PrimaryButton)->isEnabled()); Oversized.reject();
    });
    RejectLarge.start(25); Timeout.start(20000); Oversized.exec(); RejectLarge.stop(); Timeout.stop();
    QVERIFY(SawSizeError); QCOMPARE(Backend.Read(Key, Before.name).value.bytes, Before.bytes);
    // A materialized source revision failure must abort before applying any step.
    QVERIFY(File.open(QIODevice::WriteOnly | QIODevice::Truncate)); QCOMPARE(File.write(Bytes), Bytes.size()); File.close();
    auto* Commands = Main->findChild<RegistryCommands*>(); QVERIFY(Commands);
    auto CheckedSource = std::make_shared<std::atomic_bool>(false); bool AcceptedPreview = false;
    QTimer RejectChanged;
    QObject::connect(&RejectChanged, &QTimer::timeout, [&] {
        if (auto* Dialog = dynamic_cast<RegistryImportDialog*>(QApplication::activeModalWidget())) {
            if (!AcceptedPreview && Dialog->button(fm::ui::ContentDialog::PrimaryButton)->isEnabled()) {
                AcceptedPreview = true; Dialog->button(fm::ui::ContentDialog::PrimaryButton)->click();
            }
        } else if (CheckedSource->load()) {
            if (auto* Dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget())) Dialog->reject();
        }
    });
    RejectChanged.start(25); Timeout.start(20000);
    Commands->OpenImport(Path, {}, {}, [CheckedSource] { CheckedSource->store(true); return false; });
    QTRY_VERIFY_WITH_TIMEOUT(CheckedSource->load() && !Commands->Busy(), 20000);
    RejectChanged.stop(); Timeout.stop(); QVERIFY(AcceptedPreview);
    QCOMPARE(Backend.Read(Key, Before.name).value.bytes, Before.bytes);
}