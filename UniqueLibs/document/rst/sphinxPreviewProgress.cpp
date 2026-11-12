#include "stdafx.h"
#include "sphinxPreviewProgress.hpp"

/// 모듈

/// UIs

/// 기타 헤더파일

/// 고유 라이브러리

/// 공통 라이브러리

/// 외부 라이브러리

/// 선언 헤더파일들

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QJsonDocument>
#include <QJsonObject>
#include <QLatin1String>

DECLARE_CMNLIBSV2_NAMESPACE
namespace hc::rst {

namespace {

/// 단계마다의 구간. 앞 단계의 끝이 뒤 단계의 시작이다.
///
/// 배분은 실제로 걸리는 시간을 눈대중한 것이다. 읽기가 가장 무겁고(디렉티브가
/// doxygen XML 을 훑는 문서는 이 단계에서만 수십 초가 걸린다), 쓰기는 그보다
/// 가볍고, WebEngine 이 HTML 을 읽는 마지막 단계는 그중 짧다. 준비 구간을 0 이
/// 아닌 값에서 시작하는 이유는, 막대가 완전히 빈 채로 멈춰 있으면 아무 일도
/// 일어나지 않는 것처럼 보이기 때문이다.
struct Span
{
    int begin;
    int end;
};

constexpr Span kPrepare{ 0, 50 };
constexpr Span kBuildRead{ 50, 450 };
constexpr Span kBuildWrite{ 450, 750 };
constexpr Span kLoad{ 750, 1000 };

constexpr Span spanOf( const PreviewPhase phase )
{
    switch( phase )
    {
        case PreviewPhase::Prepare:
            return kPrepare;
        case PreviewPhase::BuildRead:
            return kBuildRead;
        case PreviewPhase::BuildWrite:
            return kBuildWrite;
        case PreviewPhase::Load:
            return kLoad;
    }
    return kPrepare;
}

}  // namespace

PreviewBuildProgress parsePreviewProgressLine( const QString& line )
{
    PreviewBuildProgress progress;

    const auto tag = QLatin1String( kPreviewProgressTag );
    if( !line.startsWith( tag ) )
        return progress;

    const QByteArray payload = line.mid( tag.size() ).trimmed().toUtf8();
    const QJsonObject object = QJsonDocument::fromJson( payload ).object();
    if( object.isEmpty() )
        return progress;

    progress.phase = object.value( QStringLiteral( "phase" ) ).toString();
    progress.done  = object.value( QStringLiteral( "done" ) ).toInt( -1 );
    progress.total = object.value( QStringLiteral( "total" ) ).toInt( -1 );
    if( progress.phase.isEmpty() || progress.done < 0 || progress.total < 0 )
        return progress;

    progress.valid = true;
    return progress;
}

int previewOverallPermille( const PreviewPhase phase, const int done, const int total )
{
    const Span span = spanOf( phase );
    if( total <= 0 )
        return span.begin;

    // 빌더가 보내는 분모는 어림값이다. `html-page-context` 는 genindex·search 처럼
    // 문서 목록에 없는 페이지에도 오므로 done 이 total 을 넘을 수 있다. 넘긴 값을
    // 그대로 쓰면 이 단계가 다음 단계의 구간까지 먹는다.
    const int bounded = qBound( 0, done, total );
    return span.begin + static_cast< int >(
                   ( static_cast< qint64 >( span.end - span.begin ) * bounded ) / total );
}

PreviewPhase previewPhaseFromTag( const QString& tag )
{
    if( tag == QStringLiteral( "write" ) )
        return PreviewPhase::BuildWrite;
    if( tag == QStringLiteral( "load" ) )
        return PreviewPhase::Load;
    if( tag == QStringLiteral( "prepare" ) )
        return PreviewPhase::Prepare;
    return PreviewPhase::BuildRead;
}

}  // namespace hc::rst
