#pragma once

/// UIs
#include "HyperCommander/UIs/Components/cmpRegistryDetailView.hpp"
#include "HyperCommander/UIs/registryEditors.hpp"
/// 외부 라이브러리
#include <fmwidgets/ValueInputs.h>
/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QProcess>

inline void RegistryFocusSmoke(QMainUI* Main, const std::function<bool(HWND)>& Activate)
{
    using namespace hc::registry;
    auto* Bridge = TyStCommandMgr::GetInstance()->Bridge(); QVERIFY(Bridge);
    auto* Left = Main->findChild<CmpPanel*>("cmpLeftPanel"); auto* Right = Main->findChild<CmpPanel*>("cmpRightPanel");
    QVERIFY(Left); QVERIFY(Right);
    const auto Saved = Main->CaptureWorkspace();
    WinRegistryBackend Backend;
    const auto Base = RegistryLocation::Parse("HKCU\\Software\\HyperCommanderTests").value();
    QCOMPARE(Backend.Create(Base), quint32(0));
    const auto Key = Base.Child("RegistryEditor-task_4ceba737038e-" + QUuid::createUuid().toString(QUuid::WithoutBraces));
    QCOMPARE(Backend.Create(Key), quint32(0));
    QPointer<CRegistryDetailView> Detail;
    const auto Cleanup = qScopeGuard([&] {
        if (Detail) { Detail->Editor()->Discard(); Right->CloseQuickView(Right->CurrentTabIndex()); }
        Left->HideSystemView(); Main->RestoreWorkspace(Saved); Backend.DeleteKey(Key);
    });
    const RegistryValue Original{"focus", REG_SZ, RegistryValueCodec::EncodeText(REG_SZ, "original").value};
    QCOMPARE(Backend.Write(Key, Original), quint32(0));
    Main->show(); const auto Root = reinterpret_cast<HWND>(Main->winId()); QVERIFY(Activate(Root));
    emit Left->sig_NotifyPanelActivated(); QVERIFY(Bridge->ExecuteCommand("hc.navigate.systemRegistry"));
    QTRY_VERIFY(Left->RegistryView()); auto* View = Left->RegistryView();
    QVERIFY(View->Navigate(Key.Path(), QString("focus")));
    QTRY_COMPARE(View->CurrentSource().data(RegistryTreeModel::ValueNameRole).toString(), QString("focus"));
    QVERIFY(Bridge->ExecuteCommand("hc.registry.edit"));
    QTRY_VERIFY(qobject_cast<CRegistryDetailView*>(Right->GetFocusView(Right->CurrentTabIndex())));
    Detail = qobject_cast<CRegistryDetailView*>(Right->GetFocusView(Right->CurrentTabIndex()));
    auto* Text = Detail->Editor()->findChild<fm::ui::TextField*>("hc.registry.text"); QVERIFY(Text);
    Text->setText("draft"); Text->setFocus(); Text->setCursorPosition(Text->text().size());
    QTRY_VERIFY(Text->hasFocus()); QTRY_VERIFY(Text->height() < 100);

    QProcess Peer; Peer.start(QCoreApplication::applicationFilePath(), {"--focus-peer"});
    QTRY_VERIFY(Peer.canReadLine());
    const auto PeerWindow = reinterpret_cast<HWND>(Peer.readLine().trimmed().toULongLong()); QVERIFY(IsWindow(PeerWindow));
    const auto StopPeer = qScopeGuard([&] {
        if (IsWindow(PeerWindow)) SendMessageW(PeerWindow, WM_APP + 117, reinterpret_cast<WPARAM>(Root), 0);
        Peer.kill(); Peer.waitForFinished(5000);
    });
    QVERIFY(Activate(PeerWindow)); QTest::qWait(1000);
    QVERIFY(SendMessageW(PeerWindow, WM_APP + 117, reinterpret_cast<WPARAM>(Root), 0));
    QTRY_COMPARE(GetForegroundWindow(), Root);
    Text->setFocus(); QTRY_VERIFY(Text->hasFocus());
    const auto AltTab = [] {
        INPUT Keys[4]{}; for (auto& Key : Keys) Key.type = INPUT_KEYBOARD;
        Keys[0].ki.wVk = VK_MENU; Keys[1].ki.wVk = VK_TAB;
        Keys[2].ki.wVk = VK_TAB; Keys[2].ki.dwFlags = KEYEVENTF_KEYUP;
        Keys[3].ki.wVk = VK_MENU; Keys[3].ki.dwFlags = KEYEVENTF_KEYUP;
        if (SendInput(1, Keys, sizeof(INPUT)) != 1) return false;
        QTest::qWait(30);
        if (SendInput(2, Keys + 1, sizeof(INPUT)) != 2) return false;
        QTest::qWait(80);
        return SendInput(1, Keys + 3, sizeof(INPUT)) == 1;
    };
    const auto TypeX = [&] {
        if (GetForegroundWindow() != Root) return false;
        INPUT Keys[2]{}; for (auto& Key : Keys) Key.type = INPUT_KEYBOARD;
        Keys[0].ki.wScan = 'x'; Keys[0].ki.dwFlags = KEYEVENTF_UNICODE;
        Keys[1].ki.wScan = 'x'; Keys[1].ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
        return SendInput(2, Keys, sizeof(INPUT)) == 2;
    };
    QVERIFY(AltTab()); QTRY_COMPARE(GetForegroundWindow(), PeerWindow);
    QCOMPARE(Text->text(), QString("draft")); QVERIFY(Backend.Read(Key, "focus").value == Original);
    QVERIFY(AltTab()); QTRY_COMPARE(GetForegroundWindow(), Root);
    QTRY_VERIFY(Text->hasFocus()); QVERIFY(TypeX()); QTRY_COMPARE(Text->text(), QString("draftx"));
    QVERIFY(AltTab()); QTRY_COMPARE(GetForegroundWindow(), PeerWindow);
    QVERIFY(SendMessageW(PeerWindow, WM_APP + 117, reinterpret_cast<WPARAM>(Root), 0));
    QTRY_COMPARE(GetForegroundWindow(), Root); QTRY_VERIFY(Text->hasFocus());
    QVERIFY(TypeX()); QTRY_COMPARE(Text->text(), QString("draftxx"));
    const auto Click = [&](QWidget* Widget) {
        const auto Visible = Widget->visibleRegion().boundingRect();
        qInfo() << "native click" << Widget->objectName() << Widget->rect() << Visible << Widget->mapTo(Main, Visible.center());
        if (Visible.isEmpty()) return false;
        const auto Position = Widget->mapTo(Main, Visible.center());
        POINT Point{qRound(Position.x() * Main->devicePixelRatioF()), qRound(Position.y() * Main->devicePixelRatioF())};
        if (!ClientToScreen(Root, &Point)) return false;
        INPUT Events[3]{}; for (auto& Event : Events) Event.type = INPUT_MOUSE;
        Events[0].mi.dx = MulDiv(Point.x - GetSystemMetrics(SM_XVIRTUALSCREEN), 65535, GetSystemMetrics(SM_CXVIRTUALSCREEN) - 1);
        Events[0].mi.dy = MulDiv(Point.y - GetSystemMetrics(SM_YVIRTUALSCREEN), 65535, GetSystemMetrics(SM_CYVIRTUALSCREEN) - 1);
        Events[0].mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
        Events[1].mi.dwFlags = MOUSEEVENTF_LEFTDOWN; Events[2].mi.dwFlags = MOUSEEVENTF_LEFTUP;
        return SendInput(3, Events, sizeof(INPUT)) == 3;
    };
    auto* Grid = View->findChild<Qtitan::TreeGrid*>("hc.registry.items"); QVERIFY(Grid);
    QVERIFY(Click(Grid->viewport())); QTRY_VERIFY(!Text->hasFocus());
    QCOMPARE(Text->text(), QString("draftxx")); QVERIFY(Detail->Editor()->IsModified());
    QVERIFY(Click(Text));
    QTest::qWait(500);
    if (!Text->hasFocus()) { qWarning() << "click focus" << QApplication::focusWidget(); Main->grab().save(qEnvironmentVariable("HC_UI_ARTIFACT_DIR") + "/registry-focus-click.png"); }
    QTRY_VERIFY(Text->hasFocus()); Text->setCursorPosition(Text->text().size());
    QVERIFY(TypeX()); QTRY_COMPARE(Text->text(), QString("draftxxx"));
    QVERIFY(Backend.Read(Key, "focus").value == Original);
}