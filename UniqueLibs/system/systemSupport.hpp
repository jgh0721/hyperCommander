#pragma once

// 프로세스·서비스 보기의 공용 도우미: 설정, 분류 색, 표시 문자열, 비동기 실행, 관리자 도우미 세션.

#include <QColor>
#include <QFuture>
#include <QFutureWatcher>
#include <QJsonObject>
#include <QPointer>
#include <QString>
#include <QtConcurrent>

#include <functional>

#include "commonLibs/system/processActions.hpp"
#include "commonLibs/system/processInfo.hpp"
#include "commonLibs/system/serviceControl.hpp"

namespace hc::admin { class SystemSessionClient; }

namespace hc::sys {

struct Settings
{
    int        refreshMs              = 1000;
    bool       pauseWhenHidden        = true;
    int        highlightCycles        = 2;
    bool       confirmSingleTerminate = true;
    bool       gracefulClose          = true;
    int        gracefulWaitSec        = 5;
    bool       cycleCpu               = true;   // cpuMethod "auto"
    bool       showInterrupts         = true;
    bool       treeLines              = true;   // 프로세스 트리 연결선
    int        followDebounceMs       = 300;
    bool       problemExcludesDelayed = true;
    int        helperIdleMinutes      = 10;
    bool       autoStartHelper        = true;   // 보기를 열 때 관리자 도우미를 띄워 관리자 권한으로 조회
    bool       allUsers               = true;
    QJsonObject colors;
    static Settings Load();
    static QJsonObject Normalize( const QJsonObject& Value );
};

enum class Category : int { None, System, Own, Service, Packaged, Suspended, New, Exiting };

// 현재 테마 토큰(Surface·Accent·Danger·Ok·Paused·KImg)에서 분류 색을 파생한다. 사용자 지정 색이 우선한다.
QColor  CategoryColor( Category Value, const QJsonObject& Overrides = {} );
QString CategoryKey( Category Value );
QString CategoryLabel( Category Value );
QColor  DimColor();

QString FormatBytes( quint64 Bytes );
QString FormatRate( double BytesPerSecond );
QString FormatCpu( double Percent );
QString PriorityText( quint32 PriorityClass );
QString IntegrityText( hc::system::Integrity Value );
QString ServiceStateText( hc::system::ServiceState State );
QString StartTypeText( hc::system::StartType Type, bool Trigger = false );
QString ServiceTypeText( quint32 Type );
QString AccountText( const QString& Account );
QString OutcomeText( const QString& Outcome, const QString& Reason, quint32 NativeCode );
QString Win32ErrorText( quint32 Code );
QString FileTimeText( quint64 FileTime );

// 끝내기 등 동작의 허용 정책. HyperCommander 자신과 관리자 도우미는 차단, 다른 제품 프로세스는 경고 후 허용.
hc::system::ActionLimits ProcessLimits( bool AllowWarned );
bool IsProductImage( const QString& Path );

// 관리자 도우미 세션(앱 전체에서 하나). 첫 요청에서 UAC 확인 후 열린다.
hc::admin::SystemSessionClient& AdminSession();
bool AdminSessionConnected();

// 관리자 도우미가 연결되면(자동·수동 시작) 알린다. 보기는 표준 권한으로 읽지 못한 정보를 다시 조회한다.
class AdminSessionEvents final : public QObject
{
    Q_OBJECT
signals:
    void sigConnected();
};
AdminSessionEvents& AdminEvents();
// 표준 권한이고 아직 연결이 없으면 도우미를 띄운다(UAC 확인). 이미 연결되어 있으면 그대로 쓴다.
// 이번 실행에서 사용자가 UAC를 거절했으면 다시 묻지 않는다(수동 시작은 그대로 된다).
void EnsureAdminHelper();
// 수동 시작 등 다른 경로로 연결되었을 때 알린다.
void NotifyAdminConnected();

// Work를 스레드 풀에서 실행하고 Finished를 Context의 스레드(GUI)에서 부른다. Context가 사라지면 버린다.
template <typename Work, typename Finished>
void RunAsync( QObject* Context, Work&& Job, Finished&& Done )
{
    using Result = std::invoke_result_t<Work>;
    auto* Watcher = new QFutureWatcher<Result>( Context );
    QObject::connect( Watcher, &QFutureWatcher<Result>::finished, Context, [Watcher, Done = std::forward<Finished>( Done )]() mutable {
        Watcher->deleteLater();
        Done( Watcher->result() );
    } );
    Watcher->setFuture( QtConcurrent::run( std::forward<Work>( Job ) ) );
}

} // namespace hc::sys
