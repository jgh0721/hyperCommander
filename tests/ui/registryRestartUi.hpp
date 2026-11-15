#pragma once

#include "registryHistoryUi.hpp"
#include <QJsonDocument>
#include <QSaveFile>

// The VM runner launches a fresh complete HC UI process for each phase and waits
// for its exit before starting the next, using one isolated settings directory.
inline void RegistryRestartSmoke(QMainUI* Main)
{
    using namespace hc::registry;
    const auto Phase = qEnvironmentVariableIntValue("HC_REGISTRY_RESTART_PHASE");
    if (Phase < 1 || Phase > 4) QSKIP("Dedicated sequential process restart runner required");
    const auto Directory = qEnvironmentVariable("HC_UI_LOCAL_ROOT"); QVERIFY(!Directory.isEmpty());
    const auto Marker = QDir(Directory).filePath("registry-restart.json");
    QJsonObject State;
    if (Phase != 1) { QFile File(Marker); QVERIFY(File.open(QIODevice::ReadOnly)); State = QJsonDocument::fromJson(File.readAll()).object(); }
    const auto KeyPath = Phase == 1 ? QString("HKCU\\Software\\HyperCommanderTests\\RegistryRestart-task_4ceba737038e-") + QUuid::createUuid().toString(QUuid::WithoutBraces) : State["key"].toString();
    const auto Key = RegistryLocation::Parse(KeyPath); QVERIFY(Key);
    QVERIFY(Key->key.startsWith("Software\\HyperCommanderTests\\RegistryRestart-task_4ceba737038e-"));
    WinRegistryBackend Backend;
    if (Phase == 1) {
        QCOMPARE(Backend.Enumerate(*Key).error, quint32(ERROR_FILE_NOT_FOUND)); QCOMPARE(Backend.Create(*Key), quint32(0));
        QCOMPARE(Backend.Write(*Key, {"restart", REG_SZ, RegistryValueCodec::EncodeText(REG_SZ, "before").value}), quint32(0));
        State["key"] = KeyPath;
        QSaveFile File(Marker); QVERIFY(File.open(QIODevice::WriteOnly)); QVERIFY(File.write(QJsonDocument(State).toJson()) > 0); QVERIFY(File.commit());
    }
    auto* Bridge = TyStCommandMgr::GetInstance()->Bridge(); QVERIFY(Bridge);
    auto* Left = Main->findChild<CmpPanel*>("cmpLeftPanel"); auto* Right = Main->findChild<CmpPanel*>("cmpRightPanel"); QVERIFY(Left); QVERIFY(Right);
    Main->show(); Main->activateWindow(); QTRY_VERIFY(Main->isActiveWindow()); emit Left->sig_NotifyPanelActivated();
    QVERIFY(Bridge->ExecuteCommand("hc.navigate.systemRegistry")); QTRY_VERIFY(Left->RegistryView()); auto* View = Left->RegistryView();
    QVERIFY(View->Navigate(KeyPath, QString("restart")));
    QTRY_COMPARE(View->CurrentSource().data(RegistryTreeModel::ValueNameRole).toString(), QString("restart"));
    if (Phase == 1) {
        QVERIFY(Bridge->ExecuteCommand("hc.registry.edit"));
        QTRY_VERIFY(qobject_cast<CRegistryDetailView*>(Right->GetFocusView(Right->CurrentTabIndex())));
        auto* Detail = qobject_cast<CRegistryDetailView*>(Right->GetFocusView(Right->CurrentTabIndex()));
        auto* Text = Detail->Editor()->findChild<fm::ui::TextField*>("hc.registry.text"); QVERIFY(Text);
        Text->setText("after"); Detail->Editor()->findChild<fm::ui::Button*>("hc.registry.save")->click();
        QTRY_VERIFY(!Detail->Editor()->IsModified());
        QTRY_COMPARE(RegistryValueCodec::DecodeText(Backend.Read(*Key, "restart").value).text, QString("after"));
        QTRY_VERIFY(!View->History()->Loading() && View->History()->UndoCandidate());
        State["record"] = View->History()->UndoCandidate()->plan.id;
        QSaveFile File(Marker); QVERIFY(File.open(QIODevice::WriteOnly)); QVERIFY(File.write(QJsonDocument(State).toJson()) > 0); QVERIFY(File.commit());
    } else {
        QVERIFY(!State["record"].toString().isEmpty());
        QVERIFY(Bridge->ExecuteCommand("hc.registry.history"));
        QTRY_VERIFY(!View->History()->Loading());
        const auto HasOriginal = [&] {
            for (int Row = 0; Row < View->History()->rowCount(); ++Row)
                if (View->History()->index(Row, 0).data(RegistryHistoryModel::RecordIdRole).toString() == State["record"].toString()) return true;
            return false;
        };
        QTRY_VERIFY(HasOriginal()); // Same durable record, not a newly generated equivalent.
        const auto Expected = Phase == 3 ? QString("before") : QString("after");
        QCOMPARE(RegistryValueCodec::DecodeText(Backend.Read(*Key, "restart").value).text, Expected);
        if (Phase == 2 || Phase == 3) {
            const auto Command = Phase == 2 ? "hc.registry.undo" : "hc.registry.redo";
            QTRY_VERIFY(Bridge->Action(Command)->isEnabled()); QVERIFY(Bridge->ExecuteCommand(Command));
            QTRY_COMPARE(RegistryValueCodec::DecodeText(Backend.Read(*Key, "restart").value).text, Phase == 2 ? QString("before") : QString("after"));
            QTRY_VERIFY(!View->History()->Loading());
            QTRY_VERIFY(Bridge->Action(Phase == 2 ? "hc.registry.redo" : "hc.registry.undo")->isEnabled());
        } else {
            QCOMPARE(Backend.DeleteValue(*Key, "restart"), quint32(0)); QCOMPARE(Backend.DeleteKey(*Key), quint32(0));
            QCOMPARE(Backend.Enumerate(*Key).error, quint32(ERROR_FILE_NOT_FOUND));
        }
    }
}
