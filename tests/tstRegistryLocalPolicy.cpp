#include "stdafx.h"
#include "tstRegistryLocalPolicy.hpp"

/// 모듈
/// UIs
/// 기타 헤더파일
/// 고유 라이브러리
/// 공통 라이브러리
#include "commonLibs/system/registryLocalPolicy.hpp"
#include "commonLibs/system/registryValue.hpp"
/// 외부 라이브러리
/// 선언 헤더파일들
/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QTest>
#include <QElapsedTimer>
#include <QSysInfo>
#include <QFile>
#include <QDir>
#include <QProcess>
#include <QScopeGuard>
#include <QUuid>

#include "registry/policyRestartWorker.hpp"
DECLARE_CMNLIBSV2_NAMESPACE
Q_LOGGING_CATEGORY(lcRegistryLocalPolicyTests, "hc.tests.registryLocalPolicy")
using namespace hc::registry;
namespace {
bool Refresh()
{
    wchar_t System[32768]{}; if (!GetSystemDirectoryW(System, DWORD(std::size(System)))) return false;
    QProcess Process; Process.start(QString::fromWCharArray(System) + "/gpupdate.exe", {"/target:computer", "/force", "/wait:30"});
    if (!Process.waitForFinished(45000)) { Process.kill(); Process.waitForFinished(5000); return false; }
    return Process.exitStatus() == QProcess::NormalExit && Process.exitCode() == 0;
}
bool Save(const QString& Path, const QByteArray& Bytes)
{
    QFile File(Path); return File.open(QIODevice::WriteOnly | QIODevice::NewOnly) && File.write(Bytes) == Bytes.size() && File.flush();
}
}
void RegistryLocalPolicyTests::nativeRoundtrip()
{
    if (qEnvironmentVariable("HC_REGISTRY_TEST_LOCAL_GPO") != "task_4ceba737038e") QSKIP("Explicit isolated VM GPO runner required");
    QCOMPARE(QSysInfo::machineHostName().toLower(), QString("imonagr-win11"));
    const auto Artifacts = qEnvironmentVariable("HC_REGISTRY_TEST_GPO_ARTIFACTS");
    QVERIFY(Artifacts.startsWith("C:\\HyperCommanderTestRegistryPolicy-", Qt::CaseInsensitive)); QVERIFY(QDir(Artifacts).exists());
    const auto Original = LocalPolicyStore::Read(true); QVERIFY(Original);
    const auto OriginalCanonical = LocalPolicyStore::Canonical(Original.value); QVERIFY(OriginalCanonical);
    QVERIFY(Save(QDir(Artifacts).filePath("original-machine.pol"), Original.value));
    const auto Parsed = Original.value.isEmpty() ? RegistryPolResult{} : RegistryPolCodec::Read(Original.value); QVERIFY(Parsed);
    const auto Location = RegistryLocation::Parse("HKLM\\Software\\Policies\\HyperCommanderTestRegistryEditorP5-" + QUuid::createUuid().toString(QUuid::WithoutBraces)).value();
    const auto List = Location.Child("List"); WinRegistryBackend Backend;
    QCOMPARE(Backend.Enumerate(Location).error, quint32(ERROR_FILE_NOT_FOUND));
    QVERIFY(Save(QDir(Artifacts).filePath("fixture-path.txt"), Location.Path().toUtf8()));
    const auto String = [](const QString& Name, const QString& Text) { return RegistryValue{Name, REG_SZ, RegistryValueCodec::EncodeText(REG_SZ, Text).value}; };
    auto Records = Parsed.records;
    Records.append({Location.key, {"Enabled", REG_DWORD, RegistryValueCodec::EncodeNumber(REG_DWORD, 1).value}});
    Records.append({Location.key, String("**soft.Preserve", "default")});
    Records.append({List.key, String("**DelVals.", " ")}); Records.append({List.key, String("1", "first")});
    const auto Desired = RegistryPolCodec::Write(Records); QVERIFY(Desired);
    const auto DesiredCanonical = LocalPolicyStore::Canonical(Desired.value); QVERIFY(DesiredCanonical);
    QVERIFY(Save(QDir(Artifacts).filePath("desired-machine.pol"), Desired.value));
    bool Attempted = false, Restored = false;
    const auto Cleanup = qScopeGuard([&] {
        if (Attempted && !Restored) {
            const auto Current = LocalPolicyStore::Read(true);
            const auto Canonical = Current ? LocalPolicyStore::Canonical(Current.value) : RegistryResult<QByteArray>{{}, Current.error};
            if (!Canonical || (Canonical.value != OriginalCanonical.value && Canonical.value != DesiredCanonical.value)) {
                QTest::qFail("Policy changed outside owned images; preserve evidence and do not overwrite", __FILE__, __LINE__); return;
            }
            if (Canonical.value != OriginalCanonical.value && !LocalPolicyStore::Apply(true, Current.value, Original.value)) {
                QTest::qFail("Could not restore original policy; preserve evidence", __FILE__, __LINE__); return;
            }
            if (!Refresh()) { QTest::qFail("Could not refresh restored policy", __FILE__, __LINE__); return; }
        }
        const auto Native = Location.key.toStdWString(); const auto Error = RegDeleteTreeW(HKEY_LOCAL_MACHINE, Native.c_str());
        if (Error != ERROR_SUCCESS && Error != ERROR_FILE_NOT_FOUND) { QTest::qFail("Could not remove owned policy fixture", __FILE__, __LINE__); return; }
        if (Backend.Enumerate(Location).error != ERROR_FILE_NOT_FOUND) { QTest::qFail("Owned policy fixture remains", __FILE__, __LINE__); return; }
        const auto FinalPolicy = LocalPolicyStore::Read(true);
        if (!FinalPolicy || LocalPolicyStore::Canonical(FinalPolicy.value).value != OriginalCanonical.value) { QTest::qFail("Original policy verification failed", __FILE__, __LINE__); return; }
        if (!Save(QDir(Artifacts).filePath("restored.ok"), "fixture removed; original policy re-read")) QTest::qFail("Restore marker failed", __FILE__, __LINE__);
    });
    QCOMPARE(Backend.Create(Location), quint32(0)); QCOMPARE(Backend.Create(List), quint32(0));
    QCOMPARE(Backend.Write(Location, String("Preserve", "original")), quint32(0)); QCOMPARE(Backend.Write(List, String("old", "remove")), quint32(0));
    Attempted = true;
    RegistryTransaction Transaction(Backend); RegistryChangeStore Store(QDir(Artifacts).filePath("registry-backup"));
    auto Plan = Transaction.Prepare({}, QString::fromStdWString(hc::system::CurrentUserSid()), QString::fromLatin1(QSysInfo::machineUniqueId().toHex())); QVERIFY(Plan);
    RegistryStep PolicyStep; PolicyStep.kind = StepKind::PolicyFile; PolicyStep.location.hive = Hive::LocalMachine;
    PolicyStep.name = "Registry.pol"; PolicyStep.before = RegistryValue{"Registry.pol", REG_BINARY, Original.value};
    PolicyStep.after = RegistryValue{"Registry.pol", REG_BINARY, Desired.value};
    PolicyStep.policyChecks = {
        {Location, "Enabled", {}, {}, Records[Parsed.records.size()].value},
        {Location, "Preserve", String("Preserve", "original"), String("Preserve", "original"), String("Preserve", "original")},
        {List, "old", String("old", "remove"), {}, {}},
        {List, "1", {}, {}, String("1", "first")}
    };
    for (int Phase = 0; Phase < 3; ++Phase) { PolicyStep.policyPhase = Phase; Plan.value.steps.append(PolicyStep); }
    auto Journal = Store.SavePrepared(Plan.value); QVERIFY(Journal);
    QVERIFY(Save(QDir(Artifacts).filePath("journal-id.txt"), Journal.value.plan.id.toUtf8()));
    const auto Applied = Store.Apply(Journal.value, [&](const RegistryStep& Step, int) { return Transaction.ApplyStep(Step); });
    QCOMPARE(Applied.error, quint32(0));
    QCOMPARE(LocalPolicyStore::Canonical(LocalPolicyStore::Read(true).value).value, DesiredCanonical.value);
    QCOMPARE(LocalPolicyStore::Apply(true, Original.value, Desired.value).error, quint32(ERROR_REVISION_MISMATCH));
    QVERIFY(Refresh());
    QCOMPARE(RegistryValueCodec::DecodeNumber(Backend.Read(Location, "Enabled").value).value, quint64(1));
    QCOMPARE(RegistryValueCodec::DecodeText(Backend.Read(Location, "Preserve").value).text, QString("original"));
    QCOMPARE(RegistryValueCodec::DecodeText(Backend.Read(List, "1").value).text, QString("first"));
    QCOMPARE(Backend.Read(List, "old").error, quint32(ERROR_FILE_NOT_FOUND));
    if (qEnvironmentVariableIntValue("HC_REGISTRY_TEST_GPO_UI") == 1) {
        QVERIFY(Save(QDir(Artifacts).filePath("gpedit-ready"), "saved and refreshed"));
        const auto ResultPath = QDir(Artifacts).filePath("gpedit-result.txt");
        QElapsedTimer Timer; Timer.start();
        while (!QFile::exists(ResultPath) && Timer.elapsed() < 120000) QTest::qWait(100);
        QFile Result(ResultPath); QVERIFY2(Result.open(QIODevice::ReadOnly), "gpedit inspection did not finish");
        const auto Text = Result.readAll(); QVERIFY2(Text == "passed", Text.constData());
    }
    for (const auto& Phase : {QString("undo"), QString("redo")}) {
        QProcess Worker; Worker.start(QDir(Artifacts).filePath("HyperCommander.exe"), {"--policy-restart-worker", Artifacts, Phase});
        QVERIFY(Worker.waitForStarted()); QVERIFY(Worker.waitForFinished(150000));
        QCOMPARE(Worker.exitStatus(), QProcess::NormalExit);
        const auto WorkerOutput = Worker.readAllStandardError() + Worker.readAllStandardOutput();
        QVERIFY2(Worker.exitCode() == 0, qPrintable(Phase + ": " + QString::fromUtf8(WorkerOutput)));
        QCOMPARE(Worker.state(), QProcess::NotRunning);
        const auto Expected = Phase == "undo" ? OriginalCanonical.value : DesiredCanonical.value;
        QCOMPARE(LocalPolicyStore::Canonical(LocalPolicyStore::Read(true).value).value, Expected);
        if (Phase == "undo") {
            QCOMPARE(Backend.Read(Location, "Enabled").error, quint32(ERROR_FILE_NOT_FOUND));
            QCOMPARE(Backend.Read(List, "old").error, quint32(ERROR_FILE_NOT_FOUND)); // No raw manual-value restoration during policy undo.
        } else {
            QCOMPARE(RegistryValueCodec::DecodeNumber(Backend.Read(Location, "Enabled").value).value, quint64(1));
            QCOMPARE(Backend.Read(List, "old").error, quint32(ERROR_FILE_NOT_FOUND));
        }
        QVERIFY(Refresh());
        QCOMPARE(LocalPolicyStore::Canonical(LocalPolicyStore::Read(true).value).value, Expected);
    }
    const auto Reverted = LocalPolicyStore::Apply(true, Desired.value, Original.value); QVERIFY2(Reverted, qPrintable(QString::number(Reverted.error)));
    QVERIFY(Refresh());
    QCOMPARE(LocalPolicyStore::Canonical(LocalPolicyStore::Read(true).value).value, OriginalCanonical.value);
    Restored = true;
}
int main(int Count, char** Arguments)
{
    QCoreApplication Application(Count, Arguments);
    const auto Args = Application.arguments();
    if (Args.size() == 4 && Args[1] == "--policy-restart-worker") return RegistryPolicyRestartWorker(Args[2], Args[3]);
    RegistryLocalPolicyTests Tests; return QTest::qExec(&Tests, Count, Arguments);
}
