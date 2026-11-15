#include "stdafx.h"
#include "tstRegistrySession.hpp"

/// 모듈
/// UIs
/// 기타 헤더파일
#include "registry/fakeRegistryBackend.hpp"
/// 고유 라이브러리
#include "UniqueLibs/registry/registryHistory.hpp"
#include "UniqueLibs/registry/registryProtocol.hpp"
#include "UniqueLibs/registry/registryMutation.hpp"
#include "commonLibs/system/registryLocalPolicy.hpp"
#include "UniqueLibs/systemSessionService.hpp"
#include "commonLibs/system/processInfo.hpp"
/// 공통 라이브러리
/// 외부 라이브러리
/// 선언 헤더파일들
/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QFile>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QTest>
#include <QScopeGuard>
#include <QDir>
#include <QUuid>
#include <QSysInfo>
#include <shellapi.h>

DECLARE_CMNLIBSV2_NAMESPACE
Q_LOGGING_CATEGORY(lcRegistrySessionTests, "hc.tests.registrySession")
using namespace hc::registry;
namespace {
const RegistryLocation Root = RegistryLocation::Parse("HKCU").value();
RegistryApplyResult Apply(RegistryChangeStore& Store, RegistryHistoryRecord& Record, RegistryTransaction& Transaction)
{
    return Store.Apply(Record, [&](const RegistryStep& Step, int) { return Transaction.ApplyStep(Step); });
}
}
void RegistrySessionTests::orderedPrepareAndUndo()
{
    FakeRegistryBackend Backend; Backend.Seed(Root); RegistryTransaction Transaction(Backend);
    const auto Key = Root.Child("Software").Child("HyperCommanderTests").Child("Import");
    const QList<RegistryOperation> Operations{{OperationKind::CreateKey, Key},
        {OperationKind::SetValue, Key, {"x", REG_BINARY, "one"}}, {OperationKind::DeleteValue, Key, {"x"}},
        {OperationKind::SetValue, Key, {"x", REG_BINARY, "two"}}};
    const auto Plan = Transaction.Prepare(Operations, "S-1-5-21-1", "fixture-machine");
    QVERIFY(Plan); QCOMPARE(Backend.mutations_, 0); QCOMPARE(Plan.value.steps.size(), 6);
    QTemporaryDir Directory; RegistryChangeStore Store(Directory.filePath("registry-backup"));
    auto Record = Store.SavePrepared(Plan.value); QVERIFY(Record);
    QVERIFY(QFile::exists(Record.value.directory + "/before.reg")); QVERIFY(QFile::exists(Record.value.directory + "/after.reg"));
    QCOMPARE(Apply(Store, Record.value, Transaction).error, quint32(0));
    QCOMPARE(Backend.Read(Key, "x").value.bytes, QByteArray("two"));
    RegistryChangeStore Reopened(Store.Directory()); const auto Disk = Reopened.Read(Plan.value.id); QVERIFY(Disk);
    QCOMPARE(Disk.value.state, "committed"); QCOMPARE(Disk.value.plan.Hash(), Plan.value.Hash());
    const auto Undo = Store.PrepareUndo(Disk.value, Transaction); QVERIFY(Undo);
    auto UndoRecord = Store.SavePrepared(Undo.value); QVERIFY(UndoRecord);
    QCOMPARE(Apply(Store, UndoRecord.value, Transaction).error, quint32(0));
    QCOMPARE(Backend.records_.size(), 1); QVERIFY(Backend.records_.contains(Root.Id()));
}
void RegistrySessionTests::subtreeLinksAndRename()
{
    FakeRegistryBackend Backend; Backend.Seed(Root); const auto Key = Root.Child("branch");
    Backend.Create(Key); Backend.Create(Key.Child("child")); Backend.Write(Key.Child("child"), {"v", REG_SZ, QByteArray::fromHex("4100")});
    Backend.CreateLink(Key.Child("link"), QByteArray::fromHex("41000000"));
    RegistryTransaction Transaction(Backend); QTemporaryDir Directory; RegistryChangeStore Store(Directory.filePath("registry-backup"));
    auto Plan = Transaction.Prepare({{OperationKind::DeleteKey, Key}}, "caller", "machine"); QVERIFY(Plan);
    auto Record = Store.SavePrepared(Plan.value); QVERIFY(Record); QCOMPARE(Apply(Store, Record.value, Transaction).error, quint32(0));
    QCOMPARE(Backend.records_.size(), 1);
    auto Undo = Store.PrepareUndo(Record.value, Transaction); QVERIFY(Undo);
    auto UndoRecord = Store.SavePrepared(Undo.value); QVERIFY(UndoRecord); QCOMPARE(Apply(Store, UndoRecord.value, Transaction).error, quint32(0));
    QVERIFY(Backend.Enumerate(Key.Child("link")).value.info.link);
    QCOMPARE(Backend.Read(Key.Child("child"), "v").value.bytes, QByteArray::fromHex("4100"));
    auto Rename = Transaction.PrepareRename(Key, "renamed", "caller", "machine"); QVERIFY(Rename);
    auto RenameRecord = Store.SavePrepared(Rename.value); QVERIFY(RenameRecord);
    Backend.Write(Key.Child("child"), {"other", REG_BINARY, "external"});
    auto Conflicted = Apply(Store, RenameRecord.value, Transaction); QVERIFY(Conflicted.conflict); QCOMPARE(Conflicted.completed, 0);
    Backend.DeleteValue(Key.Child("child"), "other");
    Rename = Transaction.PrepareRename(Key, "renamed", "caller", "machine"); QVERIFY(Rename);
    RenameRecord = Store.SavePrepared(Rename.value); QVERIFY(RenameRecord); QCOMPARE(Apply(Store, RenameRecord.value, Transaction).error, quint32(0));
    QVERIFY(Backend.Enumerate(Root.Child("renamed").Child("child")));
    QFile Backup(RenameRecord.value.directory + "/before.reg"); QVERIFY(Backup.open(QIODevice::ReadOnly));
    const auto BackupPlan = RegFileParser::Parse(Backup.readAll(), View::Native64); QVERIFY(BackupPlan); QVERIFY(BackupPlan.operations.size() >= 5);
    Undo = Store.PrepareUndo(RenameRecord.value, Transaction); QVERIFY(Undo);
    UndoRecord = Store.SavePrepared(Undo.value); QVERIFY(UndoRecord); QCOMPARE(Apply(Store, UndoRecord.value, Transaction).error, quint32(0));
    QVERIFY(Backend.Enumerate(Key));
}
void RegistrySessionTests::conflictAndBackupFailure()
{
    FakeRegistryBackend Backend; Backend.Seed(Root); Backend.Write(Root, {"v", REG_BINARY, "original"});
    RegistryTransaction Transaction(Backend);
    auto Plan = Transaction.Prepare({{OperationKind::SetValue, Root, {"v", REG_BINARY, "new"}}}, "caller", "machine"); QVERIFY(Plan);
    QTemporaryDir Directory; QFile Block(Directory.filePath("blocked")); QVERIFY(Block.open(QIODevice::WriteOnly)); Block.close();
    RegistryChangeStore BadStore(Block.fileName()); const int Before = Backend.mutations_;
    QVERIFY(!BadStore.SavePrepared(Plan.value)); QCOMPARE(Backend.mutations_, Before);
    RegistryChangeStore Store(Directory.filePath("registry-backup")); auto Record = Store.SavePrepared(Plan.value); QVERIFY(Record);
    Backend.Write(Root, {"v", REG_BINARY, "foreign"});
    const auto Result = Apply(Store, Record.value, Transaction); QVERIFY(Result.conflict); QCOMPARE(Result.completed, 0);
    QCOMPARE(Backend.Read(Root, "v").value.bytes, QByteArray("foreign"));
    QCOMPARE(Store.Read(Plan.value.id).value.state, "conflict");
}
void RegistrySessionTests::uncertainRecoveryAndPartialUndo()
{
    FakeRegistryBackend Backend; Backend.Seed(Root); Backend.Write(Root, {"x", REG_BINARY, "old"}); Backend.Write(Root, {"y", REG_BINARY, "old"});
    RegistryTransaction Transaction(Backend); QTemporaryDir Directory; RegistryChangeStore Store(Directory.filePath("registry-backup"));
    auto Plan = Transaction.Prepare({{OperationKind::SetValue, Root, {"x", REG_BINARY, "new"}}}, "caller", "machine"); QVERIFY(Plan);
    auto Record = Store.SavePrepared(Plan.value); QVERIFY(Record);
    const auto Lost = Store.Apply(Record.value, [&](const RegistryStep& Step, int) {
        const auto Applied = Transaction.ApplyStep(Step);
        return Applied.error ? Applied : RegistryApplyResult{ERROR_BROKEN_PIPE, 0, false, true};
    });
    QVERIFY(Lost.uncertain); QCOMPARE(Record.value.inFlight, 0);
    auto Recovered = Store.Read(Plan.value.id); QVERIFY(Recovered);
    const int Writes = Backend.mutations_; QCOMPARE(Store.Recover(Recovered.value, Transaction), quint32(0));
    QCOMPARE(Recovered.value.state, "committed"); QCOMPARE(Backend.mutations_, Writes);
    Plan = Transaction.Prepare({{OperationKind::SetValue, Root, {"x", REG_BINARY, "later"}}, {OperationKind::SetValue, Root, {"y", REG_BINARY, "later"}}}, "caller", "machine"); QVERIFY(Plan);
    Record = Store.SavePrepared(Plan.value); QVERIFY(Record); Backend.failAt_ = Backend.mutations_ + 2;
    const auto Partial = Apply(Store, Record.value, Transaction); QCOMPARE(Partial.completed, 1); QVERIFY(Partial.uncertain);
    QCOMPARE(Store.Recover(Record.value, Transaction), quint32(0)); QCOMPARE(Record.value.state, "partial");
    const auto Undo = Store.PrepareUndo(Record.value, Transaction); QVERIFY(Undo); QCOMPARE(Undo.value.steps.size(), 1);
    auto UndoRecord = Store.SavePrepared(Undo.value); QVERIFY(UndoRecord); QCOMPARE(Apply(Store, UndoRecord.value, Transaction).error, quint32(0));
    QCOMPARE(Backend.Read(Root, "x").value.bytes, QByteArray("new")); QCOMPARE(Backend.Read(Root, "y").value.bytes, QByteArray("old"));
}
void RegistrySessionTests::serializationRejectsTampering()
{
    auto Value = RegistryValueJson({"v", 0x1234, QByteArray::fromHex("010002")}); QVERIFY(RegistryValueFromJson(Value));
    Value["bytes"] = "@@@"; QVERIFY(!RegistryValueFromJson(Value));
    FakeRegistryBackend Backend; Backend.Seed(Root); RegistryTransaction Transaction(Backend);
    auto Plan = Transaction.Prepare({{OperationKind::SetValue, Root, {"v", REG_BINARY, "new"}}}, "caller", "machine"); QVERIFY(Plan);
    auto Object = Plan.value.ToJson(); Object["id"] = "../foreign"; QVERIFY(!RegistryPlan::FromJson(Object));
    QTemporaryDir Directory; RegistryChangeStore Store(Directory.filePath("registry-backup")); auto Record = Store.SavePrepared(Plan.value); QVERIFY(Record);
    QFile File(Record.value.directory + "/manifest.json"); QVERIFY(File.open(QIODevice::ReadWrite));
    auto Manifest = QJsonDocument::fromJson(File.readAll()).object(); Manifest["planHash"] = "wrong";
    File.resize(0); File.write(QJsonDocument(Manifest).toJson()); File.close();
    QVERIFY(!Store.Read(Plan.value.id));
    const int Writes = Backend.mutations_; QVERIFY(Apply(Store, Record.value, Transaction).error != 0); QCOMPARE(Backend.mutations_, Writes);
}
void RegistrySessionTests::redoAndBranchAfterRestart()
{
    FakeRegistryBackend Backend; Backend.Seed(Root); RegistryTransaction Transaction(Backend);
    QTemporaryDir Directory; RegistryChangeStore Store(Directory.filePath("registry-backup"));
    const auto Plan = Transaction.Prepare({{OperationKind::SetValue, Root, {"v", REG_BINARY, "first"}}}, "caller", "machine"); QVERIFY(Plan);
    auto Record = Store.SavePrepared(Plan.value); QVERIFY(Record); QCOMPARE(Apply(Store, Record.value, Transaction).error, quint32(0));
    auto Undo = Store.PrepareUndo(Record.value, Transaction); QVERIFY(Undo);
    auto UndoRecord = Store.SavePrepared(Undo.value); QVERIFY(UndoRecord); QCOMPARE(Apply(Store, UndoRecord.value, Transaction).error, quint32(0));
    RegistryChangeStore Reopened(Store.Directory());
    auto Original = Reopened.Read(Plan.value.id); QVERIFY(Original); QCOMPARE(Original.value.state, "undone"); QVERIFY(Original.value.redoAllowed);
    const auto Redo = Reopened.PrepareRedo(Original.value, UndoRecord.value, Transaction); QVERIFY(Redo);
    auto RedoRecord = Reopened.SavePrepared(Redo.value); QVERIFY(RedoRecord); QCOMPARE(Apply(Reopened, RedoRecord.value, Transaction).error, quint32(0));
    QCOMPARE(Backend.Read(Root, "v").value.bytes, QByteArray("first")); QCOMPARE(Reopened.Read(Plan.value.id).value.state, "committed");
    Original = Reopened.Read(Plan.value.id); Undo = Reopened.PrepareUndo(Original.value, Transaction); QVERIFY(Undo);
    UndoRecord = Reopened.SavePrepared(Undo.value); QVERIFY(UndoRecord); QCOMPARE(Apply(Reopened, UndoRecord.value, Transaction).error, quint32(0));
    const auto New = Transaction.Prepare({{OperationKind::SetValue, Root, {"v", REG_BINARY, "branch"}}}, "caller", "machine"); QVERIFY(New);
    auto NewRecord = Reopened.SavePrepared(New.value); QVERIFY(NewRecord); QCOMPARE(Apply(Reopened, NewRecord.value, Transaction).error, quint32(0));
    Original = Reopened.Read(Plan.value.id); QVERIFY(Original); QVERIFY(!Original.value.redoAllowed);
    QVERIFY(!Reopened.PrepareRedo(Original.value, UndoRecord.value, Transaction));
    QCOMPARE(Backend.Read(Root, "v").value.bytes, QByteArray("branch"));
}
void RegistrySessionTests::chunkedProtocolAndIdentity()
{
    const QString Sid = "S-1-5-21-123";
    FakeRegistryBackend Backend; auto BoundRoot = Root; BoundRoot.sid = Sid; Backend.Seed(BoundRoot);
    const auto Key = Root.Child("fixture"); auto Bound = Key; Bound.sid = Sid; Backend.Seed(Bound);
    RegistrySessionServer Server(Backend, Sid, "machine", Key.Path());
    qsizetype LargestFrame = 0;
    RegistryRemoteSession Remote([&](const QString& Kind, QJsonObject Request) {
        LargestFrame = std::max(LargestFrame, QJsonDocument(Request).toJson(QJsonDocument::Compact).size());
        return Server.Handle(Kind, Request);
    });
    auto Forged = Key; Forged.sid = "S-1-5-21-999";
    const auto Plan = Remote.Prepare({{OperationKind::SetValue, Forged, {"large", REG_BINARY, QByteArray(80000, '\xf1')}}});
    QVERIFY(Plan); QCOMPARE(Backend.mutations_, 0); QCOMPARE(Plan.value.steps[0].location.sid, Sid);
    QVERIFY(LargestFrame < 60000);
    QCOMPARE(Remote.Read(Root).error, quint32(ERROR_ACCESS_DENIED));
    QCOMPARE(Remote.Read(RegistryLocation::Parse("HKCR\\fixture").value()).error, quint32(ERROR_INVALID_PARAMETER));
    const auto Hash = QString::fromLatin1(Plan.value.Hash());
    const auto Rejected = Server.Handle("registry.apply", {{"operationId", Plan.value.id}, {"index", 0}, {"hash", Hash}});
    QCOMPARE(Rejected["nativeCode"].toString(), QString::number(ERROR_INVALID_PARAMETER)); QCOMPARE(Backend.mutations_, 0);
    QTemporaryDir Directory; RegistryChangeStore Store(Directory.filePath("registry-backup")); auto Record = Store.SavePrepared(Plan.value); QVERIFY(Record);
    const auto Applied = Store.Apply(Record.value, [&](const RegistryStep&, int Index) { return Remote.Apply(Plan.value, Index); });
    QCOMPARE(Applied.error, quint32(0)); QCOMPARE(Backend.Read(Bound, "large").value.bytes.size(), 80000);
    const int Writes = Backend.mutations_; QCOMPARE(Remote.Apply(Plan.value, 0).completed, 1); QCOMPARE(Backend.mutations_, Writes);
    QCOMPARE(Remote.Status(Plan.value.id)["next"].toInt(), 1);
    const auto Read = Remote.Read(Forged); QVERIFY(Read); QCOMPARE(Read.value.values[0].bytes.size(), 80000);
    RegistryTransaction Transaction(Backend); const auto Undo = Store.PrepareUndo(Record.value, Transaction); QVERIFY(Undo);
    const auto RemoteUndo = Remote.PrepareExisting(Undo.value); QVERIFY(RemoteUndo);
    auto UndoRecord = Store.SavePrepared(RemoteUndo.value); QVERIFY(UndoRecord);
    QCOMPARE(Store.Apply(UndoRecord.value, [&](const RegistryStep&, int Index) { return Remote.Apply(RemoteUndo.value, Index); }).error, quint32(0));
    QCOMPARE(Backend.Read(Bound, "large").error, quint32(ERROR_FILE_NOT_FOUND));
    Remote.Release(Plan.value.id); QCOMPARE(Remote.Status(Plan.value.id)["outcome"].toString(), "Unknown");
}
void RegistrySessionTests::authenticatedPipeAndReconnect()
{
    WinRegistryBackend Backend;
    const auto Base = RegistryLocation::Parse("HKCU\\Software\\HyperCommanderTests").value(); Backend.Create(Base);
    const auto Key = Base.Child("RegistryEditor-task_4ceba737038e-" + QUuid::createUuid().toString(QUuid::WithoutBraces));
    QCOMPARE(Backend.Create(Key), quint32(0));
    const auto Cleanup = qScopeGuard([&] { Backend.DeleteKey(Key); });
    const QByteArray OldScope = qgetenv("HC_REGISTRY_TEST_ROOT"); qputenv("HC_REGISTRY_TEST_ROOT", Key.Path().toUtf8());
    const auto RestoreScope = qScopeGuard([&] { if (OldScope.isNull()) qunsetenv("HC_REGISTRY_TEST_ROOT"); else qputenv("HC_REGISTRY_TEST_ROOT", OldScope); });
    auto Launcher = [](const QString& Program, const QString& Arguments, bool) -> void* {
        auto Command = ('"' + QDir::toNativeSeparators(Program) + "\" " + Arguments).toStdWString();
        STARTUPINFOW Startup{}; Startup.cb = sizeof(Startup); PROCESS_INFORMATION Process{};
        if (!CreateProcessW(nullptr, Command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &Startup, &Process)) return nullptr;
        CloseHandle(Process.hThread); return Process.hProcess;
    };
    hc::admin::SystemSessionClient Client(QCoreApplication::applicationFilePath(), Launcher, false);
    RegistryRemoteSession Remote([&](const QString& Kind, QJsonObject Payload) { return Client.Request(Kind, std::move(Payload)); });
    const auto Plan = Remote.Prepare({{OperationKind::SetValue, Key, {"pipe", REG_BINARY, "authenticated"}}});
    QVERIFY2(Plan, qPrintable(QString::number(Plan.error)));
    QCOMPARE(Plan.value.callerSid, QString::fromStdWString(hc::system::CurrentUserSid()));
    QCOMPARE(Backend.Read(Key, "pipe").error, quint32(ERROR_FILE_NOT_FOUND));
    QTemporaryDir Directory; RegistryChangeStore Store(Directory.filePath("registry-backup")); auto Record = Store.SavePrepared(Plan.value); QVERIFY(Record);
    QCOMPARE(Store.Apply(Record.value, [&](const RegistryStep&, int Index) { return Remote.Apply(Plan.value, Index); }).error, quint32(0));
    QCOMPARE(Backend.Read(Key, "pipe").value.bytes, QByteArray("authenticated"));
    QCOMPARE(Remote.Read(Base).error, quint32(ERROR_ACCESS_DENIED));
    Client.Close();
    QCOMPARE(Remote.Status(Plan.value.id)["outcome"].toString(), "Unknown");
    const auto Unknown = Remote.Apply(Plan.value, 0); QVERIFY(Unknown.uncertain);
    QCOMPARE(Backend.Read(Key, "pipe").value.bytes, QByteArray("authenticated"));
    Client.Close();
    hc::admin::SystemSessionClient Cancelled(QCoreApplication::applicationFilePath(), [](const QString&, const QString&, bool) -> void* { SetLastError(ERROR_CANCELLED); return nullptr; });
    QCOMPARE(Cancelled.Request("ping", {})["nativeCode"].toString(), QString::number(ERROR_CANCELLED));
    QCOMPARE(Backend.DeleteKey(Key), quint32(0));
    QCOMPARE(WinRegistryBackend::Open(Key, KEY_QUERY_VALUE).error, quint32(ERROR_FILE_NOT_FOUND));
}
void RegistrySessionTests::nativeRenameAndLinkUndo()
{
    WinRegistryBackend Backend; const auto Base = RegistryLocation::Parse("HKCU\\Software\\HyperCommanderTests").value(); Backend.Create(Base);
    const auto Key = Base.Child("RegistryEditor-task_4ceba737038e-" + QUuid::createUuid().toString(QUuid::WithoutBraces));
    QCOMPARE(Backend.Create(Key), quint32(0));
    const auto Cleanup = qScopeGuard([&] {
        Backend.DeleteKey(Key.Child("link")); Backend.DeleteKey(Key.Child("Link"));
        Backend.DeleteKey(Key.Child("target")); Backend.DeleteKey(Key.Child("Target")); Backend.DeleteKey(Key.Child("RenamedTarget")); Backend.DeleteKey(Key);
    });
    QCOMPARE(Backend.Create(Key.Child("target")), quint32(0));
    QCOMPARE(Backend.Write(Key.Child("target"), {"v", REG_BINARY, "original"}), quint32(0));
    const QString Native = "\\Registry\\User\\" + QString::fromStdWString(hc::system::CurrentUserSid()) + "\\" + Key.Child("target").key;
    const QByteArray LinkTarget(reinterpret_cast<const char*>(Native.utf16()), Native.size() * 2);
    QCOMPARE(Backend.CreateLink(Key.Child("link"), LinkTarget), quint32(0));
    RegistryTransaction Transaction(Backend); QTemporaryDir Directory; RegistryChangeStore Store(Directory.filePath("registry-backup"));
    auto Rename = Transaction.PrepareRename(Key.Child("target"), "RenamedTarget", "caller", "machine"); QVERIFY(Rename);
    auto Record = Store.SavePrepared(Rename.value); QVERIFY(Record);
    QCOMPARE(Apply(Store, Record.value, Transaction).error, quint32(0));
    auto Undo = Store.PrepareUndo(Record.value, Transaction); QVERIFY(Undo); auto UndoRecord = Store.SavePrepared(Undo.value); QVERIFY(UndoRecord);
    QCOMPARE(Apply(Store, UndoRecord.value, Transaction).error, quint32(0));
    Rename = Transaction.PrepareRename(Key.Child("link"), "Link", "caller", "machine"); QVERIFY(Rename);
    QCOMPARE(Rename.value.steps.size(), 2);
    Record = Store.SavePrepared(Rename.value); QVERIFY(Record); QCOMPARE(Apply(Store, Record.value, Transaction).error, quint32(0));
    QVERIFY(Backend.Enumerate(Key.Child("Link")).value.info.link);
    Undo = Store.PrepareUndo(Record.value, Transaction); QVERIFY(Undo); UndoRecord = Store.SavePrepared(Undo.value); QVERIFY(UndoRecord);
    QCOMPARE(Apply(Store, UndoRecord.value, Transaction).error, quint32(0));
    bool Lowercase = false;
    for (const auto& Child : Backend.Enumerate(Key).value.keys) if (Child.name == "link") Lowercase = true;
    QVERIFY(Lowercase);
    auto Delete = Transaction.Prepare({{OperationKind::DeleteKey, Key.Child("Link")}}, "caller", "machine"); QVERIFY(Delete);
    Record = Store.SavePrepared(Delete.value); QVERIFY(Record); QCOMPARE(Apply(Store, Record.value, Transaction).error, quint32(0));
    Undo = Store.PrepareUndo(Record.value, Transaction); QVERIFY(Undo); UndoRecord = Store.SavePrepared(Undo.value); QVERIFY(UndoRecord);
    QCOMPARE(Apply(Store, UndoRecord.value, Transaction).error, quint32(0));
    QCOMPARE(Backend.Read(Key.Child("Link"), "SymbolicLinkValue").value.bytes, LinkTarget);
    QCOMPARE(Backend.Read(Key.Child("target"), "v").value.bytes, QByteArray("original"));
    auto Bound = Key; Bound.sid = QString::fromStdWString(hc::system::CurrentUserSid());
    const auto Exported = RegFileParser::Parse(RegFileWriter::Write({{OperationKind::CreateKey, Bound}}), View::Native64);
    QVERIFY(Exported); QCOMPARE(Exported.operations[0].location.hive, Hive::Users); QCOMPARE(Exported.operations[0].location.Id(), Bound.Id());
}
void RegistrySessionTests::uacTransition()
{
    const auto Mode = qEnvironmentVariable("HC_REGISTRY_TEST_UAC");
    if (Mode != "approve" && Mode != "cancel") QSKIP("Interactive UAC probe is enabled by the dedicated VM runner only");
    QCOMPARE(QSysInfo::machineHostName().toLower(), QString("imonagr-win11"));
    HANDLE Token = nullptr;
    QVERIFY(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &Token));
    TOKEN_ELEVATION Elevation{}; DWORD Returned = 0;
    const BOOL Queried = GetTokenInformation(Token, TokenElevation, &Elevation, sizeof(Elevation), &Returned);
    CloseHandle(Token);
    QVERIFY(Queried); QCOMPARE(Elevation.TokenIsElevated, DWORD(0));
    // This slot is run in a Limited interactive task, never in the regular
    // session-0 suite. The real runas dialog must be approved or cancelled.
    WinRegistryBackend Backend;
    const auto Key = RegistryLocation::Parse("HKLM\\Software\\HyperCommanderTestRegistryEditor-task_4ceba737038e-" + QUuid::createUuid().toString(QUuid::WithoutBraces)).value();
    qInfo().noquote() << "UAC_TEST_ROOT=" + Key.Path();
    QCOMPARE(WinRegistryBackend::Open(Key, KEY_QUERY_VALUE).error, quint32(ERROR_FILE_NOT_FOUND));
    qputenv("HC_REGISTRY_TEST_ROOT", Key.Path().toUtf8());
    const auto RestoreScope = qScopeGuard([] { qunsetenv("HC_REGISTRY_TEST_ROOT"); });
    auto Launcher = [Scope = Key.Path()](const QString& Program, const QString& Arguments, bool Elevated) -> void* {
        if (!Elevated) { SetLastError(ERROR_INVALID_PARAMETER); return nullptr; }
        const int Separator = Arguments.indexOf(' ');
        auto Bootstrap = QJsonDocument::fromJson(QByteArray::fromBase64(Arguments.mid(Separator + 1).toLatin1(), QByteArray::Base64UrlEncoding)).object();
        Bootstrap["registryTestRoot"] = Scope;
        const auto Payload = QJsonDocument(Bootstrap).toJson(QJsonDocument::Compact).toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
        const auto Parameters = Arguments.left(Separator + 1) + QString::fromLatin1(Payload);
        const auto NativeProgram = QDir::toNativeSeparators(Program);
        SHELLEXECUTEINFOW Info{}; Info.cbSize = sizeof(Info);
        Info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
        Info.lpVerb = L"runas"; Info.lpFile = reinterpret_cast<LPCWSTR>(NativeProgram.utf16());
        Info.lpParameters = reinterpret_cast<LPCWSTR>(Parameters.utf16()); Info.nShow = SW_HIDE;
        return ShellExecuteExW(&Info) ? Info.hProcess : nullptr;
    };
    hc::admin::SystemSessionClient Client(QCoreApplication::applicationFilePath(), Launcher, true);
    RegistryRemoteSession Remote([&](const QString& Kind, QJsonObject Payload) { return Client.Request(Kind, std::move(Payload)); });
    const auto Plan = Remote.Prepare({{OperationKind::CreateKey, Key}, {OperationKind::SetValue, Key, {"Owner", REG_SZ, QByteArray::fromHex("480043000000")}}});
    if (Mode == "cancel") {
        QCOMPARE(Plan.error, quint32(ERROR_CANCELLED));
        QCOMPARE(WinRegistryBackend::Open(Key, KEY_QUERY_VALUE).error, quint32(ERROR_FILE_NOT_FOUND));
        return;
    }
    QVERIFY2(Plan, qPrintable(QString::number(Plan.error)));
    QTemporaryDir Directory; RegistryChangeStore Store(Directory.filePath("registry-backup")); auto Record = Store.SavePrepared(Plan.value); QVERIFY(Record);
    QCOMPARE(Store.Apply(Record.value, [&](const RegistryStep&, int Index) { return Remote.Apply(Plan.value, Index); }).error, quint32(0));
    QCOMPARE(Backend.Read(Key, "Owner").value.bytes, QByteArray::fromHex("480043000000"));
    const auto Delete = Remote.Prepare({{OperationKind::DeleteKey, Key}}); QVERIFY(Delete);
    auto DeleteRecord = Store.SavePrepared(Delete.value); QVERIFY(DeleteRecord);
    QCOMPARE(Store.Apply(DeleteRecord.value, [&](const RegistryStep&, int Index) { return Remote.Apply(Delete.value, Index); }).error, quint32(0));
    QCOMPARE(WinRegistryBackend::Open(Key, KEY_QUERY_VALUE).error, quint32(ERROR_FILE_NOT_FOUND));
    Client.Close();
}
void RegistrySessionTests::mutationAdapter()
{
    const QString Sid = "S-1-5-21-1", Machine = "fixture-machine";
    FakeRegistryBackend Backend; auto BoundRoot = Root; BoundRoot.sid = Sid; Backend.Seed(BoundRoot);
    const auto Key = Root.Child("Software\\HyperCommanderTests\\Mutation");
    auto BoundKey = Key; BoundKey.sid = Sid;
    QTemporaryDir Directory; RegistryChangeStore Store(Directory.filePath("registry-backup"));
    RegistryMutation Mutation(Backend, Store, Sid, Machine);
    const QList<RegistryOperation> Operations{{OperationKind::CreateKey, Key}, {OperationKind::SetValue, Key, {"test", REG_BINARY, "original"}}};
    auto Result = Mutation.Apply(Operations); QCOMPARE(Result.applied.error, quint32(0));
    QCOMPARE(Result.record.state, QString("committed")); QVERIFY(!Result.elevated);
    QCOMPARE(Backend.Read(BoundKey, "test").value.bytes, QByteArray("original"));
    QCOMPARE(Backend.Read(Key, "test").error, quint32(ERROR_FILE_NOT_FOUND));
    QVERIFY(QFile::exists(Result.record.directory + "/before.reg"));
    auto Undo = Mutation.Undo(Result.record); QCOMPARE(Undo.applied.error, quint32(0));
    QCOMPARE(Backend.Read(BoundKey, "test").error, quint32(ERROR_FILE_NOT_FOUND));
    auto Redo = Mutation.Redo(Result.record, Undo.record); QCOMPARE(Redo.applied.error, quint32(0));
    QCOMPARE(Backend.Read(BoundKey, "test").value.bytes, QByteArray("original"));
    const auto Before = Backend.mutations_;
    auto MachineKey = RegistryLocation::Parse("HKLM\\Software\\HyperCommanderTests\\Mutation").value();
    QCOMPARE(Mutation.Apply({{OperationKind::CreateKey, MachineKey}}).applied.error, quint32(ERROR_ELEVATION_REQUIRED));
    QCOMPARE(Mutation.Apply({{OperationKind::CreateKey, RegistryLocation::Parse("HKCR\\Example").value()}}).applied.error, quint32(ERROR_NOT_SUPPORTED));
    RegistryRemoteSession Cancelled([](const QString&, QJsonObject) {
        return QJsonObject{{"outcome", "Failed"}, {"nativeCode", QString::number(ERROR_CANCELLED)}};
    });
    RegistryMutation CancelledMutation(Backend, Store, Sid, Machine, &Cancelled);
    const auto CancelledResult = CancelledMutation.Apply({{OperationKind::CreateKey, MachineKey}});
    QCOMPARE(CancelledResult.applied.error, quint32(ERROR_CANCELLED)); QVERIFY(CancelledResult.elevated);
    QVERIFY(CancelledResult.record.directory.isEmpty()); QCOMPARE(Backend.mutations_, Before);
    RegistryMutation WrongIdentity(Backend, Store, "different-user", Machine);
    QCOMPARE(WrongIdentity.Undo(Redo.record).applied.error, quint32(ERROR_ACCESS_DENIED));
    QCOMPARE(Backend.mutations_, Before);
    // Elevated requests still save the backup locally before asking the helper
    // to apply. Inject the in-memory authenticated endpoint, never real HKLM.
    Backend.Seed(*MachineKey.Parent()->Parent());
    RegistrySessionServer Server(Backend, Sid, Machine);
    RegistryRemoteSession Remote([&](const QString& Kind, QJsonObject Request) { return Server.Handle(Kind, Request); });
    RegistryMutation Elevated(Backend, Store, Sid, Machine, &Remote);
    auto ElevatedResult = Elevated.Apply({{OperationKind::CreateKey, MachineKey}, {OperationKind::SetValue, MachineKey, {"test", REG_BINARY, "remote"}}});
    QCOMPARE(ElevatedResult.applied.error, quint32(0)); QVERIFY(ElevatedResult.elevated);
    QCOMPARE(Backend.Read(MachineKey, "test").value.bytes, QByteArray("remote"));
    QCOMPARE(Elevated.Undo(ElevatedResult.record).applied.error, quint32(0));
    const RegistryLocation UserClasses{Hive::Users, Sid + "_Classes"}; Backend.Seed(UserClasses);
    const auto UserClassEdit = Mutation.SaveValue(UserClasses, {}, {"HyperCommanderTest", REG_SZ, QByteArray(2, char(0))});
    QCOMPARE(UserClassEdit.applied.error, quint32(0)); QVERIFY(!UserClassEdit.elevated);
    QCOMPARE(Mutation.Undo(UserClassEdit.record).applied.error, quint32(0));
    const RegistryLocation OtherClasses{Hive::Users, Sid + "-other_Classes"}; Backend.Seed(OtherClasses);
    QCOMPARE(Mutation.SaveValue(OtherClasses, {}, {"HyperCommanderTest", REG_SZ, QByteArray(2, char(0))}).applied.error, quint32(ERROR_ELEVATION_REQUIRED));
}
void RegistrySessionTests::editorConflict()
{
    const QString Sid = "S-1-5-21-1", Machine = "fixture-machine";
    FakeRegistryBackend Backend;
    QTemporaryDir Directory; RegistryChangeStore Store(Directory.filePath("registry-backup"));
    RegistrySessionServer Server(Backend, Sid, Machine);
    RegistryRemoteSession Remote([&](const QString& Kind, QJsonObject Request) { return Server.Handle(Kind, Request); });
    RegistryMutation Mutation(Backend, Store, Sid, Machine, &Remote);
    for (const auto HiveValue : {Hive::CurrentUser, Hive::LocalMachine}) {
        RegistryLocation Key; Key.hive = HiveValue; Key.key = "Software\\HyperCommanderTests\\Draft";
        auto Bound = Key; if (HiveValue == Hive::CurrentUser) Bound.sid = Sid; Backend.Seed(Bound);
        const RegistryValue Original{"value", REG_BINARY, "original"}, Draft{"value", REG_BINARY, "draft"};
        QCOMPARE(Backend.Write(Bound, Original), quint32(0));
        QCOMPARE(Backend.Write(Bound, {"value", REG_BINARY, "external"}), quint32(0));
        const auto Before = Backend.mutations_;
        const auto Conflict = Mutation.SaveValue(Key, Original, Draft);
        QVERIFY(Conflict.applied.conflict); QCOMPARE(Conflict.applied.error, quint32(ERROR_REVISION_MISMATCH));
        QCOMPARE(Backend.mutations_, Before); QCOMPARE(Backend.Read(Bound, "value").value.bytes, QByteArray("external"));
        const auto Current = Backend.Read(Bound, "value").value;
        const auto Saved = Mutation.SaveValue(Key, Current, Draft); QCOMPARE(Saved.applied.error, quint32(0));
        QCOMPARE(Backend.Read(Bound, "value").value.bytes, Draft.bytes); QVERIFY(QFile::exists(Saved.record.directory + "/before.reg"));
        QCOMPARE(Mutation.Undo(Saved.record).applied.error, quint32(0));
        QVERIFY(Backend.Read(Bound, "value").value == Current);
        QVERIFY(Mutation.SaveValue(Key, {}, Draft).applied.conflict);
    }
}
void RegistrySessionTests::valueRenameConflict()
{
    const QString Sid = "S-1-5-21-1", Machine = "fixture-machine";
    FakeRegistryBackend Backend; QTemporaryDir Directory;
    RegistryChangeStore Store(Directory.filePath("registry-backup"));
    RegistrySessionServer Server(Backend, Sid, Machine);
    RegistryRemoteSession Remote([&](const QString& Kind, QJsonObject Request) { return Server.Handle(Kind, Request); });
    RegistryMutation Mutation(Backend, Store, Sid, Machine, &Remote);
    for (const auto HiveValue : {Hive::CurrentUser, Hive::LocalMachine}) {
        RegistryLocation Key; Key.hive = HiveValue; Key.key = "Software\\HyperCommanderTests\\Rename";
        auto Bound = Key; if (HiveValue == Hive::CurrentUser) Bound.sid = Sid; Backend.Seed(Bound);
        const RegistryValue Original{"old", REG_BINARY, QByteArray::fromHex("00ff01")};
        QCOMPARE(Backend.Write(Bound, Original), quint32(0));
        QCOMPARE(Backend.Write(Bound, {"taken", REG_SZ, "foreign"}), quint32(0));
        auto Before = Backend.mutations_;
        QVERIFY(Mutation.RenameValue(Key, Original, "taken").applied.conflict);
        QCOMPARE(Backend.mutations_, Before);
        QVERIFY(Backend.Read(Bound, "old").value == Original);
        const auto Changed = Mutation.RenameValue(Key, Original, "new");
        QCOMPARE(Changed.applied.error, quint32(0));
        QVERIFY(!Backend.Read(Bound, "old")); QCOMPARE(Backend.Read(Bound, "new").value.bytes, Original.bytes);
        QCOMPARE(Mutation.Undo(Changed.record).applied.error, quint32(0));
        QVERIFY(Backend.Read(Bound, "old").value == Original);
        const auto Case = Mutation.RenameValue(Key, Original, "OLD");
        QCOMPARE(Case.applied.error, quint32(0)); QCOMPARE(Backend.Read(Bound, "OLD").value.name, QString("OLD"));
        QCOMPARE(Mutation.Undo(Case.record).applied.error, quint32(0));
        QVERIFY(Backend.Read(Bound, "old").value == Original);
        QCOMPARE(Backend.Write(Bound, {"old", REG_BINARY, "external"}), quint32(0));
        Before = Backend.mutations_;
        QVERIFY(Mutation.RenameValue(Key, Original, "new").applied.conflict);
        QCOMPARE(Backend.mutations_, Before); QVERIFY(!Backend.Read(Bound, "new"));
        QCOMPARE(Mutation.RenameValue(Key, Original, "").applied.error, quint32(ERROR_INVALID_PARAMETER));
    }
}
#include "registry/crudCases.hpp"
void RegistrySessionTests::crudConflicts() { RegistryCrudCases(); }
#include "registry/staleRecoveryCase.hpp"
void RegistrySessionTests::staleRecovery() { RegistryStaleRecoveryCase(); }
void RegistrySessionTests::policyRemoteConflict()
{
    FakeRegistryBackend Backend; const auto Key = RegistryLocation::Parse("HKLM\\Software\\HyperCommanderTests\\PolicyRemote").value(); Backend.Seed(Key);
    QByteArray Image; int PolicyWrites = 0;
    const auto Read = [&](bool) -> RegistryResult<QByteArray> { return {Image}; };
    const auto Write = [&](bool, const QByteArray&, const QByteArray& Desired) -> RegistryResult<QByteArray> { Image = Desired; ++PolicyWrites; return {Image}; };
    RegistrySessionServer Server(Backend, "S-1-5-21-1", "fixture", {}, Read, Write);
    const auto External = RegistryPolCodec::Write({{"external", {"Other", REG_BINARY, "external"}}}).value;
    bool Race = false;
    RegistryRemoteSession Remote([&](const QString& Kind, QJsonObject Request) {
        if (Race && Kind == "registry.apply") { Image = External; Race = false; }
        return Server.Handle(Kind, Request);
    }, [&](bool, const std::atomic_bool*) -> quint32 {
        // Simulate the OS policy service, independently of transaction writes.
        if (Image.isEmpty()) { const auto Error = Backend.DeleteValue(Key, "On"); return Error == ERROR_FILE_NOT_FOUND ? 0 : Error; }
        const auto Parsed = RegistryPolCodec::Read(Image); if (!Parsed || Parsed.records.size() != 1) return ERROR_INVALID_DATA;
        return Backend.Write(Key, Parsed.records[0].value);
    });
    QTemporaryDir Directory; RegistryChangeStore Store(Directory.path());
    RegistryMutation Mutation(Backend, Store, "S-1-5-21-1", "fixture", &Remote);
    const RegistryValue Value{"On", REG_DWORD, QByteArray::fromHex("01000000")};
    const auto Desired = RegistryPolCodec::Write({{Key.key, Value}}).value;
    const QList<RegistryOperation> Operations{{OperationKind::SetValue, Key, Value}};
    RegistryStep Expected; Expected.location = Key; Expected.name = Value.name; Expected.after = Value;
    Image = External;
    QCOMPARE(Mutation.ApplyPolicy(true, {}, Desired, Operations, {Expected}).applied.error, quint32(ERROR_REVISION_MISMATCH));
    QCOMPARE(Backend.mutations_, 0); QCOMPARE(PolicyWrites, 0);
    Image.clear(); Race = true;
    const auto Raced = Mutation.ApplyPolicy(true, {}, Desired, Operations, {Expected});
    QVERIFY(Raced.applied.conflict); QCOMPARE(Backend.mutations_, 0); QCOMPARE(PolicyWrites, 0);
    Image.clear();
    const auto Applied = Mutation.ApplyPolicy(true, {}, Desired, Operations, {Expected});
    QCOMPARE(Applied.applied.error, quint32(0)); QCOMPARE(Image, Desired); QCOMPARE(PolicyWrites, 1);
    QCOMPARE(Backend.Read(Key, "On").value, Value);
    const auto Undo = Mutation.Undo(Applied.record); QCOMPARE(Undo.applied.error, quint32(0));
    QVERIFY(Image.isEmpty()); QVERIFY(!Backend.Read(Key, "On")); QCOMPARE(PolicyWrites, 2);
    const auto Redo = Mutation.Redo(Applied.record, Undo.record); QCOMPARE(Redo.applied.error, quint32(0));
    QCOMPARE(Image, Desired); QCOMPARE(Backend.Read(Key, "On").value, Value); QCOMPARE(PolicyWrites, 3);
}
void RegistrySessionTests::policyCancellation()
{
    for (const auto Failure : {quint32(ERROR_CANCELLED), quint32(ERROR_TIMEOUT)}) {
        FakeRegistryBackend Backend; const auto Key = RegistryLocation::Parse("HKLM\\Software\\HyperCommanderTests\\PolicyCancel").value(); Backend.Seed(Key);
        QByteArray Image; int Writes = 0;
        const auto Read = [&](bool) -> RegistryResult<QByteArray> { return {Image}; };
        const auto Write = [&](bool, const QByteArray&, const QByteArray& Desired) -> RegistryResult<QByteArray> { Image = Desired; ++Writes; return {Image}; };
        RegistrySessionServer Server(Backend, "S-1-5-21-1", "fixture", {}, Read, Write);
        std::atomic_bool Cancel{false}; QList<int> Phases;
        RegistryRemoteSession Remote([&](const QString& Kind, QJsonObject Request) { return Server.Handle(Kind, Request); },
            [&](bool, const std::atomic_bool*) -> quint32 { return Failure; });
        Remote.SetPolicyControl(&Cancel, [&](int Phase) { Phases.append(Phase); });
        QTemporaryDir Directory; RegistryChangeStore Store(Directory.path());
        RegistryMutation Mutation(Backend, Store, "S-1-5-21-1", "fixture", &Remote);
        const RegistryValue Value{"On", REG_DWORD, QByteArray::fromHex("01000000")};
        const auto Desired = RegistryPolCodec::Write({{Key.key, Value}}).value;
        RegistryStep Expected; Expected.location = Key; Expected.name = Value.name; Expected.after = Value;
        auto Result = Mutation.ApplyPolicy(true, {}, Desired, {}, {Expected});
        QCOMPARE(Result.applied.error, Failure); QVERIFY(Result.applied.uncertain);
        QCOMPARE(Result.record.completed, 1); QCOMPARE(Result.record.inFlight, 1);
        QCOMPARE(Result.record.state, QString("recovery-required")); QCOMPARE(Image, Desired);
        QCOMPARE(Writes, 1); QCOMPARE(Backend.mutations_, 0); QCOMPARE(Phases, QList<int>({0, 1}));
        RegistryTransaction Transaction(Backend, Read, Write);
        QCOMPARE(Store.Recover(Result.record, Transaction), quint32(ERROR_RETRY));
        QCOMPARE(Result.record.state, QString("partial")); QCOMPARE(Result.record.inFlight, -1);
        QCOMPARE(Writes, 1); QCOMPARE(Backend.mutations_, 0);
    }
}
void RegistrySessionTests::policyJournal()
{
    FakeRegistryBackend Backend; QByteArray Image; int Writes = 0; bool LoseAcknowledgement = false;
    const auto Read = [&](bool) -> RegistryResult<QByteArray> { return {Image}; };
    const auto Write = [&](bool, const QByteArray& Expected, const QByteArray& Desired) -> RegistryResult<QByteArray> {
        if (LocalPolicyStore::Canonical(Image).value != LocalPolicyStore::Canonical(Expected).value) return {{}, ERROR_REVISION_MISMATCH};
        Image = Desired; ++Writes;
        return LoseAcknowledgement ? RegistryResult<QByteArray>{{}, ERROR_BROKEN_PIPE} : RegistryResult<QByteArray>{Image};
    };
    RegistryTransaction Transaction(Backend, Read, Write, [](bool) -> quint32 { return 0; }); QTemporaryDir Directory; QVERIFY(Directory.isValid());
    RegistryChangeStore Store(Directory.path());
    auto Plan = Transaction.Prepare({}, "fixture-sid", "fixture-machine"); QVERIFY(Plan);
    const auto Desired = RegistryPolCodec::Write({{"Software\\Policies\\HyperCommanderTest", {"Enabled", REG_DWORD, QByteArray::fromHex("01000000")}}}); QVERIFY(Desired);
    RegistryStep Step; Step.kind = StepKind::PolicyFile; Step.location = RegistryLocation::Parse("HKLM").value();
    Step.name = "Registry.pol"; Step.before = RegistryValue{Step.name, REG_BINARY, {}}; Step.after = RegistryValue{Step.name, REG_BINARY, Desired.value};
    for (int Phase = 0; Phase < 3; ++Phase) { Step.policyPhase = Phase; Plan.value.steps.append(Step); }
    Step.policyPhase = 0; QVERIFY(RegistryPlan::FromJson(Plan.value.ToJson()));
    auto Invalid = Plan.value; Invalid.steps[0].location.key = "arbitrary-path"; QVERIFY(!RegistryPlan::FromJson(Invalid.ToJson()));
    Invalid = Plan.value; Invalid.steps[0].after->bytes = "bad policy"; QVERIFY(!RegistryPlan::FromJson(Invalid.ToJson()));
    auto Record = Store.SavePrepared(Plan.value); QVERIFY(Record);
    QVERIFY(QFileInfo::exists(QDir(Record.value.directory).filePath("policy-0-before.pol")));
    QFile Export(QDir(Record.value.directory).filePath("after.reg")); QVERIFY(Export.open(QIODevice::ReadOnly));
    const auto Exported = RegFileParser::Parse(Export.readAll(), View::Native64); QVERIFY(Exported); QVERIFY(Exported.operations.isEmpty());
    LoseAcknowledgement = true; const auto Applied = Apply(Store, Record.value, Transaction);
    QVERIFY(Applied.uncertain); QCOMPARE(Writes, 1); QCOMPARE(Record.value.inFlight, 0);
    LoseAcknowledgement = false; QCOMPARE(Store.Recover(Record.value, Transaction), quint32(0)); QCOMPARE(Record.value.completed, 1);
    const auto Undo = Store.PrepareUndo(Record.value, Transaction); QVERIFY(Undo);
    auto UndoRecord = Store.SavePrepared(Undo.value); QVERIFY(UndoRecord); QCOMPARE(Apply(Store, UndoRecord.value, Transaction).error, quint32(0));
    QVERIFY(Image.isEmpty()); QCOMPARE(Writes, 2);
    const auto Redo = Store.PrepareRedo(Record.value, UndoRecord.value, Transaction); QVERIFY(Redo);
    auto RedoRecord = Store.SavePrepared(Redo.value); QVERIFY(RedoRecord); QCOMPARE(Apply(Store, RedoRecord.value, Transaction).error, quint32(0));
    QCOMPARE(Image, Desired.value); QCOMPARE(Writes, 3);
    auto Reverse = Transaction.Reverse(Step);
    Image = RegistryPolCodec::Write({{"Other", {"Value", REG_SZ, QByteArray::fromHex("78000000")}}}).value;
    QVERIFY(Transaction.ApplyStep(Reverse).conflict); QCOMPARE(Writes, 3);
    const auto Duplicate = RegistryPolCodec::Write({{"same", {"Name", REG_BINARY, "a"}}, {"SAME", {"name", REG_BINARY, "b"}}});
    QVERIFY(Duplicate); QVERIFY(!LocalPolicyStore::Canonical(Duplicate.value));
}
int main(int Count, char** Arguments)
{
    QCoreApplication Application(Count, Arguments); const auto Args = Application.arguments();
    if (Args.size() == 3 && Args[1] == "--system-session") {
        const auto Bootstrap = QJsonDocument::fromJson(QByteArray::fromBase64(Args[2].toLatin1(), QByteArray::Base64UrlEncoding)).object();
        const auto Scope = Bootstrap["registryTestRoot"].toString(qEnvironmentVariable("HC_REGISTRY_TEST_ROOT"));
        const bool UserScope = Scope.startsWith("HKEY_CURRENT_USER\\Software\\HyperCommanderTests\\RegistryEditor-task_4ceba737038e-");
        const bool MachineScope = QSysInfo::machineHostName().compare("iMonAGR-Win11", Qt::CaseInsensitive) == 0
            && Scope.startsWith("HKEY_LOCAL_MACHINE\\Software\\HyperCommanderTestRegistryEditor-task_4ceba737038e-");
        if (!UserScope && !MachineScope) return ERROR_ACCESS_DENIED;
        return int(hc::admin::ExecuteSystemSession(Bootstrap, Application.applicationFilePath(), false, Scope));
    }
    RegistrySessionTests Tests; return QTest::qExec(&Tests, Count, Arguments);
}
