#include "processInfo.hpp"

#include "ntApi.hpp"

#include <appmodel.h>
#include <sddl.h>

#include <algorithm>
#include <cwctype>
#include <map>
#include <mutex>

namespace hc::system {

namespace {
struct Handle
{
    HANDLE value = nullptr;
    explicit Handle( HANDLE Value = nullptr ) : value( Value ) {}
    ~Handle() { if( value && value != INVALID_HANDLE_VALUE ) CloseHandle( value ); }
    Handle( const Handle& ) = delete;
    Handle& operator=( const Handle& ) = delete;
};

std::wstring Lower( std::wstring Text )
{
    std::transform( Text.begin(), Text.end(), Text.begin(), [] ( wchar_t C ) { return wchar_t( std::towlower( C ) ); } );
    return Text;
}

std::wstring FileName( const std::wstring& Path )
{
    const auto Cut = Path.find_last_of( L"\\/" );
    return Cut == std::wstring::npos ? Path : Path.substr( Cut + 1 );
}

std::vector<unsigned char> TokenInformation( HANDLE Token, TOKEN_INFORMATION_CLASS Class )
{
    DWORD Size = 0;
    GetTokenInformation( Token, Class, nullptr, 0, &Size );
    if( Size == 0 )
        return {};
    std::vector<unsigned char> Buffer( Size );
    if( !GetTokenInformation( Token, Class, Buffer.data(), Size, &Size ) )
        return {};
    return Buffer;
}

std::wstring SidString( PSID Sid )
{
    LPWSTR Text = nullptr;
    if( !Sid || !ConvertSidToStringSidW( Sid, &Text ) )
        return {};
    std::wstring Result( Text );
    LocalFree( Text );
    return Result;
}

Integrity IntegrityFromRid( DWORD Rid )
{
    if( Rid >= SECURITY_MANDATORY_PROTECTED_PROCESS_RID ) return Integrity::Protected;
    if( Rid >= SECURITY_MANDATORY_SYSTEM_RID ) return Integrity::System;
    if( Rid >= SECURITY_MANDATORY_HIGH_RID ) return Integrity::High;
    if( Rid >= SECURITY_MANDATORY_MEDIUM_PLUS_RID ) return Integrity::MediumPlus;
    if( Rid >= SECURITY_MANDATORY_MEDIUM_RID ) return Integrity::Medium;
    if( Rid >= SECURITY_MANDATORY_LOW_RID ) return Integrity::Low;
    return Integrity::Untrusted;
}

std::wstring CommandLine( HANDLE Process )
{
    const auto& Api = nt::Functions();
    if( !Api.QueryInformationProcess )
        return {};
    ULONG Length = 0;
    Api.QueryInformationProcess( Process, nt::ProcessCommandLineInformationClass, nullptr, 0, &Length );
    if( Length == 0 || Length > 1024 * 1024 )
        return {};
    std::vector<unsigned char> Buffer( Length + sizeof( wchar_t ) );
    if( !nt::Succeeded( Api.QueryInformationProcess( Process, nt::ProcessCommandLineInformationClass, Buffer.data(), ULONG( Buffer.size() ), &Length ) ) )
        return {};
    const auto* Text = reinterpret_cast<const nt::UnicodeString*>( Buffer.data() );
    if( !Text->Buffer || Text->Length == 0 )
        return {};
    return std::wstring( Text->Buffer, Text->Length / sizeof( wchar_t ) );
}
} // namespace

std::wstring ProcessImagePath( void* Process )
{
    std::wstring Path( 32768, L'\0' );
    DWORD Size = DWORD( Path.size() );
    if( !Process || !QueryFullProcessImageNameW( HANDLE( Process ), 0, Path.data(), &Size ) )
        return {};
    Path.resize( Size );
    return Path;
}

uint64_t ProcessCreateTime( void* Process )
{
    FILETIME Created{}, Exited{}, Kernel{}, User{};
    if( !Process || !GetProcessTimes( HANDLE( Process ), &Created, &Exited, &Kernel, &User ) )
        return 0;
    return ( uint64_t( Created.dwHighDateTime ) << 32 ) | Created.dwLowDateTime;
}

std::wstring ProcessUserSid( void* Process )
{
    HANDLE Raw = nullptr;
    if( !Process || !OpenProcessToken( HANDLE( Process ), TOKEN_QUERY, &Raw ) )
        return {};
    Handle Token( Raw );
    const auto Buffer = TokenInformation( Raw, TokenUser );
    if( Buffer.empty() )
        return {};
    return SidString( reinterpret_cast<const TOKEN_USER*>( Buffer.data() )->User.Sid );
}

std::wstring CurrentUserSid()
{
    static const std::wstring Value = ProcessUserSid( GetCurrentProcess() );
    return Value;
}

std::wstring AccountName( const std::wstring& Sid )
{
    static std::mutex Lock;
    static std::map<std::wstring, std::wstring> Cache;
    {
        std::lock_guard Guard( Lock );
        if( const auto Found = Cache.find( Sid ); Found != Cache.end() )
            return Found->second;
    }
    PSID Native = nullptr;
    std::wstring Result;
    if( ConvertStringSidToSidW( Sid.c_str(), &Native ) )
    {
        wchar_t Name[256]{}, Domain[256]{};
        DWORD NameSize = 256, DomainSize = 256;
        SID_NAME_USE Use{};
        if( LookupAccountSidW( nullptr, Native, Name, &NameSize, Domain, &DomainSize, &Use ) )
            Result = DomainSize && Use != SidTypeWellKnownGroup ? std::wstring( Domain ) + L"\\" + Name : std::wstring( Name );
        LocalFree( Native );
    }
    if( Result.empty() )
        Result = Sid;
    std::lock_guard Guard( Lock );
    if( Cache.size() > 512 )
        Cache.clear();
    Cache.emplace( Sid, Result );
    return Result;
}

bool QueryProcessDetails( uint32_t Pid, uint64_t CreateTime, ProcessDetails& Out )
{
    Out = {};
    if( Pid == 0 || Pid == 4 )
        return false;
    Handle Process( OpenProcess( PROCESS_QUERY_LIMITED_INFORMATION, FALSE, Pid ) );
    if( !Process.value )
        return false;
    if( CreateTime && ProcessCreateTime( Process.value ) != CreateTime )
        return false;
    Out.opened = true;
    Out.imagePath = ProcessImagePath( Process.value );
    Out.commandLine = CommandLine( Process.value );
    Out.priorityClass = GetPriorityClass( Process.value );
    BOOL Critical = FALSE;
    if( IsProcessCritical( Process.value, &Critical ) )
        Out.critical = Critical != FALSE;
    PROCESS_PROTECTION_LEVEL_INFORMATION Protection{};
    if( GetProcessInformation( Process.value, ProcessProtectionLevelInfo, &Protection, sizeof( Protection ) ) )
        Out.protectedProcess = Protection.ProtectionLevel != PROTECTION_LEVEL_NONE;
    USHORT ProcessMachine = 0, NativeMachine = 0;
    if( IsWow64Process2( Process.value, &ProcessMachine, &NativeMachine ) )
        Out.wow64 = ProcessMachine != IMAGE_FILE_MACHINE_UNKNOWN;
    UINT32 PackageLength = 0;
    if( GetPackageFullName( Process.value, &PackageLength, nullptr ) == ERROR_INSUFFICIENT_BUFFER && PackageLength )
    {
        std::wstring Package( PackageLength, L'\0' );
        if( GetPackageFullName( Process.value, &PackageLength, Package.data() ) == ERROR_SUCCESS )
        {
            Package.resize( wcslen( Package.c_str() ) );
            Out.packaged = true;
            Out.packageName = Package;
        }
    }
    HANDLE RawToken = nullptr;
    if( OpenProcessToken( Process.value, TOKEN_QUERY, &RawToken ) )
    {
        Handle Token( RawToken );
        Out.tokenRead = true;
        if( const auto User = TokenInformation( RawToken, TokenUser ); !User.empty() )
        {
            Out.userSid = SidString( reinterpret_cast<const TOKEN_USER*>( User.data() )->User.Sid );
            Out.userName = AccountName( Out.userSid );
        }
        if( const auto Label = TokenInformation( RawToken, TokenIntegrityLevel ); !Label.empty() )
        {
            const auto* Mandatory = reinterpret_cast<const TOKEN_MANDATORY_LABEL*>( Label.data() );
            const auto Count = *GetSidSubAuthorityCount( Mandatory->Label.Sid );
            if( Count > 0 )
                Out.integrity = IntegrityFromRid( *GetSidSubAuthority( Mandatory->Label.Sid, Count - 1 ) );
        }
        TOKEN_ELEVATION Elevation{};
        DWORD Size = 0;
        if( GetTokenInformation( RawToken, TokenElevation, &Elevation, sizeof( Elevation ), &Size ) )
            Out.elevated = Elevation.TokenIsElevated != 0;
    }
    return true;
}

bool IsSystemCriticalName( const std::wstring& ImageName )
{
    static const std::vector<std::wstring> Names{ L"system", L"registry", L"smss.exe", L"csrss.exe", L"wininit.exe", L"services.exe",
                                                  L"lsass.exe", L"winlogon.exe", L"memory compression", L"secure system", L"lsaiso.exe" };
    const auto Name = Lower( FileName( ImageName ) );
    return std::find( Names.begin(), Names.end(), Name ) != Names.end();
}

ActionCheck CheckAction( void* Process, uint32_t Pid, uint32_t CallerPid, const std::vector<std::wstring>& Blocked, const std::vector<std::wstring>& Product )
{
    if( Pid <= 4 )
        return { Verdict::Blocked, "system-process" };
    if( Pid == CallerPid || Pid == GetCurrentProcessId() )
        return { Verdict::Blocked, "self" };
    const auto Image = Process ? ProcessImagePath( Process ) : std::wstring{};
    if( Image.empty() )
        return { Verdict::Blocked, "process-unverified" };
    const auto Name = Lower( FileName( Image ) );
    for( const auto& Item : Blocked )
        if( Lower( Item ) == Name )
            return { Verdict::Blocked, "self" };
    BOOL Critical = FALSE;
    if( IsProcessCritical( HANDLE( Process ), &Critical ) && Critical )
        return { Verdict::Blocked, "critical-process" };
    static const std::wstring WindowsDirectory = [] {
        wchar_t Buffer[MAX_PATH]{};
        const UINT Length = GetWindowsDirectoryW( Buffer, MAX_PATH );
        return Lower( std::wstring( Buffer, Length ) + L"\\" );
    }();
    if( IsSystemCriticalName( Image ) && Lower( Image ).rfind( WindowsDirectory, 0 ) == 0 )
        return { Verdict::Blocked, "critical-process" };
    PROCESS_PROTECTION_LEVEL_INFORMATION Protection{};
    if( GetProcessInformation( HANDLE( Process ), ProcessProtectionLevelInfo, &Protection, sizeof( Protection ) )
        && Protection.ProtectionLevel != PROTECTION_LEVEL_NONE )
        return { Verdict::Blocked, "protected-process" };
    for( const auto& Item : Product )
        if( Lower( Item ) == Name )
            return { Verdict::Warn, "product-process" };
    return { Verdict::Allowed, {} };
}

} // namespace hc::system
