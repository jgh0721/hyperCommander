#include "stdafx.h"
#include "tstEnvironmentVm.hpp"

/// 모듈
/// UIs
/// 기타 헤더파일
/// 고유 라이브러리
#include "UniqueLibs/system/environmentStorage.hpp"
#include "UniqueLibs/system/environmentProcessProtocol.hpp"
#include "UniqueLibs/systemSessionService.hpp"
/// 공통 라이브러리
#include "commonLibs/system/processInfo.hpp"
/// 외부 라이브러리
/// 선언 헤더파일들
/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QTest>
#include <QDir>
#include <QSysInfo>
#include <QTemporaryDir>
#include <QProcess>
#include <QProcessEnvironment>
#include <QScopeGuard>
#include <QUuid>

DECLARE_CMNLIBSV2_NAMESPACE
Q_LOGGING_CATEGORY(lcEnvironmentVm, "hc.tests.environmentVm")
using namespace hc::environment;
using namespace hc::registry;
void EnvironmentVmTests::initTestCase()
{
    // This target is intentionally absent from automatic ctest registration.
    // Both an explicit runner authorization and the disposable VM's identity
    // are required before constructing the native writer.
    if (qEnvironmentVariable("HC_ENV_VM_WRITE_TEST") != "task_d202c1092c2c"
        || QSysInfo::machineHostName().compare("iMonAGR-Win11", Qt::CaseInsensitive) != 0)
        QSKIP("Native writes require the explicitly authorized iMonAGR-Win11 runner");
    WinRegistryBackend Backend;
    const RegistryLocation Bios{Hive::LocalMachine, "HARDWARE\\DESCRIPTION\\System\\BIOS"};
    const auto Product = Backend.Read(Bios, "SystemProductName"); QVERIFY(Product);
    QVERIFY(RegistryValueCodec::DecodeText(Product.value).text.contains("Virtual", Qt::CaseInsensitive));
}
void EnvironmentVmTests::typedWriteRestoreThroughHelper()
{
    WinRegistryBackend Backend;
    const auto Sid = QString::fromStdWString(hc::system::CurrentUserSid()), Machine = QString::fromLatin1(QSysInfo::machineUniqueId().toHex());
    hc::admin::SystemSessionClient Client;
    RegistryRemoteSession Remote([&](const QString& Kind, QJsonObject Payload) { return Client.Request(Kind, std::move(Payload)); });
    const auto Close = qScopeGuard([&] { Client.Close(); });
    for (const auto Source : {Scope::User, Scope::System}) {
        const auto Location = EnvironmentLocation(Source, Sid);
        const auto Name = "HC_ENV_D202_NATIVE_" + QUuid::createUuid().toString(QUuid::WithoutBraces).remove('-');
        const auto Initial = Backend.Read(Location, Name); QVERIFY(!Initial); QCOMPARE(Initial.error, quint32(ERROR_FILE_NOT_FOUND));
        const auto Cleanup = qScopeGuard([&] { Backend.DeleteValue(Location, Name); });
        QTemporaryDir Directory; QVERIFY(Directory.isValid()); RegistryChangeStore History(Directory.path());
        EnvironmentStorage Storage(Backend, History, Sid, Machine, &Remote);
        const Value Empty{Name.toStdWString(), L"", ValueType::String, Source};
        const auto Create = Storage.Commit(Storage.Preview({{Source, Name, {}, Empty}}));
        QCOMPARE(Create.applied_.error, quint32(0)); QCOMPARE(DecodeEnvironmentValue(Backend.Read(Location, Name).value, Source).value, Empty);
        auto Expanded = Empty; Expanded.type_ = ValueType::ExpandString; Expanded.raw_ = L"%SystemRoot%\\Synthetic";
        const auto Edit = Storage.Commit(Storage.Preview({{Source, Name, Empty, Expanded}}));
        QCOMPARE(Edit.applied_.error, quint32(0)); QCOMPARE(DecodeEnvironmentValue(Backend.Read(Location, Name).value, Source).value, Expanded);
        RegistryChangeStore Reopened(Directory.path()); const auto Disk = Reopened.Read(Edit.record_.plan.id); QVERIFY(Disk);
        QCOMPARE(Storage.Commit(Storage.Undo(Disk.value)).applied_.error, quint32(0));
        QCOMPARE(DecodeEnvironmentValue(Backend.Read(Location, Name).value, Source).value, Empty);
        QCOMPARE(Storage.Commit(Storage.Undo(Create.record_)).applied_.error, quint32(0));
        QVERIFY(!Backend.Read(Location, Name));
        if (Source == Scope::System) QVERIFY(Client.Connected());
    }
}
void EnvironmentVmTests::nativeAndWow64ProcessEnvironment()
{
    const auto Root = qEnvironmentVariable("SystemRoot");
    hc::admin::SystemSessionClient Client;
    const auto Close = qScopeGuard([&] { Client.Close(); });
    for (const auto& Folder : {QString("System32"), QString("SysWOW64")}) {
        QProcess Child; Child.setProgram(Root + "/" + Folder + "/WindowsPowerShell/v1.0/powershell.exe");
        Child.setArguments({"-NoProfile", "-NonInteractive", "-Command", "Start-Sleep -Seconds 60"});
        auto Environment = QProcessEnvironment::systemEnvironment(); Environment.insert("HC_ENV_D202_PEB", "synthetic-process-value");
        Child.setProcessEnvironment(Environment); Child.setWorkingDirectory(QDir::tempPath());
        Child.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* Arguments) { Arguments->flags |= CREATE_NO_WINDOW; });
        Child.start(); QVERIFY(Child.waitForStarted());
        const auto Stop = qScopeGuard([&] { Child.kill(); Child.waitForFinished(5000); });
        const HANDLE Process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, DWORD(Child.processId())); QVERIFY(Process);
        FILETIME Created{}, Exited{}, Kernel{}, User{}; const bool Times = GetProcessTimes(Process, &Created, &Exited, &Kernel, &User); CloseHandle(Process); QVERIFY(Times);
        const hc::system::ProcessKey Key{quint32(Child.processId()), (quint64(Created.dwHighDateTime) << 32) | Created.dwLowDateTime};
        ProcessEnvironment Local;
        QTRY_VERIFY(([&] { Local = ReadProcessEnvironment(Key); return Local.status_ == ProcessReadStatus::Ok; })());
        QVERIFY(Local.values_.contains(L"HC_ENV_D202_PEB")); QCOMPARE(Local.values_.at(L"HC_ENV_D202_PEB"), std::wstring(L"synthetic-process-value"));
        QCOMPARE(Local.wow64_, Folder == "SysWOW64"); QVERIFY(Local.currentDirectory_.has_value());
        const auto Elevated = ReadRemoteEnvironment(Key, [&](const QString& Kind, QJsonObject Payload) { return Client.Request(Kind, std::move(Payload)); });
        QCOMPARE(Elevated.status_, ProcessReadStatus::Ok); QVERIFY(Elevated.values_.contains(L"HC_ENV_D202_PEB"));
        QCOMPARE(Elevated.values_.at(L"HC_ENV_D202_PEB"), std::wstring(L"synthetic-process-value"));
        auto Wrong = Key; ++Wrong.createTime; QVERIFY(ReadProcessEnvironment(Wrong).status_ != ProcessReadStatus::Ok);
        // Never print or compare the rest of a real process environment.
    }
}
QTEST_GUILESS_MAIN(EnvironmentVmTests)
