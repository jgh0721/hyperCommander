#pragma once
#include "HyperCommander/UIs/Components/cmpRegistryView.hpp"
#include "commonLibs/system/registryValue.hpp"
#include <fmwidgets/FindBox.h>
#include <fmwidgets/Button.h>
#include <QtitanGrid.h>
#include <fmwidgets/ContentDialog.h>
#include <fmwidgets/ValueInputs.h>
#include <QCheckBox>
#include <QTimer>
#include <QProcess>
#include <QScreen>
#include <QFile>
#include "UniqueLibs/registry/registryCommands.hpp"
inline void RegistrySearchSmoke(QMainUI* Main)
{
    using namespace hc::registry;
    auto* Bridge = TyStCommandMgr::GetInstance()->Bridge(); QVERIFY(Bridge);
    auto* Panel = Main->findChild<CmpPanel*>("cmpLeftPanel"); QVERIFY(Panel);
    const auto Saved = Main->CaptureWorkspace(); WinRegistryBackend Backend;
    const RegistryLocation Root{Hive::CurrentUser, "Software\\HyperCommanderTests\\Search-" + QUuid::createUuid().toString(QUuid::WithoutBraces)};
    QVERIFY(!Backend.Enumerate(Root)); QCOMPARE(Backend.Create(Root), quint32(0));
    const RegistryValue Value{"NeedleName", REG_SZ, RegistryValueCodec::EncodeText(REG_SZ, "NeedleData").value};
    const auto Cleanup = qScopeGuard([&] {
        Panel->HideSystemView(); Main->RestoreWorkspace(Saved);
        const auto Current = Backend.Read(Root, Value.name);
        if (Current && Current.value != Value && Current.value.bytes != RegistryValueCodec::EncodeText(REG_SZ, "ChangedData").value) { QTest::qFail("External change in owned search fixture", __FILE__, __LINE__); return; }
        if (Current) Backend.DeleteValue(Root, Value.name);
        if (Backend.DeleteKey(Root) || Backend.Enumerate(Root)) QTest::qFail("Search fixture cleanup failed", __FILE__, __LINE__);
    });
    QCOMPARE(Backend.Write(Root, Value), quint32(0));
    Main->show(); Main->activateWindow(); QTRY_VERIFY(Main->isActiveWindow()); emit Panel->sig_NotifyPanelActivated();
    QVERIFY(Bridge->ExecuteCommand("hc.navigate.systemRegistry")); QTRY_VERIFY(Panel->RegistryView()); auto* View = Panel->RegistryView(); QVERIFY(View);
    QVERIFY(View->Navigate(Root.Path())); QTRY_COMPARE(View->PathText(), Root.Path());
    QVERIFY(Bridge->ExecuteCommand("hc.search.focus"));
    auto* Query = View->findChild<fm::ui::FindBox*>("hc.registry.findQuery"); QVERIFY(Query); QVERIFY(Query->isVisible());
    Query->setText("NeedleData"); QTest::keyClick(Query, Qt::Key_Return);
    QTRY_VERIFY(View->SearchShown()); QTRY_VERIFY_WITH_TIMEOUT(View->Search()->Status().done, 30000);
    QCOMPARE(View->Search()->Rows().size(), 1);
    auto* FindBar = View->findChild<QWidget*>("hc.registry.findBar"); QVERIFY(FindBar);
    auto* CloseFind = View->findChild<fm::ui::Button*>("hc.registry.find.close"); QVERIFY(CloseFind);
    QTRY_VERIFY(FindBar->rect().contains(CloseFind->geometry()));
    auto* Grid = View->findChild<Qtitan::TreeGrid*>("hc.registry.searchGrid"); QVERIFY(Grid); auto* Rows = Grid->view<Qtitan::GridTreeView>();
    QTRY_VERIFY(Rows->getRowCount() >= 2); Rows->setFocusedRowIndex(1); QTRY_VERIFY(View->CurrentSearchResult().has_value());
    auto* Header = View->findChild<fm::ui::ScrollPanel*>("hc.registry.headerScroll"); QVERIFY(Header);
    Header->ensureWidgetVisible(CloseFind, 0, 0);
    QTRY_VERIFY(Header->viewport()->rect().contains(QRect(CloseFind->mapTo(Header->viewport(), QPoint()), CloseFind->size())));
    const auto Artifacts = qEnvironmentVariable("HC_UI_ARTIFACT_DIR");
    if (!Artifacts.isEmpty()) QVERIFY(Main->grab().save(QDir(Artifacts).filePath("registry-search-controls.png")));
    Header->ensureWidgetVisible(Query, 0, 0);
    if (!Artifacts.isEmpty()) {
        auto& Theme = fm::style::ThemeManager::instance(); const auto Scheme = Theme.scheme();
        const auto RestoreTheme = qScopeGuard([&] { Theme.setScheme(Scheme); });
        for (const bool Dark : {false, true}) {
            Theme.setScheme(Dark ? fm::style::ThemeManager::Scheme::Dark : fm::style::ThemeManager::Scheme::Light);
            QTest::qWait(50); QTRY_VERIFY(FindBar->rect().contains(CloseFind->geometry()));
            QVERIFY(Main->grab().save(QDir(Artifacts).filePath(Dark ? "registry-search-dark.png" : "registry-search-light.png")));
        }
    }
    QVERIFY(Bridge->ExecuteCommand("hc.registry.open")); QTRY_VERIFY(!View->SearchShown());
    QTRY_COMPARE(View->CurrentSource().data(RegistryTreeModel::ValueNameRole).toString(), Value.name);
    View->OpenFind(); QTest::keyClick(Query, Qt::Key_Escape); QVERIFY(!Query->isVisible());
    View->OpenFind(); QTest::keyClick(Query, Qt::Key_Return);
    QTRY_VERIFY(View->Search()->Status().done); QTRY_VERIFY(View->SearchShown());
    QTimer Answer, Deadline; int InputSeen = 0, PreviewSeen = 0;
    QObject::connect(&Answer, &QTimer::timeout, [&] {
        auto* Dialog = qobject_cast<fm::ui::ContentDialog*>(QApplication::activeModalWidget()); if (!Dialog) return;
        if (Dialog->objectName() == "hc.registry.replaceInput" && !InputSeen) {
            auto* Field = Dialog->findChild<fm::ui::TextField*>("hc.registry.replacement"); if (!Field) return;
            Field->setText("ChangedData"); ++InputSeen; Dialog->button(fm::ui::ContentDialog::PrimaryButton)->click();
        } else if (Dialog->objectName() == "hc.registry.replacePreview" && !PreviewSeen) {
            auto* Backup = Dialog->findChild<QCheckBox*>("hc.registry.replaceBackup"); if (!Backup) return;
            Backup->setChecked(true); if (!Dialog->button(fm::ui::ContentDialog::PrimaryButton)->isEnabled()) return;
            ++PreviewSeen; if (!Artifacts.isEmpty()) Dialog->grab().save(QDir(Artifacts).filePath("registry-replace.png"));
            Dialog->button(fm::ui::ContentDialog::PrimaryButton)->click();
        }
    });
    QObject::connect(&Deadline, &QTimer::timeout, [&] { if (auto* Dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget())) Dialog->reject(); });
    Answer.start(25); Deadline.setSingleShot(true); Deadline.start(20000);
    QVERIFY(Bridge->ExecuteCommand("hc.registry.replaceResults")); Answer.stop(); Deadline.stop();
    QCOMPARE(InputSeen, 1); QCOMPARE(PreviewSeen, 1);
    QTRY_COMPARE_WITH_TIMEOUT(Backend.Read(Root, Value.name).value.bytes, RegistryValueCodec::EncodeText(REG_SZ, "ChangedData").value, 30000);
    auto* Commands = Main->findChild<RegistryCommands*>(); QVERIFY(Commands);
    QTRY_VERIFY_WITH_TIMEOUT(!Commands->Busy(), 30000);
    View->ReloadHistory();
    QTRY_VERIFY(!View->History()->Loading()); QTRY_VERIFY(View->History()->UndoCandidate().has_value());
    QCOMPARE(View->History()->UndoCandidate()->plan.steps.size(), 1);
    QTRY_VERIFY(Bridge->Action("hc.registry.undo")->isEnabled()); QVERIFY(Bridge->ExecuteCommand("hc.registry.undo"));
    QTRY_COMPARE_WITH_TIMEOUT(Backend.Read(Root, Value.name).value.bytes, Value.bytes, 30000);
    QTRY_VERIFY_WITH_TIMEOUT(!Commands->Busy(), 30000);
    View->ShowSearch(false); QVERIFY(View->Navigate(Root.Path()));
    QTimer CompareAnswer; bool CompareAccepted = false;
    QObject::connect(&CompareAnswer, &QTimer::timeout, [&] {
        auto* Dialog = qobject_cast<fm::ui::ContentDialog*>(QApplication::activeModalWidget());
        if (!Dialog || Dialog->objectName() != "hc.registry.nameDialog" || CompareAccepted) return;
        auto* Field = Dialog->findChild<fm::ui::TextField*>("hc.registry.newName"); if (!Field) return;
        CompareAccepted = true; Field->setText(Root.Path()); Dialog->button(fm::ui::ContentDialog::PrimaryButton)->click();
    });
    CompareAnswer.start(25); Deadline.start(20000);
    QVERIFY(Bridge->ExecuteCommand("hc.registry.compare")); CompareAnswer.stop(); Deadline.stop();
    QPointer<QProcess> Compare;
    const auto FindProcess = [&] {
        for (auto* Process : Main->findChildren<QProcess*>()) if (Process->program().endsWith("HyperCompare.exe", Qt::CaseInsensitive)
            && Process->state() == QProcess::Running && Process->arguments().size() == 6) { Compare = Process; return true; }
        return false;
    };
    QTRY_VERIFY_WITH_TIMEOUT(FindProcess(), 20000);
    const DWORD ComparePid = DWORD(Compare->processId()); const QStringList Inputs{Compare->arguments()[3], Compare->arguments()[5]};
    const auto CloseCompare = [&] {
        EnumWindows([](HWND Window, LPARAM Param) -> BOOL {
            DWORD Pid = 0; GetWindowThreadProcessId(Window, &Pid);
            if (Pid == DWORD(Param) && IsWindowVisible(Window)) PostMessageW(Window, WM_CLOSE, 0, 0);
            return TRUE;
        }, LPARAM(ComparePid));
    };
    const auto StopCompare = qScopeGuard(CloseCompare);
    HWND CompareWindow = nullptr;
    const auto WindowReady = [&] {
        struct State { DWORD pid; HWND window; } Scan{ComparePid, nullptr};
        EnumWindows([](HWND Window, LPARAM Param) -> BOOL {
            auto* Scan = reinterpret_cast<State*>(Param); DWORD Pid = 0; GetWindowThreadProcessId(Window, &Pid);
            if (Pid == Scan->pid && IsWindowVisible(Window)) {
                wchar_t Title[1024]{}; GetWindowTextW(Window, Title, 1024);
                const auto Text = QString::fromWCharArray(Title);
                RECT Bounds{}; GetWindowRect(Window, &Bounds);
                if (Text == "HyperCompare" && Bounds.right - Bounds.left >= 700 && Bounds.bottom - Bounds.top >= 400) Scan->window = Window;
            }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&Scan)); CompareWindow = Scan.window; return CompareWindow != nullptr;
    };
    QTRY_VERIFY_WITH_TIMEOUT(WindowReady(), 20000);
    QTest::qWait(500);
    if (!Artifacts.isEmpty()) QVERIFY(Main->screen()->grabWindow(WId(CompareWindow)).save(QDir(Artifacts).filePath("registry-compare.png")));
    QFile Left(Inputs[0]), Right(Inputs[1]); QVERIFY(Left.open(QIODevice::ReadOnly)); QVERIFY(Right.open(QIODevice::ReadOnly));
    QCOMPARE(Left.readAll(), Right.readAll()); Left.close(); Right.close();
    const auto LeasePath = QFileInfo(Inputs[0]).absolutePath(); QVERIFY(QDir(LeasePath).exists());
    CloseCompare(); QTRY_VERIFY_WITH_TIMEOUT(Compare.isNull(), 20000);
    QTRY_VERIFY(!QDir(LeasePath).exists());
    QCOMPARE(Backend.Read(Root, Value.name).value.bytes, Value.bytes);
}