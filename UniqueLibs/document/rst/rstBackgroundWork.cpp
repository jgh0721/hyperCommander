#include "stdafx.h"
#include "rstBackgroundWork.hpp"

/// 모듈

/// UIs

/// 기타 헤더파일

/// 고유 라이브러리

/// 공통 라이브러리

/// 외부 라이브러리

/// 선언 헤더파일들

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QThreadPool>
#include <atomic>

DECLARE_CMNLIBSV2_NAMESPACE
namespace hc::rst {
namespace {

std::atomic_bool& shutdownFlag()
{
    static std::atomic_bool flag{ false };
    return flag;
}

}  // namespace

bool isShuttingDown()
{
    return shutdownFlag().load( std::memory_order_relaxed );
}

void requestShutdown()
{
    shutdownFlag().store( true, std::memory_order_relaxed );
}

void cancelShutdownRequest()
{
    shutdownFlag().store( false, std::memory_order_relaxed );
}

QThreadPool& persistencePool()
{
    // 함수 지역 static. QThreadPool 은 복사·이동이 되지 않으므로 제자리에서
    // 만들고 첫 호출에서 한 번만 설정한다. 스레드 수를 2로 묶는 이유는 이 풀의
    // 일이 순수 디스크 쓰기라 병렬로 늘려도 이득이 없고, 종료 시 기다려야 하는
    // 작업 수를 예측 가능하게 두는 편이 낫기 때문이다.
    static QThreadPool pool;
    static const bool configured = [] {
        pool.setMaxThreadCount( 2 );
        pool.setObjectName( QStringLiteral( "mrst.persistence" ) );
        return true;
    }();
    Q_UNUSED( configured );
    return pool;
}

}  // namespace hc::rst
