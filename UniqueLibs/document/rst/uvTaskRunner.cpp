#include "stdafx.h"
#include "uvTaskRunner.hpp"

/// 모듈
#include "rstProcessReaper.hpp"

/// UIs

/// 기타 헤더파일

/// 고유 라이브러리

/// 공통 라이브러리

/// 외부 라이브러리

/// 선언 헤더파일들

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QRegularExpression>
#include <QTimer>

DECLARE_CMNLIBSV2_NAMESPACE
namespace hc::rst {
namespace {

/// terminate() 이후 kill() 까지 기다리는 시간.
constexpr int kKillGraceMs = 800;

}  // namespace

QString stripAnsiEscapes( const QString& text )
{
    // CSI 시퀀스( ESC [ ... 종결문자 )와 OSC 시퀀스( ESC ] ... BEL )를 제거한다.
    static const QRegularExpression csi( QStringLiteral( "\x1B\\[[0-9;?]*[ -/]*[@-~]" ) );
    static const QRegularExpression osc( QStringLiteral( "\x1B\\][^\x07\x1B]*(?:\x07|\x1B\\\\)" ) );

    QString cleaned = text;
    cleaned.remove( csi );
    cleaned.remove( osc );
    return cleaned;
}

QProcessEnvironment utf8ProcessEnvironment()
{
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert( QStringLiteral( "PYTHONUTF8" ), QStringLiteral( "1" ) );
    env.insert( QStringLiteral( "PYTHONIOENCODING" ), QStringLiteral( "utf-8" ) );
    // 색상 자체를 끄면 ANSI 제거 부담이 줄어든다.
    env.insert( QStringLiteral( "NO_COLOR" ), QStringLiteral( "1" ) );
    return env;
}

UvTask::UvTask( Request request, QObject* parent )
    : QObject( parent )
    , request_( std::move( request ) )
{
    if( request_.environment.isEmpty() )
        request_.environment = utf8ProcessEnvironment();
}

UvTask::~UvTask()
{
    // 예전에는 kill() + waitForFinished(200) 이었다. 200ms 안에 안 죽으면
    // 이어서 ~QProcess 가 waitForFinished() 를 **30초 기본값**으로 부른다.
    // abandonProcess() 는 실행 중인 객체를 종료 신호 뒤에 지워 대기를 피한다.
    abandonProcess( std::move( process_ ) );
}

QString UvTask::tag() const
{
    return request_.tag;
}

QString UvTask::collectedOutput() const
{
    return collected_;
}

bool UvTask::isRunning() const
{
    return process_ != nullptr && process_->state() != QProcess::NotRunning;
}

bool UvTask::wasCancelled() const
{
    return cancelled_;
}

void UvTask::start()
{
    if( process_ != nullptr )
        return;

    process_ = std::make_unique< QProcess >();
    process_->setProgram( request_.program );
    process_->setArguments( request_.arguments );
    if( !request_.workingDirectory.isEmpty() )
        process_->setWorkingDirectory( request_.workingDirectory );
    process_->setProcessEnvironment( request_.environment );
    process_->setProcessChannelMode( request_.mergeChannels ? QProcess::MergedChannels
                                                            : QProcess::SeparateChannels );

    connect( process_.get(), &QProcess::readyReadStandardOutput, this, [this] { drain( false ); } );
    if( !request_.mergeChannels )
        connect( process_.get(), &QProcess::readyReadStandardError, this, [this] { drain( false ); } );
    connect( process_.get(), &QProcess::finished, this, &UvTask::onFinished );
    connect( process_.get(), &QProcess::errorOccurred, this, &UvTask::onErrorOccurred );
    connect( process_.get(), &QProcess::started, this, [this] {
        // 앱이 죽으면 같이 죽는 그룹에 넣는다. 지금까지는 Esbonio 만 등록돼
        // 있었는데, 프리뷰 빌더도 sphinx 확장을 통해 손자를 띄울 수 있고
        // uv sync 는 python 다운로더를 띄운다. 등록하지 않으면 앱이 사라진 뒤에도
        // 남아 Environment/ 를 물고 있어 업데이터의 파일 교체를 막는다.
        //
        // waitForStarted() 는 쓸 수 없다(이 클래스의 존재 이유가 그것이다).
        // processId() 는 started 시점에 유효하다.
        assignToKillOnExitJob( process_->processId() );
        emit started();
    } );

    process_->start();
}

void UvTask::cancel()
{
    if( cancelled_ || !isRunning() )
        return;

    cancelled_ = true;
    process_->terminate();

    QPointer< UvTask > guard( this );
    QTimer::singleShot( kKillGraceMs, this, [guard] {
        if( guard && guard->isRunning() )
            guard->process_->kill();
    } );
}

void UvTask::killNow()
{
    if( process_ == nullptr )
        return;

    cancelled_ = true;
    // finished 를 내보내지 않는다. 종료 경로라 수신자(컨트롤러/매니저)가
    // 곧 사라지고, 여기서 온 신호로 새 작업이 시작되면 안 된다.
    finishedEmitted_ = true;
    abandonProcess( std::move( process_ ) );
}

void UvTask::drain( bool flushPartial )
{
    if( process_ == nullptr )
        return;

    pending_ += QString::fromUtf8( process_->readAllStandardOutput() );
    if( !request_.mergeChannels )
        pending_ += QString::fromUtf8( process_->readAllStandardError() );

    // uv 는 진행률을 CR 로 덮어쓰므로 CR 도 줄 구분자로 취급한다.
    pending_.replace( QLatin1Char( '\r' ), QLatin1Char( '\n' ) );

    // 버퍼 앞을 잘라내지 않고 오프셋만 옮긴다.
    //
    // 예전에는 줄마다 `pending_.remove( 0, newline + 1 )` 이었다. QString::remove
    // 는 뒤쪽 전체를 앞으로 memmove 하므로, 한 번에 도착한 덩어리가 n줄이면
    // 비용이 O(n^2) 이다. Sphinx 빌더는 한 번에 수천 줄을 뱉는다.
    qsizetype consumed = 0;
    qsizetype newline = pending_.indexOf( QLatin1Char( '\n' ) );
    while( newline >= 0 )
    {
        const QString line
            = stripAnsiEscapes( pending_.mid( consumed, newline - consumed ) ).trimmed();
        consumed = newline + 1;
        if( !line.isEmpty() )
        {
            collected_ += line;
            collected_ += QLatin1Char( '\n' );
            emit outputLine( line );
        }
        newline = pending_.indexOf( QLatin1Char( '\n' ), consumed );
    }
    if( consumed > 0 )
        pending_.remove( 0, consumed );   // 남은 부분 조각만 앞으로 옮긴다 (1회)

    if( flushPartial && !pending_.isEmpty() )
    {
        const QString line = stripAnsiEscapes( pending_ ).trimmed();
        pending_.clear();
        if( !line.isEmpty() )
        {
            collected_ += line;
            collected_ += QLatin1Char( '\n' );
            emit outputLine( line );
        }
    }
}

void UvTask::onFinished( int exitCode, QProcess::ExitStatus status )
{
    if( finishedEmitted_ )
        return;

    finishedEmitted_ = true;
    drain( true );
    emit finished( exitCode, status == QProcess::CrashExit );
}

void UvTask::onErrorOccurred( QProcess::ProcessError error )
{
    if( error != QProcess::FailedToStart )
        return;   // 그 외 오류는 finished 로 이어진다.

    if( finishedEmitted_ )
        return;

    finishedEmitted_ = true;
    emit failedToStart( tr( "실행할 수 없습니다: %1" ).arg( QDir::toNativeSeparators( request_.program ) ) );
}

}  // namespace hc::rst
