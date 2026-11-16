#include "stdafx.h"
#include "tstEnvironmentStorage.hpp"

/// 모듈
/// UIs
/// 기타 헤더파일
#include "registry/fakeRegistryBackend.hpp"
/// 고유 라이브러리
#include "UniqueLibs/commandUiConfig.hpp"
#include "UniqueLibs/system/environmentStorage.hpp"
#include "UniqueLibs/registry/registryHistoryModel.hpp"
#include "UniqueLibs/system/environmentProfiles.hpp"
#include "UniqueLibs/system/environmentSnapshot.hpp"
#include "UniqueLibs/system/environmentTransfer.hpp"
#include "UniqueLibs/system/environmentModel.hpp"
#include "UniqueLibs/system/environmentSession.hpp"
/// 공통 라이브러리
/// 외부 라이브러리
/// 선언 헤더파일들
/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QTest>
#include <QTemporaryDir>
#include <QFile>
#include <QDirIterator>

DECLARE_CMNLIBSV2_NAMESPACE
Q_LOGGING_CATEGORY(lcEnvironmentStorageTests, "hc.tests.environmentStorage")
using namespace hc::environment;
using namespace hc::registry;
namespace {
const QString Caller = "S-1-5-21-1-2-3-1001", Machine = "synthetic-machine";
hc::environment::Value Item(const wchar_t* Name, const wchar_t* Raw, Scope Source = Scope::User)
{ return {Name, Raw, ValueType::ExpandString, Source}; }
QByteArray ReadFile(const QString& Path) { QFile File(Path); if (!File.open(QIODevice::ReadOnly)) return {}; return File.readAll(); }
}
void EnvironmentStorageTests::failedRenameRestoresDestination()
{
    FakeRegistryBackend Backend; const auto Location = EnvironmentLocation(Scope::User, Caller); Backend.Seed(Location);
    QTemporaryDir Directory; RegistryChangeStore Store(Directory.path()); EnvironmentStorage Storage(Backend, Store, Caller, Machine);
    const auto Before = Item(L"SyntheticBefore", L"original"), After = Item(L"SyntheticAfter", L"original");
    Backend.Write(Location, EncodeEnvironmentValue(Before));
    const auto Preview = Storage.Preview({{Scope::User, "SyntheticBefore", Before, {}}, {Scope::User, "SyntheticAfter", {}, After}});
    QVERIFY(Preview); QVERIFY(Preview.plan_.steps[0].after.has_value());
    Backend.failAt_ = Backend.mutations_ + 2;
    const auto Result = Storage.Commit(Preview);
    QCOMPARE(Result.applied_.error, quint32(ERROR_ACCESS_DENIED)); QVERIFY2(Result.restored_, qPrintable(QString("recovery=%1 completed=%2 uncertain=%3 state=%4 inFlight=%5").arg(Result.recoveryError_).arg(Result.applied_.completed).arg(Result.applied_.uncertain).arg(Result.record_.state).arg(Result.record_.inFlight))); QVERIFY(!Result.recoveryId_.isEmpty());
    QCOMPARE(Backend.Read(Location, "SyntheticBefore").value, EncodeEnvironmentValue(Before));
    QVERIFY(!Backend.Read(Location, "SyntheticAfter"));
}
void EnvironmentStorageTests::typedReadAndConflict()
{
    FakeRegistryBackend Backend; const auto Location = EnvironmentLocation(Scope::User, Caller); Backend.Seed(Location);
    QTemporaryDir Directory; RegistryChangeStore Store(Directory.path()); EnvironmentStorage Storage(Backend, Store, Caller, Machine);
    const auto Original = Item(L"Demo", L"%SystemRoot%\\Synthetic");
    Backend.Write(Location, EncodeEnvironmentValue(Original));
    const auto Read = Storage.ReadScope(Scope::User); QVERIFY(Read); QCOMPARE(Read.value.size(), size_t(1)); QCOMPARE(Read.value[0], Original);
    const auto Desired = Item(L"Demo", L"new"); const auto Preview = Storage.Preview({{Scope::User, "Demo", Original, Desired}}); QVERIFY(Preview);
    Backend.Write(Location, EncodeEnvironmentValue(Item(L"Demo", L"outside")));
    const auto Result = Storage.Commit(Preview); QVERIFY(Result.applied_.conflict); QCOMPARE(Result.applied_.completed, 0);
    QVERIFY(QDir(Directory.path()).entryList(QDir::Dirs | QDir::NoDotAndDotDot).empty());
}
void EnvironmentStorageTests::durableUndoRedo()
{
    FakeRegistryBackend Backend; const auto Location = EnvironmentLocation(Scope::User, Caller); Backend.Seed(Location);
    QTemporaryDir Directory; RegistryChangeStore Store(Directory.path()); EnvironmentStorage Storage(Backend, Store, Caller, Machine);
    const auto Result = Storage.Commit(Storage.Preview({{Scope::User, "Demo", {}, Item(L"Demo", L"")}})); QCOMPARE(Result.applied_.error, quint32(0));
    QVERIFY(Backend.Read(Location, "Demo")); QVERIFY(QFile::exists(Result.record_.directory + "/before.reg"));
    const auto Undo = Storage.Commit(Storage.Undo(Result.record_)); QCOMPARE(Undo.applied_.error, quint32(0)); QVERIFY(!Backend.Read(Location, "Demo"));
    const auto Redo = Storage.Commit(Storage.Redo(Result.record_, Undo.record_)); QCOMPARE(Redo.applied_.error, quint32(0)); QVERIFY(Backend.Read(Location, "Demo"));
    QCOMPARE(Store.History().value.size(), 3);
}
void EnvironmentStorageTests::recoverOnlyCallerSideWrites()
{
    FakeRegistryBackend Backend;
    Backend.Seed(EnvironmentLocation(Scope::User, Caller)); Backend.Seed(EnvironmentLocation(Scope::System));
    QTemporaryDir Directory; RegistryChangeStore Store(Directory.path()); EnvironmentStorage Storage(Backend, Store, Caller, Machine);
    for (const auto Source : {Scope::User, Scope::System}) {
        const auto Preview = Storage.Preview({{Source, "SyntheticRecovery", {}, Item(L"SyntheticRecovery", L"after", Source)}});
        QVERIFY(Preview); auto Saved = Store.SavePrepared(Preview.plan_); QVERIFY(Saved);
        const auto Applied = Store.Apply(Saved.value, [&](const RegistryStep& Step, int) {
            const auto Error = Backend.Write(Step.location, *Step.after); if (Error) return RegistryApplyResult{Error};
            return RegistryApplyResult{ERROR_TIMEOUT, 0, false, true};
        });
        QVERIFY(Applied.uncertain);
        QCOMPARE(Storage.RecoverLocalHistory(), quint32(0));
        const auto Record = Store.Read(Saved.value.plan.id); QVERIFY(Record);
        if (Source == Scope::System) { QCOMPARE(Record.value.inFlight, 0); QCOMPARE(Record.value.state, QString("recovery-required")); }
        else {
            QCOMPARE(Record.value.inFlight, -1); QCOMPARE(Record.value.state, QString("committed"));
            QCOMPARE(Storage.Commit(Storage.Undo(Record.value)).applied_.error, quint32(0));
            QVERIFY(!Backend.Read(EnvironmentLocation(Source, Caller), "SyntheticRecovery"));
        }
    }
}
void EnvironmentStorageTests::protectedHistory()
{
    FakeRegistryBackend Backend; const auto Location = EnvironmentLocation(Scope::User, Caller); Backend.Seed(Location);
    QTemporaryDir Directory; RegistryChangeStore Store(Directory.path()); EnvironmentStorage Storage(Backend, Store, Caller, Machine);
    const auto Result = Storage.Commit(Storage.Preview({{Scope::User, "SYNTHETIC_TOKEN", {}, Item(L"SYNTHETIC_TOKEN", L"invented-fixture-secret")}}));
    QCOMPARE(Result.applied_.error, quint32(0));
    QVERIFY(!QFile::exists(Result.record_.directory + "/before.reg")); QVERIFY(QFile::exists(Result.record_.directory + "/after.reg.dpapi"));
    QDirIterator Files(Result.record_.directory, QDir::Files);
    const auto Raw = EncodeEnvironmentValue(Item(L"SYNTHETIC_TOKEN", L"invented-fixture-secret")).bytes;
    while (Files.hasNext()) { const auto Bytes = ReadFile(Files.next()); QVERIFY(!Bytes.contains("invented-fixture-secret")); QVERIFY(!Bytes.contains(Raw)); QVERIFY(!Bytes.contains(Raw.toBase64())); }
    const auto Disk = Store.Read(Result.record_.plan.id); QVERIFY(Disk); QCOMPARE(Disk.value.plan.Hash(), Result.record_.plan.Hash());
    QCOMPARE(Storage.Commit(Storage.Undo(Disk.value)).applied_.error, quint32(0));
}
void EnvironmentStorageTests::derivedProtectedHistory()
{
    FakeRegistryBackend Backend; const auto Location = EnvironmentLocation(Scope::User, Caller); Backend.Seed(Location);
    Backend.Write(Location, EncodeEnvironmentValue(Item(L"Bridge", L"%SYNTHETIC_TOKEN%")));
    const auto Before = Item(L"Ordinary", L"%Bridge%"); Backend.Write(Location, EncodeEnvironmentValue(Before));
    QTemporaryDir Directory; RegistryChangeStore Store(Directory.path()); EnvironmentStorage Storage(Backend, Store, Caller, Machine);
    const auto Result = Storage.Commit(Storage.Preview({{Scope::User, "Ordinary", Before, Item(L"Ordinary", L"synthetic-expanded")}}));
    QCOMPARE(Result.applied_.error, quint32(0)); QVERIFY(Result.record_.plan.steps.front().sensitive);
    QVERIFY(!QFile::exists(Result.record_.directory + "/after.reg"));
    QVERIFY(QFile::exists(Result.record_.directory + "/after.reg.dpapi"));
    RegistryHistoryModel Model; Model.SetRecords({Result.record_});
    QVERIFY(!Model.index(0, RegistryHistoryModel::Change).data().toString().contains("synthetic-expanded"));
    const auto Disk = Store.Read(Result.record_.plan.id); QVERIFY(Disk);
    QCOMPARE(Disk.value.plan.Hash(), Result.record_.plan.Hash()); QVERIFY(Disk.value.plan.steps.front().sensitive);
    QCOMPARE(Storage.Commit(Storage.Undo(Disk.value)).applied_.error, quint32(0));
}
void EnvironmentStorageTests::missingUserKeyAndUndo()
{
    FakeRegistryBackend Backend; const auto Location = EnvironmentLocation(Scope::User, Caller); Backend.Seed(*Location.Parent());
    QTemporaryDir Directory; RegistryChangeStore Store(Directory.path()); EnvironmentStorage Storage(Backend, Store, Caller, Machine);
    QVERIFY(Storage.ReadScope(Scope::User)); QVERIFY(Storage.ReadScope(Scope::User).value.empty());
    const auto Preview = Storage.Preview({{Scope::User, "SyntheticOne", {}, Item(L"SyntheticOne", L"one")},
        {Scope::User, "SyntheticTwo", {}, Item(L"SyntheticTwo", L"two")}});
    QVERIFY(Preview); QCOMPARE(Preview.plan_.steps.size(), 3); QCOMPARE(Preview.plan_.steps.front().kind, StepKind::Key);
    const auto Applied = Storage.Commit(Preview); QCOMPARE(Applied.applied_.error, quint32(0));
    const auto Undo = Storage.Commit(Storage.Undo(Applied.record_)); QCOMPARE(Undo.applied_.error, quint32(0));
    QVERIFY(!Backend.Enumerate(Location)); QVERIFY(Backend.Enumerate(*Location.Parent()));
    // A failed value write removes only the key and values this transaction made.
    Backend.failAt_ = Backend.mutations_ + 3;
    const auto Failed = Storage.Commit(Storage.Preview({{Scope::User, "SyntheticOne", {}, Item(L"SyntheticOne", L"one")},
        {Scope::User, "SyntheticTwo", {}, Item(L"SyntheticTwo", L"two")}}));
    QCOMPARE(Failed.applied_.error, quint32(ERROR_ACCESS_DENIED)); QVERIFY(Failed.restored_);
    QVERIFY(!Backend.Enumerate(Location)); QVERIFY(Backend.Enumerate(*Location.Parent()));
}
void EnvironmentStorageTests::systemRequiresHelper()
{
    FakeRegistryBackend Backend; Backend.Seed(EnvironmentLocation(Scope::System));
    QTemporaryDir Directory; RegistryChangeStore Store(Directory.path()); EnvironmentStorage Storage(Backend, Store, Caller, Machine);
    const auto Result = Storage.Commit(Storage.Preview({{Scope::System, "Demo", {}, Item(L"Demo", L"synthetic", Scope::System)}}));
    QCOMPARE(Result.applied_.error, quint32(ERROR_ELEVATION_REQUIRED)); QCOMPARE(Backend.mutations_, 0);
}
void EnvironmentStorageTests::mixedScopeUsesCaller()
{
    FakeRegistryBackend Local, Elevated; const auto User = EnvironmentLocation(Scope::User, Caller), System = EnvironmentLocation(Scope::System);
    Local.Seed(User); Local.Seed(System); Elevated.Seed(System);
    const auto Original = Item(L"Demo", L"synthetic"); Local.Write(User, EncodeEnvironmentValue(Original));
    RegistrySessionServer Server(Elevated, Caller, Machine);
    RegistryRemoteSession Remote([&](const QString& Kind, QJsonObject Request) {
        // Only the helper backend has HKLM; an accidental HKCU send would fail.
        return Server.Handle(Kind, Request);
    });
    QTemporaryDir Directory; RegistryChangeStore Store(Directory.path()); EnvironmentStorage Storage(Local, Store, Caller, Machine, &Remote);
    const auto Preview = Storage.Preview({{Scope::System, "Demo", {}, Item(L"Demo", L"synthetic", Scope::System)}, {Scope::User, "Demo", Original, {}}}); QVERIFY(Preview);
    const auto Result = Storage.Commit(Preview); QCOMPARE(Result.applied_.error, quint32(0)); QCOMPARE(Result.applied_.completed, 2);
    QVERIFY(Elevated.Read(System, "Demo")); QVERIFY(!Local.Read(User, "Demo")); QVERIFY(!Local.Read(System, "Demo"));
}
void EnvironmentStorageTests::profileLifecycle()
{
    FakeRegistryBackend Backend; const auto Location = EnvironmentLocation(Scope::User, Caller); Backend.Seed(Location);
    const auto Original = Item(L"Demo", L"before"); Backend.Write(Location, EncodeEnvironmentValue(Original));
    QTemporaryDir Directory; RegistryChangeStore Store(Directory.filePath("history")); EnvironmentStorage Storage(Backend, Store, Caller, Machine);
    EnvironmentProfiles Profiles(Directory.filePath("profiles.json")); QCOMPARE(Profiles.Load(Store), quint32(0));
    EnvironmentProfile Profile; Profile.id_ = "fdd29751-a9bf-4775-a89b-f7654027f812"; Profile.name_ = "Synthetic"; Profile.values_ = {Item(L"Demo", L"during")};
    QCOMPARE(Profiles.Put(Profile), quint32(0)); const auto Preview = Profiles.PreviewToggle(Profile.id_, Storage, Store); QVERIFY(Preview);
    QCOMPARE(Profiles.Toggle(Profile.id_, Preview, Storage).applied_.error, quint32(0)); QVERIFY(Profiles.Items()[0].active_);
    QCOMPARE(Profiles.Put(Profile), quint32(ERROR_BUSY)); QCOMPARE(Profiles.Remove(Profile.id_), quint32(ERROR_BUSY));
    const auto Undo = Profiles.PreviewToggle(Profile.id_, Storage, Store); QVERIFY(Undo);
    QCOMPARE(Profiles.Toggle(Profile.id_, Undo, Storage).applied_.error, quint32(0)); QVERIFY(!Profiles.Items()[0].active_);
    QCOMPARE(DecodeEnvironmentValue(Backend.Read(Location, "Demo").value, Scope::User).value, Original);
}
void EnvironmentStorageTests::activeProfileUpdate()
{
    FakeRegistryBackend Backend; const auto Location = EnvironmentLocation(Scope::User, Caller); Backend.Seed(Location);
    const auto Original = Item(L"Demo", L"original"); Backend.Write(Location, EncodeEnvironmentValue(Original));
    QTemporaryDir Directory; RegistryChangeStore Store(Directory.filePath("history")); EnvironmentStorage Storage(Backend, Store, Caller, Machine);
    const auto Path = Directory.filePath("profiles.json"); EnvironmentProfiles Profiles(Path);
    EnvironmentProfile Profile; Profile.id_ = "fdd29751-a9bf-4775-a89b-f7654027f812"; Profile.name_ = "Synthetic";
    Profile.values_ = {Item(L"Demo", L"first")}; QCOMPARE(Profiles.Put(Profile), quint32(0));
    const auto Enable = Profiles.PreviewToggle(Profile.id_, Storage, Store); QVERIFY(Enable);
    QCOMPARE(Profiles.Toggle(Profile.id_, Enable, Storage).applied_.error, quint32(0));
    auto Next = Profiles.Items().front(); Next.name_ = "Updated"; Next.values_ = {Item(L"Demo", L"second"), Item(L"Added", L"new")};
    const auto Update = Profiles.PreviewUpdate(Next, Storage); QVERIFY(Update);
    QCOMPARE(Profiles.Toggle(Next.id_, Update, Storage).applied_.error, quint32(0));
    EnvironmentProfiles Reopened(Path); QCOMPARE(Reopened.Load(Store), quint32(0));
    QCOMPARE(Reopened.Items().front().name_, QString("Updated")); QVERIFY(Reopened.Items().front().active_);
    QCOMPARE(Reopened.Items().front().baseline_.at(L"Demo").value(), Original);
    QVERIFY(!Reopened.Items().front().baseline_.at(L"Added"));
    Next = Reopened.Items().front(); Next.values_ = {Item(L"Added", L"third")};
    const auto Remove = Reopened.PreviewUpdate(Next, Storage); QVERIFY(Remove);
    QCOMPARE(Reopened.Toggle(Next.id_, Remove, Storage).applied_.error, quint32(0));
    QCOMPARE(DecodeEnvironmentValue(Backend.Read(Location, "Demo").value, Scope::User).value, Original);
    const auto Disable = Reopened.PreviewToggle(Next.id_, Storage, Store); QVERIFY(Disable);
    QCOMPARE(Reopened.Toggle(Next.id_, Disable, Storage).applied_.error, quint32(0));
    QVERIFY(!Backend.Read(Location, "Added")); QVERIFY(!Reopened.Items().front().active_);
    // A preview never authorizes an intervening registry edit.
    const auto Again = Reopened.PreviewToggle(Next.id_, Storage, Store); QVERIFY(Again);
    Backend.Write(Location, EncodeEnvironmentValue(Item(L"Added", L"outside")));
    QVERIFY(Reopened.Toggle(Next.id_, Again, Storage).applied_.conflict);
    QVERIFY(Reopened.Items().front().pendingId_.isEmpty()); QVERIFY(!Reopened.Items().front().active_);
}
void EnvironmentStorageTests::profileConflictAndPersistence()
{
    FakeRegistryBackend Backend; const auto Location = EnvironmentLocation(Scope::User, Caller); Backend.Seed(Location);
    QTemporaryDir Directory; RegistryChangeStore Store(Directory.filePath("history")); EnvironmentStorage Storage(Backend, Store, Caller, Machine);
    const auto Path = Directory.filePath("profiles.json"); EnvironmentProfiles Profiles(Path);
    EnvironmentProfile First; First.id_ = "fdd29751-a9bf-4775-a89b-f7654027f812"; First.name_ = "One"; First.values_ = {Item(L"Demo", L"during")};
    auto Second = First; Second.id_ = "80dfc810-6cd3-4acc-9985-d4864bbecf85"; Second.name_ = "Two"; Second.values_[0].name_ = L"DEMO";
    QCOMPARE(Profiles.Put(First), quint32(0)); QCOMPARE(Profiles.Put(Second), quint32(0));
    const auto Preview = Profiles.PreviewToggle(First.id_, Storage, Store); QCOMPARE(Profiles.Toggle(First.id_, Preview, Storage).applied_.error, quint32(0));
    QCOMPARE(Profiles.PreviewToggle(Second.id_, Storage, Store).error_, quint32(ERROR_DUP_NAME));
    EnvironmentProfiles Reopened(Path); QCOMPARE(Reopened.Load(Store), quint32(0)); QVERIFY(Reopened.Items()[0].active_);
    QVERIFY(!ReadFile(Path).contains("during"));
    Backend.Write(Location, EncodeEnvironmentValue(Item(L"Demo", L"outside")));
    const auto Undo = Reopened.PreviewToggle(First.id_, Storage, Store); QCOMPARE(Undo.error_, quint32(ERROR_REVISION_MISMATCH));
    QVERIFY(Reopened.Items()[0].active_);
}
void EnvironmentStorageTests::runtimeOverlayAndDelete()
{
    ValueMap Inherited{{L"SESSION_ONLY", L"keep"}}, Last{{L"Persistent", L"old"}}, Next{{L"New", L"fresh"}}, Current=Last;
    Current[L"SESSION_ONLY"] = L"keep";
    auto Result = ReconcileEnvironment(Inherited, Last, Next, Current, {{L"COMMANDER_PATH", L"C:\\SyntheticHC"}});
    QVERIFY(!Result.contains(L"Persistent")); QCOMPARE(Result.at(L"SESSION_ONLY"), std::wstring(L"keep"));
    QCOMPARE(Result.at(L"COMMANDER_PATH"), std::wstring(L"C:\\SyntheticHC"));
    const auto Again = ReconcileEnvironment(Inherited, Next, Next, Result, {}); QVERIFY(!Again.contains(L"Persistent"));
    Current[L"Persistent"] = L"plugin"; Result = ReconcileEnvironment(Inherited, Last, Next, Current, {});
    QCOMPARE(Result.at(L"Persistent"), std::wstring(L"plugin"));
}
void EnvironmentStorageTests::environmentScopeIsolation()
{
    using namespace hc::ui;
    QCOMPARE(ScopeKey(InputScope::SystemEnvironmentView), QString("systemEnvironmentView"));
    QCOMPARE(ScopeFromKey(ScopeKey(InputScope::SystemEnvironmentView)), InputScope::SystemEnvironmentView);
    int Count = 0;
    for (const auto& Command : hc::defaults::Commands()) if (QString(Command.Name).startsWith("hc.systemEnvironment.")) {
        ++Count; QVERIFY(ScopeAccepts(InputScope::SystemEnvironmentView, Command.Name));
        QVERIFY(!ScopeAccepts(InputScope::RegistryView, Command.Name)); QVERIFY(!ScopeAccepts(InputScope::Global, Command.Name));
        QCOMPARE(HomeScope(Command.Name), InputScope::SystemEnvironmentView);
    }
    QCOMPARE(Count, 16);
    const auto Environment = DefaultScopeShortcuts(InputScope::SystemEnvironmentView), Registry = DefaultScopeShortcuts(InputScope::RegistryView);
    QCOMPARE(CommandId(Environment.value(QKeyCombination(Qt::AltModifier, Qt::Key_F10))), QString("hc.systemEnvironment.scopeTree"));
    QCOMPARE(CommandId(Registry.value(QKeyCombination(Qt::AltModifier, Qt::Key_F10))), QString("hc.registry.tree"));
    QCOMPARE(FindCommand("hc.navigate.systemEnvironment").Default, QKeyCombination(Qt::ControlModifier | Qt::AltModifier, Qt::Key_E));
    QCOMPARE(FindCommand("hc.navigate.systemRegistry").Default, QKeyCombination(Qt::ControlModifier | Qt::AltModifier, Qt::Key_G));
}
void EnvironmentStorageTests::environmentProtectedKeys()
{
    using namespace hc::ui;
    QJsonObject Overrides;
    for (const auto Key : hc::defaults::ProtectedScopeKeys()) {
        const auto Text = QKeySequence(Key).toString(QKeySequence::PortableText);
        Overrides[ScopeKey(InputScope::SystemEnvironmentView)] = QJsonObject{{Text, "hc.systemEnvironment.edit"}};
        QVERIFY(!EffectiveScopeShortcuts(Overrides, InputScope::SystemEnvironmentView).contains(Key));
    }
    for (const auto* Id : {"hc.search.focus", "hc.viewer.quick", "hc.navigate.back", "hc.navigate.forward", "hc.navigate.systemEnvironment"}) QVERIFY(IsSystemViewGlobalCommand(Id));
}
void EnvironmentStorageTests::environmentFunctionBar()
{
    using namespace hc::ui;
    QCOMPARE(FunctionBarKey(FunctionBarKind::SystemEnvironmentView), ScopeKey(InputScope::SystemEnvironmentView));
    const auto Commands = FunctionCommandsFor({}, FunctionBarKind::SystemEnvironmentView); QCOMPARE(Commands.size(), 9);
    QCOMPARE(Commands, hc::defaults::EnvironmentFunctionCommands());
    QJsonObject General{{"functionBars", QJsonObject{{FunctionBarKey(FunctionBarKind::SystemEnvironmentView), QJsonObject{{"commands", QJsonArray::fromStringList(Commands)}}}}}};
    QCOMPARE(FunctionCommandsFor(General, FunctionBarKind::SystemEnvironmentView), Commands);
}
QTEST_GUILESS_MAIN(EnvironmentStorageTests)

void EnvironmentStorageTests::transferValidationAndRedaction()
{
    const std::vector<hc::environment::Value> Values{
        Item(L"DEMO_TOKEN", L"invented-secret"), Item(L"Alias", L"%DEMO_TOKEN%"),
        Item(L"Indirect", L"%Alias%"), Item(L"Visible", L"synthetic")};
    const auto Export = ExportEnvironmentFile(Values, EnvironmentFormat::Json); QVERIFY(Export);
    QVERIFY(!Export.value.contains("invented-secret")); QVERIFY(!Export.value.contains("%Alias%"));
    const auto Import = ParseEnvironmentFile(Export.value, EnvironmentFormat::Json); QVERIFY(Import);
    QCOMPARE(Import.values_.size(), 1); QCOMPARE(Import.values_[0].name_, QString("Visible"));
    QVERIFY(!ParseEnvironmentFile("A=1\na=2", EnvironmentFormat::DotEnv));
    QVERIFY(!ParseEnvironmentFile("A=\"unfinished", EnvironmentFormat::DotEnv));
    const auto Literal = ParseEnvironmentFile("A=\"$(synthetic) %NOT_EXPANDED%\"", EnvironmentFormat::DotEnv);
    QVERIFY(Literal); QCOMPARE(Literal.values_[0].value_->raw_, std::wstring(L"$(synthetic) %NOT_EXPANDED%"));
    const auto Reg = ExportEnvironmentFile({Item(L"Visible", L"synthetic")}, EnvironmentFormat::Reg);
    QVERIFY(Reg); QVERIFY(ParseEnvironmentFile(Reg.value, EnvironmentFormat::Reg));
}
void EnvironmentStorageTests::modelMasksDerivedValues()
{
    EnvironmentSnapshot Snapshot;
    Snapshot.input_.system_ = {Item(L"Alias", L"%DEMO_SECRET%", Scope::System)};
    Snapshot.input_.user_ = {Item(L"Alias", L"overridden"), Item(L"DEMO_SECRET", L"invented-secret"),
        Item(L"Indirect", L"%Alias%"), Item(L"path", L"C:\\Synthetic;D:\\Synthetic")};
    Snapshot.merged_ = BuildEnvironment(Snapshot.input_);
    EnvironmentModel Model;
    Model.SetSnapshot(Snapshot, EnvironmentViewScope::System);
    QCOMPARE(Model.rowCount(), 1); QVERIFY(Model.index(0, 1).data(EnvironmentModel::MaskedRole).toBool());
    QVERIFY(!Model.index(0, 1).data().toString().contains("DEMO_SECRET"));
    Model.SetSnapshot(Snapshot, EnvironmentViewScope::User);
    QCOMPARE(Model.index(0, 0).data().toString(), QString("path"));
    QCOMPARE(Model.rowCount(Model.index(0, 0)), 2);
    QCOMPARE(Model.parent(Model.index(1, 0, Model.index(0, 0))), Model.index(0, 0));
    for (int Row = 0; Row < Model.rowCount(); ++Row) for (int Column = 0; Column < 4; ++Column)
        for (int Role : {int(Qt::DisplayRole), int(Qt::EditRole), int(Qt::ToolTipRole), int(EnvironmentModel::GridSortRole)})
            QVERIFY(!Model.index(Row, Column).data(Role).toString().contains("invented-secret"));
    Model.SetMaskExceptions({"demo_secret"}); Model.SetSnapshot(Snapshot, EnvironmentViewScope::User);
    const auto Secret = Model.Find(QString::number(int(EnvironmentViewScope::User)) + "/demo_secret");
    QVERIFY(Secret.isValid()); QVERIFY(!Secret.data(EnvironmentModel::MaskedRole).toBool());
    QCOMPARE(Secret.siblingAtColumn(1).data().toString(), QString("invented-secret"));
    Model.SetMaskExceptions({}); Model.SetSnapshot(Snapshot, EnvironmentViewScope::User);
    QVERIFY(Model.Find(QString::number(int(EnvironmentViewScope::User)) + "/demo_secret").data(EnvironmentModel::MaskedRole).toBool());
}
void EnvironmentStorageTests::sessionRefreshAndDeletion()
{
    auto Backend = std::make_shared<FakeRegistryBackend>();
    const auto User = EnvironmentLocation(Scope::User, Caller);
    Backend->Seed(User); Backend->Seed(EnvironmentLocation(Scope::System));
    Backend->Write(User, EncodeEnvironmentValue(Item(L"Synthetic", L"before")));
    EnvironmentSession Session(Backend, {}, Caller);
    QVERIFY(!Session.Ready()); Session.Refresh();
    QTRY_VERIFY_WITH_TIMEOUT(Session.Ready() && !Session.Pending(), 5000);
    QCOMPARE(Session.Snapshot().merged_.values_.at(L"Synthetic").value_, std::wstring(L"before"));
    const auto Revision = Session.Revision();
    QCOMPARE(Backend->DeleteValue(User, "Synthetic"), quint32(0));
    Session.Refresh(); Session.Refresh();
    QTRY_VERIFY_WITH_TIMEOUT(Session.Revision() > Revision && !Session.Pending(), 5000);
    QVERIFY(!Session.Snapshot().merged_.values_.contains(L"Synthetic"));
    QCOMPARE(Session.Snapshot().userError_, quint32(0));
    QVERIFY_EXCEPTION_THROWN(EnvironmentSession(nullptr, {}, Caller), std::invalid_argument);
}