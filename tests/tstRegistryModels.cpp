#include "stdafx.h"
#include "tstRegistryModels.hpp"

/// 모듈
/// UIs
/// 기타 헤더파일
#include "registry/fakeRegistryBackend.hpp"
/// 고유 라이브러리
#include "UniqueLibs/registry/registryModels.hpp"
#include "UniqueLibs/registry/registryResultModel.hpp"
#include "UniqueLibs/registry/registryPolicies.hpp"
#include "UniqueLibs/registry/registryLinks.hpp"
#include "UniqueLibs/registry/registryLinkCache.hpp"
#include "UniqueLibs/registry/registryAutoruns.hpp"
#include "commonLibs/system/registryValue.hpp"
#include "UniqueLibs/registry/registryRootProxy.hpp"
#include "UniqueLibs/registry/registryNavigation.hpp"
#include "UniqueLibs/registry/registryFilterProxy.hpp"
#include "UniqueLibs/registry/registryFavorites.hpp"
/// 공통 라이브러리
/// 외부 라이브러리
/// 선언 헤더파일들
/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QAbstractItemModelTester>
#include <QSignalSpy>
#include <QTest>
#include <QThreadPool>
#include <QGuiApplication>
#include <QScopeGuard>
#include <QUuid>
#include <QTemporaryDir>

DECLARE_CMNLIBSV2_NAMESPACE
Q_LOGGING_CATEGORY(lcRegistryModelTests, "hc.tests.registryModels")
using namespace hc::registry;
namespace {
const auto UserRoot = RegistryLocation::Parse("HKCU").value();
std::shared_ptr<FakeRegistryBackend> Fixture()
{
    auto Backend = std::make_shared<FakeRegistryBackend>(); Backend->Seed(UserRoot);
    Backend->Seed(UserRoot.Child("A")); Backend->Seed(UserRoot.Child("B"));
    Backend->Write(UserRoot, {"", REG_BINARY, "default"}); Backend->Write(UserRoot, {"a", REG_BINARY, "value"});
    return Backend;
}
}
void RegistryModelsTests::policyScan()
{
    FakeRegistryBackend Backend; const auto Root = RegistryLocation::Parse("HKCU\\Software\\Policies").value();
    Backend.Seed(Root); const auto Key = Root.Child("HyperCommanderTests"); Backend.Seed(Key);
    Backend.Write(Key, {"On", REG_DWORD, RegistryValueCodec::EncodeNumber(REG_DWORD, 1).value});
    Backend.Write(Key, {"Unknown", REG_SZ, RegistryValueCodec::EncodeText(REG_SZ, "data").value});
    AdmxCatalog Catalog; AdmxPolicy Policy; Policy.id = "fixture:one"; Policy.title = "fixture"; Policy.scope = "User"; Policy.key = Key.key;
    Policy.definition = *AdmxCatalog::Parse(R"XML(<policy key="Software\Policies\HyperCommanderTests" valueName="On"><enabledValue><decimal value="1"/></enabledValue><disabledValue><decimal value="0"/></disabledValue></policy>)XML");
    Catalog.policies.append(Policy); Catalog.keyIndex[Key.key.toCaseFolded()] = {0};
    const int Writes = Backend.mutations_;
    auto Result = RegistryPolicies::Scan(Catalog, Backend, {}, {});
    QCOMPARE(Result.entries.size(), 1); QCOMPARE(Result.rows.size(), 2); QCOMPARE(Backend.mutations_, Writes);
    QCOMPARE(Result.entries["user:fixture:one"].observation.state, PolicyState::Enabled);
    QVERIFY(!Result.rows[1].payload["known"].toBool()); QCOMPARE(Result.rows[1].value->name, QString("Unknown"));
    const auto Denied = RegistryPolicies::Scan(Catalog, Backend, {}, {{}, ERROR_ACCESS_DENIED});
    QVERIFY(!Denied.diagnostics.isEmpty()); QCOMPARE(Denied.entries["user:fixture:one"].localError, quint32(ERROR_ACCESS_DENIED));
    std::atomic_bool Cancel(true); QVERIFY(RegistryPolicies::Scan(Catalog, Backend, {}, {}, &Cancel).rows.isEmpty());
}
void RegistryModelsTests::registryLinks()
{
    FakeRegistryBackend Backend;
    const auto Seed = [&](RegistryLocation Key) { while (true) { if (!Backend.records_.contains(Key.Id())) Backend.Seed(Key); const auto Parent = Key.Parent(); if (!Parent) break; Key = *Parent; } };
    const RegistryLocation Extension{Hive::ClassesRoot, ".HyperCommanderTest"}; const RegistryLocation Program{Hive::ClassesRoot, "HyperCommanderTest.Document"};
    const RegistryLocation Com{Hive::ClassesRoot, "CLSID\\{01234567-1234-1234-1234-0123456789ab}"};
    Seed(Extension); Seed(Program); Seed(Com); Seed(Program.Child("shell\\open\\command"));
    const auto String = [](const QString& Name, const QString& Text) { return RegistryValue{Name, REG_SZ, RegistryValueCodec::EncodeText(REG_SZ, Text).value}; };
    const auto Value = String({}, Program.key); Backend.Write(Extension, Value);
    auto Links = RegistryLinkResolver::ValueLinks(Backend, Extension, Value, {}, false); QCOMPARE(Links.size(), 1); QCOMPARE(Links[0].target->Id(), Program.Id()); QCOMPARE(Links[0].error, quint32(0));
    Links = RegistryLinkResolver::ValueLinks(Backend, Program, String("CLSID", Com.key.section('\\', -1)), {}, false); QCOMPARE(Links.size(), 1); QCOMPARE(Links[0].target->Id(), Com.Id());
    const auto Index = RegistryLinkResolver::BuildIndex(Backend, View::Native64); QCOMPARE(Index.incoming[Program.Id()].size(), 1);
    const auto Limited = RegistryLinkResolver::BuildIndex(Backend, View::Native64, nullptr, 1); QVERIFY(!Limited.complete);
    std::atomic_bool Cancel{true}; QCOMPARE(RegistryLinkResolver::BuildIndex(Backend, View::Native64, &Cancel).keys, qsizetype(0));
    QProcessEnvironment Environment; Environment.insert("SystemRoot", "C:\\Windows");
    QCOMPARE(RegistryLinkResolver::FilePath("\"C:\\Program Files\\HyperCommanderTest.exe\" /test", Environment), QString("C:\\Program Files\\HyperCommanderTest.exe"));
    QCOMPARE(RegistryLinkResolver::FilePath("@%SystemRoot%\\System32\\shell32.dll,-1", Environment), QString("C:\\Windows\\System32\\shell32.dll"));
    QVERIFY(RegistryLinkResolver::FilePath("%Missing%\\test.exe", Environment).isEmpty());
    wchar_t Native[MAX_PATH]{}, Wow[MAX_PATH]{};
    QVERIFY(GetSystemDirectoryW(Native, DWORD(std::size(Native))));
    QVERIFY(GetSystemWow64DirectoryW(Wow, DWORD(std::size(Wow))));
    const auto Dll = QString::fromWCharArray(Native) + "\\shell32.dll";
    auto Inproc = Com.Child("InprocServer32"); Inproc.view = View::Wow32;
    Links = RegistryLinkResolver::ValueLinks(Backend, Inproc, String({}, Dll), Environment);
    QVERIFY(!Links.isEmpty()); QCOMPARE(Links.last().file, QString::fromWCharArray(Wow) + "\\shell32.dll"); QCOMPARE(Links.last().error, quint32(0));
    Inproc.view = View::Native64;
    Links = RegistryLinkResolver::ValueLinks(Backend, Inproc, String({}, Dll), Environment);
    QCOMPARE(Links.last().file, Dll);
    QCOMPARE(RegistryLinkResolver::CanonicalKey({Hive::CurrentUser, "Software\\Classes\\.HyperCommanderTest"}), Extension.key);
}
void RegistryModelsTests::physicalClasses()
{
    FakeRegistryBackend Backend; const RegistryLocation User{Hive::CurrentUser, "Software\\Classes"};
    Backend.Seed(User); QCOMPARE(RegistryLinkResolver::PhysicalClasses(Backend, Hive::CurrentUser, View::Native64).value, User);
    const auto Bytes = RegistryValueCodec::EncodeText(REG_SZ, "\\Registry\\User\\S-1-5-21-1_Classes").value;
    Backend.Write(User, {"SymbolicLinkValue", REG_LINK, Bytes});
    const auto Physical = RegistryLinkResolver::PhysicalClasses(Backend, Hive::CurrentUser, View::Native64);
    QVERIFY(Physical); QCOMPARE(Physical.value.hive, Hive::Users); QCOMPARE(Physical.value.key, QString("S-1-5-21-1_Classes"));
    QCOMPARE(RegistryLinkResolver::CanonicalKey(Physical.value.Child(".HyperCommanderTest")), QString(".HyperCommanderTest"));
    Backend.Write(User, {"SymbolicLinkValue", REG_LINK, QByteArray("odd")});
    QCOMPARE(RegistryLinkResolver::PhysicalClasses(Backend, Hive::CurrentUser, View::Native64).error, quint32(ERROR_INVALID_DATA));
}
void RegistryModelsTests::autorunApprovals()
{
    const auto Enabled = StartupApprovedCodec::Encode("HyperCommanderTest", true, 123);
    const auto Disabled = StartupApprovedCodec::Encode("HyperCommanderTest", false, 123);
    QCOMPARE(Enabled.bytes, QByteArray::fromHex("020000000000000000000000"));
    QCOMPARE(StartupApprovedCodec::Decode(Disabled).state, StartupApproval::State::Disabled);
    QCOMPARE(StartupApprovedCodec::Decode(Disabled).disabledAt, quint64(123));
    auto Unknown = Disabled; Unknown.bytes[0] = char(7); QCOMPARE(StartupApprovedCodec::Decode(Unknown).state, StartupApproval::State::Unknown);
    Unknown = Enabled; Unknown.bytes.append('x'); QCOMPARE(StartupApprovedCodec::Decode(Unknown).state, StartupApproval::State::Unknown);
    RegistryLocation Run{Hive::LocalMachine, "Software\\Microsoft\\Windows\\CurrentVersion\\Run", View::Wow32};
    const auto Target = StartupApprovedCodec::LocationFor(Run); QVERIFY(Target); QCOMPARE(Target->view, View::Native64); QVERIFY(Target->key.endsWith("\\Run32"));
    Run.hive = Hive::CurrentUser; QVERIFY(!StartupApprovedCodec::LocationFor(Run)); Run.view = View::Native64; QVERIFY(StartupApprovedCodec::LocationFor(Run));
    Run.key += "Once"; QVERIFY(!StartupApprovedCodec::LocationFor(Run));
}
void RegistryModelsTests::autorunGuardedToggle()
{
    FakeRegistryBackend Backend; const RegistryLocation Source{Hive::LocalMachine, "Software\\Microsoft\\Windows\\CurrentVersion\\Run"};
    const auto Target = StartupApprovedCodec::LocationFor(Source).value();
    for (auto Key : {Source, Target}) while (true) { if (!Backend.records_.contains(Key.Id())) Backend.Seed(Key); const auto Parent = Key.Parent(); if (!Parent) break; Key = *Parent; }
    const RegistryValue Command{"HyperCommanderTest", REG_SZ, RegistryValueCodec::EncodeText(REG_SZ, "C:\\HyperCommanderTest.exe").value};
    Backend.Write(Source, Command); Backend.mutations_ = 0;
    RegistrySessionServer Server(Backend, "fixture", "machine"); RegistryRemoteSession Remote([&](const QString& Kind, QJsonObject Request) { return Server.Handle(Kind, Request); });
    QTemporaryDir Directory; RegistryChangeStore Store(Directory.path()); RegistryMutation Mutation(Backend, Store, "fixture", "machine", &Remote);
    RegistryAutorunEntry Entry; Entry.location = Source; Entry.value = Command; Entry.switchLocation = Target; Entry.canToggle = true;
    const auto Applied = RegistryAutoruns::Toggle(Mutation, Entry, 123); QCOMPARE(Applied.applied.error, quint32(0));
    QCOMPARE(Backend.mutations_, 1); QCOMPARE(Backend.Read(Source, Command.name).value, Command);
    QCOMPARE(StartupApprovedCodec::Decode(Backend.Read(Target, Command.name).value).state, StartupApproval::State::Disabled);
    const auto Undo = Mutation.Undo(Applied.record); QCOMPARE(Undo.applied.error, quint32(0)); QCOMPARE(Backend.mutations_, 2); QVERIFY(!Backend.Read(Target, Command.name));
    auto Changed = Command; Changed.bytes = RegistryValueCodec::EncodeText(REG_SZ, "C:\\DifferentHyperCommanderTest.exe").value; Backend.Write(Source, Changed); const auto Count = Backend.mutations_;
    const auto Raced = RegistryAutoruns::Toggle(Mutation, Entry, 456); QVERIFY(Raced.applied.conflict); QCOMPARE(Backend.mutations_, Count); QVERIFY(!Backend.Read(Target, Command.name));
}
void RegistryModelsTests::groupedResults()
{
    RegistryResultModel Model({"name", "state"});
    QAbstractItemModelTester Tester(&Model, QAbstractItemModelTester::FailureReportingMode::QtTest);
    RegistryResultRow First; First.id = "one"; First.groups = {"user", "system"};
    First.cells = {"first", "enabled"}; First.location = UserRoot; First.value = RegistryValue{"", REG_BINARY, "raw"};
    First.payload = {{"policyId", "fixture"}}; First.searchText = "hidden metadata";
    auto Second = First; Second.id = "two"; Second.cells = {"second", "disabled"};
    auto Third = First; Third.id = "three"; Third.groups = {"machine", "system"};
    Model.SetRows({First, Second, Third, First}); // Duplicate identities cannot create ambiguous selections.
    QCOMPARE(Model.rowCount(), 2);
    const auto User = Model.index(0, 0); const auto Group = Model.index(0, 0, User);
    QCOMPARE(Model.rowCount(Group), 2); QVERIFY(User.data(RegistryResultModel::GroupRole).toBool());
    const auto Value = Model.index(0, 0, Group);
    QCOMPARE(Value.parent(), Group); QCOMPARE(Value.data().toString(), QString("first"));
    QCOMPARE(Model.index(0, 1, Group).data().toString(), QString("enabled"));
    QCOMPARE(Value.data(RegistryTreeModel::RawValueRole).toByteArray(), QByteArray("raw"));
    QCOMPARE(Value.data(RegistryResultModel::PayloadRole).toMap()["policyId"].toString(), QString("fixture"));
    QVERIFY(Value.data(RegistryResultModel::SearchRole).toString().contains("hidden metadata"));
    QVERIFY(!Model.Row(Group)); QVERIFY(Model.Row(Value)); QCOMPARE(Model.rowCount(Value), 0);
    QPersistentModelIndex Stale(Value); Model.SetRows({}); QVERIFY(!Stale.isValid()); QCOMPARE(Model.rowCount(), 0);
}
void RegistryModelsTests::lazySharedCache()
{
    auto Backend = Fixture(); RegistryService Service(Backend);
    RegistryKeyTreeModel Keys(&Service); RegistryItemsModel Items(&Service);
    QAbstractItemModelTester KeyTester(&Keys, QAbstractItemModelTester::FailureReportingMode::QtTest); KeyTester.setUseFetchMore(false);
    QAbstractItemModelTester ItemTester(&Items, QAbstractItemModelTester::FailureReportingMode::QtTest); ItemTester.setUseFetchMore(false);
    QCOMPARE(Keys.rowCount(), 5); QCOMPARE(Items.columnCount(), 4);
    const auto KeyRoot = Keys.Find(UserRoot.Id()); const auto ItemRoot = Items.Find(UserRoot.Id());
    Keys.fetchMore(KeyRoot); Items.fetchMore(ItemRoot);
    QVERIFY(!Keys.canFetchMore(KeyRoot));
    QTRY_VERIFY(Service.Cached(UserRoot) && Service.Cached(UserRoot)->loaded);
    QCOMPARE(Keys.rowCount(KeyRoot), 2); QCOMPARE(Items.rowCount(ItemRoot), 4);
    QCOMPARE(Items.index(2, 0, ItemRoot).data().toString(), QString("(기본값)"));
    QCOMPARE(Items.index(2, 0, ItemRoot).data(RegistryTreeModel::ValueNameRole).toString(), QString());
    QVERIFY(Items.index(0, 0, ItemRoot).data(RegistryTreeModel::IdRole) != Items.index(3, 0, ItemRoot).data(RegistryTreeModel::IdRole));
}
void RegistryModelsTests::refreshPreservesIndexes()
{
    auto Backend = Fixture(); RegistryService Service(Backend); RegistryItemsModel Items(&Service);
    const auto Root = Items.Find(UserRoot.Id()); Items.fetchMore(Root);
    QTRY_VERIFY(Service.Cached(UserRoot)->loaded);
    QPersistentModelIndex Stable(Items.Find(UserRoot.Child("B").Id())); QVERIFY(Stable.isValid());
    Backend->DeleteKey(UserRoot.Child("A")); Backend->Seed(UserRoot.Child("C"));
    Items.Refresh(Root); QTRY_VERIFY(!Service.Cached(UserRoot)->pending);
    QVERIFY(Stable.isValid()); QCOMPARE(Stable.data().toString(), QString("B")); QCOMPARE(Stable.row(), 0);
    QVERIFY(!Items.Find(UserRoot.Child("A").Id()).isValid());
    QVERIFY(Items.Find(UserRoot.Child("C").Id()).isValid());
}
void RegistryModelsTests::rootProxyAndBitView()
{
    auto Backend = Fixture(); RegistryService Service(Backend); RegistryItemsModel Items(&Service);
    RegistryRootProxy Proxy; Proxy.setSourceModel(&Items); Proxy.SetRoot(Items.Find(UserRoot.Id()));
    QAbstractItemModelTester Tester(&Proxy, QAbstractItemModelTester::FailureReportingMode::QtTest); Tester.setUseFetchMore(false);
    Proxy.fetchMore({}); QTRY_VERIFY(Service.Cached(UserRoot)->loaded); QCOMPARE(Proxy.rowCount(), 4);
    QCOMPARE(Proxy.index(0, 0).data().toString(), QString("A"));
    Items.SetView(View::Wow32); QCOMPARE(Items.rowCount(), 5);
    QVERIFY(!Items.Find(UserRoot.Id()).isValid());
    auto Other = UserRoot; Other.view = View::Wow32; QVERIFY(Items.Find(Other.Id()).isValid());
}
void RegistryModelsTests::closePending()
{
    auto Backend = Fixture();
    auto* Service = new RegistryService(Backend); auto* Items = new RegistryItemsModel(Service);
    Service->Request(UserRoot); Service->Request(UserRoot, true); delete Items; delete Service;
    QVERIFY(QThreadPool::globalInstance()->waitForDone(5000));
    QCoreApplication::processEvents();
}
void RegistryModelsTests::descendingKeysFirst()
{
    auto Backend = Fixture(); RegistryService Service(Backend); RegistryItemsModel Items(&Service);
    const auto Root = Items.Find(UserRoot.Id()); Items.fetchMore(Root);
    QTRY_VERIFY(Service.Cached(UserRoot)->loaded);
    Items.SetSort(RegistryTreeModel::Name, Qt::DescendingOrder);
    QList<QModelIndex> Rows; for (int I = 0; I < Items.rowCount(Root); ++I) Rows.append(Items.index(I, 0, Root));
    std::sort(Rows.begin(), Rows.end(), [](const QModelIndex& A, const QModelIndex& B) {
        return A.data(RegistryTreeModel::GridSortRole).toLongLong() > B.data(RegistryTreeModel::GridSortRole).toLongLong();
    });
    QCOMPARE(Rows[0].data().toString(), QString("B")); QCOMPARE(Rows[1].data().toString(), QString("A"));
    QCOMPARE(Rows[2].data(RegistryTreeModel::KindRole).toInt(), int(RegistryTreeModel::Kind::Value));
    Backend->Write(UserRoot, {"z", REG_BINARY, "last"}); Items.Refresh(Root);
    QTRY_VERIFY(!Service.Cached(UserRoot)->pending);
    QCOMPARE(Items.Find(UserRoot.Child("B").Id()).data(RegistryTreeModel::GridSortRole).toLongLong(), 0);
}
void RegistryModelsTests::nativeWatchRefresh()
{
    WinRegistryBackend Backend;
    const auto Base = RegistryLocation::Parse("HKCU\\Software\\HyperCommanderTests").value();
    QCOMPARE(Backend.Create(Base), quint32(0));
    const auto Key = Base.Child("RegistryEditor-task_4ceba737038e-" + QUuid::createUuid().toString(QUuid::WithoutBraces));
    QCOMPARE(Backend.Create(Key), quint32(0));
    const auto Cleanup = qScopeGuard([&] { Backend.DeleteKey(Key); });
    RegistryService Service; Service.Request(Key);
    QTRY_VERIFY(Service.Cached(Key) && Service.Cached(Key)->loaded);
    Service.SetWatched({Key});
    // Writes start independently of registration timing; the service must see a
    // native notification, not a manual refresh or a registry polling timer.
    QTimer Writer; Writer.setInterval(40);
    connect(&Writer, &QTimer::timeout, &Service, [&] { Backend.Write(Key, {"watch", REG_BINARY, "changed"}); });
    Writer.start();
    QTRY_VERIFY_WITH_TIMEOUT(!Service.Cached(Key)->snapshot.value.values.isEmpty(), 5000);
    Writer.stop(); QCOMPARE(Service.Cached(Key)->snapshot.value.values[0].bytes, QByteArray("changed"));
    Service.SetWatched({});
    QVERIFY(QThreadPool::globalInstance()->waitForDone(5000));
    QCOMPARE(Backend.DeleteKey(Key), quint32(0));
    QCOMPARE(WinRegistryBackend::Open(Key, KEY_QUERY_VALUE).error, quint32(ERROR_FILE_NOT_FOUND));
}
void RegistryModelsTests::navigationAndRestore()
{
    auto Backend = Fixture(); Backend->Seed(UserRoot.Child("A").Child("nested"));
    RegistryService Service(Backend); RegistryKeyTreeModel Keys(&Service); RegistryItemsModel Items(&Service);
    RegistryNavigation Navigation(&Service, &Keys, &Items); QSignalSpy Errors(&Navigation, &RegistryNavigation::Failed);
    QVERIFY(!Navigation.Current());
    QVERIFY(Navigation.Navigate(QStringLiteral("컴퓨터\\HKCU\\A\\nested")));
    QTRY_VERIFY(!Navigation.Pending()); QCOMPARE(Navigation.Path(), UserRoot.Child("A").Child("nested").Path());
    Navigation.Up(); QTRY_VERIFY(!Navigation.Pending()); QCOMPARE(Navigation.Path(), UserRoot.Child("A").Path());
    Navigation.Back(); QTRY_VERIFY(!Navigation.Pending()); QCOMPARE(Navigation.Path(), UserRoot.Child("A").Child("nested").Path());
    Navigation.Forward(); QTRY_VERIFY(!Navigation.Pending()); QCOMPARE(Navigation.Path(), UserRoot.Child("A").Path());
    const auto Saved = Navigation.CaptureState(); Navigation.Navigate({}); QTRY_VERIFY(!Navigation.Pending()); QVERIFY(!Navigation.Current());
    Navigation.RestoreState(Saved); QTRY_VERIFY(!Navigation.Pending()); QCOMPARE(Navigation.Path(), UserRoot.Child("A").Path());
    Navigation.Navigate("HKCU\\does-not-exist"); QTRY_COMPARE(Errors.size(), 1); QCOMPARE(Navigation.Path(), UserRoot.Child("A").Path());
    Navigation.Navigate("HKCU\\A\\nested"); Navigation.Navigate("HKCU\\B");
    QTRY_VERIFY(!Navigation.Pending()); QCOMPARE(Navigation.Path(), UserRoot.Child("B").Path());
    auto Other = UserRoot; Other.view = View::Wow32; Backend->Seed(Other);
    Navigation.SetView(View::Wow32); QTRY_VERIFY(!Navigation.Pending());
    QVERIFY(!Navigation.Current()); QCOMPARE(Navigation.SelectedView(), View::Wow32);
    QCOMPARE(Errors.size(), 2);
    Navigation.SetView(View::Native64); Navigation.Navigate("HKCU\\A");
    QTRY_VERIFY(!Navigation.Pending());
    QTRY_COMPARE(Items.rowCount(Items.Find(UserRoot.Child("A").Id())), 1);
    QCOMPARE(Keys.rowCount(Keys.Find(UserRoot.Child("A").Id())), 1);
}
void RegistryModelsTests::failedHistoryNavigation()
{
    auto Backend = Fixture(); RegistryService Service(Backend);
    RegistryKeyTreeModel Keys(&Service); RegistryItemsModel Items(&Service);
    RegistryNavigation Navigation(&Service, &Keys, &Items);
    QSignalSpy Errors(&Navigation, &RegistryNavigation::Failed);
    Navigation.Navigate("HKCU\\A"); QTRY_VERIFY(!Navigation.Pending());
    Navigation.Navigate("HKCU\\B"); QTRY_VERIFY(!Navigation.Pending());
    const auto Saved = Navigation.CaptureState();
    QCOMPARE(Backend->DeleteKey(UserRoot.Child("A")), quint32(0));
    Service.Invalidate(UserRoot); QTRY_VERIFY(!Service.Cached(UserRoot)->pending);
    Navigation.Back(); QTRY_COMPARE(Errors.size(), 1);
    QCOMPARE(Navigation.Path(), UserRoot.Child("B").Path());
    QCOMPARE(Navigation.CaptureState()["historyIndex"], Saved["historyIndex"]);
    QVERIFY(!Navigation.CanForward());
    // A subsequent valid back request still starts from the committed index.
    Backend->Seed(UserRoot.Child("A")); Service.Invalidate(UserRoot);
    QTRY_VERIFY(!Service.Cached(UserRoot)->pending);
    Navigation.Back(); QTRY_VERIFY(!Navigation.Pending());
    QCOMPARE(Navigation.Path(), UserRoot.Child("A").Path());
}
void RegistryModelsTests::flatRowsAndFilter()
{
    auto Backend = Fixture(); Backend->Seed(UserRoot.Child("A").Child("nested"));
    RegistryService Service(Backend); RegistryItemsModel Items(&Service);
    RegistryRootProxy Root; Root.setSourceModel(&Items); Root.SetRoot(Items.Find(UserRoot.Id()));
    Root.fetchMore({}); QTRY_VERIFY(Service.Cached(UserRoot)->loaded);
    const auto Child = Root.index(0, 0); QVERIFY(Root.canFetchMore(Child));
    Root.fetchMore(Child); QTRY_VERIFY(Service.Cached(UserRoot.Child("A"))->loaded);
    QCOMPARE(Root.rowCount(Child), 1);
    Root.SetFlat(true); const auto FlatChild = Root.index(0, 0);
    QVERIFY(!Root.hasChildren(FlatChild)); QVERIFY(!Root.canFetchMore(FlatChild));
    QCOMPARE(Root.rowCount(FlatChild), 0); QVERIFY(!Root.index(0, 0, FlatChild).isValid());
    RegistryFilterProxy Filter; Filter.setSourceModel(&Root);
    QAbstractItemModelTester Tester(&Filter, QAbstractItemModelTester::FailureReportingMode::QtTest); Tester.setUseFetchMore(false);
    Filter.SetText("nested"); QCOMPARE(Filter.rowCount(), 0);
    Root.SetFlat(false); QCOMPARE(Filter.rowCount(), 1);
    QCOMPARE(Filter.index(0, 0).data().toString(), QString("A"));
    Filter.SetText({}); QCOMPARE(Filter.rowCount(), 4);
    Filter.SetKnownOnly(true); QCOMPARE(Filter.rowCount(), 0);
    Filter.SetKnownOnly(false); QCOMPARE(Filter.rowCount(), 4);
}
void RegistryModelsTests::sharedFavorites()
{
    const auto Store = UserRoot.Child("Software\\HyperCommanderTests\\Favorites");
    const auto Target = UserRoot.Child("Software\\Example");
    KeySnapshot Snapshot; Snapshot.values.append({"Example", REG_BINARY, "foreign"});
    const auto Add = RegistryFavorites::Toggle(Store, Snapshot, Target);
    QCOMPARE(Add.size(), 2); QCOMPARE(Add[1].value.name, QString("Example (2)"));
    QCOMPARE(Add[1].location.Id(), Store.Id());
    Snapshot.values.append(Add[1].value);
    const auto Favorites = RegistryFavorites::Decode(Snapshot, View::Wow32);
    QCOMPARE(Favorites.size(), 1); QCOMPARE(Favorites[0].location.Path(), Target.Path());
    QCOMPARE(Favorites[0].location.view, View::Wow32);
    const auto Remove = RegistryFavorites::Toggle(Store, Snapshot, Target);
    QCOMPARE(Remove.size(), 1); QCOMPARE(Remove[0].kind, OperationKind::DeleteValue);
    QCOMPARE(Remove[0].value.name, QString("Example (2)"));
    auto Malformed = Add[1].value; Malformed.name = "malformed"; Malformed.bytes.chop(1);
    Snapshot.values.append(Malformed);
    QCOMPARE(RegistryFavorites::Decode(Snapshot, View::Native64).size(), 1);
}
// QAbstractItemModelTester probes GUI roles (including QPixmap, type 4097).
// Register GUI metatypes even though the model itself has no widgets.
#include "registry/historyModelCases.hpp"
void RegistryModelsTests::historyCandidates() { RegistryHistoryModelCases(); }
void RegistryModelsTests::historyRecovery()
{
    WinRegistryBackend Backend;
    const auto Base = RegistryLocation::Parse("HKCU\\Software\\HyperCommanderTests").value(); QCOMPARE(Backend.Create(Base), quint32(0));
    const auto Key = Base.Child("RegistryEditor-task_4ceba737038e-" + QUuid::createUuid().toString(QUuid::WithoutBraces));
    QCOMPARE(Backend.Create(Key), quint32(0)); const auto Cleanup = qScopeGuard([&] { Backend.DeleteKey(Key); });
    QCOMPARE(Backend.Write(Key, {"value", REG_BINARY, "before"}), quint32(0));
    RegistryTransaction Transaction(Backend); QTemporaryDir Directory; RegistryChangeStore Store(Directory.path());
    auto Plan = Transaction.Prepare({{OperationKind::SetValue, Key, {"value", REG_BINARY, "after"}}}, "S-1-5-21-1", "fixture-machine"); QVERIFY(Plan);
    auto Record = Store.SavePrepared(Plan.value); QVERIFY(Record);
    const auto Applied = Store.Apply(Record.value, [&](const RegistryStep& Step, int) {
        const auto Result = Transaction.ApplyStep(Step);
        return RegistryApplyResult{ERROR_BROKEN_PIPE, Result.completed, false, true};
    });
    QVERIFY(Applied.uncertain); QCOMPARE(Record.value.inFlight, 0);
    RegistryHistoryModel Model;
    Model.Reload(Directory.path(), "different-user", "fixture-machine"); QTRY_VERIFY(!Model.Loading());
    QVERIFY(!Model.UndoCandidate()); QCOMPARE(Store.Read(Plan.value.id).value.inFlight, 0);
    Model.Reload(Directory.path(), "S-1-5-21-1", "fixture-machine"); QTRY_VERIFY(!Model.Loading());
    QVERIFY(Model.UndoCandidate()); QCOMPARE(Model.UndoCandidate()->completed, 1);
    QCOMPARE(Store.Read(Plan.value.id).value.inFlight, -1);
    QCOMPARE(Store.Read(Plan.value.id).value.error, quint32(0));
    QCOMPARE(Backend.Read(Key, "value").value.bytes, QByteArray("after"));
}
QTEST_MAIN(RegistryModelsTests)

void RegistryModelsTests::linkCacheResume()
{
    FakeRegistryBackend Backend;
    const RegistryLocation Root{Hive::ClassesRoot}, Ext = Root.Child(".HyperCommanderTest"), Prog = Root.Child("HyperCommanderTest.Document");
    const RegistryLocation User{Hive::CurrentUser, "Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts"};
    Backend.Seed(Root); Backend.Seed(Ext); Backend.Seed(Prog); Backend.Seed(User);
    Backend.Write(Ext, {{}, REG_SZ, RegistryValueCodec::EncodeText(REG_SZ, Prog.key).value});
    const auto CurVer = Prog.Child("CurVer"); Backend.Seed(CurVer);
    Backend.Write(CurVer, {{}, REG_SZ, RegistryValueCodec::EncodeText(REG_SZ, "HyperCommanderTest.Document.2").value});
    RegistryLinkCache Cache(View::Native64); std::atomic_bool Cancel{true};
    Cache.Advance(Backend, &Cancel, 1); QCOMPARE(Cache.Keys(), qsizetype(0));
    Cancel.store(false); Cache.Advance(Backend, &Cancel, 1); QVERIFY(!Cache.Complete());
    QTemporaryDir Directory; const auto Path = Directory.filePath("links.json");
    QVERIFY(Cache.Save(Path, "test-sid")); RegistryLinkCache Resumed(View::Native64);
    QVERIFY(!Resumed.Load(Path, "other-sid")); QVERIFY(Resumed.Load(Path, "test-sid"));
    for (int I = 0; I < 100 && !Resumed.Complete(); ++I) Resumed.Advance(Backend, &Cancel, 1);
    QVERIFY(Resumed.FastComplete()); QVERIFY(Resumed.Complete());
    QCOMPARE(Resumed.Snapshot().incoming[Prog.Id()].size(), 1);
    QVERIFY(Resumed.Save(Path, "test-sid")); RegistryLinkCache Warm(View::Native64); QVERIFY(Warm.Load(Path, "test-sid"));
    Warm.Validate(); for (int I = 0; I < 100 && !Warm.Complete(); ++I) Warm.Advance(Backend, &Cancel, 2);
    QVERIFY(Warm.Complete()); QCOMPARE(Warm.Reads(), qsizetype(0));
    Backend.Write(Ext, {{}, REG_SZ, RegistryValueCodec::EncodeText(REG_SZ, "HyperCommanderTest.Changed").value});
    ++Backend.records_[Ext.Id()].written;
    Warm.Validate(); for (int I = 0; I < 100 && !Warm.Complete(); ++I) Warm.Advance(Backend, &Cancel, 2);
    QCOMPARE(Warm.Reads(), qsizetype(1)); QVERIFY(Warm.Snapshot().incoming[Prog.Id()].isEmpty());
    QCOMPARE(Warm.Snapshot().incoming[RegistryLocation(Hive::ClassesRoot, "HyperCommanderTest.Changed").Id()].size(), 1);
    Backend.DeleteKey(Ext); Warm.Validate(); for (int I = 0; I < 100 && !Warm.Complete(); ++I) Warm.Advance(Backend, &Cancel, 2);
    QVERIFY(Warm.Snapshot().incoming[Prog.Id()].isEmpty());
}