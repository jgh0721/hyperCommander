#include "stdafx.h"
#include "tstRegistryEditors.hpp"

/// 모듈
/// UIs
#include "HyperCommander/UIs/registryEditors.hpp"
#include "HyperCommander/UIs/registryPolicyEditor.hpp"
/// 기타 헤더파일
/// 고유 라이브러리
#include "UniqueLibs/ui/pathListEditor.hpp"
/// 공통 라이브러리
/// 외부 라이브러리
#include <fmwidgets/ValueInputs.h>
#include <fmwidgets/HexBufferEditor.h>
#include <fmwidgets/PathEntryDelegate.h>
#include <fmwidgets/Button.h>
#include <fmwidgets/SegmentedControl.h>
#include <fmwidgets/ReorderList.h>
/// 선언 헤더파일들
/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QTest>
#include <QSignalSpy>
#include <QAbstractItemModelTester>
#include <QThread>

DECLARE_CMNLIBSV2_NAMESPACE
Q_LOGGING_CATEGORY(lcRegistryEditorTests, "hc.tests.registryEditors")
using namespace hc::registry;
void RegistryEditorTests::policyOptions()
{
    RegistryPolicyEntry Entry; Entry.policy.key = "Software\\HyperCommanderTests\\Editor"; Entry.policy.scope = "User";
    const auto Node = AdmxCatalog::Parse(R"XML(<policy key="Software\HyperCommanderTests\Editor" valueName="On"><elements><decimal id="number" valueName="Number" required="true" minValue="2" maxValue="8"/><text id="text" valueName="Text" required="true"/><list id="list" key="Software\HyperCommanderTests\Editor\List" explicitValue="true"/></elements></policy>)XML");
    QVERIFY(Node); Entry.policy.definition = *Node; Entry.observation.state = PolicyState::Partial;
    Entry.observation.options = {{"number", "4"}, {"text", "before"}, {"list", QVariantList{QVariantMap{{"name", "a"}, {"data", "first"}}, QVariantMap{{"name", "b"}, {"data", "second"}}}}};
    RegistryPolicyEditor Editor; QVERIFY(Editor.SetPolicy(Entry)); QVERIFY(!Editor.IsModified());
    auto* State = Editor.findChild<fm::ui::SegmentedControl*>("hc.registry.policy.state"); QVERIFY(State);
    QCOMPARE(State->currentIndex(), -1); for (auto* Button : State->findChildren<QPushButton*>()) QVERIFY(!Button->isChecked());
    auto* Save = Editor.findChild<fm::ui::Button*>("hc.registry.policy.save"); QVERIFY(Save); QVERIFY(!Save->isEnabled());
    State->setCurrentIndex(1); QCOMPARE(Editor.State(), PolicyState::Enabled); QVERIFY(Editor.IsModified()); QVERIFY(Save->isEnabled());
    auto* Number = Editor.findChild<fm::ui::UnsignedNumberEdit*>("hc.policy.number"); QVERIFY(Number); Number->setDraft("9"); QVERIFY(!Save->isEnabled());
    Number->setDraft("8"); QVERIFY(Save->isEnabled());
    auto* Text = Editor.findChild<fm::ui::TextField*>("hc.policy.text"); QVERIFY(Text); Text->setText(""); QVERIFY(!Save->isEnabled()); Text->setText("after"); QVERIFY(Save->isEnabled());
    auto* List = Editor.findChild<fm::ui::ReorderListWidget*>("hc.policy.list"); QVERIFY(List); QVERIFY(List->MoveRow(0, 1));
    QCOMPARE(Editor.Options()["list"].toList()[0].toMap()["name"].toString(), QString("b"));
    QSignalSpy Saved(&Editor, &RegistryPolicyEditor::SaveRequested); Save->click(); QCOMPARE(Saved.count(), 1);
    Editor.SetBusy(true); QVERIFY(!Save->isEnabled()); Editor.Discard(); QVERIFY(Editor.IsModified()); QVERIFY(!Editor.SetPolicy(Entry));
    QSignalSpy Cancelled(&Editor, &RegistryPolicyEditor::CancelRequested);
    const auto Buttons = Editor.findChildren<fm::ui::Button*>();
    const auto Cancel = std::find_if(Buttons.cbegin(), Buttons.cend(), [](auto* Button) { return Button->text() == RegistryPolicyEditor::tr("취소"); });
    QVERIFY(Cancel != Buttons.cend()); QVERIFY((*Cancel)->isEnabled()); (*Cancel)->click(); QCOMPARE(Cancelled.count(), 1);
    QVERIFY(Editor.IsModified()); QCOMPARE(Editor.Options()["text"].toString(), QString("after"));
    Editor.SetBusy(false); Editor.Discard(); QVERIFY(!Editor.IsModified()); QCOMPARE(Editor.State(), PolicyState::Partial);
    Entry.observation.options["text"] = QString(1100, 'x');
    Entry.policy.presentation = *AdmxCatalog::Parse(R"XML(<presentation id="p"><text>Before field</text><comboBox refId="text"><label>Text choice</label><default>default choice</default><suggestion>second choice</suggestion><suggestion>first choice</suggestion></comboBox></presentation>)XML");
    QVERIFY(Editor.SetPolicy(Entry));
    QCOMPARE(Editor.Options()["text"].toString().size(), 1100);
    auto* Choices = Editor.findChild<fm::ui::ChoiceBox*>("hc.policy.choices.text"); QVERIFY(Choices);
    QCOMPARE(Choices->itemText(0), QString("first choice"));
    QCOMPARE(RegistryPolicyDefinition::Defaults(Entry.policy)["text"].toString(), QString("default choice"));
    Editor.findChild<fm::ui::SegmentedControl*>("hc.registry.policy.state")->setCurrentIndex(1);
    QVERIFY(!Save->isEnabled());
    Choices->setCurrentIndex(0); QVERIFY(Save->isEnabled()); QCOMPARE(Editor.Options()["text"].toString(), QString("first choice"));
    Editor.Discard();
    Entry.localError = ERROR_ACCESS_DENIED; QVERIFY(Editor.SetPolicy(Entry)); QVERIFY(!Editor.findChild<fm::ui::SegmentedControl*>("hc.registry.policy.state")->isEnabled());
}
void RegistryEditorTests::textDraftAndSave()
{
    RegistryValueEditor Editor;
    const RegistryValue Original{"Text", REG_SZ, RegistryValueCodec::EncodeText(REG_SZ, "before").value};
    QVERIFY(Editor.SetValue(Original)); QVERIFY(!Editor.IsModified());
    auto* Field = Editor.findChild<fm::ui::TextField*>("hc.registry.text"); QVERIFY(Field);
    Field->setText("after"); QVERIFY(Editor.IsModified()); QVERIFY(!Editor.SetValue(Original));
    QCOMPARE(RegistryValueCodec::DecodeText(Editor.Draft().value).text, QString("after"));
    bool Saved = false;
    connect(&Editor, &RegistryValueEditor::SaveRequested, &Editor, [&](const RegistryValue& Before, const RegistryValue& After) {
        QVERIFY(Before == Original); QCOMPARE(RegistryValueCodec::DecodeText(After).text, QString("after")); Saved = true;
    });
    Editor.findChild<fm::ui::Button*>("hc.registry.save")->click(); QVERIFY(Saved);
    Editor.SetBusy(true); Editor.Discard(); QVERIFY(Editor.IsModified());
    Editor.SetBusy(false); Editor.Discard(); QVERIFY(!Editor.IsModified()); QVERIFY(Editor.Draft().value == Original);
    Field->setText("saved"); const auto Draft = Editor.Draft().value;
    Editor.AcceptSaved(Draft); QVERIFY(!Editor.IsModified()); QVERIFY(Editor.Draft().value == Draft);
    QVERIFY(Editor.SetValue({"Multi", REG_MULTI_SZ, RegistryValueCodec::EncodeText(REG_MULTI_SZ, "one\ntwo").value}));
    auto* Multi = Editor.findChild<fm::ui::MultilineField*>(); QVERIFY(Multi); Multi->setPlainText("one\n\nthree");
    QCOMPARE(RegistryValueCodec::DecodeText(Editor.Draft().value).text, QString("one\nthree"));
}
void RegistryEditorTests::malformedPreservation()
{
    RegistryValueEditor Editor;
    for (const auto& Value : QList<RegistryValue>{{"Bad", REG_SZ, QByteArray::fromHex("00d80000")},
            {"MissingNul", REG_EXPAND_SZ, QByteArray::fromHex("4100")},
            {"Short", REG_DWORD, "123"}, {"Unknown", 0xf00, QByteArray::fromHex("ff0000")}}) {
        QVERIFY(Editor.SetValue(Value)); QVERIFY(!Editor.IsModified()); QVERIFY(Editor.Draft().value == Value);
        auto* Hex = Editor.findChild<fm::ui::HexBufferEditor*>(); QVERIFY(Hex); QCOMPARE(Hex->data(), Value.bytes);
        Hex->setSelection(0, 1); QVERIFY(Hex->replaceSelection(QByteArray::fromHex("42"))); QVERIFY(Editor.IsModified());
        Editor.Discard(); QVERIFY(Editor.Draft().value == Value);
    }
}
void RegistryEditorTests::numbersAndBinary()
{
    RegistryValueEditor Editor;
    const RegistryValue Value{"Number", REG_QWORD, RegistryValueCodec::EncodeNumber(REG_QWORD, Q_UINT64_C(0xffffffffffffffff)).value};
    QVERIFY(Editor.SetValue(Value)); auto* Number = Editor.findChild<fm::ui::UnsignedNumberEdit*>(); QVERIFY(Number);
    QVERIFY(Number->setRadix(10)); QVERIFY(!Editor.IsModified());
    Number->setDraft("18446744073709551616"); QVERIFY(Editor.IsModified()); QVERIFY(!Editor.Draft());
    QVERIFY(!Editor.findChild<fm::ui::Button*>("hc.registry.save")->isEnabled()); Editor.Discard();
    QVERIFY(Editor.SetValue({"Binary", REG_BINARY, QByteArray::fromHex("010203")}));
    auto* Hex = Editor.findChild<fm::ui::HexBufferEditor*>(); Hex->setSelection(1, 1);
    QVERIFY(Hex->replaceSelection(QByteArray::fromHex("aa"))); QCOMPARE(Editor.Draft().value.bytes, QByteArray::fromHex("01aa03"));
    Hex->undo(); QVERIFY(!Editor.IsModified()); Hex->redo(); QVERIFY(Editor.IsModified());
}
void RegistryEditorTests::pathsAndCancellation()
{
    hc::ui::PathEntryModel Model; QAbstractItemModelTester Tester(&Model, QAbstractItemModelTester::FailureReportingMode::QtTest);
    QVERIFY(Model.SetValue("A;;%ROOT%;A;")); QCOMPARE(Model.rowCount(), 5); QCOMPARE(Model.Value(), QString("A;;%ROOT%;A;"));
    QVERIFY(Model.moveRows({}, 2, 1, {}, 0)); QCOMPARE(Model.Value(), QString("%ROOT%;A;;A;"));
    QVERIFY(Model.Remove(2)); QVERIFY(Model.Insert(1, "B")); QCOMPARE(Model.Value(), QString("%ROOT%;B;A;A;"));
    QVERIFY(!Model.SetValue(QString(QChar(0)))); QCOMPARE(Model.rowCount(), 5);
    Model.SetResolver([](const QString& Raw) {
        QThread::msleep(5); return hc::ui::PathResolution{Raw + "-old", hc::ui::PathResolution::Status::Missing, {}};
    });
    QTest::qWait(170);
    Model.SetResolver([](const QString& Raw) { return hc::ui::PathResolution{Raw + "-new", hc::ui::PathResolution::Status::Exists, {}}; });
    QVERIFY(Model.SetValue("latest"));
    QTRY_COMPARE(Model.index(0).data(fm::ui::PathEntryDelegate::ExpandedPathRole).toString(), QString("latest-new"));
    Model.SetResolver([](const QString&) -> hc::ui::PathResolution { throw 1; });
    QTRY_COMPARE(Model.index(0).data(fm::ui::PathEntryDelegate::PathStatusRole).toInt(), int(hc::ui::PathResolution::Status::Unresolved));
    hc::ui::PathListEditor Editor; QVERIFY(Editor.SetValue("A;;B;")); QVERIFY(!Editor.IsModified());
    QVERIFY(Editor.Model()->moveRows({}, 2, 1, {}, 0)); QVERIFY(Editor.IsModified()); QCOMPARE(Editor.Value(), QString("B;A;;"));
    auto* Toggle = Editor.findChild<fm::ui::Button*>("hc.pathList.textMode"); QVERIFY(Toggle); Toggle->click();
    auto* Raw = Editor.findChild<fm::ui::TextField*>("hc.pathList.raw"); QVERIFY(Raw); QCOMPARE(Raw->text(), Editor.Value());
    Raw->setText("X;;Y;"); QCOMPARE(Editor.Value(), QString("X;;Y;")); Toggle->click(); QCOMPARE(Editor.Model()->Value(), QString("X;;Y;"));
    Editor.SetResolver([](const QString& Entry) { return hc::ui::PathResolution{Entry, Entry == "X" ? hc::ui::PathResolution::Status::Missing : hc::ui::PathResolution::Status::Unresolved, {}}; });
    QTRY_COMPARE(Editor.Model()->index(0).data(fm::ui::PathEntryDelegate::PathStatusRole).toInt(), int(hc::ui::PathResolution::Status::Missing));
    Editor.findChild<fm::ui::Button*>("hc.pathList.clean")->click(); QCOMPARE(Editor.Value(), QString(";Y;"));
}
QTEST_MAIN(RegistryEditorTests)
