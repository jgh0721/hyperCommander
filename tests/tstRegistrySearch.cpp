#include "stdafx.h"
#include "tstRegistrySearch.hpp"

/// 모듈
/// UIs
/// 기타 헤더파일
#include "registry/fakeRegistryBackend.hpp"
/// 고유 라이브러리
#include "UniqueLibs/registry/registrySearch.hpp"
/// 공통 라이브러리
#include "commonLibs/system/registryValue.hpp"
/// 외부 라이브러리
/// 선언 헤더파일들
/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QTest>
#include <QTemporaryDir>
#include "UniqueLibs/registry/registryBatch.hpp"
#include "UniqueLibs/registry/registryImport.hpp"
#include "UniqueLibs/registry/registryCompareLease.hpp"
#include <QJsonDocument>
#include <QFile>
#include <QAbstractItemModelTester>
#include <QSignalSpy>

DECLARE_CMNLIBSV2_NAMESPACE
Q_LOGGING_CATEGORY(lcRegistrySearchTests, "hc.tests.registrySearch")
using namespace hc::registry;
namespace {
QList<RegistryResultRow> Finish(RegistrySearchCursor& Cursor, FakeRegistryBackend& Backend)
{
    QList<RegistryResultRow> Rows;
    for (int I = 0; I < 100; ++I) {
        const auto Batch = Cursor.Advance(Backend, nullptr, 1, 1000);
        Rows.append(Batch.rows);
        if (Batch.done) break;
    }
    return Rows;
}
}
void RegistrySearchTests::rootAndResume()
{
    FakeRegistryBackend Backend;
    const RegistryLocation Root{Hive::CurrentUser, "Software\\HyperCommanderTests\\Needle"};
    Backend.Seed(Root); Backend.Seed(Root.Child("child"));
    Backend.Write(Root, {"a", REG_SZ, RegistryValueCodec::EncodeText(REG_SZ, "needle").value});
    Backend.Write(Root.Child("child"), {"needle", REG_BINARY, "data"});
    const int Writes = Backend.mutations_;
    RegistrySearchOptions Options; Options.roots = {Root}; Options.text = "needle";
    RegistrySearchCursor Cursor(Options);
    auto Batch = Cursor.Advance(Backend, nullptr, 1, 1000);
    QCOMPARE(Batch.rows.size(), 1); QVERIFY(!Batch.rows[0].value); QVERIFY(!Batch.done);
    std::atomic_bool Cancel{true};
    Batch = Cursor.Advance(Backend, &Cancel); QVERIFY(Batch.rows.isEmpty()); QVERIFY(!Batch.done);
    const auto Rows = Finish(Cursor, Backend); QCOMPARE(Rows.size(), 2);
    QVERIFY(Rows[0].value); QVERIFY(Rows[1].value); QVERIFY(Rows[0].id != Rows[1].id);
    QCOMPARE(Backend.mutations_, Writes);
}
void RegistrySearchTests::typeTimeAndRegex()
{
    FakeRegistryBackend Backend;
    const RegistryLocation Root{Hive::CurrentUser, "Software\\HyperCommanderTests"};
    Backend.Seed(Root); Backend.records_[Root.Id()].written = 100;
    Backend.Write(Root, {"text", REG_SZ, RegistryValueCodec::EncodeText(REG_SZ, "ABC123").value});
    Backend.Write(Root, {"number", REG_DWORD, RegistryValueCodec::EncodeNumber(REG_DWORD, 123).value});
    RegistrySearchOptions Options; Options.roots = {Root}; Options.text = "^abc[0-9]+$"; Options.regex = true;
    RegistrySearchCursor Regex(Options); QCOMPARE(Finish(Regex, Backend).size(), 1);
    Options.text = "("; RegistrySearchCursor Invalid(Options); QVERIFY(!Invalid.Error().isEmpty());
    Options.regex = false; Options.text = "123"; Options.type = RegistrySearchOptions::Type::Number;
    RegistrySearchCursor Number(Options); const auto Rows = Finish(Number, Backend);
    QCOMPARE(Rows.size(), 1); QCOMPARE(Rows[0].value->type, quint32(REG_DWORD));
    Options.since = 101; RegistrySearchCursor Newer(Options); QVERIFY(Finish(Newer, Backend).isEmpty());
    Options.text.clear(); Options.since = 100; RegistrySearchCursor Time(Options);
    const auto Timed = Finish(Time, Backend); QCOMPARE(Timed.size(), 1); QVERIFY(!Timed[0].value);
}
void RegistrySearchTests::incrementalModel()
{
    RegistryResultModel Model({"Name"});
    QAbstractItemModelTester Tester(&Model, QAbstractItemModelTester::FailureReportingMode::QtTest);
    QSignalSpy Resets(&Model, &QAbstractItemModel::modelReset);
    RegistryResultRow First; First.id = "first"; First.groups = {"group"}; First.cells = {"first"};
    Model.AppendRows({First});
    QPersistentModelIndex Index(Model.index(0, 0, Model.index(0, 0)));
    QVERIFY(Index.isValid()); QCOMPARE(Index.data().toString(), QString("first"));
    auto Second = First; Second.id = "second"; Second.cells = {"second"};
    Model.AppendRows({Second, First}); QCOMPARE(Model.rowCount(Model.index(0, 0)), 2);
    QCOMPARE(Index.data().toString(), QString("first")); QCOMPARE(Resets.count(), 0);
}
void RegistrySearchTests::mergedOrigins()
{
    FakeRegistryBackend Backend;
    const RegistryLocation Merged{Hive::ClassesRoot, "HyperCommanderTest"};
    const RegistryLocation User{Hive::CurrentUser, "Software\\Classes\\HyperCommanderTest"};
    const RegistryLocation Machine{Hive::LocalMachine, "Software\\Classes\\HyperCommanderTest"};
    Backend.Seed(*User.Parent()); Backend.Seed(*Machine.Parent());
    for (const auto& Key : {Merged, User, Machine}) Backend.Seed(Key);
    const RegistryValue UserValue{"name", REG_SZ, RegistryValueCodec::EncodeText(REG_SZ, "needle user").value};
    const RegistryValue MachineValue{"name", REG_SZ, RegistryValueCodec::EncodeText(REG_SZ, "needle machine").value};
    Backend.Write(Merged, UserValue); Backend.Write(User, UserValue); Backend.Write(Machine, MachineValue);
    RegistrySearchOptions Options; Options.roots = {Machine, User, Merged}; Options.text = "needle"; Options.keys = false;
    RegistrySearchCursor Cursor(Options); const auto Rows = Finish(Cursor, Backend);
    QCOMPARE(Rows.size(), 2);
    QCOMPARE(Rows[0].location->Id(), User.Id()); QCOMPARE(Rows[1].location->Id(), Machine.Id());
    QCOMPARE(Rows[0].value->bytes, UserValue.bytes); QCOMPARE(Rows[1].value->bytes, MachineValue.bytes);
}
void RegistrySearchTests::replacementConflictAndUndo()
{
    FakeRegistryBackend Backend; const RegistryLocation Root{Hive::CurrentUser, "Software\\HyperCommanderTests", View::Native64, "S-1-5-21-123"};
    Backend.Seed(Root);
    const RegistryValue Before{"v", REG_EXPAND_SZ, RegistryValueCodec::EncodeText(REG_EXPAND_SZ, "%PATH% needle").value};
    Backend.Write(Root, Before);
    RegistryResultRow Row; Row.id = "v"; Row.location = Root; Row.value = Before;
    RegistrySearchOptions Options; Options.text = "needle"; Options.keys = false;
    auto Preview = RegistryBatch::Replace({Row}, Options, "changed", "S-1-5-21-123", "machine");
    QVERIFY(Preview); QCOMPARE(Preview.plan.steps.size(), 1); QCOMPARE(Preview.plan.steps[0].after->type, quint32(REG_EXPAND_SZ));
    QTemporaryDir Directory; RegistryChangeStore Store(Directory.path()); RegistryMutation Mutation(Backend, Store, "S-1-5-21-123", "machine");
    auto External = Before; External.bytes = RegistryValueCodec::EncodeText(REG_EXPAND_SZ, "external").value; Backend.Write(Root, External);
    const int Writes = Backend.mutations_; const auto Conflict = Mutation.ApplyPreview(Preview.operations, Preview.plan);
    QVERIFY(Conflict.applied.conflict); QCOMPARE(Backend.mutations_, Writes); QVERIFY(Store.History().value.isEmpty());
    Backend.Write(Root, Before); const auto Applied = Mutation.ApplyPreview(Preview.operations, Preview.plan);
    QCOMPARE(Applied.applied.error, quint32(0)); QCOMPARE(Store.History().value.size(), 1);
    QCOMPARE(Backend.Read(Root, "v").value.bytes, Preview.plan.steps[0].after->bytes);
    const auto Undo = Mutation.Undo(Applied.record); QCOMPARE(Undo.applied.error, quint32(0)); QCOMPARE(Backend.Read(Root, "v").value, Before);
    Row.value->type = REG_BINARY; Preview = RegistryBatch::Replace({Row}, Options, "changed", "S-1-5-21-123", "machine"); QVERIFY(Preview.rows.isEmpty());
}
void RegistrySearchTests::orderedImportPreview()
{
    FakeRegistryBackend Backend; const RegistryLocation Root{Hive::CurrentUser, "Software\\HyperCommanderTests", View::Native64, "S-1-5-21-123"};
    Backend.Seed(Root);
    const RegistryValue One{"x", REG_BINARY, "one"}, Two{"x", REG_BINARY, "two"};
    Backend.Write(Root, One);
    const QList<RegistryOperation> Operations{{OperationKind::SetValue, Root, One}, {OperationKind::DeleteValue, Root, One}, {OperationKind::SetValue, Root, Two}};
    const int Writes = Backend.mutations_;
    const auto Preview = RegistryImport::Prepare(Backend, Operations, {}, Root.sid, "machine");
    QVERIFY(Preview); QCOMPARE(Preview.rows.size(), 3); QCOMPARE(Preview.plan.steps.size(), 2);
    QCOMPARE(Preview.plan.steps[0].before, std::optional<RegistryValue>(One)); QVERIFY(!Preview.plan.steps[0].after);
    QVERIFY(!Preview.plan.steps[1].before); QCOMPARE(Preview.plan.steps[1].after, std::optional<RegistryValue>(Two));
    QCOMPARE(Backend.mutations_, Writes);
    const auto Excluded = RegistryImport::Prepare(Backend, Operations, {1}, Root.sid, "machine");
    QVERIFY(Excluded); QCOMPARE(Excluded.plan.steps.size(), 1); QCOMPARE(Excluded.plan.steps[0].before, std::optional<RegistryValue>(One));
    QTemporaryDir Directory; RegistryChangeStore Store(Directory.filePath("registry-backup"));
    RegistryMutation Mutation(Backend, Store, Root.sid, "machine");
    const auto Applied = Mutation.ApplyPreview(Preview.operations, Preview.plan); QCOMPARE(Applied.applied.error, quint32(0));
    QCOMPARE(Backend.Read(Root, "x").value, Two); QCOMPARE(Mutation.Undo(Applied.record).applied.error, quint32(0)); QCOMPARE(Backend.Read(Root, "x").value, One);
}
void RegistrySearchTests::compareLeaseOwnership()
{
    QTemporaryDir Root; QString Path;
    {
        RegistryCompareLease Lease(Root.path()); QVERIFY(Lease.IsValid()); Path = QFileInfo(Lease.FilePath("left.reg")).absolutePath();
        QFile Data(Lease.FilePath("left.reg")); QVERIFY(Data.open(QIODevice::WriteOnly)); Data.write("fixture"); Data.close();
        QVERIFY(Lease.Starting()); QVERIFY(Lease.SetProcess(QCoreApplication::applicationPid()));
    }
    QVERIFY(QDir(Path).exists()); QCOMPARE(RegistryCompareLease::Sweep(Root.path()), 0);
    QFile Marker(QDir(Path).filePath("owner.json")); QVERIFY(Marker.open(QIODevice::ReadOnly));
    auto Object = QJsonDocument::fromJson(Marker.readAll()).object(); Marker.close();
    Object["ownerPid"] = "4294967294"; Object["ownerStamp"] = "1";
    QVERIFY(Marker.open(QIODevice::WriteOnly | QIODevice::Truncate)); Marker.write(QJsonDocument(Object).toJson()); Marker.close();
    QCOMPARE(RegistryCompareLease::Sweep(Root.path()), 0); // Consumer is still this live test process.
    Object["consumerPid"] = "4294967294"; Object["consumerStamp"] = "1"; Object["launchPending"] = true;
    QVERIFY(Marker.open(QIODevice::WriteOnly | QIODevice::Truncate)); Marker.write(QJsonDocument(Object).toJson()); Marker.close();
    QCOMPARE(RegistryCompareLease::Sweep(Root.path()), 0); // Unknown launch outcome must be retained.
    Object["launchPending"] = false;
    QVERIFY(Marker.open(QIODevice::WriteOnly | QIODevice::Truncate)); Marker.write(QJsonDocument(Object).toJson()); Marker.close();
    QCOMPARE(RegistryCompareLease::Sweep(Root.path()), 1); QVERIFY(!QDir(Path).exists());
    {
        RegistryCompareLease Lease(Root.path()); QVERIFY(Lease.IsValid()); Path = QFileInfo(Lease.FilePath("right.reg")).absolutePath();
        QVERIFY(Lease.Starting()); Lease.CancelStart();
    }
    QVERIFY(!QDir(Path).exists());
}
void RegistrySearchTests::exportAndNormalize()
{
    FakeRegistryBackend Backend; const RegistryLocation Root{Hive::CurrentUser, "Software\\HyperCommanderTests"};
    Backend.Seed(Root); const auto Child = Root.Child("child"); Backend.Seed(Child);
    const RegistryValue Before{"name", REG_SZ, RegistryValueCodec::EncodeText(REG_SZ, QString(5000, 'x') + ",\"\nend").value};
    auto After = Before; After.bytes = RegistryValueCodec::EncodeText(REG_SZ, "changed").value;
    Backend.Write(Root, Before); Backend.Write(Child, After);
    const auto Exported = RegistryBatch::Export(Backend, {Root, Child}); QVERIFY(Exported); QCOMPARE(Exported.value.size(), 4);
    std::atomic_bool Cancel{true}; QCOMPARE(RegistryBatch::Export(Backend, {Root}, {}, &Cancel).error, quint32(ERROR_CANCELLED));
    RegistryResultRow Row; Row.id = "one"; Row.location = Root; Row.value = Before;
    const auto Csv = RegistryBatch::Csv({Row}); QVERIFY(Csv); QVERIFY(Csv.value.contains(QByteArray(5000, 'x'))); QVERIFY(Csv.value.contains(",\"\"\nend"));
    QCOMPARE(RegistryBatch::Csv({Row}, &Cancel).error, quint32(ERROR_CANCELLED));
    const auto Sorted = RegistryBatch::Normalize({{OperationKind::SetValue, Root, Before}, {OperationKind::CreateKey, Root},
        {OperationKind::SetValue, Root, After}, {OperationKind::DeleteValue, Root, After}, {OperationKind::SetValue, Root, Before}});
    QCOMPARE(Sorted.size(), 4); QCOMPARE(Sorted[0].kind, OperationKind::CreateKey);
    QCOMPARE(Sorted[1].value, After); QCOMPARE(Sorted[2].kind, OperationKind::DeleteValue); QCOMPARE(Sorted[3].value, Before);
    RegistryResultModel Model({"name"}); Row.groups = {"group"}; Model.AppendRows({Row});
    QVERIFY(Model.Find("one").isValid()); QCOMPARE(Model.Find("one").data(RegistryTreeModel::IdRole).toString(), QString("one"));
    Model.SetRows({}); QVERIFY(!Model.Find("one").isValid());
}
QTEST_MAIN(RegistrySearchTests)