#pragma once

/// UIs
#include "HyperCommander/UIs/Components/cmpRegistryDetailView.hpp"
#include "HyperCommander/UIs/registryEditors.hpp"
/// 고유 라이브러리
#include "UniqueLibs/settings/appSettings.hpp"
/// 외부 라이브러리
#include <fmwidgets/ValueInputs.h>
#include <fmwidgets/Button.h>
/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QMessageBox>
#include <QTimer>

inline void RegistryEditorSmoke(QMainUI* Main)
{
    using namespace hc::registry;
    auto* Bridge = TyStCommandMgr::GetInstance()->Bridge(); QVERIFY(Bridge);
    auto* Left = Main->findChild<CmpPanel*>("cmpLeftPanel"); auto* Right = Main->findChild<CmpPanel*>("cmpRightPanel"); QVERIFY(Left); QVERIFY(Right);
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
    const RegistryValue Original{"text", REG_SZ, RegistryValueCodec::EncodeText(REG_SZ, "before").value};
    QCOMPARE(Backend.Write(Key, Original), quint32(0)); QCOMPARE(Backend.Write(Key, {"other", REG_BINARY, "other"}), quint32(0));
    Main->show(); Main->activateWindow(); emit Left->sig_NotifyPanelActivated();
    QVERIFY(Bridge->ExecuteCommand("hc.navigate.systemRegistry")); QTRY_VERIFY(Left->RegistryView()); auto* View = Left->RegistryView();
    QVERIFY(View->Navigate(Key.Path(), QString("text")));
    QTRY_COMPARE(View->CurrentSource().data(RegistryTreeModel::ValueNameRole).toString(), QString("text"));
    qInfo("registry editor: opening detail");
    QVERIFY(Bridge->ExecuteCommand("hc.registry.edit"));
    QTRY_VERIFY(qobject_cast<CRegistryDetailView*>(Right->GetFocusView(Right->CurrentTabIndex())));
    Detail = qobject_cast<CRegistryDetailView*>(Right->GetFocusView(Right->CurrentTabIndex()));
    auto* Editor = Detail->Editor(); auto* Text = Editor->findChild<fm::ui::TextField*>("hc.registry.text"); QVERIFY(Text);
    qInfo("registry editor: detail open");
    Text->setFocus(); QTRY_VERIFY(Text->hasFocus());
    for (const auto* Id : {"hc.file.rename", "hc.file.delete", "hc.clipboard.copy", "hc.file.attributes"})
        if (auto* Action = Bridge->Action(Id)) QTRY_VERIFY(!Action->isEnabled());
    qInfo("registry editor: saving first draft");
    QCOMPARE(Text->text(), QString("before")); Text->setText("saved"); QVERIFY(Editor->IsModified());
    Editor->findChild<fm::ui::Button*>("hc.registry.save")->click();
    QTRY_COMPARE(RegistryValueCodec::DecodeText(Backend.Read(Key, "text").value).text, QString("saved"));
    QTRY_VERIFY(!Editor->IsModified());
    const auto Backup = QDir(QFileInfo(hc::settings::AppSettings().FileName()).absolutePath()).filePath("registry-backup");
    QVERIFY(QDir(Backup).exists());
    qInfo("registry editor: first save complete");
    Text->setText("draft");
    QVERIFY(View->Navigate(Key.Path(), QString("other")));
    QTRY_COMPARE(View->CurrentSource().data(RegistryTreeModel::ValueNameRole).toString(), QString("other"));
    QCOMPARE(Editor->Draft().value.name, QString("text")); QCOMPARE(Text->text(), QString("draft"));
    qInfo("registry editor: refusing dirty close");
    QTimer Answer;
    bool Answered = false;
    QObject::connect(&Answer, &QTimer::timeout, [&] {
        if (auto* Dialog = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
            Dialog && Dialog->parentWidget() == Detail && Dialog->button(QMessageBox::No)) {
            Answered = true; Answer.stop(); Dialog->button(QMessageBox::No)->click();
        }
    });
    Answer.start(25);
    QVERIFY(!Detail->RequestClose()); Answer.stop(); QVERIFY(Answered); QVERIFY(Editor->IsModified());
    qInfo("registry editor: testing conflict");
    const RegistryValue External{"text", REG_SZ, RegistryValueCodec::EncodeText(REG_SZ, "external").value}; QCOMPARE(Backend.Write(Key, External), quint32(0));
    Editor->findChild<fm::ui::Button*>("hc.registry.save")->click();
    auto* Save = Editor->findChild<fm::ui::Button*>("hc.registry.save"); QTRY_VERIFY(Save->isEnabled());
    QVERIFY(Editor->IsModified()); QVERIFY(Backend.Read(Key, "text").value == External);
    Editor->findChild<fm::ui::Button*>("hc.registry.cancel")->click();
    QTRY_COMPARE(Editor->Draft().value.name, QString("other")); QVERIFY(!Editor->IsModified());
    qInfo("registry editor: closing clean detail");
    emit Left->sig_NotifyPanelActivated(); QVERIFY(Bridge->ExecuteCommand("hc.viewer.quick"));
    QTRY_VERIFY(!qobject_cast<CRegistryDetailView*>(Right->GetFocusView(Right->CurrentTabIndex())));
    Detail.clear();
    qInfo("registry editor: details belong to their visible tabs");
    QVERIFY(View->Navigate(Key.Path(), QString("text")));
    QTRY_COMPARE(View->CurrentSource().data(RegistryTreeModel::ValueNameRole).toString(), QString("text"));
    emit Left->sig_NotifyPanelActivated(); QVERIFY(Bridge->ExecuteCommand("hc.registry.edit"));
    Detail = qobject_cast<CRegistryDetailView*>(Right->GetFocusView(Right->CurrentTabIndex())); QVERIFY(Detail);
    const int OriginalTab = Right->CurrentTabIndex();
    auto* OriginalText = Detail->Editor()->findChild<fm::ui::TextField*>("hc.registry.text"); QVERIFY(OriginalText);
    OriginalText->setText("hidden draft");
    Right->AddTab(false);
    const int AddedTab = Right->CurrentTabIndex(); QVERIFY(AddedTab != OriginalTab);
    emit Left->sig_NotifyPanelActivated(); QVERIFY(Bridge->ExecuteCommand("hc.viewer.quick"));
    QTRY_VERIFY(qobject_cast<CRegistryDetailView*>(Right->GetFocusView(AddedTab)));
    QVERIFY(Right->GetFocusView(AddedTab) != Detail);
    QVERIFY(Detail->Editor()->IsModified()); QCOMPARE(OriginalText->text(), QString("hidden draft"));
    emit Left->sig_NotifyPanelActivated(); QVERIFY(Bridge->ExecuteCommand("hc.viewer.quick"));
    QTRY_VERIFY(!qobject_cast<CRegistryDetailView*>(Right->GetFocusView(AddedTab)));
    QCOMPARE(Right->GetFocusView(OriginalTab), Detail.data());
    QVERIFY(Detail->Editor()->IsModified());
    Detail->Editor()->Discard(); QVERIFY(Right->CloseQuickView(OriginalTab)); Detail.clear();
    Right->CloseTab();
}
