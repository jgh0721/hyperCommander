#include "stdafx.h"
#include "rstPhaseTrace.hpp"

/// 모듈

/// UIs

/// 기타 헤더파일

/// 고유 라이브러리

/// 공통 라이브러리

/// 외부 라이브러리

/// 선언 헤더파일들

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QByteArray>
#include <QElapsedTimer>
#include <QMutex>
#include <cstdio>

DECLARE_CMNLIBSV2_NAMESPACE
namespace hc::rst {
namespace {

/// 환경 변수는 한 번만 읽는다. 비어 있으면 이 트레이스는 통째로 꺼진 것이다.
/// (killOnExitJob() / traceLsp() 와 같은 함수 지역 static 관용구.)
const QByteArray& tracePath()
{
    static const QByteArray path = qgetenv( "HC_RST_PHASE_TRACE" ).trimmed();
    return path;
}

/// 원점. startPhaseClock() 이 없어도 첫 traceP() 가 세운다.
QElapsedTimer& clock()
{
    static QElapsedTimer timer;
    if( !timer.isValid() )
        timer.start();
    return timer;
}

/// 게이트 판정과 파일 스캔은 워커 스레드에서 돌므로 줄이 섞이지 않게 막는다.
QMutex& writeLock()
{
    static QMutex mutex;
    return mutex;
}

}  // namespace

bool phaseTraceEnabled()
{
    return !tracePath().isEmpty();
}

void startPhaseClock()
{
    if( tracePath().isEmpty() )
        return;
    clock().restart();
}

void traceP( const char* tag, const QString& detail )
{
    if( tracePath().isEmpty() || tag == nullptr )
        return;

    const qint64 elapsed = clock().elapsed();
    const QByteArray detailUtf8 = detail.toUtf8();

    const QMutexLocker locker( &writeLock() );
    // QFile 대신 fopen 을 쓰는 이유는 헤더 주석에 있다. 매 호출 닫으므로
    // 크래시나 강제 종료에도 앞부분이 남는다.
    std::FILE* file = std::fopen( tracePath().constData(), "ab" );
    if( file == nullptr )
        return;

    std::fprintf( file, "%8lld\t%s\t%s\n", static_cast< long long >( elapsed ), tag,
                 detailUtf8.constData() );
    std::fclose( file );
}

PhaseSpan::PhaseSpan( const char* tag, QString detail )
    : tag_( tag )
    , detail_( std::move( detail ) )
{
    if( tracePath().isEmpty() || tag_ == nullptr )
        return;

    startedAtNs_ = clock().nsecsElapsed();
    traceP( QByteArray( tag_ ).append( ".begin" ).constData(), detail_ );
}

PhaseSpan::~PhaseSpan()
{
    if( tracePath().isEmpty() || tag_ == nullptr )
        return;

    // ns 로 재서 ms 로 낸다. 정수 ms 로는 이 계측의 절반이 0 으로 나온다 —
    // 줄넘김 토글 한 구획이나 상태바 갱신은 애초에 1 ms 미만이고, 그 안에서
    // 무엇이 비싼지 가르는 것이 목적이다.
    const double tookMs = static_cast< double >( clock().nsecsElapsed() - startedAtNs_ ) / 1e6;
    QString detail = QStringLiteral( "%1ms" ).arg( tookMs, 0, 'f', 3 );
    if( !detail_.isEmpty() )
        detail += QLatin1Char( ' ' ) + detail_;
    traceP( QByteArray( tag_ ).append( ".end" ).constData(), detail );
}

}  // namespace hc::rst
