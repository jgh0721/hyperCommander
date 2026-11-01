#include "stdafx.h"
#include "adminChannel.hpp"

/// 공통 라이브러리
#include "commonLibs/system/processInfo.hpp"

/// 플랫폼
#include <shellapi.h>
#include <sddl.h>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonDocument>
#include <QUuid>

namespace hc::admin {
namespace {
struct Handle
{
    HANDLE value_ = nullptr;
    explicit Handle( HANDLE Value = nullptr ) : value_( Value ) {}
    ~Handle() { if( value_ && value_ != INVALID_HANDLE_VALUE ) CloseHandle( value_ ); }
};
QString PipeName( const ChannelSpec& Spec, const QString& Nonce ) { return "\\\\.\\pipe\\" + Spec.pipePrefix + Nonce; }
QString Text( const std::wstring& Value ) { return QString::fromStdWString( Value ); }
QString CreationText( HANDLE Process ) { return QString::number( hc::system::ProcessCreateTime( Process ) ); }
} // namespace

QString NewId() { return QUuid::createUuid().toString( QUuid::WithoutBraces ); }

bool SameImage( const QString& First, const QString& Second )
{
    return !First.isEmpty() && QDir::cleanPath( QDir::fromNativeSeparators( First ) ).compare( QDir::cleanPath( QDir::fromNativeSeparators( Second ) ), Qt::CaseInsensitive ) == 0;
}

QJsonObject TransportError( bool Action, unsigned long Code )
{
    return { { "outcome", Action ? "Unknown" : "Unavailable" }, { "nativeCode", QString::number( Code ) }, { "stage", "transport" }, { "requiresRefresh", true } };
}

bool PipeIo( void* Pipe, bool Write, QByteArray& Bytes, unsigned long Timeout, const std::atomic_bool* Cancel )
{
    Handle Event( CreateEventW( nullptr, TRUE, FALSE, nullptr ) );
    if( !Event.value_ )
        return false;
    OVERLAPPED Pending{};
    Pending.hEvent = Event.value_;
    DWORD Count = 0;
    const bool Done = Write ? WriteFile( HANDLE( Pipe ), Bytes.constData(), DWORD( Bytes.size() ), &Count, &Pending )
                            : ReadFile( HANDLE( Pipe ), Bytes.data(), DWORD( Bytes.size() ), &Count, &Pending );
    if( !Done )
    {
        if( GetLastError() != ERROR_IO_PENDING )
            return false;
        QElapsedTimer Timer;
        Timer.start();
        while( ( !Cancel || !Cancel->load() ) && Timer.elapsed() < qint64( Timeout ) )
            if( WaitForSingleObject( Event.value_, 50 ) == WAIT_OBJECT_0 )
                break;
        if( ( Cancel && Cancel->load() ) || Timer.elapsed() >= qint64( Timeout ) )
        {
            CancelIoEx( HANDLE( Pipe ), &Pending );
            GetOverlappedResult( HANDLE( Pipe ), &Pending, &Count, TRUE );
            return false;
        }
        if( !GetOverlappedResult( HANDLE( Pipe ), &Pending, &Count, FALSE ) )
            return false;
    }
    if( Write )
        return Count == DWORD( Bytes.size() );
    Bytes.resize( int( Count ) );
    return Count > 0;
}

bool SendFrame( void* Pipe, QJsonObject Message, const ChannelSpec& Spec, const QString& Nonce, const std::atomic_bool* Cancel )
{
    Message["protocol"] = Spec.protocol;
    Message["version"] = Spec.version;
    Message["nonce"] = Nonce;
    auto Bytes = QJsonDocument( Message ).toJson( QJsonDocument::Compact );
    return quint32( Bytes.size() ) <= Spec.frameLimit && PipeIo( Pipe, true, Bytes, 5000, Cancel );
}

std::optional<QJsonObject> ReceiveFrame( void* Pipe, const ChannelSpec& Spec, const QString& Nonce, unsigned long Timeout, const std::atomic_bool* Cancel )
{
    QByteArray Bytes( int( Spec.frameLimit ), '\0' );
    if( !PipeIo( Pipe, false, Bytes, Timeout, Cancel ) )
        return {};
    QJsonParseError Error;
    const auto Document = QJsonDocument::fromJson( Bytes, &Error );
    if( Error.error != QJsonParseError::NoError || !Document.isObject() )
        return {};
    const auto Message = Document.object();
    if( Message["protocol"] != Spec.protocol || Message["version"].toInt() != Spec.version || Message["nonce"].toString() != Nonce )
        return {};
    return Message;
}

ChannelClient::ChannelClient( ChannelSpec Spec, bool Elevated, QString Helper, Launcher Launch )
    : spec_( std::move( Spec ) ), elevated_( Elevated ), helper_( std::move( Helper ) ), nonce_( NewId() ), launcher_( std::move( Launch ) )
{
    if( helper_.isEmpty() )
        helper_ = QDir( QCoreApplication::applicationDirPath() ).filePath( "HyperCommanderAdmin.exe" );
}

ChannelClient::~ChannelClient()
{
    if( pipe_ )
        CloseHandle( std::exchange( pipe_, nullptr ) );
    if( process_ )
    {
        // 파이프를 닫으면 도우미가 끝난다. 끝나지 않으면 이 채널 전용 인스턴스이므로 종료한다.
        if( WaitForSingleObject( process_, 100 ) == WAIT_TIMEOUT )
            TerminateProcess( process_, ERROR_CANCELLED );
        CloseHandle( process_ );
    }
}

bool ChannelClient::HelperAlive() const
{
    return process_ && WaitForSingleObject( process_, 0 ) == WAIT_TIMEOUT;
}

bool ChannelClient::start()
{
    if( pipe_ || cancelled_ || !QFileInfo::exists( helper_ ) )
        return false;
    const auto Sid = Text( hc::system::ProcessUserSid( GetCurrentProcess() ) );
    if( Sid.isEmpty() )
        return false;
    const auto Sddl = "D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;" + Sid + ')';
    PSECURITY_DESCRIPTOR Descriptor = nullptr;
    if( !ConvertStringSecurityDescriptorToSecurityDescriptorW( reinterpret_cast<LPCWSTR>( Sddl.utf16() ), SDDL_REVISION_1, &Descriptor, nullptr ) )
        return false;
    SECURITY_ATTRIBUTES Security{ sizeof( Security ), Descriptor, FALSE };
    const auto Name = PipeName( spec_, nonce_ );
    pipe_ = CreateNamedPipeW( reinterpret_cast<LPCWSTR>( Name.utf16() ), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                              PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, spec_.frameLimit, spec_.frameLimit, 0, &Security );
    LocalFree( Descriptor );
    if( pipe_ == INVALID_HANDLE_VALUE )
    {
        pipe_ = nullptr;
        return false;
    }
    QJsonObject Bootstrap{ { "version", 1 }, { "nonce", nonce_ }, { "pid", int( GetCurrentProcessId() ) }, { "created", CreationText( GetCurrentProcess() ) },
                           { "callerSid", Sid }, { "utc", QDateTime::currentDateTimeUtc().toString( Qt::ISODateWithMs ) }, { "elevated", elevated_ } };
    for( auto It = extra_.begin(); It != extra_.end(); ++It )
        if( !Bootstrap.contains( It.key() ) )
            Bootstrap.insert( It.key(), It.value() );
    const auto Payload = QJsonDocument( Bootstrap ).toJson( QJsonDocument::Compact ).toBase64( QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals );
    if( Payload.size() > 24000 )
        return false;
    const auto Arguments = spec_.mode + ' ' + QString::fromLatin1( Payload );
    if( launcher_ )
        process_ = launcher_( helper_, Arguments, elevated_ );
    else if( elevated_ )
    {
        const auto Program = QDir::toNativeSeparators( helper_ );
        SHELLEXECUTEINFOW Info{};
        Info.cbSize = sizeof( Info );
        Info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
        Info.lpVerb = L"runas";
        Info.lpFile = reinterpret_cast<LPCWSTR>( Program.utf16() );
        Info.lpParameters = reinterpret_cast<LPCWSTR>( Arguments.utf16() );
        Info.nShow = SW_HIDE;
        if( ShellExecuteExW( &Info ) )
            process_ = Info.hProcess;
        else
            lastError_ = GetLastError();
    }
    else
    {
        auto Command = ( '"' + QDir::toNativeSeparators( helper_ ) + "\" " + Arguments ).toStdWString();
        STARTUPINFOW Startup{};
        Startup.cb = sizeof( Startup );
        PROCESS_INFORMATION Process{};
        if( CreateProcessW( reinterpret_cast<LPCWSTR>( helper_.utf16() ), Command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &Startup, &Process ) )
        {
            process_ = Process.hProcess;
            CloseHandle( Process.hThread );
        }
        else
            lastError_ = GetLastError();
    }
    if( !process_ )
        return false;
    Handle Event( CreateEventW( nullptr, TRUE, FALSE, nullptr ) );
    OVERLAPPED Pending{};
    Pending.hEvent = Event.value_;
    bool Connected = ConnectNamedPipe( pipe_, &Pending );
    const auto Error = Connected ? ERROR_SUCCESS : GetLastError();
    if( !Connected && Error == ERROR_PIPE_CONNECTED )
        Connected = true;
    if( !Connected && Error == ERROR_IO_PENDING )
    {
        QElapsedTimer Timer;
        Timer.start();
        while( !cancelled_ && Timer.elapsed() < 30000 && WaitForSingleObject( process_, 0 ) == WAIT_TIMEOUT )
        {
            if( WaitForSingleObject( Event.value_, 50 ) == WAIT_OBJECT_0 )
            {
                DWORD Bytes = 0;
                Connected = GetOverlappedResult( pipe_, &Pending, &Bytes, FALSE );
                break;
            }
        }
        if( !Connected )
        {
            CancelIoEx( pipe_, &Pending );
            DWORD Bytes = 0;
            GetOverlappedResult( pipe_, &Pending, &Bytes, TRUE );
        }
    }
    ULONG Peer = 0;
    if( !Connected || !GetNamedPipeClientProcessId( pipe_, &Peer ) || Peer != GetProcessId( process_ )
        || !SameImage( Text( hc::system::ProcessImagePath( process_ ) ), QFileInfo( helper_ ).absoluteFilePath() ) )
        return false;
    return true;
}

QJsonObject ChannelClient::Exchange( QJsonObject Message, unsigned long Timeout, bool Action )
{
    std::lock_guard Lock( mutex_ );
    if( cancelled_ )
        return TransportError( false, ERROR_CANCELLED );
    if( ( !pipe_ && !start() ) || !process_ )
    {
        const DWORD Error = lastError_ ? lastError_ : GetLastError();
        lastError_ = Error;
        cancelled_ = true;
        return TransportError( false, Error );
    }
    const auto RequestId = Message["requestId"].toString();
    if( !SendFrame( pipe_, Message, spec_, nonce_, &cancelled_ ) )
    {
        cancelled_ = true;
        return TransportError( Action, ERROR_BROKEN_PIPE );
    }
    auto Reply = ReceiveFrame( pipe_, spec_, nonce_, Timeout, &cancelled_ );
    if( !Reply || ( *Reply )["requestId"].toString() != RequestId )
    {
        cancelled_ = true;
        return TransportError( Action, ERROR_TIMEOUT );
    }
    return *Reply;
}

AcceptedChannel::~AcceptedChannel()
{
    if( pipe && pipe != INVALID_HANDLE_VALUE )
        CloseHandle( pipe );
    if( parent )
        CloseHandle( parent );
}

bool AcceptedChannel::ParentAlive() const
{
    return parent && WaitForSingleObject( parent, 0 ) == WAIT_TIMEOUT;
}

unsigned long AcceptChannel( const QJsonObject& Bootstrap, const ChannelSpec& Spec, const QString& ExpectedCaller, AcceptedChannel& Out )
{
    const auto Nonce = Bootstrap["nonce"].toString();
    const auto Stamp = QDateTime::fromString( Bootstrap["utc"].toString(), Qt::ISODateWithMs );
    const auto Age = Stamp.msecsTo( QDateTime::currentDateTimeUtc() );
    if( Bootstrap["version"].toInt() != 1 || QUuid( Nonce ).isNull() || !Stamp.isValid() || Age < -5000 || Age > 120000 )
        return ERROR_INVALID_PARAMETER;
    const auto Pid = DWORD( Bootstrap["pid"].toInt() );
    Out.parent = OpenProcess( PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, Pid );
    const auto Trusted = ExpectedCaller.isEmpty() ? QDir( QCoreApplication::applicationDirPath() ).filePath( "HyperCommander.exe" ) : ExpectedCaller;
    if( !Out.parent || CreationText( Out.parent ) != Bootstrap["created"].toString()
        || Text( hc::system::ProcessUserSid( Out.parent ) ) != Bootstrap["callerSid"].toString()
        || !SameImage( Text( hc::system::ProcessImagePath( Out.parent ) ), QFileInfo( Trusted ).absoluteFilePath() ) )
        return ERROR_ACCESS_DENIED;
    if( Bootstrap["elevated"].toBool() )
    {
        HANDLE Raw = nullptr;
        TOKEN_ELEVATION Elevation{};
        DWORD Size = 0;
        if( !OpenProcessToken( GetCurrentProcess(), TOKEN_QUERY, &Raw ) )
            return ERROR_ELEVATION_REQUIRED;
        Handle Token( Raw );
        if( !GetTokenInformation( Raw, TokenElevation, &Elevation, sizeof( Elevation ), &Size ) || !Elevation.TokenIsElevated )
            return ERROR_ELEVATION_REQUIRED;
    }
    else if( Text( hc::system::ProcessUserSid( GetCurrentProcess() ) ) != Bootstrap["callerSid"].toString() )
        return ERROR_ACCESS_DENIED;
    const auto Name = PipeName( Spec, Nonce );
    if( !WaitNamedPipeW( reinterpret_cast<LPCWSTR>( Name.utf16() ), 5000 ) )
        return GetLastError();
    Out.pipe = CreateFileW( reinterpret_cast<LPCWSTR>( Name.utf16() ), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr );
    ULONG Peer = 0;
    if( Out.pipe == INVALID_HANDLE_VALUE || !GetNamedPipeServerProcessId( HANDLE( Out.pipe ), &Peer ) || Peer != Pid )
        return ERROR_ACCESS_DENIED;
    DWORD Mode = PIPE_READMODE_MESSAGE;
    if( !SetNamedPipeHandleState( HANDLE( Out.pipe ), &Mode, nullptr, nullptr ) )
        return GetLastError();
    Out.parentPid = Pid;
    Out.nonce = Nonce;
    return ERROR_SUCCESS;
}

} // namespace hc::admin
