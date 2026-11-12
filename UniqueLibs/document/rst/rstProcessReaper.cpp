#include "stdafx.h"
#include "rstProcessReaper.hpp"

/// 모듈

/// UIs

/// 기타 헤더파일

/// 고유 라이브러리

/// 공통 라이브러리

/// 외부 라이브러리

/// 선언 헤더파일들

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QProcess>
#include <QTimer>

DECLARE_CMNLIBSV2_NAMESPACE
#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace hc::rst {

#ifdef Q_OS_WIN

namespace {

/// 프로세스 전체에서 하나만 쓰는 Job. 앱이 끝나면 핸들이 닫히고, 그 순간
/// 이 Job 에 속한 모든 프로세스(손자 포함)가 커널에 의해 종료된다.
HANDLE killOnExitJob()
{
    static HANDLE job = [] () -> HANDLE {
        HANDLE created = CreateJobObjectW( nullptr, nullptr );
        if( created == nullptr )
            return nullptr;

        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if( !SetInformationJobObject( created, JobObjectExtendedLimitInformation,
                                     &limits, sizeof( limits ) ) )
        {
            CloseHandle( created );
            return nullptr;
        }
        return created;
    }();

    return job;
}

}  // namespace

void assignToKillOnExitJob( const qint64 processId )
{
    if( processId <= 0 )
        return;

    HANDLE job = killOnExitJob();
    if( job == nullptr )
        return;

    HANDLE process = OpenProcess( PROCESS_SET_QUOTA | PROCESS_TERMINATE, FALSE,
                                 static_cast< DWORD >( processId ) );
    if( process == nullptr )
        return;

    // 이미 다른 Job 에 속해 있으면 실패할 수 있다. 그 경우는 그냥 넘어간다.
    AssignProcessToJobObject( job, process );
    CloseHandle( process );
}

bool isProcessRunning( const qint64 processId )
{
    if( processId <= 0 )
        return false;

    HANDLE process = OpenProcess( SYNCHRONIZE, FALSE, static_cast< DWORD >( processId ) );
    // 권한이 모자라 열 수 없는 프로세스( 관리자 권한으로 실행한 다른 인스턴스 등 )도 살아 있다.
    // 이것을 "없음" 으로 보면 그 인스턴스가 쓰는 폴더를 지우게 된다.
    if( process == nullptr )
        return GetLastError() == ERROR_ACCESS_DENIED;

    // 핸들이 열려도 이미 끝난 프로세스일 수 있다 (좀비). 대기 상태로 구분한다.
    const DWORD wait = WaitForSingleObject( process, 0 );
    CloseHandle( process );
    return wait == WAIT_TIMEOUT;
}

#else

void assignToKillOnExitJob( qint64 )
{
}

bool isProcessRunning( qint64 )
{
    return false;
}

#endif

void abandonProcess( std::unique_ptr< QProcess > process )
{
    if( process == nullptr )
        return;

    // Completed (including failed-to-start) processes have no destructor wait.
    if( process->state() == QProcess::NotRunning )
        return;

    QProcess* raw = process.release();
    raw->disconnect();
    raw->setParent( nullptr );
    // Keep the object alive only until the child stops. State changes also cover
    // FailedToStart, which does not emit finished(). No synchronous wait is used.
    QObject::connect( raw, &QProcess::stateChanged, raw,
                      [raw]( QProcess::ProcessState state ) {
        if( state == QProcess::NotRunning )
            raw->deleteLater();
    } );
    raw->kill();

}

void reapProcessLater( std::unique_ptr< QProcess > process, const int graceMs )
{
    if( process == nullptr )
        return;

    QProcess* raw = process.release();
    // 옛 소유자에게 가던 신호를 끊는다. 아래에서 우리 것만 다시 건다.
    raw->disconnect();

    // 프로세스가 끝난 **뒤에만** 객체를 지운다. 그래야 ~QProcess 가
    // waitForFinished() 로 넘어가지 않는다.
    QObject::connect( raw, &QProcess::finished, raw, [raw] { raw->deleteLater(); } );

    if( raw->state() == QProcess::NotRunning )
    {
        raw->deleteLater();
        return;
    }

    // 유예 시간이 지나도 살아 있으면 죽인다. 컨텍스트 객체가 raw 자신이라
    // 그 전에 지워지면 타이머도 함께 사라진다.
    QTimer::singleShot( graceMs, raw, [raw] {
        if( raw->state() != QProcess::NotRunning )
            raw->kill();   // 뒤이어 오는 finished 가 위 연결로 정리를 마무리한다
    } );
}

}  // namespace hc::rst
