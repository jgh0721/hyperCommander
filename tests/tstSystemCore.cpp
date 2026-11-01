#include "stdafx.h"
#include "commonLibs/system/imageInfo.hpp"
#include "commonLibs/system/processActions.hpp"
#include "commonLibs/system/processInfo.hpp"
#include "commonLibs/system/processSnapshot.hpp"
#include "commonLibs/system/serviceControl.hpp"
#include "UniqueLibs/systemSessionService.hpp"

#include <QtTest>
#include <QJsonArray>
#include <QJsonDocument>
#include <QScopeGuard>

#include <atomic>
#include <thread>

using namespace hc::system;

namespace {
struct Child
{
    PROCESS_INFORMATION info{};
    explicit Child( const QString& Arguments )
    {
        const auto Program = QCoreApplication::applicationFilePath();
        auto Command = ( '"' + QDir::toNativeSeparators( Program ) + "\" " + Arguments ).toStdWString();
        STARTUPINFOW Startup{};
        Startup.cb = sizeof( Startup );
        CreateProcessW( reinterpret_cast<LPCWSTR>( Program.utf16() ), Command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &Startup, &info );
    }
    ~Child()
    {
        if( info.hProcess )
        {
            TerminateProcess( info.hProcess, 0 );
            WaitForSingleObject( info.hProcess, 5000 );
            CloseHandle( info.hProcess );
            CloseHandle( info.hThread );
        }
    }
    ProcessTarget Target() const
    {
        return { info.dwProcessId, ProcessCreateTime( info.hProcess ), ProcessImagePath( info.hProcess ) };
    }
};

const ProcessEntry* Find( const Snapshot& Value, uint32_t Pid )
{
    for( const auto& Entry : Value.processes )
        if( Entry.sample.key.pid == Pid )
            return &Entry;
    return nullptr;
}

void Spin( std::chrono::milliseconds Duration )
{
    const auto End = std::chrono::steady_clock::now() + Duration;
    volatile uint64_t Value = 0;
    while( std::chrono::steady_clock::now() < End )
        Value = Value + 1;
}
} // namespace

class SystemCoreTests : public QObject
{
    Q_OBJECT
private slots:
    void cpuFormulas()
    {
        CpuInterval Interval;
        Interval.elapsed100ns = 1.0e7;   // 1초
        Interval.processors = 4;
        QCOMPARE( CpuPercentByTime( 1.0e7, Interval ), 25.0 );  // 한 코어 전체
        QCOMPARE( CpuPercentByTime( 4.0e7, Interval ), 100.0 );
        QCOMPARE( CpuPercentByTime( 9.0e7, Interval ), 100.0 ); // 상한
        Interval.processCycles = 300;
        Interval.idleCycles = 700;
        QCOMPARE( CpuPercentByCycles( 100, Interval ), -1.0 );  // 사이클 정보 없음
        Interval.cyclesValid = true;
        QCOMPARE( CpuPercentByCycles( 100, Interval ), 10.0 );
        Interval.interruptShare = 0.5;
        QCOMPARE( CpuPercentByCycles( 100, Interval ), 5.0 );
        QVERIFY( CycleIntervalPlausible( Interval, 0.68 ) );
        QVERIFY( !CycleIntervalPlausible( Interval, 0.2 ) );    // 유휴 몫이 크게 어긋남
        QVERIFY( CycleIntervalPlausible( Interval, -1.0 ) );    // 시간 기준 없음
    }

    void snapshotMeasuresOwnLoad()
    {
        ProcessCollector Collector;
        Snapshot First, Second;
        QVERIFY( Collector.Sample( First ) );
        QVERIFY( First.baseline );
        QVERIFY( First.processes.size() > 10 );
        QVERIFY( Find( First, 0 ) && Find( First, 4 ) );
        Spin( std::chrono::milliseconds( 400 ) );
        QVERIFY( Collector.Sample( Second ) );
        QVERIFY( !Second.baseline );
        const auto* Self = Find( Second, GetCurrentProcessId() );
        QVERIFY( Self );
        QVERIFY2( Self->cpu > 0.0, qPrintable( QString::number( Self->cpu ) ) );
        // 한 스레드는 코어 하나를 넘지 못한다(측정 오차를 감안).
        QVERIFY2( Self->cpu <= 100.0 / Second.totals.processors * 1.5 + 1.0, qPrintable( QString::number( Self->cpu ) ) );
        double Sum = 0;
        for( const auto& Entry : Second.processes )
            if( Entry.cpu > 0 ) Sum += Entry.cpu;
        Sum += Second.totals.interruptCpu;
        QVERIFY2( Sum > 60.0 && Sum < 140.0, qPrintable( QString::number( Sum ) ) );
        QVERIFY( Second.totals.physicalTotal > 0 && Second.totals.commitLimit > 0 );
    }

    void detailsOfCurrentProcess()
    {
        ProcessDetails Details;
        QVERIFY( QueryProcessDetails( GetCurrentProcessId(), ProcessCreateTime( GetCurrentProcess() ), Details ) );
        QCOMPARE( QDir::cleanPath( QString::fromStdWString( Details.imagePath ) ).toLower(), QDir::cleanPath( QCoreApplication::applicationFilePath() ).toLower() );
        QCOMPARE( Details.userSid, CurrentUserSid() );
        QVERIFY( Details.integrity >= Integrity::Medium );
        QVERIFY( !Details.commandLine.empty() );
        QVERIFY( !QueryProcessDetails( GetCurrentProcessId(), 1, Details ) );  // 다른 생성 시각
    }

    void actionPolicy()
    {
        const auto Self = CheckAction( GetCurrentProcess(), GetCurrentProcessId(), 0, {}, {} );
        QCOMPARE( Self.verdict, Verdict::Blocked );
        QCOMPARE( QString::fromStdString( Self.reason ), QString( "self" ) );
        QCOMPARE( CheckAction( nullptr, 4, 0, {}, {} ).verdict, Verdict::Blocked );
        QVERIFY( IsSystemCriticalName( L"C:\\Windows\\System32\\csrss.exe" ) );
        QVERIFY( !IsSystemCriticalName( L"notepad.exe" ) );
        Child Sleeper( "--sleep" );
        QVERIFY( Sleeper.info.hProcess );
        const auto Name = QFileInfo( QCoreApplication::applicationFilePath() ).fileName().toStdWString();
        QCOMPARE( CheckAction( Sleeper.info.hProcess, Sleeper.info.dwProcessId, 0, {}, { Name } ).verdict, Verdict::Warn );
        QCOMPARE( CheckAction( Sleeper.info.hProcess, Sleeper.info.dwProcessId, 0, { Name }, {} ).verdict, Verdict::Blocked );
        QCOMPARE( CheckAction( Sleeper.info.hProcess, Sleeper.info.dwProcessId, 0, {}, {} ).verdict, Verdict::Allowed );
    }

    void suspendAndTerminateVerifyIdentity()
    {
        Child Sleeper( "--sleep" );
        QVERIFY( Sleeper.info.hProcess );
        const auto Target = Sleeper.Target();
        ActionLimits Limits;
        auto Wrong = Target;
        Wrong.createTime += 1;
        QCOMPARE( TerminateProcessTarget( Wrong, Limits ).outcome, Outcome::TargetChanged );
        QCOMPARE( SuspendProcessTarget( Target, true, Limits ).outcome, Outcome::Completed );
        ProcessCollector Collector;
        Snapshot Value;
        QVERIFY( Collector.Sample( Value ) );
        const auto* Entry = Find( Value, Target.pid );
        QVERIFY( Entry && Entry->sample.suspended );
        QCOMPARE( SuspendProcessTarget( Target, false, Limits ).outcome, Outcome::Completed );
        QCOMPARE( SetPriorityTarget( Target, BELOW_NORMAL_PRIORITY_CLASS, Limits ).outcome, Outcome::Completed );
        QCOMPARE( GetPriorityClass( Sleeper.info.hProcess ), DWORD( BELOW_NORMAL_PRIORITY_CLASS ) );
        QCOMPARE( TerminateProcessTarget( Target, Limits ).outcome, Outcome::Completed );
        QCOMPARE( TerminateProcessTarget( Target, Limits ).outcome, Outcome::AlreadyGone );
    }

    void commandSplitting()
    {
        std::wstring File, Arguments;
        SplitCommand( L"  \"C:\\Program Files\\App\\a.exe\" -x \"y z\"", File, Arguments );
        QCOMPARE( File, std::wstring( L"C:\\Program Files\\App\\a.exe" ) );
        QCOMPARE( Arguments, std::wstring( L"-x \"y z\"" ) );
        SplitCommand( L"pwsh.exe -NoLogo", File, Arguments );
        QCOMPARE( File, std::wstring( L"pwsh.exe" ) );
        QCOMPARE( Arguments, std::wstring( L"-NoLogo" ) );
        SplitCommand( L"notepad", File, Arguments );
        QCOMPARE( File, std::wstring( L"notepad" ) );
        QVERIFY( Arguments.empty() );
    }

    void serviceImagePaths()
    {
        wchar_t Windows[MAX_PATH]{};
        const auto Root = QString::fromWCharArray( Windows, GetWindowsDirectoryW( Windows, MAX_PATH ) );
        const auto Svchost = QDir::toNativeSeparators( Root + "/System32/svchost.exe" ).toLower();
        QCOMPARE( QString::fromStdWString( ResolveServiceImage( L"%SystemRoot%\\System32\\svchost.exe -k netsvcs -p" ) ).toLower(), Svchost );
        QCOMPARE( QString::fromStdWString( ResolveServiceImage( L"\"C:\\Program Files\\Vendor\\svc.exe\" /run" ) ), QString( "C:\\Program Files\\Vendor\\svc.exe" ) );
        QCOMPARE( QString::fromStdWString( ResolveServiceImage( L"\\SystemRoot\\System32\\drivers\\null.sys" ) ).toLower(),
                  QDir::toNativeSeparators( Root + "/System32/drivers/null.sys" ).toLower() );
        QCOMPARE( QString::fromStdWString( ResolveServiceImage( L"\\??\\C:\\x\\y.sys" ) ), QString( "C:\\x\\y.sys" ) );
        QCOMPARE( QString::fromStdWString( ResolveServiceImage( L"System32\\svchost.exe -k x" ) ).toLower(), Svchost );
    }

    void servicesEnumerateAndQuery()
    {
        std::vector<ServiceStatusInfo> List;
        QVERIFY( EnumerateServices( false, List ) );
        QVERIFY( List.size() > 20 );
        const auto Found = std::find_if( List.begin(), List.end(), [] ( const auto& Item ) { return _wcsicmp( Item.name.c_str(), L"EventLog" ) == 0; } );
        QVERIFY( Found != List.end() );
        QCOMPARE( Found->state, ServiceState::Running );
        QVERIFY( Found->pid > 0 );
        ServiceConfig Config;
        QVERIFY( QueryServiceConfiguration( L"EventLog", Config ) );
        QCOMPARE( Config.start, StartType::Auto );
        QVERIFY( !Config.resolvedImage.empty() );
        std::vector<ServiceStatusInfo> Drivers;
        QVERIFY( EnumerateServices( true, Drivers ) );
        QVERIFY( Drivers.size() > List.size() );
        // 표준 권한으로 서비스 제어는 거부되거나 없는 서비스 오류가 된다.
        QCOMPARE( ControlServiceByName( L"HcNoSuchService-0F3B", ServiceControl::Start ), uint32_t( ERROR_SERVICE_DOES_NOT_EXIST ) );
    }

    void serviceMonitorDelivers()
    {
        ServiceMonitor Monitor;
        std::atomic_int Calls{ 0 };
        std::atomic_size_t Count{ 0 };
        Monitor.Start( false, [&] ( std::vector<ServiceStatusInfo> List ) { Count = List.size(); ++Calls; } );
        QTRY_VERIFY_WITH_TIMEOUT( Calls.load() > 0, 10000 );
        QVERIFY( Count.load() > 20 );
        Monitor.Stop();
    }

    void imageInformation()
    {
        wchar_t System[MAX_PATH]{};
        GetSystemDirectoryW( System, MAX_PATH );
        const auto Kernel = std::wstring( System ) + L"\\kernel32.dll";
        VersionInfo Version;
        QVERIFY( ReadVersionInfo( Kernel, Version ) );
        QVERIFY( QString::fromStdWString( Version.company ).contains( "Microsoft" ) );
        const auto Notepad = std::wstring( System ) + L"\\notepad.exe";
        if( QFileInfo::exists( QString::fromStdWString( Notepad ) ) )
            QCOMPARE( VerifySignature( Kernel ).state, SignatureState::Signed );
    }

    void systemSessionProtocol()
    {
        HANDLE Observed = nullptr;
        const auto Cleanup = qScopeGuard( [&] { if( Observed ) CloseHandle( Observed ); } );
        auto Launcher = [&] ( const QString& Program, const QString& Arguments, bool ) -> void* {
            auto Command = ( '"' + QDir::toNativeSeparators( Program ) + "\" " + Arguments ).toStdWString();
            STARTUPINFOW Startup{};
            Startup.cb = sizeof( Startup );
            PROCESS_INFORMATION Process{};
            if( !CreateProcessW( reinterpret_cast<LPCWSTR>( Program.utf16() ), Command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &Startup, &Process ) )
                return nullptr;
            CloseHandle( Process.hThread );
            DuplicateHandle( GetCurrentProcess(), Process.hProcess, GetCurrentProcess(), &Observed, 0, FALSE, DUPLICATE_SAME_ACCESS );
            return Process.hProcess;
        };
        hc::admin::SystemSessionClient Client( QCoreApplication::applicationFilePath(), Launcher, false );
        QCOMPARE( Client.Request( "ping", {} )["outcome"].toString(), QString( "Completed" ) );
        QVERIFY( Client.Connected() );
        Child Sleeper( "--sleep" );
        const auto Target = Sleeper.Target();
        const QJsonObject TargetJson{ { "pid", qint64( Target.pid ) }, { "created", QString::number( Target.createTime ) }, { "image", QString::fromStdWString( Target.image ) } };
        auto Wrong = TargetJson;
        Wrong["created"] = QString::number( Target.createTime + 1 );
        auto Reply = Client.Request( "process.terminate", { { "targets", QJsonArray{ Wrong } } } );
        QCOMPARE( Reply["results"].toArray().first().toObject()["outcome"].toString(), QString( "TargetChanged" ) );
        Reply = Client.Request( "process.suspend", { { "target", TargetJson } } );
        QCOMPARE( Reply["outcome"].toString(), QString( "Completed" ) );
        Reply = Client.Request( "process.terminate", { { "targets", QJsonArray{ TargetJson } } } );
        QCOMPARE( Reply["results"].toArray().first().toObject()["outcome"].toString(), QString( "Completed" ) );
        Reply = Client.Request( "service.control", { { "name", "HcNoSuchService-0F3B" }, { "control", "start" } } );
        QCOMPARE( Reply["nativeCode"].toString(), QString::number( ERROR_SERVICE_DOES_NOT_EXIST ) );
        Reply = Client.Request( "unknown.kind", {} );
        QCOMPARE( Reply["outcome"].toString(), QString( "Rejected" ) );
        // 도우미가 끝나면 다음 요청은 새 세션을 연다.
        QVERIFY( Observed );
        TerminateProcess( Observed, 0 );
        WaitForSingleObject( Observed, 5000 );
        CloseHandle( Observed );
        Observed = nullptr;
        QVERIFY( !Client.Connected() );
        QCOMPARE( Client.Request( "ping", {} )["outcome"].toString(), QString( "Completed" ) );
    }

    void systemSessionRejectsUntrustedCaller()
    {
        QJsonObject Bootstrap{ { "version", 1 }, { "nonce", QUuid::createUuid().toString( QUuid::WithoutBraces ) }, { "pid", int( GetCurrentProcessId() ) },
                               { "created", QString::number( ProcessCreateTime( GetCurrentProcess() ) ) }, { "callerSid", QString::fromStdWString( CurrentUserSid() ) },
                               { "utc", QDateTime::currentDateTimeUtc().toString( Qt::ISODateWithMs ) }, { "elevated", false } };
        QCOMPARE( hc::admin::ExecuteSystemSession( Bootstrap, QDir::temp().filePath( "untrusted.exe" ), false ), 0UL + ERROR_ACCESS_DENIED );
        QCOMPARE( hc::admin::ExecuteSystemSession( Bootstrap ), 0UL + ERROR_INVALID_PARAMETER );  // 제품 경로는 관리자 권한 세션만
        Bootstrap["utc"] = QDateTime::currentDateTimeUtc().addSecs( -600 ).toString( Qt::ISODateWithMs );
        QCOMPARE( hc::admin::ExecuteSystemSession( Bootstrap, QCoreApplication::applicationFilePath(), false ), 0UL + ERROR_INVALID_PARAMETER );
    }
};

int main( int Argc, char** Argv )
{
    QCoreApplication App( Argc, Argv );
    const auto Args = App.arguments();
    if( Args.size() == 2 && Args[1] == "--sleep" )
    {
        Sleep( 60000 );
        return 0;
    }
    if( Args.size() == 3 && Args[1] == "--system-session" )
    {
        const auto Request = QJsonDocument::fromJson( QByteArray::fromBase64( Args[2].toLatin1(), QByteArray::Base64UrlEncoding ) ).object();
        return int( hc::admin::ExecuteSystemSession( Request, QCoreApplication::applicationFilePath(), false ) );
    }
    SystemCoreTests Tests;
    return QTest::qExec( &Tests, Argc, Argv );
}
#include "tstSystemCore.moc"
