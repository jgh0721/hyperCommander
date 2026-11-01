#include "stdafx.h"
#include "systemSessionService.hpp"

/// 내부 라이브러리
#include "adminChannel.hpp"

/// 공통 라이브러리
#include "commonLibs/system/processActions.hpp"
#include "commonLibs/system/processInfo.hpp"
#include "commonLibs/system/serviceControl.hpp"

/// 플랫폼
#include <QHash>
#include <QJsonArray>
#include <QUuid>

namespace hc::admin {
namespace {
const ChannelSpec& Spec()
{
    static const ChannelSpec Value{ "hc.system", 1, "HyperCommander-system-", "--system-session", 65536 };
    return Value;
}

std::wstring Wide( const QString& Text ) { return Text.toStdWString(); }
QString      Text( const std::wstring& Value ) { return QString::fromStdWString( Value ); }

// 조회 요청은 결과를 모르는 채 끝나도(전송 실패) 다시 보내도 된다. 동작 요청만 Unknown으로 알린다.
bool IsAction( const QString& Kind )
{
    return Kind != "ping" && Kind != "process.details" && Kind != "service.query";
}

hc::system::ProcessTarget TargetFrom( const QJsonObject& Object )
{
    hc::system::ProcessTarget Target;
    Target.pid = quint32( Object["pid"].toInteger() );
    Target.createTime = Object["created"].toString().toULongLong();
    Target.image = Wide( Object["image"].toString() );
    return Target;
}

QJsonObject ResultObject( const hc::system::ActionResult& Result )
{
    return { { "outcome", QString::fromLatin1( hc::system::OutcomeName( Result.outcome ) ) }, { "nativeCode", QString::number( Result.nativeCode ) },
             { "reason", QString::fromStdString( Result.reason ) } };
}

QJsonObject ErrorObject( unsigned long Code )
{
    return { { "outcome", Code == ERROR_SUCCESS ? "Completed" : ( Code == ERROR_ACCESS_DENIED ? "AccessDenied" : "Failed" ) }, { "nativeCode", QString::number( Code ) } };
}

QString StartTypeName( hc::system::StartType Type )
{
    using hc::system::StartType;
    switch( Type )
    {
        case StartType::Boot:        return "boot";
        case StartType::System:      return "system";
        case StartType::Auto:        return "auto";
        case StartType::AutoDelayed: return "autoDelayed";
        case StartType::Manual:      return "manual";
        case StartType::Disabled:    return "disabled";
        default:                     return {};
    }
}

hc::system::StartType StartTypeFrom( const QString& Name )
{
    if( Name == "boot" ) return hc::system::StartType::Boot;
    if( Name == "system" ) return hc::system::StartType::System;
    if( Name == "auto" ) return hc::system::StartType::Auto;
    if( Name == "autoDelayed" ) return hc::system::StartType::AutoDelayed;
    if( Name == "manual" ) return hc::system::StartType::Manual;
    if( Name == "disabled" ) return hc::system::StartType::Disabled;
    return hc::system::StartType::Unknown;
}

hc::system::ActionLimits Limits( quint32 CallerPid, bool AllowWarned )
{
    hc::system::ActionLimits Value;
    Value.callerPid = CallerPid;
    Value.blocked = { L"HyperCommander.exe", L"HyperCommanderAdmin.exe" };
    Value.product = { L"HyperCommanderUpdater.exe", L"hc-plugin-host-x64.exe", L"HyperViewer.exe", L"HyperEditor.exe", L"HyperEditorWeb.exe",
                      L"HyperCompare.exe", L"hc-search-index.exe" };
    Value.allowWarned = AllowWarned;
    return Value;
}

QJsonObject Handle( const QString& Kind, const QJsonObject& Request, quint32 CallerPid )
{
    const bool AllowWarned = Request["allowWarned"].toBool();
    if( Kind == "ping" )
        return { { "outcome", "Completed" } };
    if( Kind == "process.terminate" )
    {
        const int Graceful = std::clamp( Request["gracefulMs"].toInt( 0 ), 0, 60000 );
        QJsonArray Results;
        for( const auto& Value : Request["targets"].toArray() )
        {
            const auto Target = TargetFrom( Value.toObject() );
            if( Graceful > 0 && hc::system::HasTopLevelWindows( Target.pid ) )
                if( hc::system::RequestCloseWindows( Target ).outcome == hc::system::Outcome::Completed && hc::system::WaitForExit( Target, DWORD( Graceful ) ) )
                {
                    Results.append( QJsonObject{ { "pid", qint64( Target.pid ) }, { "outcome", "Completed" }, { "graceful", true } } );
                    continue;
                }
            auto Result = ResultObject( hc::system::TerminateProcessTarget( Target, Limits( CallerPid, AllowWarned ) ) );
            Result["pid"] = qint64( Target.pid );
            Results.append( Result );
        }
        return { { "outcome", "Completed" }, { "results", Results } };
    }
    if( Kind == "process.suspend" )
        return ResultObject( hc::system::SuspendProcessTarget( TargetFrom( Request["target"].toObject() ), !Request["resume"].toBool(), Limits( CallerPid, AllowWarned ) ) );
    if( Kind == "process.priority" )
        return ResultObject( hc::system::SetPriorityTarget( TargetFrom( Request["target"].toObject() ), quint32( Request["priorityClass"].toInteger() ), Limits( CallerPid, AllowWarned ) ) );
    if( Kind == "process.efficiency" )
        return ResultObject( hc::system::SetEfficiencyTarget( TargetFrom( Request["target"].toObject() ), Request["enable"].toBool(), Limits( CallerPid, AllowWarned ) ) );
    if( Kind == "service.query" )
    {
        hc::system::ServiceConfig Config;
        uint32_t Error = 0;
        if( !hc::system::QueryServiceConfiguration( Wide( Request["name"].toString() ), Config, &Error ) )
            return ErrorObject( Error ? Error : ERROR_GEN_FAILURE );
        return { { "outcome", "Completed" }, { "startType", StartTypeName( Config.start ) }, { "trigger", Config.triggerStart },
                 { "account", Text( Config.account ) }, { "description", Text( Config.description ) }, { "image", Text( Config.resolvedImage ) },
                 { "binaryPath", Text( Config.binaryPath ) } };
    }
    if( Kind == "process.details" )
    {
        hc::system::ProcessDetails Details;
        if( !hc::system::QueryProcessDetails( quint32( Request["pid"].toInteger() ), Request["created"].toString().toULongLong(), Details ) )
            return { { "outcome", "Failed" }, { "nativeCode", QString::number( GetLastError() ) } };
        return { { "outcome", "Completed" }, { "image", Text( Details.imagePath ) }, { "commandLine", Text( Details.commandLine ) },
                 { "user", Text( Details.userName ) }, { "userSid", Text( Details.userSid ) }, { "integrity", int( Details.integrity ) },
                 { "elevated", Details.elevated }, { "wow64", Details.wow64 }, { "priorityClass", qint64( Details.priorityClass ) },
                 { "critical", Details.critical }, { "protected", Details.protectedProcess }, { "packaged", Details.packaged },
                 { "package", Text( Details.packageName ) } };
    }
    if( Kind == "service.control" )
    {
        const auto Name = Wide( Request["name"].toString() );
        const auto Control = Request["control"].toString();
        if( Name.empty() )
            return ErrorObject( ERROR_INVALID_PARAMETER );
        if( Control == "restart" )
            return ErrorObject( hc::system::RestartServiceByName( Name ) );
        const auto Code = Control == "start"      ? hc::system::ControlServiceByName( Name, hc::system::ServiceControl::Start )
                          : Control == "stop"     ? hc::system::ControlServiceByName( Name, hc::system::ServiceControl::Stop )
                          : Control == "pause"    ? hc::system::ControlServiceByName( Name, hc::system::ServiceControl::Pause )
                          : Control == "continue" ? hc::system::ControlServiceByName( Name, hc::system::ServiceControl::Continue )
                                                  : ERROR_INVALID_PARAMETER;
        return ErrorObject( Code );
    }
    if( Kind == "service.config" )
    {
        const auto Name = Wide( Request["name"].toString() );
        const auto Changes = Request["changes"].toObject();
        if( Name.empty() )
            return ErrorObject( ERROR_INVALID_PARAMETER );
        hc::system::ServiceConfigChange Change;
        if( Changes.contains( "startType" ) ) Change.start = StartTypeFrom( Changes["startType"].toString() );
        if( Changes.contains( "displayName" ) ) Change.displayName = Wide( Changes["displayName"].toString() );
        if( Changes.contains( "description" ) ) Change.description = Wide( Changes["description"].toString() );
        if( Changes.contains( "errorControl" ) ) Change.errorControl = quint32( Changes["errorControl"].toInteger() );
        if( Changes.contains( "sidType" ) ) Change.sidType = quint32( Changes["sidType"].toInteger() );
        if( Changes.contains( "account" ) )
        {
            Change.account = Wide( Changes["account"].toString() );
            if( Changes.contains( "password" ) ) Change.password = Wide( Changes["password"].toString() );
        }
        if( Changes.contains( "failure" ) )
        {
            const auto Failure = Changes["failure"].toObject();
            hc::system::FailureActions Actions;
            Actions.resetPeriodSec = quint32( Failure["resetPeriodSec"].toInteger() );
            Actions.command = Wide( Failure["command"].toString() );
            Actions.rebootMessage = Wide( Failure["rebootMessage"].toString() );
            Actions.onNonCrashFailures = Failure["onNonCrashFailures"].toBool();
            for( const auto& Item : Failure["actions"].toArray() )
                Actions.actions.push_back( { quint32( Item.toObject()["type"].toInteger() ), quint32( Item.toObject()["delayMs"].toInteger() ) } );
            Change.failure = Actions;
        }
        if( Change.start && *Change.start == hc::system::StartType::Unknown )
            return ErrorObject( ERROR_INVALID_PARAMETER );
        const auto Code = hc::system::ApplyServiceConfig( Name, Change );
        if( Change.password )
            SecureZeroMemory( Change.password->data(), Change.password->size() * sizeof( wchar_t ) );
        return ErrorObject( Code );
    }
    return { { "outcome", "Rejected" }, { "nativeCode", QString::number( ERROR_INVALID_FUNCTION ) } };
}
} // namespace

SystemSessionClient::SystemSessionClient( QString Helper, Launcher Launch, bool Elevated )
    : helper_( std::move( Helper ) ), launcher_( std::move( Launch ) ), elevated_( Elevated )
{
}

SystemSessionClient::~SystemSessionClient() = default;

bool SystemSessionClient::Connected() const
{
    std::lock_guard Lock( mutex_ );
    return channel_ && channel_->Connected() && !channel_->Cancelled() && channel_->HelperAlive();
}

void SystemSessionClient::Close()
{
    std::lock_guard Lock( mutex_ );
    channel_.reset();
}

QJsonObject SystemSessionClient::RequestIfConnected( const QString& Kind, QJsonObject Payload, unsigned long Timeout )
{
    // 확인과 전송을 한 잠금 안에서 한다. 그 사이 도우미가 끝나도 새로 띄우지 않는다.
    std::lock_guard Lock( mutex_ );
    if( !channel_ || !channel_->Connected() || channel_->Cancelled() || !channel_->HelperAlive() )
        return { { "outcome", "Unavailable" }, { "nativeCode", QString::number( ERROR_NOT_CONNECTED ) } };
    Payload["kind"] = Kind;
    Payload["requestId"] = NewId();
    return channel_->Exchange( std::move( Payload ), Timeout, IsAction( Kind ) );
}

QJsonObject SystemSessionClient::Request( const QString& Kind, QJsonObject Payload, unsigned long Timeout )
{
    std::lock_guard Lock( mutex_ );
    if( channel_ && ( channel_->Cancelled() || ( channel_->Connected() && !channel_->HelperAlive() ) ) )
        channel_.reset();
    if( !channel_ )
    {
        channel_ = std::make_unique<ChannelClient>( Spec(), elevated_, helper_, launcher_ );
        channel_->SetBootstrap( { { "idleMs", idleMs_ } } );
    }
    Payload["kind"] = Kind;
    Payload["requestId"] = NewId();
    auto Reply = channel_->Exchange( std::move( Payload ), Timeout, IsAction( Kind ) );
    // 비밀번호가 담긴 요청 객체는 이 함수가 끝나면 사라진다. 응답에는 비밀번호를 넣지 않는다.
    return Reply;
}

unsigned long ExecuteSystemSession( const QJsonObject& Bootstrap, const QString& ExpectedCaller, bool RequireElevation )
{
    if( RequireElevation && !Bootstrap["elevated"].toBool() )
        return ERROR_INVALID_PARAMETER;
    AcceptedChannel Channel;
    if( const auto Code = AcceptChannel( Bootstrap, Spec(), ExpectedCaller, Channel ) )
        return Code;
    const auto Idle = static_cast<unsigned long>( std::clamp( Bootstrap["idleMs"].toInt( 600000 ), 60000, 7200000 ) );
    QHash<QString, QJsonObject> Receipts;
    while( Channel.ParentAlive() )
    {
        const auto Request = ReceiveFrame( Channel.pipe, Spec(), Channel.nonce, Idle );
        if( !Request )
            return Channel.ParentAlive() ? ERROR_TIMEOUT : ERROR_BROKEN_PIPE;
        const auto Kind = ( *Request )["kind"].toString(), RequestId = ( *Request )["requestId"].toString();
        if( QUuid( RequestId ).isNull() )
            return ERROR_INVALID_PARAMETER;
        QJsonObject Reply;
        if( Receipts.contains( RequestId ) )
            Reply = Receipts.value( RequestId );
        else
        {
            Reply = Handle( Kind, *Request, Channel.parentPid );
            Reply["requestId"] = RequestId;
            if( Receipts.size() >= 256 )
                Receipts.clear();
            Receipts.insert( RequestId, Reply );
        }
        if( !SendFrame( Channel.pipe, Reply, Spec(), Channel.nonce ) )
            return ERROR_BROKEN_PIPE;
    }
    return ERROR_SUCCESS;
}

} // namespace hc::admin
