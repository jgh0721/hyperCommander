#pragma once

/// UIs
#include "HyperCommander/UIs/Components/cmpRegistryView.hpp"
#include "HyperCommander/UIs/Components/cmpRegistryDetailView.hpp"
#include "HyperCommander/UIs/registryEditors.hpp"
/// 외부 라이브러리
#include <fmwidgets/ContentDialog.h>
#include <fmwidgets/ValueInputs.h>
#include <QTimer>
#include <QClipboard>

inline void RegistryMutationSmoke(QMainUI* Main)
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
    const auto Cleanup = qScopeGuard([&] {
        if (auto* Detail = qobject_cast<CRegistryDetailView*>(Right->GetFocusView(Right->CurrentTabIndex()))) {
            Detail->Editor()->Discard(); Right->CloseQuickView(Right->CurrentTabIndex());
        }
        Left->HideSystemView(); Main->RestoreWorkspace(Saved);
        Backend.DeleteKey(Key.Child("child")); Backend.DeleteKey(Key);
    });
    QCOMPARE(Backend.Write(Key, {"original", REG_BINARY, QByteArray::fromHex("00ff80")}), quint32(0));
    Main->show(); Main->activateWindow(); QTRY_VERIFY(Main->isActiveWindow()); emit Left->sig_NotifyPanelActivated();
    QVERIFY(Bridge->ExecuteCommand("hc.navigate.systemRegistry")); QTRY_VERIFY(Left->RegistryView());
    auto* View = Left->RegistryView();
    QVERIFY(View->Navigate(Key.Path(), QString("original")));
    QTRY_COMPARE(View->CurrentSource().data(RegistryTreeModel::ValueNameRole).toString(), QString("original"));
    View->EnsureKeyboardFocusOnView(); QTest::qWait(50);
    QVERIFY(Bridge->ExecuteCommand("hc.registry.rename"));
    QTRY_VERIFY(View->findChild<fm::ui::InlineNameEditor*>("hc.registry.renameEditor"));
    auto* Inline = View->findChild<fm::ui::InlineNameEditor*>("hc.registry.renameEditor");
    Inline->setText("renamed"); QTest::keyClick(Inline, Qt::Key_Return);
    QTRY_VERIFY(Backend.Read(Key, "renamed")); QTRY_VERIFY(!Backend.Read(Key, "original"));
    QCOMPARE(Backend.Read(Key, "renamed").value.bytes, QByteArray::fromHex("00ff80"));
    QTRY_COMPARE(View->CurrentSource().data(RegistryTreeModel::ValueNameRole).toString(), QString("renamed"));
    QVERIFY(Bridge->ExecuteCommand("hc.registry.copyName")); QCOMPARE(QApplication::clipboard()->text(), QString("renamed"));
    QVERIFY(Bridge->ExecuteCommand("hc.registry.copyData")); QCOMPARE(QApplication::clipboard()->text(), QString("00 ff 80"));
    const auto DialogCommand = [&](const char* Command, const std::function<void(fm::ui::ContentDialog*)>& Fill) {
        QTimer Answer, Timeout; bool Answered = false, TimedOut = false;
        QObject::connect(&Answer, &QTimer::timeout, [&] {
            auto* Dialog = qobject_cast<fm::ui::ContentDialog*>(QApplication::activeModalWidget()); if (!Dialog) return;
            Answer.stop(); Answered = true; Fill(Dialog);
        });
        QObject::connect(&Timeout, &QTimer::timeout, [&] {
            TimedOut = true; Answer.stop();
            if (auto* Dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget())) Dialog->reject();
        });
        Answer.start(25); Timeout.setSingleShot(true); Timeout.start(10000);
        const bool Executed = Bridge->ExecuteCommand(Command);
        Answer.stop(); Timeout.stop(); return Executed && Answered && !TimedOut;
    };
    QVERIFY(DialogCommand("hc.registry.newKey", [](fm::ui::ContentDialog* Dialog) {
        Dialog->findChild<fm::ui::TextField*>("hc.registry.newName")->setText("child");
        Dialog->button(fm::ui::ContentDialog::PrimaryButton)->click();
    }));
    QTRY_VERIFY(Backend.Enumerate(Key.Child("child")));
    QTRY_COMPARE(View->PathText(), Key.Child("child").Path());
    QVERIFY(View->Navigate(Key.Path(), QString("renamed")));
    QTRY_COMPARE(View->CurrentSource().data(RegistryTreeModel::ValueNameRole).toString(), QString("renamed"));
    QVERIFY(DialogCommand("hc.registry.newValue", [](fm::ui::ContentDialog* Dialog) {
        Dialog->findChild<fm::ui::TextField*>("hc.registry.newName")->setText("number");
        auto* Type = Dialog->findChild<fm::ui::ChoiceBox*>("hc.registry.newType"); Type->setCurrentIndex(Type->findData(quint32(REG_DWORD)));
        Dialog->button(fm::ui::ContentDialog::PrimaryButton)->click();
    }));
    QTRY_VERIFY(Backend.Read(Key, "number")); QCOMPARE(Backend.Read(Key, "number").value.type, quint32(REG_DWORD));
    QTRY_COMPARE(View->CurrentSource().data(RegistryTreeModel::ValueNameRole).toString(), QString("number"));
    emit Left->sig_NotifyPanelActivated();
    QVERIFY(DialogCommand("hc.registry.delete", [](fm::ui::ContentDialog* Dialog) { Dialog->button(fm::ui::ContentDialog::PrimaryButton)->click(); }));
    QTRY_VERIFY(!Backend.Read(Key, "number"));
    QVERIFY(Backend.Read(Key, "renamed"));
}