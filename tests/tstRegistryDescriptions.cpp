#include "stdafx.h"
#include "tstRegistryDescriptions.hpp"
#include "registry/fakeRegistryBackend.hpp"
#include "UniqueLibs/registry/registryPolicyState.hpp"
#include "UniqueLibs/registry/registryPolicyEdit.hpp"

/// 모듈
/// UIs
/// 기타 헤더파일
/// 고유 라이브러리
#include "UniqueLibs/registry/registryAdmx.hpp"
#include "UniqueLibs/registry/registryCatalog.hpp"
#include "UniqueLibs/registry/registryDescriptions.hpp"
#include "UniqueLibs/registry/registryPolicyDefinition.hpp"
#include "commonLibs/system/registryValue.hpp"
#include <limits>
/// 공통 라이브러리
/// 외부 라이브러리
/// 선언 헤더파일들
/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QTest>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QFile>
#include <QDir>

DECLARE_CMNLIBSV2_NAMESPACE
Q_LOGGING_CATEGORY(lcRegistryDescriptionTests, "hc.tests.registryDescriptions")
using namespace hc::registry;
namespace {
bool Write(const QString& Path, const QByteArray& Bytes)
{
    QDir().mkpath(QFileInfo(Path).absolutePath()); QFile File(Path);
    return File.open(QIODevice::WriteOnly) && File.write(Bytes) == Bytes.size();
}
}
void RegistryDescriptionTests::xmlLimits()
{
    QString Error;
    QVERIFY(!AdmxCatalog::Parse("<!DOCTYPE root [<!ENTITY evil SYSTEM 'file:///no'>]><root>&evil;</root>", &Error));
    QVERIFY(!Error.isEmpty());
    QVERIFY(!AdmxCatalog::Parse(QByteArray("<x>").repeated(100) + QByteArray("</x>").repeated(100)));
    QVERIFY(!AdmxCatalog::Parse("<root><unclosed>"));
    QVERIFY(!AdmxCatalog::Parse("<policy xmlns=\"urn:unknown\" key=\"spoof\"/>"));
    QVERIFY(!AdmxCatalog::Parse("<policy xmlns:x=\"urn:unknown\" x:key=\"spoof\"/>"));
    QVERIFY(AdmxCatalog::Parse("<policy xmlns=\"http://schemas.microsoft.com/GroupPolicy/2006/07/PolicyDefinitions\" key=\"valid\"/>"));
    const auto Good = AdmxCatalog::Parse("<root title='a &amp; b'><item><![CDATA[plain <text>]]></item></root>");
    QVERIFY(Good); QCOMPARE(Good->Attribute("title"), QString("a & b"));
    QCOMPARE(Good->Child("item")->text, QString("plain <text>"));
}
void RegistryDescriptionTests::namespacesTranslationsAndCache()
{
    QTemporaryDir Temp; QVERIFY(Temp.isValid()); const auto Root = Temp.path();
    const QByteArray Base = R"XML(<policyDefinitions><policyNamespaces><target prefix="base" namespace="Fixture.Base"/></policyNamespaces>
<categories><category name="root" displayName="$(string.root)"/></categories>
<supportedOn><definitions><definition name="os" displayName="$(string.os)"/></definitions></supportedOn></policyDefinitions>)XML";
    const QByteArray Vendor = R"XML(<policyDefinitions><policyNamespaces><target prefix="v" namespace="Fixture.Vendor"/>
<using prefix="base" namespace="Fixture.Base"/></policyNamespaces><policies>
<policy name="test" class="Both" key="Software\HyperCommanderTests\Policy" valueName="Enabled" displayName="$(string.title)" explainText="$(string.help)" presentation="$(presentation.form)">
<parentCategory ref="base:root"/><supportedOn ref="base:os"/><enabledValue><decimal value="1"/></enabledValue>
<elements><text id="text" valueName="Text"/></elements></policy>
<policy name="unsupported" class="User" key="Software\HyperCommanderTests\Unknown" displayName="Unknown">
<elements><futureElement id="future"/></elements></policy></policies></policyDefinitions>)XML";
    QVERIFY(Write(Root + "/base.admx", Base)); QVERIFY(Write(Root + "/vendor.admx", Vendor));
    QVERIFY(Write(Root + "/broken.admx", "<broken>"));
    QVERIFY(Write(Root + "/en-US/base.adml", R"XML(<policyDefinitionResources><resources><stringTable><string id="root">Base category</string><string id="os">Test OS</string></stringTable></resources></policyDefinitionResources>)XML"));
    QVERIFY(Write(Root + "/en-US/vendor.adml", R"XML(<policyDefinitionResources><resources><stringTable><string id="title">English title</string><string id="help">English fallback</string></stringTable><presentationTable><presentation id="form"><textBox refId="text"><label>Text label</label></textBox></presentation></presentationTable></resources></policyDefinitionResources>)XML"));
    QVERIFY(Write(Root + "/ko-KR/vendor.adml", QStringLiteral(R"XML(<policyDefinitionResources><resources><stringTable><string id="title">한국어 제목</string></stringTable></resources></policyDefinitionResources>)XML").toUtf8()));
    const auto Catalog = AdmxCatalog::Load(Root, "ko-KR", Root + "/cache");
    QCOMPARE(Catalog.policies.size(), 2); QCOMPARE(Catalog.parsedFiles, 5); QCOMPARE(Catalog.cachedFiles, 0);
    QVERIFY(!Catalog.diagnostics.isEmpty());
    const auto& Policy = Catalog.policies[0]; QCOMPARE(Policy.id, QString("Fixture.Vendor:test"));
    QCOMPARE(Policy.title, QStringLiteral("한국어 제목")); QCOMPARE(Policy.explanation, QString("English fallback"));
    QCOMPARE(Policy.category, QString("Base category")); QCOMPARE(Policy.supportedOn, QString("Test OS"));
    QVERIFY(Policy.presentation.Child("textBox")); QVERIFY(Policy.unsupportedReason.isEmpty());
    QVERIFY(!Catalog.policies[1].unsupportedReason.isEmpty());
    const auto Location = RegistryLocation::Parse("HKCU\\Software\\HyperCommanderTests\\Policy").value();
    QCOMPARE(Catalog.Find(Location, QString("Text")), QList<int>{0}); QVERIFY(Catalog.Find(Location, QString("Missing")).isEmpty());
    const auto Cached = AdmxCatalog::Load(Root, "ko-KR", Root + "/cache");
    QCOMPARE(Cached.cachedFiles, 5); QCOMPARE(Cached.parsedFiles, 0);
    QVERIFY(Write(Root + "/ko-KR/vendor.adml", R"XML(<policyDefinitionResources><resources><stringTable><string id="title">changed</string></stringTable></resources></policyDefinitionResources>)XML"));
    const auto Changed = AdmxCatalog::Load(Root, "ko-KR", Root + "/cache");
    QCOMPARE(Changed.cachedFiles, 4); QCOMPARE(Changed.parsedFiles, 1); QCOMPARE(Changed.policies[0].title, QString("changed"));
    std::atomic_bool Cancel{true}; QVERIFY(AdmxCatalog::Load(Root, "ko-KR", {}, &Cancel).policies.isEmpty());
}
void RegistryDescriptionTests::systemCatalogReadOnly()
{
    QTemporaryDir Cache; QVERIFY(Cache.isValid());
    const auto Root = QDir(qEnvironmentVariable("WINDIR")).filePath("PolicyDefinitions");
    if (!QDir(Root).exists()) QSKIP("Windows policy definitions absent");
    const auto Cold = AdmxCatalog::Load(Root, "ko-KR", Cache.path());
    QVERIFY(!Cold.policies.isEmpty());
    const auto Warm = AdmxCatalog::Load(Root, "ko-KR", Cache.path());
    QCOMPARE(Warm.policies.size(), Cold.policies.size()); QVERIFY(Warm.cachedFiles > 0);
    qInfo() << "ADMX policies" << Cold.policies.size() << "files" << Cold.parsedFiles
        << "cold ms" << Cold.elapsedMs << "warm ms" << Warm.elapsedMs << "diagnostics" << Cold.diagnostics.size();
    for (const auto& Diagnostic : Cold.diagnostics) qInfo().noquote() << Diagnostic;
}
void RegistryDescriptionTests::builtinCatalog()
{
    const auto Catalog = BuiltinRegistryCatalog::Bundled();
    QCOMPARE(Catalog.Count(), 150); QVERIFY2(Catalog.Errors().isEmpty(), qPrintable(Catalog.Errors().join('\n')));
    QVERIFY(BuiltinRegistryCatalog::PathMatches("Software\\*\\Leaf", "software\\Vendor\\leaf"));
    QVERIFY(!BuiltinRegistryCatalog::PathMatches("Software\\*\\Leaf", "Software\\Vendor\\Sub\\Leaf"));
    QVERIFY(BuiltinRegistryCatalog::PathMatches("Software\\**\\Leaf", "Software\\Vendor\\Sub\\Leaf"));
    QVERIFY(BuiltinRegistryCatalog::PathMatches("Software\\**\\Leaf", "Software\\Leaf"));
    const auto Env = RegistryLocation::Parse("HKCU\\Environment").value();
    const auto Korean = Catalog.Find(Env, QString("Path"), REG_EXPAND_SZ, "ko-KR"); QVERIFY(Korean);
    QVERIFY(Korean.title.contains(QStringLiteral("검색 경로"))); QVERIFY(!Korean.sourceUrls.isEmpty());
    const auto English = Catalog.Find(Env, QString("Path"), REG_EXPAND_SZ, "fr-FR"); QVERIFY(English);
    QVERIFY(English.title.contains("search path"));
    QVERIFY(!Catalog.Find(Env, QString("Path"), REG_DWORD, "en-US"));
    QVERIFY(!Catalog.Find(Env, QString("NoSuchSetting"), REG_SZ, "en-US"));
    const auto Com = RegistryLocation::Parse("HKCR\\CLSID\\{123}\\DefaultIcon").value();
    const auto Icon = Catalog.Find(Com, {}, {}, "en-US"); QVERIFY(Icon); QCOMPARE(Icon.category, QString("COM"));
    QVERIFY(!Catalog.KnownLocations().isEmpty());
}
void RegistryDescriptionTests::asynchronousReload()
{
    QTemporaryDir Temp; QVERIFY(Temp.isValid()); const auto Root = Temp.path();
    const QByteArray Definition = R"XML(<policyDefinitions><policyNamespaces><target prefix="t" namespace="Fixture.Async"/></policyNamespaces><policies><policy name="one" class="User" key="Software\HyperCommanderTests\Description" valueName="Enabled" displayName="first"><enabledValue><decimal value="1"/></enabledValue></policy></policies></policyDefinitions>)XML";
    QVERIFY(Write(Root + "/fixture.admx", Definition));
    RegistryDescriptions Service(Root, "en-US", Root + "/cache");
    QSignalSpy Changed(&Service, &RegistryDescriptions::Changed);
    QTRY_VERIFY(!Service.Loading()); QVERIFY(Changed.count() > 0);
    const auto Location = RegistryLocation::Parse("HKCU\\Software\\HyperCommanderTests\\Description").value();
    QCOMPARE(Service.Find(Location, QString("Enabled"), REG_DWORD).title, QString("first"));
    auto Updated = Definition; Updated.replace("displayName=\"first\"", "displayName=\"second\"");
    QVERIFY(Write(Root + "/fixture.admx", Updated));
    QTRY_COMPARE_WITH_TIMEOUT(Service.Find(Location, QString("Enabled"), REG_DWORD).title, QString("second"), 10000);
    const auto Env = RegistryLocation::Parse("HKCU\\Environment").value();
    QVERIFY(Service.Find(Env, QString("Path"), REG_SZ).Known());
    auto* Closing = new RegistryDescriptions(Root, "en-US", Root + "/cache");
    delete Closing;
}
void RegistryDescriptionTests::policyPrograms()
{
    const QByteArray Xml = R"XML(<policy name="compound" class="User" key="Software\HyperCommanderTests\Policy" valueName="Enabled">
<enabledValue><decimal value="7"/></enabledValue><disabledValue><delete/></disabledValue>
<enabledList><item valueName="Extra"><value><string>active</string></value></item></enabledList>
<elements>
<boolean id="flag" valueName="Flag"><trueValue><decimal value="9"/></trueValue><falseValue><decimal value="2"/></falseValue></boolean>
<decimal id="number" valueName="Number" required="true" minValue="2" maxValue="20"/>
<longDecimal id="wide" valueName="Wide" maxValue="18446744073709551615"/>
<text id="text" valueName="Text" required="true" expandable="true" maxLength="20"/>
<enum id="choice" valueName="Choice"><item displayName="A"><value><decimal value="4"/></value></item><item displayName="B"><value><decimal value="8"/></value><valueList><item valueName="Other"><value><string>chosen</string></value></item></valueList></item></enum>
<list id="list" key="Software\HyperCommanderTests\Policy\List" valuePrefix="Item"/>
</elements></policy>)XML";
    const auto Node = AdmxCatalog::Parse(Xml); QVERIFY(Node);
    AdmxPolicy Policy; Policy.definition = *Node; Policy.key = Node->Attribute("key"); Policy.scope = "User";
    auto Options = RegistryPolicyDefinition::Defaults(Policy);
    Options["flag"] = true; Options["number"] = "20"; Options["wide"] = "18446744073709551615";
    Options["text"] = "%TEMP%"; Options["choice"] = 1;
    Options["list"] = QVariantList{QVariantMap{{"data", "one"}}, QVariantMap{{"data", "two"}}};
    const auto Enabled = RegistryPolicyDefinition::Compile(Policy, PolicyState::Enabled, Options);
    QVERIFY2(Enabled, qPrintable(Enabled.error)); QCOMPARE(Enabled.clearKeys.size(), 1);
    const auto Find = [&](const PolicyProgram& Program, const QString& Name) -> std::optional<RegistryValue> {
        for (const auto& Assignment : Program.assignments) if (Assignment.name == Name) return Assignment.value;
        return {};
    };
    QVERIFY(Find(Enabled, "Enabled")); QCOMPARE(RegistryValueCodec::DecodeNumber(*Find(Enabled, "Enabled")).value, quint64(7));
    QCOMPARE(RegistryValueCodec::DecodeNumber(*Find(Enabled, "Flag")).value, quint64(9));
    QCOMPARE(RegistryValueCodec::DecodeNumber(*Find(Enabled, "Wide")).value, std::numeric_limits<quint64>::max());
    QCOMPARE(Find(Enabled, "Text")->type, quint32(REG_EXPAND_SZ));
    QCOMPARE(RegistryValueCodec::DecodeNumber(*Find(Enabled, "Choice")).value, quint64(8));
    QCOMPARE(RegistryValueCodec::DecodeText(*Find(Enabled, "Other")).text, QString("chosen"));
    QCOMPARE(RegistryValueCodec::DecodeText(*Find(Enabled, "Item2")).text, QString("two"));
    Options["number"] = "21"; QVERIFY(!RegistryPolicyDefinition::Compile(Policy, PolicyState::Enabled, Options));
    Options["number"] = "2"; Options["text"] = ""; QVERIFY(!RegistryPolicyDefinition::Compile(Policy, PolicyState::Enabled, Options));
    const auto Unconfigured = RegistryPolicyDefinition::Compile(Policy, PolicyState::NotConfigured, {});
    QVERIFY(Unconfigured); for (const auto& Assignment : Unconfigured.assignments) QVERIFY(!Assignment.value);
    const auto Disabled = RegistryPolicyDefinition::Compile(Policy, PolicyState::Disabled, {});
    QVERIFY(Disabled); QVERIFY(!Find(Disabled, "Enabled")); QVERIFY(!Find(Disabled, "Other"));
    Policy.unsupportedReason = "custom extension"; QVERIFY(!RegistryPolicyDefinition::Compile(Policy, PolicyState::Enabled, Options));
}
void RegistryDescriptionTests::policyObservation()
{
    const auto Node = AdmxCatalog::Parse(R"XML(<policy key="Software\HyperCommanderTests\Observe" valueName="On"><enabledValue><decimal value="1"/></enabledValue><disabledValue><decimal value="0"/></disabledValue><elements><decimal id="number" valueName="Number" required="true" minValue="2" maxValue="10"/></elements></policy>)XML");
    QVERIFY(Node); AdmxPolicy Policy; Policy.definition = *Node; Policy.key = Node->Attribute("key"); Policy.scope = "User";
    const auto Location = RegistryLocation::Parse("HKCU\\" + Policy.key).value();
    FakeRegistryBackend Backend; Backend.Seed(Location);
    auto Inspect = [&] { return RegistryPolicyInspector(Backend).Inspect(Policy, false, {}); };
    QCOMPARE(Inspect().state, PolicyState::NotConfigured);
    const auto WriteNumber = [&](const QString& Name, quint64 Number) {
        QCOMPARE(Backend.Write(Location, {Name, REG_DWORD, RegistryValueCodec::EncodeNumber(REG_DWORD, Number).value}), quint32(0));
    };
    WriteNumber("On", 1); QCOMPARE(Inspect().state, PolicyState::Partial);
    WriteNumber("Number", 7); QCOMPARE(Inspect().state, PolicyState::Enabled);
    QCOMPARE(Inspect().options["number"].toString(), QString("7"));
    WriteNumber("Number", 11); QCOMPARE(Inspect().state, PolicyState::Partial);
    WriteNumber("On", 0); QCOMPARE(Inspect().state, PolicyState::Partial);
    QCOMPARE(Backend.DeleteValue(Location, "Number"), quint32(0)); QCOMPARE(Inspect().state, PolicyState::Disabled);
    WriteNumber("Unrelated", 99); QCOMPARE(Inspect().state, PolicyState::Disabled);
    auto DeleteNode = AdmxCatalog::Parse(R"XML(<policy key="Software\HyperCommanderTests\Observe" valueName="On"><enabledValue><decimal value="1"/></enabledValue><disabledValue><delete/></disabledValue></policy>)XML");
    QVERIFY(DeleteNode); auto DeletePolicy = Policy; DeletePolicy.definition = *DeleteNode;
    QCOMPARE(Backend.DeleteValue(Location, "On"), quint32(0));
    const QList<RegistryPolRecord> Local{{Policy.key, {"**Del.On", REG_SZ, RegistryValueCodec::EncodeText(REG_SZ, " ").value}}};
    QCOMPARE(RegistryPolicyInspector(Backend).Inspect(DeletePolicy, false, Local).state, PolicyState::Disabled);
    QCOMPARE(RegistryPolicyInspector(Backend).Inspect(DeletePolicy, false, {}).state, PolicyState::NotConfigured);
    auto Unsupported = Policy; Unsupported.unsupportedReason = "Unsupported client extension";
    auto Missing = RegistryPolicyInspector(Backend).Inspect(Unsupported, false, {});
    QVERIFY(!Missing.present); QVERIFY(Missing.diagnostic.isEmpty());
    WriteNumber("On", 1);
    const auto Present = RegistryPolicyInspector(Backend).Inspect(Unsupported, false, {});
    QVERIFY(Present.present); QCOMPARE(Present.state, PolicyState::Unknown);
    QCOMPARE(Present.diagnostic, Unsupported.unsupportedReason);
    std::atomic_bool Cancel(true);
    const auto Cancelled = RegistryPolicyInspector(Backend, &Cancel).Inspect(Policy, false, {});
    QCOMPARE(Cancelled.state, PolicyState::Unknown); QVERIFY(!Cancelled.diagnostic.isEmpty());
}
void RegistryDescriptionTests::policyEditPlan()
{
    const auto Node = AdmxCatalog::Parse(R"XML(<policy key="Software\HyperCommanderTests\Edit" valueName="On"><enabledValue><decimal value="1"/></enabledValue><disabledValue><delete/></disabledValue><elements><list id="items" key="Software\HyperCommanderTests\Edit\List"/></elements></policy>)XML");
    QVERIFY(Node); AdmxPolicy Policy; Policy.definition = *Node; Policy.key = Node->Attribute("key"); Policy.scope = "User";
    const auto Location = RegistryLocation::Parse("HKCU\\" + Policy.key).value(); const auto List = Location.Child("List");
    const auto String = [](const QString& Name, const QString& Text) { return RegistryValue{Name, REG_SZ, RegistryValueCodec::EncodeText(REG_SZ, Text).value}; };
    FakeRegistryBackend Backend; Backend.Seed(Location); Backend.Seed(List);
    Backend.Write(Location, String("Unrelated", "preserve")); Backend.Write(List, String("Old", "old"));
    const QList<RegistryPolRecord> Original{{"Software\\Policies\\Unrelated", String("Keep", "untouched")}};
    const auto Before = RegistryPolCodec::Write(Original); QVERIFY(Before);
    const auto Observation = RegistryPolicyInspector(Backend).Inspect(Policy, false, Original);
    QVariantMap Options{{"items", QVariantList{QVariantMap{{"data", "first"}}, QVariantMap{{"data", "second"}}}}};
    const auto Plan = RegistryPolicyEdit::Prepare(Policy, false, PolicyState::Enabled, Options, Observation, Before.value);
    QVERIFY2(Plan, qPrintable(Plan.error)); QCOMPARE(Plan.beforePolicy, Before.value);
    const auto After = RegistryPolCodec::Read(Plan.afterPolicy); QVERIFY(After); QCOMPARE(After.records.size(), 5);
    QCOMPARE(After.records[0], Original[0]); QCOMPARE(After.records[1].Directive(), PolicyDirective::DeleteAllValues);
    QCOMPARE(Plan.expectedValues.size(), 4); // On, two list items, and deletion of the previous list item.
    RegistryTransaction Transaction(Backend); const auto Prepared = Transaction.Prepare(Plan.operations, "fixture", "fixture");
    QVERIFY(Prepared); for (const auto& Step : Prepared.value.steps) QCOMPARE(Transaction.ApplyStep(Step).error, quint32(0));
    QVERIFY(Backend.Read(Location, "Unrelated")); QVERIFY(!Backend.Read(List, "Old"));
    QCOMPARE(RegistryPolicyInspector(Backend).Inspect(Policy, false, After.records).state, PolicyState::Enabled);
    const auto Updated = RegistryPolicyInspector(Backend).Inspect(Policy, false, After.records);
    const auto Off = RegistryPolicyEdit::Prepare(Policy, false, PolicyState::Disabled, {}, Updated, Plan.afterPolicy);
    QVERIFY2(Off, qPrintable(Off.error)); const auto OffRecords = RegistryPolCodec::Read(Off.afterPolicy); QVERIFY(OffRecords);
    QCOMPARE(OffRecords.records.size(), 2); QCOMPARE(OffRecords.records[1].value.name, QString("**Del.On"));
    const auto Unconfigured = RegistryPolicyEdit::Prepare(Policy, false, PolicyState::NotConfigured, {}, Updated, Plan.afterPolicy);
    QVERIFY(Unconfigured); QCOMPARE(RegistryPolCodec::Read(Unconfigured.afterPolicy).records, Original);
    auto Dangerous = Original; Dangerous.append({Policy.key, String("**DeleteKeys", "Child")});
    QVERIFY(!RegistryPolicyEdit::Prepare(Policy, false, PolicyState::Enabled, Options, Observation, RegistryPolCodec::Write(Dangerous).value));
    QVERIFY(!RegistryPolicyEdit::Prepare(Policy, false, PolicyState::Enabled, Options, Observation, "malformed"));
}
void RegistryDescriptionTests::policySoftAndAdditive()
{
    AdmxPolicy Policy; Policy.key = "Software\\HyperCommanderTests\\Soft"; Policy.scope = "User";
    Policy.definition = *AdmxCatalog::Parse(R"XML(<policy key="Software\HyperCommanderTests\Soft" valueName="On"><elements><text id="text" valueName="Text" soft="true"/><list id="list" key="Software\HyperCommanderTests\Soft\List" additive="true" explicitValue="true"/></elements></policy>)XML");
    const auto Location = RegistryLocation::Parse("HKCU\\" + Policy.key).value(); const auto List = Location.Child("List");
    const auto String = [](const QString& Name, const QString& Text) { return RegistryValue{Name, REG_SZ, RegistryValueCodec::EncodeText(REG_SZ, Text).value}; };
    FakeRegistryBackend Backend; Backend.Seed(Location); Backend.Seed(List);
    Backend.Write(Location, String("Text", "user's value")); Backend.Write(List, String("external", "preserve"));
    Backend.Write(List, String("old", "remove"));
    const QList<RegistryPolRecord> Records{{List.key, String("old", "remove")}};
    const auto Original = RegistryPolCodec::Write(Records); QVERIFY(Original);
    const auto Observation = RegistryPolicyInspector(Backend).Inspect(Policy, false, Records);
    QCOMPARE(Observation.options["list"].toList().size(), 1);
    QVariantMap Options{{"text", "default"}, {"list", QVariantList{QVariantMap{{"name", "new"}, {"data", "added"}}}}};
    const auto Plan = RegistryPolicyEdit::Prepare(Policy, false, PolicyState::Enabled, Options, Observation, Original.value);
    QVERIFY2(Plan, qPrintable(Plan.error));
    const auto After = RegistryPolCodec::Read(Plan.afterPolicy); QVERIFY(After);
    bool Soft = false;
    for (const auto& Record : After.records) {
        QVERIFY(Record.Directive() != PolicyDirective::DeleteAllValues);
        if (Record.Directive() == PolicyDirective::SoftValue) { Soft = true; QCOMPARE(Record.value.name, QString("**soft.Text")); }
    }
    QVERIFY(Soft);
    for (const auto& Operation : Plan.operations) QVERIFY(Operation.value.name != "Text" && Operation.value.name != "external");
    RegistryTransaction Transaction(Backend); const auto Prepared = Transaction.Prepare(Plan.operations, "fixture", "fixture"); QVERIFY(Prepared);
    for (const auto& Step : Prepared.value.steps) QCOMPARE(Transaction.ApplyStep(Step).error, quint32(0));
    QVERIFY(Backend.Read(List, "external")); QVERIFY(!Backend.Read(List, "old")); QVERIFY(Backend.Read(List, "new"));
    QCOMPARE(RegistryValueCodec::DecodeText(Backend.Read(Location, "Text").value).text, QString("user's value"));
    const auto Updated = RegistryPolicyInspector(Backend).Inspect(Policy, false, After.records);
    QCOMPARE(Updated.state, PolicyState::Enabled); QCOMPARE(Updated.options["text"].toString(), QString("default"));
    const auto Off = RegistryPolicyEdit::Prepare(Policy, false, PolicyState::NotConfigured, {}, Updated, Plan.afterPolicy);
    QVERIFY(Off); for (const auto& Operation : Off.operations) QVERIFY(Operation.value.name != "Text" && Operation.value.name != "external");
}
QTEST_MAIN(RegistryDescriptionTests)
