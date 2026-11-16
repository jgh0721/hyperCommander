#pragma once
/// UIs
#include "HyperCommander/UIs/Components/cmpEnvironmentView.hpp"
#include "HyperCommander/UIs/Components/cmpEnvironmentDetailView.hpp"
#include "HyperCommander/UIs/environmentEditors.hpp"
/// 고유 라이브러리
#include "UniqueLibs/system/environmentApplication.hpp"
#include "UniqueLibs/system/environmentCommands.hpp"
#include "UniqueLibs/system/environmentProfiles.hpp"
#include "UniqueLibs/settings/appSettings.hpp"
/// 기타 헤더파일
#include "tests/registry/fakeRegistryBackend.hpp"
/// 외부 라이브러리
#include <fmwidgets/ContentDialog.h>
#include <fmwidgets/EnvironmentInputs.h>
#include <fmwidgets/ValueInputs.h>
#include <fmwidgets/Button.h>
/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QTimer>
#include <QScopeGuard>
#include <QTest>
static bool SaveR9Dialog(QWidget& Dialog, const QString& Name);

inline void EnvironmentCommandsSmoke(QMainUI* Main)
{
    using namespace hc::environment;
    auto* Application = EnvironmentApplication::Shared(); QVERIFY(!Application->Native());
    auto Backend = std::dynamic_pointer_cast<FakeRegistryBackend>(Application->Backend()); QVERIFY(Backend);
    QTRY_VERIFY(Application->Session()->Ready()); QTRY_VERIFY(!Application->Session()->Pending());
    const auto User = EnvironmentLocation(Scope::User, Application->CallerSid());
    const Value Original{L"SYNTHETIC_EDITOR", L"before", ValueType::String, Scope::User};
    QCOMPARE(Backend->Write(User, EncodeEnvironmentValue(Original)), quint32(0));
    auto* Bridge = TyStCommandMgr::GetInstance()->Bridge(); QVERIFY(Bridge);
    auto* Left = Main->findChild<CmpPanel*>("cmpLeftPanel"); auto* Right = Main->findChild<CmpPanel*>("cmpRightPanel"); QVERIFY(Left); QVERIFY(Right);
    const auto Saved = Main->CaptureWorkspace(); QPointer<CEnvironmentDetailView> Detail;
    QTimer Answer; int Previews = 0, Finds = 0, OldProcesses = 0; bool MakeNew = false;
    QObject::connect(&Answer, &QTimer::timeout, [&] {
        auto* Dialog = qobject_cast<fm::ui::ContentDialog*>(QApplication::activeModalWidget()); if (!Dialog || Dialog->property("hc.test.answered").toBool()) return;
        Dialog->setProperty("hc.test.answered", true);
        if (Dialog->objectName() == "hc.system.changePreview") { ++Previews; Dialog->button(fm::ui::ContentDialog::PrimaryButton)->click(); }
        else if (Dialog->objectName() == "hc.environment.commandFinder") { ++Finds; Dialog->button(fm::ui::ContentDialog::CloseButton)->click(); }
        else if (Dialog->objectName() == "hc.environment.oldProcesses") { ++OldProcesses; Dialog->button(fm::ui::ContentDialog::CloseButton)->click(); }
        else if (MakeNew && Dialog->objectName() == "hc.environment.newDialog") {
            MakeNew = false;
            Dialog->findChild<fm::ui::TextField*>("hc.environment.newName")->setText("SYNTHETIC_EMPTY");
            Dialog->findChild<fm::ui::ChoiceBox*>("hc.environment.newType")->setCurrentIndex(0);
            Dialog->findChild<fm::ui::MaskedValueEdit*>("hc.environment.newValue")->setText({});
            Dialog->button(fm::ui::ContentDialog::PrimaryButton)->click();
        }
    });
    Answer.start(20);
    const auto Cleanup = qScopeGuard([&] {
        Answer.stop();
        if (Detail) { Detail->Editor()->Discard(); Right->CloseQuickView(Right->CurrentTabIndex()); }
        Left->HideSystemView(); Main->RestoreWorkspace(Saved);
        Backend->DeleteValue(User, "SYNTHETIC_EDITOR"); Backend->DeleteValue(User, "SYNTHETIC_EMPTY"); Backend->DeleteValue(User, "SYNTHETIC_PROFILE");
        Application->Refresh();
    });
    Main->show(); Main->activateWindow(); emit Left->sig_NotifyPanelActivated();
    QVERIFY(Bridge->ExecuteCommand("hc.navigate.systemEnvironment")); QTRY_VERIFY(Left->EnvironmentView());
    auto* View = Left->EnvironmentView(); View->Refresh(); QTRY_VERIFY(!View->Session()->Pending());
    View->Select("SYNTHETIC_EDITOR"); QTRY_VERIFY(View->Current()); QTRY_COMPARE(View->Current()->name_, QString("SYNTHETIC_EDITOR"));
    QVERIFY(Bridge->ExecuteCommand("hc.systemEnvironment.edit"));
    QTRY_VERIFY(qobject_cast<CEnvironmentDetailView*>(Right->GetFocusView(Right->CurrentTabIndex())));
    Detail = qobject_cast<CEnvironmentDetailView*>(Right->GetFocusView(Right->CurrentTabIndex()));
    auto* Editor = Detail->Editor(); auto* Text = Editor->findChild<fm::ui::TextField*>("hc.environment.text"); QVERIFY(Text);
    Text->setFocus(); QTRY_VERIFY(Text->hasFocus());
    for (const auto* Id : {"hc.file.rename", "hc.file.delete", "hc.clipboard.copy", "hc.file.attributes"})
        if (auto* Action = Bridge->Action(Id)) QTRY_VERIFY(!Action->isEnabled());
    Text->setText("saved"); Editor->findChild<fm::ui::Button*>("hc.environment.save")->click();
    QTRY_COMPARE(hc::registry::RegistryValueCodec::DecodeText(Backend->Read(User, "SYNTHETIC_EDITOR").value).text, QString("saved"));
    QTRY_VERIFY(!Editor->IsModified()); QTRY_VERIFY(Application->OwnEnvironment().contains(L"SYNTHETIC_EDITOR"));
    QTRY_COMPARE(Application->OwnEnvironment().at(L"SYNTHETIC_EDITOR"), std::wstring(L"saved"));
    emit Left->sig_NotifyPanelActivated(); QVERIFY(Bridge->ExecuteCommand("hc.systemEnvironment.undo"));
    QTRY_COMPARE(hc::registry::RegistryValueCodec::DecodeText(Backend->Read(User, "SYNTHETIC_EDITOR").value).text, QString("before"));
    QTRY_VERIFY(!Bridge->Environment()->Busy()); QVERIFY(Bridge->ExecuteCommand("hc.systemEnvironment.redo"));
    QTRY_COMPARE(hc::registry::RegistryValueCodec::DecodeText(Backend->Read(User, "SYNTHETIC_EDITOR").value).text, QString("saved"));
    QTRY_VERIFY(!Bridge->Environment()->Busy()); QTRY_VERIFY(!Editor->IsModified()); Text->setText("draft");
    auto External = Original; External.raw_ = L"outside"; QCOMPARE(Backend->Write(User, EncodeEnvironmentValue(External)), quint32(0));
    Editor->findChild<fm::ui::Button*>("hc.environment.save")->click(); QTRY_VERIFY(!Bridge->Environment()->Busy());
    QVERIFY(Editor->IsModified()); QCOMPARE(DecodeEnvironmentValue(Backend->Read(User, "SYNTHETIC_EDITOR").value, Scope::User).value, External);
    Editor->Discard(); emit Left->sig_NotifyPanelActivated(); QVERIFY(Bridge->ExecuteCommand("hc.viewer.quick")); Detail.clear();
    MakeNew = true; QVERIFY(Bridge->ExecuteCommand("hc.systemEnvironment.new"));
    QTRY_VERIFY(Backend->Read(User, "SYNTHETIC_EMPTY"));
    QCOMPARE(Backend->Read(User, "SYNTHETIC_EMPTY").value.type, quint32(REG_SZ));
    QVERIFY(hc::registry::RegistryValueCodec::DecodeText(Backend->Read(User, "SYNTHETIC_EMPTY").value).text.isEmpty());
    QTRY_VERIFY(!Bridge->Environment()->Busy()); View->Select("SYNTHETIC_EMPTY"); QTRY_COMPARE(View->Current()->name_, QString("SYNTHETIC_EMPTY"));
    QVERIFY(Bridge->ExecuteCommand("hc.systemEnvironment.delete")); QTRY_VERIFY(!Backend->Read(User, "SYNTHETIC_EMPTY"));
    QTRY_VERIFY(!Bridge->Environment()->Busy()); auto* Grid = View->findChild<Qtitan::TreeGrid*>("hc.environment.variables"); QVERIFY(Grid);
    Grid->viewport()->setFocus(); QTest::keyClick(Grid->viewport(), Qt::Key_F, Qt::ControlModifier); QTRY_COMPARE(Finds, 1);
    QVERIFY(View->ExecuteCommand("hc.environment.oldProcesses")); QTRY_COMPARE(OldProcesses, 1);
    const auto Settings = QFileInfo(hc::settings::AppSettings().FileName()).absolutePath();
    hc::registry::RegistryChangeStore History(QDir(Settings).filePath("environment-backup"));
    EnvironmentProfiles Profiles(QDir(Settings).filePath("environment-profiles.json")); QCOMPARE(Profiles.Load(History), quint32(0));
    EnvironmentProfile Profile; Profile.name_ = "Synthetic profile"; Profile.id_ = "fdd29751-a9bf-4775-a89b-f7654027f812";
    Profile.values_ = {{L"SYNTHETIC_PROFILE", L"enabled", ValueType::String, Scope::User}};
    QCOMPARE(Profiles.Put(Profile), quint32(0));
    View->SetScope(EnvironmentViewScope::Profile); QTRY_VERIFY(View->CurrentProfile().has_value());
    emit Left->sig_NotifyPanelActivated(); QVERIFY(Bridge->ExecuteCommand("hc.systemEnvironment.profileToggle"));
    QTRY_VERIFY(Backend->Read(User, "SYNTHETIC_PROFILE")); QTRY_VERIFY(View->CurrentProfile()->active_);
    QVERIFY(SaveR9Dialog(*View, "environment-profile"));
    QVERIFY(Bridge->ExecuteCommand("hc.systemEnvironment.profileToggle"));
    QTRY_VERIFY(!Backend->Read(User, "SYNTHETIC_PROFILE")); QTRY_VERIFY(!View->CurrentProfile()->active_);
    QVERIFY(Previews >= 7);
}
