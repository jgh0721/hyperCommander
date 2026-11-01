#include "stdafx.h"
#include "systemSupport.hpp"

/// 내부 라이브러리
#include "UniqueLibs/productDefaults.hpp"
#include "UniqueLibs/settings/appSettings.hpp"
#include "UniqueLibs/permissionService.hpp"
#include "UniqueLibs/systemSessionService.hpp"

/// 플랫폼
#include <winsvc.h>

/// 외부 라이브러리
#include <fmstyle/ThemeManager.h>

namespace hc::sys {
namespace {
QColor Mix( const QColor& Base, const QColor& Tint, double Amount )
{
    return QColor::fromRgbF( Base.redF() + ( Tint.redF() - Base.redF() ) * Amount, Base.greenF() + ( Tint.greenF() - Base.greenF() ) * Amount,
                             Base.blueF() + ( Tint.blueF() - Base.blueF() ) * Amount );
}

bool DarkTheme()
{
    const auto& Colors = fm::style::ThemeManager::instance().colors();
    return Colors[fm::style::Token::Surface].lightnessF() < 0.5;
}
} // namespace

Settings Settings::Load()
{
    const auto Object = Normalize( hc::settings::AppSettings().Read( hc::settings::Section::General ).toObject()["processService"].toObject() );
    Settings Value;
    Value.refreshMs = Object["refreshMs"].toInt();
    Value.pauseWhenHidden = Object["pauseWhenHidden"].toBool();
    Value.highlightCycles = Object["highlightCycles"].toInt();
    Value.confirmSingleTerminate = Object["confirmSingleTerminate"].toBool();
    Value.gracefulClose = Object["gracefulClose"].toBool();
    Value.gracefulWaitSec = Object["gracefulWaitSec"].toInt();
    Value.cycleCpu = Object["cpuMethod"].toString() != "time";
    Value.showInterrupts = Object["showInterrupts"].toBool();
    Value.treeLines = Object["treeLines"].toBool();
    Value.followDebounceMs = Object["followDebounceMs"].toInt();
    Value.problemExcludesDelayed = Object["serviceProblemExcludesDelayed"].toBool();
    Value.helperIdleMinutes = Object["helperIdleMinutes"].toInt();
    Value.autoStartHelper = Object["autoStartHelper"].toBool();
    Value.allUsers = Object["allUsers"].toBool();
    Value.colors = Object["colors"].toObject();
    return Value;
}

QJsonObject Settings::Normalize( const QJsonObject& Value )
{
    auto Result = hc::defaults::ProcessService();
    for( auto It = Value.begin(); It != Value.end(); ++It )
        if( Result.contains( It.key() ) && Result[It.key()].type() == It.value().type() )
            Result[It.key()] = It.value();
    static const QList<int> Intervals{ 500, 1000, 2000, 5000 };
    if( !Intervals.contains( Result["refreshMs"].toInt() ) ) Result["refreshMs"] = 1000;
    Result["highlightCycles"] = std::clamp( Result["highlightCycles"].toInt(), 0, 10 );
    Result["gracefulWaitSec"] = std::clamp( Result["gracefulWaitSec"].toInt(), 1, 60 );
    Result["followDebounceMs"] = std::clamp( Result["followDebounceMs"].toInt(), 100, 3000 );
    Result["helperIdleMinutes"] = std::clamp( Result["helperIdleMinutes"].toInt(), 1, 120 );
    if( Result["cpuMethod"].toString() != "time" ) Result["cpuMethod"] = "auto";
    return Result;
}

QString CategoryKey( Category Value )
{
    switch( Value )
    {
        case Category::Own:       return "own";
        case Category::Service:   return "service";
        case Category::Packaged:  return "packaged";
        case Category::Suspended: return "suspended";
        case Category::New:       return "new";
        case Category::Exiting:   return "exiting";
        case Category::System:    return "system";
        default:                  return {};
    }
}

QString CategoryLabel( Category Value )
{
    switch( Value )
    {
        case Category::Own:       return QObject::tr( "사용자" );
        case Category::Service:   return QObject::tr( "서비스" );
        case Category::Packaged:  return QObject::tr( "패키지 앱" );
        case Category::Suspended: return QObject::tr( "일시 중단" );
        case Category::New:       return QObject::tr( "새 프로세스" );
        case Category::Exiting:   return QObject::tr( "종료 중" );
        case Category::System:    return QObject::tr( "시스템" );
        default:                  return {};
    }
}

QColor CategoryColor( Category Value, const QJsonObject& Overrides )
{
    if( Value == Category::None || Value == Category::System )
        return {};
    const auto Custom = Overrides[CategoryKey( Value )].toString();
    if( QColor::isValidColorName( Custom ) )
        return QColor::fromString( Custom );
    using fm::style::Token;
    const auto& Colors = fm::style::ThemeManager::instance().colors();
    const bool Dark = DarkTheme();
    const auto Surface = Colors[Token::Surface];
    switch( Value )
    {
        case Category::Own:       return Mix( Surface, Colors[Token::Accent], Dark ? 0.24 : 0.14 );
        case Category::Service:   return Mix( Surface, Colors[Token::KPdf], Dark ? 0.22 : 0.13 );
        case Category::Packaged:  return Mix( Surface, Colors[Token::KImg], Dark ? 0.24 : 0.15 );
        case Category::Suspended: return Mix( Surface, Colors[Token::Paused], Dark ? 0.28 : 0.24 );
        case Category::New:       return Mix( Surface, Colors[Token::Ok], Dark ? 0.30 : 0.22 );
        case Category::Exiting:   return Mix( Surface, Colors[Token::Danger], Dark ? 0.34 : 0.26 );
        default:                  return {};
    }
}

QColor DimColor()
{
    return fm::style::ThemeManager::instance().colors()[fm::style::Token::Fg3];
}

QString FormatBytes( quint64 Bytes )
{
    if( Bytes < 1024 ) return QObject::tr( "%1 B" ).arg( Bytes );
    const double Kilo = Bytes / 1024.0;
    if( Kilo < 1024.0 ) return QObject::tr( "%1 KB" ).arg( Kilo, 0, 'f', 0 );
    const double Mega = Kilo / 1024.0;
    if( Mega < 1024.0 ) return QObject::tr( "%1 MB" ).arg( Mega, 0, 'f', 1 );
    return QObject::tr( "%1 GB" ).arg( Mega / 1024.0, 0, 'f', 2 );
}

QString FormatRate( double BytesPerSecond )
{
    if( BytesPerSecond < 1.0 ) return {};
    return FormatBytes( quint64( BytesPerSecond ) ) + QObject::tr( "/s" );
}

QString FormatCpu( double Percent )
{
    if( Percent < 0.0 ) return {};
    if( Percent == 0.0 ) return {};
    if( Percent < 0.1 ) return QStringLiteral( "< 0.1" );
    return QString::number( Percent, 'f', 1 );
}

QString PriorityText( quint32 PriorityClass )
{
    switch( PriorityClass )
    {
        case REALTIME_PRIORITY_CLASS:     return QObject::tr( "실시간" );
        case HIGH_PRIORITY_CLASS:         return QObject::tr( "높음" );
        case ABOVE_NORMAL_PRIORITY_CLASS: return QObject::tr( "보통 초과" );
        case NORMAL_PRIORITY_CLASS:       return QObject::tr( "보통" );
        case BELOW_NORMAL_PRIORITY_CLASS: return QObject::tr( "보통 미만" );
        case IDLE_PRIORITY_CLASS:         return QObject::tr( "낮음" );
        default:                          return {};
    }
}

QString IntegrityText( hc::system::Integrity Value )
{
    using hc::system::Integrity;
    switch( Value )
    {
        case Integrity::Untrusted:  return QObject::tr( "신뢰할 수 없음" );
        case Integrity::Low:        return QObject::tr( "낮음" );
        case Integrity::Medium:     return QObject::tr( "보통" );
        case Integrity::MediumPlus: return QObject::tr( "보통+" );
        case Integrity::High:       return QObject::tr( "높음" );
        case Integrity::System:     return QObject::tr( "시스템" );
        case Integrity::Protected:  return QObject::tr( "보호" );
        default:                    return {};
    }
}

QString ServiceStateText( hc::system::ServiceState State )
{
    using hc::system::ServiceState;
    switch( State )
    {
        case ServiceState::Stopped:         return QObject::tr( "중지됨" );
        case ServiceState::StartPending:    return QObject::tr( "시작 중…" );
        case ServiceState::StopPending:     return QObject::tr( "중지 중…" );
        case ServiceState::Running:         return QObject::tr( "실행 중" );
        case ServiceState::ContinuePending: return QObject::tr( "계속하는 중…" );
        case ServiceState::PausePending:    return QObject::tr( "일시 중지하는 중…" );
        case ServiceState::Paused:          return QObject::tr( "일시 중지됨" );
        default:                            return QObject::tr( "알 수 없음" );
    }
}

QString StartTypeText( hc::system::StartType Type, bool Trigger )
{
    using hc::system::StartType;
    QString Text;
    switch( Type )
    {
        case StartType::Boot:        Text = QObject::tr( "부팅" ); break;
        case StartType::System:      Text = QObject::tr( "시스템" ); break;
        case StartType::Auto:        Text = QObject::tr( "자동" ); break;
        case StartType::AutoDelayed: Text = QObject::tr( "자동(지연된 시작)" ); break;
        case StartType::Manual:      Text = QObject::tr( "수동" ); break;
        case StartType::Disabled:    Text = QObject::tr( "사용 안 함" ); break;
        default:                     return {};
    }
    if( Trigger && Type != StartType::AutoDelayed && Type != StartType::Disabled )
        Text += QObject::tr( "(트리거 시작)" );
    return Text;
}

QString ServiceTypeText( quint32 Type )
{
    if( Type & SERVICE_KERNEL_DRIVER ) return QObject::tr( "커널 드라이버" );
    if( Type & SERVICE_FILE_SYSTEM_DRIVER ) return QObject::tr( "파일 시스템 드라이버" );
    QString Text = ( Type & SERVICE_WIN32_SHARE_PROCESS ) ? QObject::tr( "Win32 공유 프로세스" ) : QObject::tr( "Win32 독립 프로세스" );
    if( Type & SERVICE_USER_SERVICE ) Text += QObject::tr( " · 사용자 서비스" );
    if( Type & SERVICE_INTERACTIVE_PROCESS ) Text += QObject::tr( " · 대화형" );
    return Text + QStringLiteral( " (0x%1)" ).arg( Type, 0, 16 );
}

QString AccountText( const QString& Account )
{
    const auto Lower = Account.toLower();
    if( Lower.isEmpty() || Lower == "localsystem" ) return QStringLiteral( "Local System" );
    if( Lower == "nt authority\\localservice" ) return QStringLiteral( "Local Service" );
    if( Lower == "nt authority\\networkservice" ) return QStringLiteral( "Network Service" );
    return Account;
}

QString Win32ErrorText( quint32 Code )
{
    if( Code == 0 ) return {};
    wchar_t* Buffer = nullptr;
    const auto Length = FormatMessageW( FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, Code, 0,
                                        reinterpret_cast<LPWSTR>( &Buffer ), 0, nullptr );
    QString Text = Length ? QString::fromWCharArray( Buffer, int( Length ) ).trimmed() : QObject::tr( "오류 %1" ).arg( Code );
    if( Buffer ) LocalFree( Buffer );
    return Text + QStringLiteral( " (%1)" ).arg( Code );
}

QString OutcomeText( const QString& Outcome, const QString& Reason, quint32 NativeCode )
{
    if( Outcome == "Completed" ) return QObject::tr( "완료" );
    if( Outcome == "AlreadyGone" ) return QObject::tr( "이미 끝났습니다" );
    if( Outcome == "TargetChanged" ) return QObject::tr( "대상이 바뀌었습니다(PID 재사용 또는 다른 실행 파일)" );
    if( Outcome == "AccessDenied" ) return QObject::tr( "접근이 거부되었습니다" );
    if( Outcome == "Rejected" )
    {
        if( Reason == "system-process" || Reason == "critical-process" ) return QObject::tr( "끝내면 시스템이 중단되는 중요 프로세스입니다" );
        if( Reason == "protected-process" ) return QObject::tr( "보호된 프로세스입니다" );
        if( Reason == "self" ) return QObject::tr( "HyperCommander와 관리자 도우미는 끝낼 수 없습니다" );
        if( Reason == "product-process" ) return QObject::tr( "HyperCommander 구성 요소입니다" );
        if( Reason == "process-unverified" ) return QObject::tr( "실행 파일을 확인할 수 없습니다" );
        if( Reason == "no-window" ) return QObject::tr( "닫을 창이 없습니다" );
        return QObject::tr( "거부되었습니다" );
    }
    if( Outcome == "Unavailable" ) return QObject::tr( "관리자 도우미에 연결할 수 없습니다" ) + ( NativeCode ? " · " + Win32ErrorText( NativeCode ) : QString{} );
    return NativeCode ? Win32ErrorText( NativeCode ) : QObject::tr( "결과를 확인할 수 없습니다" );
}

QString FileTimeText( quint64 FileTime )
{
    if( FileTime == 0 ) return {};
    FILETIME Value{ DWORD( FileTime & 0xFFFFFFFFu ), DWORD( FileTime >> 32 ) };
    SYSTEMTIME Utc{};
    if( !FileTimeToSystemTime( &Value, &Utc ) ) return {};
    const QDateTime Time( QDate( Utc.wYear, Utc.wMonth, Utc.wDay ), QTime( Utc.wHour, Utc.wMinute, Utc.wSecond ), QTimeZone::UTC );
    return QLocale().toString( Time.toLocalTime(), QStringLiteral( "yyyy-MM-dd HH:mm:ss" ) );
}

hc::system::ActionLimits ProcessLimits( bool AllowWarned )
{
    hc::system::ActionLimits Value;
    Value.callerPid = GetCurrentProcessId();
    Value.blocked = { L"HyperCommander.exe", L"HyperCommanderAdmin.exe" };
    Value.product = { L"HyperCommanderUpdater.exe", L"hc-plugin-host-x64.exe", L"HyperViewer.exe", L"HyperEditor.exe", L"HyperEditorWeb.exe",
                      L"HyperCompare.exe", L"hc-search-index.exe" };
    Value.allowWarned = AllowWarned;
    return Value;
}

bool IsProductImage( const QString& Path )
{
    const auto Name = QFileInfo( Path ).fileName();
    for( const auto& Item : ProcessLimits( false ).product )
        if( Name.compare( QString::fromStdWString( Item ), Qt::CaseInsensitive ) == 0 )
            return true;
    return false;
}

hc::admin::SystemSessionClient& AdminSession()
{
    static hc::admin::SystemSessionClient Session;
    return Session;
}

bool AdminSessionConnected()
{
    return AdminSession().Connected();
}

AdminSessionEvents& AdminEvents()
{
    static AdminSessionEvents Events;
    return Events;
}

void NotifyAdminConnected()
{
    emit AdminEvents().sigConnected();
}

void EnsureAdminHelper()
{
    static bool Starting = false, Declined = false;
    if( hc::permissions::Elevated() || Starting || Declined || AdminSessionConnected() )
        return;
    Starting = true;
    RunAsync( &AdminEvents(), [] { return AdminSession().Request( "ping", {}, 120000 ); },
              []( const QJsonObject& Reply ) {
                  Starting = false;
                  if( Reply["outcome"].toString() == "Completed" )
                      NotifyAdminConnected();
                  else if( Reply["nativeCode"].toString().toUInt() == ERROR_CANCELLED )
                      Declined = true;
              } );
}

} // namespace hc::sys
