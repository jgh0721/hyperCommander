#include "serviceControl.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <winsvc.h>

#include <algorithm>
#include <unordered_map>
#include <chrono>
#include <cwctype>

namespace hc::system {

namespace {
struct ServiceHandle
{
    SC_HANDLE value = nullptr;
    explicit ServiceHandle( SC_HANDLE Value = nullptr ) : value( Value ) {}
    ~ServiceHandle() { if( value ) CloseServiceHandle( value ); }
    ServiceHandle( const ServiceHandle& ) = delete;
    ServiceHandle& operator=( const ServiceHandle& ) = delete;
    explicit operator bool() const { return value != nullptr; }
};

std::vector<std::wstring> MultiString( const wchar_t* Value )
{
    std::vector<std::wstring> Result;
    while( Value && *Value )
    {
        Result.emplace_back( Value );
        Value += Result.back().size() + 1;
    }
    return Result;
}

template <typename T>
bool Config2( SC_HANDLE Service, DWORD Level, std::vector<unsigned char>& Buffer, const T*& Out )
{
    DWORD Needed = 0;
    QueryServiceConfig2W( Service, Level, nullptr, 0, &Needed );
    if( Needed == 0 )
        return false;
    Buffer.resize( Needed );
    if( !QueryServiceConfig2W( Service, Level, Buffer.data(), Needed, &Needed ) )
        return false;
    Out = reinterpret_cast<const T*>( Buffer.data() );
    return true;
}

ServiceStatusInfo FromProcess( const wchar_t* Name, const wchar_t* Display, const SERVICE_STATUS_PROCESS& Status )
{
    ServiceStatusInfo Info;
    Info.name = Name ? Name : L"";
    Info.displayName = Display ? Display : L"";
    Info.type = Status.dwServiceType;
    Info.state = ServiceStateFromNative( Status.dwCurrentState );
    Info.pid = Status.dwProcessId;
    Info.controlsAccepted = Status.dwControlsAccepted;
    Info.win32ExitCode = Status.dwWin32ExitCode;
    Info.serviceExitCode = Status.dwServiceSpecificExitCode;
    Info.driver = ( Status.dwServiceType & SERVICE_DRIVER ) != 0;
    return Info;
}

std::wstring Expand( const std::wstring& Text )
{
    const DWORD Size = ExpandEnvironmentStringsW( Text.c_str(), nullptr, 0 );
    if( Size == 0 )
        return Text;
    std::wstring Result( Size, L'\0' );
    ExpandEnvironmentStringsW( Text.c_str(), Result.data(), Size );
    Result.resize( wcslen( Result.c_str() ) );
    return Result;
}

std::wstring WindowsDirectory()
{
    wchar_t Buffer[MAX_PATH]{};
    const UINT Length = GetWindowsDirectoryW( Buffer, MAX_PATH );
    return std::wstring( Buffer, Length );
}

bool Exists( const std::wstring& Path )
{
    const DWORD Attributes = GetFileAttributesW( Path.c_str() );
    return Attributes != INVALID_FILE_ATTRIBUTES && !( Attributes & FILE_ATTRIBUTE_DIRECTORY );
}

DWORD TargetState( ServiceControl Control )
{
    switch( Control )
    {
        case ServiceControl::Start:    return SERVICE_RUNNING;
        case ServiceControl::Stop:     return SERVICE_STOPPED;
        case ServiceControl::Pause:    return SERVICE_PAUSED;
        default:                       return SERVICE_RUNNING;
    }
}

uint32_t WaitState( SC_HANDLE Service, DWORD Target, uint32_t TimeoutMs )
{
    const auto Deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds( TimeoutMs );
    for( ;; )
    {
        SERVICE_STATUS_PROCESS Status{};
        DWORD Needed = 0;
        if( !QueryServiceStatusEx( Service, SC_STATUS_PROCESS_INFO, reinterpret_cast<LPBYTE>( &Status ), sizeof( Status ), &Needed ) )
            return GetLastError();
        if( Status.dwCurrentState == Target )
            return ERROR_SUCCESS;
        // 시작을 기다리는데 멈췄다면 서비스가 시작에 실패한 것이다.
        if( Target == SERVICE_RUNNING && Status.dwCurrentState == SERVICE_STOPPED )
            return Status.dwWin32ExitCode && Status.dwWin32ExitCode != ERROR_SERVICE_SPECIFIC_ERROR ? Status.dwWin32ExitCode : ERROR_SERVICE_NOT_ACTIVE;
        if( std::chrono::steady_clock::now() >= Deadline )
            return ERROR_SERVICE_REQUEST_TIMEOUT;
        Sleep( std::clamp<DWORD>( Status.dwWaitHint / 10, 100, 1000 ) );
    }
}
} // namespace

const wchar_t* ServiceStateName( ServiceState State )
{
    switch( State )
    {
        case ServiceState::Stopped:         return L"stopped";
        case ServiceState::StartPending:    return L"startPending";
        case ServiceState::StopPending:     return L"stopPending";
        case ServiceState::Running:         return L"running";
        case ServiceState::ContinuePending: return L"continuePending";
        case ServiceState::PausePending:    return L"pausePending";
        case ServiceState::Paused:          return L"paused";
        default:                            return L"unknown";
    }
}

ServiceState ServiceStateFromNative( uint32_t State )
{
    switch( State )
    {
        case SERVICE_STOPPED:          return ServiceState::Stopped;
        case SERVICE_START_PENDING:    return ServiceState::StartPending;
        case SERVICE_STOP_PENDING:     return ServiceState::StopPending;
        case SERVICE_RUNNING:          return ServiceState::Running;
        case SERVICE_CONTINUE_PENDING: return ServiceState::ContinuePending;
        case SERVICE_PAUSE_PENDING:    return ServiceState::PausePending;
        case SERVICE_PAUSED:           return ServiceState::Paused;
        default:                       return ServiceState::Unknown;
    }
}

uint32_t ServiceControlCode( ServiceControl Control )
{
    switch( Control )
    {
        case ServiceControl::Stop:     return SERVICE_CONTROL_STOP;
        case ServiceControl::Pause:    return SERVICE_CONTROL_PAUSE;
        case ServiceControl::Continue: return SERVICE_CONTROL_CONTINUE;
        default:                       return 0;
    }
}

bool EnumerateServices( bool IncludeDrivers, std::vector<ServiceStatusInfo>& Out, uint32_t* Error )
{
    Out.clear();
    ServiceHandle Manager( OpenSCManagerW( nullptr, nullptr, SC_MANAGER_CONNECT | SC_MANAGER_ENUMERATE_SERVICE ) );
    if( !Manager )
    {
        if( Error ) *Error = GetLastError();
        return false;
    }
    const DWORD Types = SERVICE_WIN32 | ( IncludeDrivers ? SERVICE_DRIVER : 0 );
    std::vector<unsigned char> Buffer( 256 * 1024 );
    DWORD Resume = 0;
    for( ;; )
    {
        DWORD Needed = 0, Returned = 0;
        const BOOL Done = EnumServicesStatusExW( Manager.value, SC_ENUM_PROCESS_INFO, Types, SERVICE_STATE_ALL, Buffer.data(), DWORD( Buffer.size() ),
                                                 &Needed, &Returned, &Resume, nullptr );
        const DWORD Code = Done ? ERROR_SUCCESS : GetLastError();
        if( !Done && Code != ERROR_MORE_DATA )
        {
            if( Error ) *Error = Code;
            return false;
        }
        const auto* Items = reinterpret_cast<const ENUM_SERVICE_STATUS_PROCESSW*>( Buffer.data() );
        for( DWORD Index = 0; Index < Returned; ++Index )
            Out.push_back( FromProcess( Items[Index].lpServiceName, Items[Index].lpDisplayName, Items[Index].ServiceStatusProcess ) );
        if( Done )
            break;
        if( Needed > Buffer.size() )
            Buffer.resize( Needed );
    }
    return true;
}

bool QueryServiceStatusInfo( const std::wstring& Name, ServiceStatusInfo& Out, uint32_t* Error )
{
    ServiceHandle Manager( OpenSCManagerW( nullptr, nullptr, SC_MANAGER_CONNECT ) );
    ServiceHandle Service( Manager ? OpenServiceW( Manager.value, Name.c_str(), SERVICE_QUERY_STATUS | SERVICE_QUERY_CONFIG ) : nullptr );
    if( !Service )
    {
        if( Error ) *Error = GetLastError();
        return false;
    }
    SERVICE_STATUS_PROCESS Status{};
    DWORD Needed = 0;
    if( !QueryServiceStatusEx( Service.value, SC_STATUS_PROCESS_INFO, reinterpret_cast<LPBYTE>( &Status ), sizeof( Status ), &Needed ) )
    {
        if( Error ) *Error = GetLastError();
        return false;
    }
    std::wstring Display;
    DWORD Length = 0;
    GetServiceDisplayNameW( Manager.value, Name.c_str(), nullptr, &Length );
    if( Length )
    {
        Display.resize( Length + 1 );
        Length = DWORD( Display.size() );
        if( GetServiceDisplayNameW( Manager.value, Name.c_str(), Display.data(), &Length ) )
            Display.resize( Length );
        else
            Display.clear();
    }
    Out = FromProcess( Name.c_str(), Display.c_str(), Status );
    return true;
}

bool QueryServiceConfiguration( const std::wstring& Name, ServiceConfig& Out, uint32_t* Error )
{
    Out = {};
    ServiceHandle Manager( OpenSCManagerW( nullptr, nullptr, SC_MANAGER_CONNECT ) );
    ServiceHandle Service( Manager ? OpenServiceW( Manager.value, Name.c_str(), SERVICE_QUERY_CONFIG ) : nullptr );
    if( !Service )
    {
        if( Error ) *Error = GetLastError();
        return false;
    }
    DWORD Needed = 0;
    QueryServiceConfigW( Service.value, nullptr, 0, &Needed );
    std::vector<unsigned char> Buffer( std::max<DWORD>( Needed, sizeof( QUERY_SERVICE_CONFIGW ) ) );
    if( !QueryServiceConfigW( Service.value, reinterpret_cast<LPQUERY_SERVICE_CONFIGW>( Buffer.data() ), DWORD( Buffer.size() ), &Needed ) )
    {
        if( Error ) *Error = GetLastError();
        return false;
    }
    const auto* Config = reinterpret_cast<const QUERY_SERVICE_CONFIGW*>( Buffer.data() );
    Out.type = Config->dwServiceType;
    Out.errorControl = Config->dwErrorControl;
    Out.binaryPath = Config->lpBinaryPathName ? Config->lpBinaryPathName : L"";
    Out.loadOrderGroup = Config->lpLoadOrderGroup ? Config->lpLoadOrderGroup : L"";
    Out.account = Config->lpServiceStartName ? Config->lpServiceStartName : L"";
    Out.displayName = Config->lpDisplayName ? Config->lpDisplayName : L"";
    Out.dependencies = MultiString( Config->lpDependencies );
    switch( Config->dwStartType )
    {
        case SERVICE_BOOT_START:   Out.start = StartType::Boot; break;
        case SERVICE_SYSTEM_START: Out.start = StartType::System; break;
        case SERVICE_AUTO_START:   Out.start = StartType::Auto; break;
        case SERVICE_DEMAND_START: Out.start = StartType::Manual; break;
        case SERVICE_DISABLED:     Out.start = StartType::Disabled; break;
        default:                   Out.start = StartType::Unknown; break;
    }
    std::vector<unsigned char> Extra;
    if( const SERVICE_DESCRIPTIONW* Description = nullptr; Config2( Service.value, SERVICE_CONFIG_DESCRIPTION, Extra, Description ) && Description->lpDescription )
        Out.description = Description->lpDescription;
    if( const SERVICE_DELAYED_AUTO_START_INFO* Delayed = nullptr; Out.start == StartType::Auto && Config2( Service.value, SERVICE_CONFIG_DELAYED_AUTO_START_INFO, Extra, Delayed ) && Delayed->fDelayedAutostart )
        Out.start = StartType::AutoDelayed;
    if( const SERVICE_TRIGGER_INFO* Triggers = nullptr; Config2( Service.value, SERVICE_CONFIG_TRIGGER_INFO, Extra, Triggers ) )
    {
        Out.triggerCount = Triggers->cTriggers;
        Out.triggerStart = Triggers->cTriggers > 0;
    }
    if( const SERVICE_FAILURE_ACTIONSW* Failure = nullptr; Config2( Service.value, SERVICE_CONFIG_FAILURE_ACTIONS, Extra, Failure ) )
    {
        Out.failure.resetPeriodSec = Failure->dwResetPeriod == INFINITE ? 0 : Failure->dwResetPeriod;
        Out.failure.rebootMessage = Failure->lpRebootMsg ? Failure->lpRebootMsg : L"";
        Out.failure.command = Failure->lpCommand ? Failure->lpCommand : L"";
        for( DWORD Index = 0; Failure->lpsaActions && Index < Failure->cActions; ++Index )
            Out.failure.actions.push_back( { uint32_t( Failure->lpsaActions[Index].Type ), Failure->lpsaActions[Index].Delay } );
    }
    if( const SERVICE_FAILURE_ACTIONS_FLAG* Flag = nullptr; Config2( Service.value, SERVICE_CONFIG_FAILURE_ACTIONS_FLAG, Extra, Flag ) )
        Out.failure.onNonCrashFailures = Flag->fFailureActionsOnNonCrashFailures != FALSE;
    if( const SERVICE_SID_INFO* Sid = nullptr; Config2( Service.value, SERVICE_CONFIG_SERVICE_SID_INFO, Extra, Sid ) )
        Out.sidType = Sid->dwServiceSidType;
    if( const SERVICE_REQUIRED_PRIVILEGES_INFOW* Privileges = nullptr; Config2( Service.value, SERVICE_CONFIG_REQUIRED_PRIVILEGES_INFO, Extra, Privileges ) )
        Out.privileges = MultiString( Privileges->pmszRequiredPrivileges );
    if( const SERVICE_LAUNCH_PROTECTED_INFO* Protected = nullptr; Config2( Service.value, SERVICE_CONFIG_LAUNCH_PROTECTED, Extra, Protected ) )
        Out.launchProtected = Protected->dwLaunchProtected;
    Out.serviceDll = ReadServiceDll( Name );
    Out.resolvedImage = Out.serviceDll.empty() ? ResolveServiceImage( Out.binaryPath ) : Out.serviceDll;
    return true;
}

bool QueryDependents( const std::wstring& Name, std::vector<ServiceStatusInfo>& Out, uint32_t* Error )
{
    Out.clear();
    ServiceHandle Manager( OpenSCManagerW( nullptr, nullptr, SC_MANAGER_CONNECT ) );
    ServiceHandle Service( Manager ? OpenServiceW( Manager.value, Name.c_str(), SERVICE_ENUMERATE_DEPENDENTS ) : nullptr );
    if( !Service )
    {
        if( Error ) *Error = GetLastError();
        return false;
    }
    DWORD Needed = 0, Returned = 0;
    if( EnumDependentServicesW( Service.value, SERVICE_STATE_ALL, nullptr, 0, &Needed, &Returned ) )
        return true;
    if( GetLastError() != ERROR_MORE_DATA )
    {
        if( Error ) *Error = GetLastError();
        return false;
    }
    std::vector<unsigned char> Buffer( Needed );
    if( !EnumDependentServicesW( Service.value, SERVICE_STATE_ALL, reinterpret_cast<LPENUM_SERVICE_STATUSW>( Buffer.data() ), Needed, &Needed, &Returned ) )
    {
        if( Error ) *Error = GetLastError();
        return false;
    }
    const auto* Items = reinterpret_cast<const ENUM_SERVICE_STATUSW*>( Buffer.data() );
    for( DWORD Index = 0; Index < Returned; ++Index )
    {
        ServiceStatusInfo Info;
        Info.name = Items[Index].lpServiceName;
        Info.displayName = Items[Index].lpDisplayName;
        Info.type = Items[Index].ServiceStatus.dwServiceType;
        Info.state = ServiceStateFromNative( Items[Index].ServiceStatus.dwCurrentState );
        Info.controlsAccepted = Items[Index].ServiceStatus.dwControlsAccepted;
        Info.driver = ( Info.type & SERVICE_DRIVER ) != 0;
        Out.push_back( std::move( Info ) );
    }
    return true;
}

std::wstring ResolveServiceImage( const std::wstring& BinaryPath )
{
    std::wstring Text = BinaryPath;
    while( !Text.empty() && std::iswspace( Text.front() ) )
        Text.erase( Text.begin() );
    if( Text.empty() )
        return {};
    std::wstring Candidate;
    if( Text.front() == L'"' )
    {
        const auto Close = Text.find( L'"', 1 );
        Candidate = Text.substr( 1, Close == std::wstring::npos ? std::wstring::npos : Close - 1 );
    }
    else
        Candidate = Text;
    Candidate = Expand( Candidate );
    const auto Windows = WindowsDirectory();
    if( _wcsnicmp( Candidate.c_str(), L"\\SystemRoot\\", 12 ) == 0 )
        Candidate = Windows + Candidate.substr( 11 );
    else if( _wcsnicmp( Candidate.c_str(), L"\\??\\", 4 ) == 0 )
        Candidate = Candidate.substr( 4 );
    else if( _wcsnicmp( Candidate.c_str(), L"System32\\", 9 ) == 0 || _wcsnicmp( Candidate.c_str(), L"SysWOW64\\", 9 ) == 0 )
        Candidate = Windows + L"\\" + Candidate;
    if( Text.front() == L'"' || Exists( Candidate ) )
        return Candidate;
    // 따옴표 없이 공백이 들어간 경로: 공백마다 잘라 가장 긴 실제 파일을 고른다.
    std::wstring Best;
    for( size_t Space = Candidate.find( L' ' ); Space != std::wstring::npos; Space = Candidate.find( L' ', Space + 1 ) )
        if( Exists( Candidate.substr( 0, Space ) ) )
            Best = Candidate.substr( 0, Space );
    if( !Best.empty() )
        return Best;
    const auto Space = Candidate.find( L' ' );
    return Space == std::wstring::npos ? Candidate : Candidate.substr( 0, Space );
}

std::wstring ReadServiceDll( const std::wstring& Name )
{
    for( const auto& Key : { L"SYSTEM\\CurrentControlSet\\Services\\" + Name + L"\\Parameters", L"SYSTEM\\CurrentControlSet\\Services\\" + Name } )
    {
        DWORD Size = 0;
        if( RegGetValueW( HKEY_LOCAL_MACHINE, Key.c_str(), L"ServiceDll", RRF_RT_REG_SZ, nullptr, nullptr, &Size ) != ERROR_SUCCESS || Size == 0 )
            continue;
        std::wstring Value( Size / sizeof( wchar_t ) + 1, L'\0' );
        Size = DWORD( Value.size() * sizeof( wchar_t ) );
        if( RegGetValueW( HKEY_LOCAL_MACHINE, Key.c_str(), L"ServiceDll", RRF_RT_REG_SZ, nullptr, Value.data(), &Size ) == ERROR_SUCCESS )
        {
            Value.resize( wcslen( Value.c_str() ) );
            return Value;
        }
    }
    return {};
}

uint32_t ControlServiceByName( const std::wstring& Name, ServiceControl Control, const std::vector<std::wstring>& Arguments, uint32_t TimeoutMs )
{
    ServiceHandle Manager( OpenSCManagerW( nullptr, nullptr, SC_MANAGER_CONNECT ) );
    if( !Manager )
        return GetLastError();
    DWORD Access = SERVICE_QUERY_STATUS;
    switch( Control )
    {
        case ServiceControl::Start: Access |= SERVICE_START; break;
        case ServiceControl::Stop:  Access |= SERVICE_STOP; break;
        default:                    Access |= SERVICE_PAUSE_CONTINUE; break;
    }
    ServiceHandle Service( OpenServiceW( Manager.value, Name.c_str(), Access ) );
    if( !Service )
        return GetLastError();
    if( Control == ServiceControl::Start )
    {
        std::vector<LPCWSTR> Pointers;
        for( const auto& Argument : Arguments )
            Pointers.push_back( Argument.c_str() );
        if( !StartServiceW( Service.value, DWORD( Pointers.size() ), Pointers.empty() ? nullptr : Pointers.data() ) )
        {
            const DWORD Error = GetLastError();
            if( Error != ERROR_SERVICE_ALREADY_RUNNING )
                return Error;
        }
    }
    else
    {
        SERVICE_STATUS Status{};
        if( !ControlService( Service.value, ServiceControlCode( Control ), &Status ) )
            return GetLastError();
    }
    return WaitState( Service.value, TargetState( Control ), TimeoutMs );
}

uint32_t RestartServiceByName( const std::wstring& Name, uint32_t TimeoutMs )
{
    ServiceStatusInfo Status;
    if( QueryServiceStatusInfo( Name, Status ) && Status.state != ServiceState::Stopped )
        if( const auto Error = ControlServiceByName( Name, ServiceControl::Stop, {}, TimeoutMs / 2 ) )
            return Error;
    return ControlServiceByName( Name, ServiceControl::Start, {}, TimeoutMs / 2 );
}

uint32_t ApplyServiceConfig( const std::wstring& Name, const ServiceConfigChange& Change )
{
    ServiceHandle Manager( OpenSCManagerW( nullptr, nullptr, SC_MANAGER_CONNECT ) );
    if( !Manager )
        return GetLastError();
    DWORD Access = SERVICE_CHANGE_CONFIG | SERVICE_QUERY_CONFIG;
    if( Change.failure )
        for( const auto& Action : Change.failure->actions )
            if( Action.type == SC_ACTION_RESTART )
                Access |= SERVICE_START;
    ServiceHandle Service( OpenServiceW( Manager.value, Name.c_str(), Access ) );
    if( !Service )
        return GetLastError();
    const bool Basic = Change.start || Change.displayName || Change.errorControl || Change.account;
    if( Basic )
    {
        DWORD Start = SERVICE_NO_CHANGE;
        if( Change.start )
        {
            switch( *Change.start )
            {
                case StartType::Boot:        Start = SERVICE_BOOT_START; break;
                case StartType::System:      Start = SERVICE_SYSTEM_START; break;
                case StartType::Auto:
                case StartType::AutoDelayed: Start = SERVICE_AUTO_START; break;
                case StartType::Manual:      Start = SERVICE_DEMAND_START; break;
                case StartType::Disabled:    Start = SERVICE_DISABLED; break;
                default:                     break;
            }
        }
        std::wstring Password = Change.password.value_or( std::wstring{} );
        const BOOL Done = ChangeServiceConfigW( Service.value, SERVICE_NO_CHANGE, Start, Change.errorControl ? *Change.errorControl : SERVICE_NO_CHANGE,
                                                nullptr, nullptr, nullptr, nullptr,
                                                Change.account ? Change.account->c_str() : nullptr,
                                                Change.account && Change.password ? Password.c_str() : nullptr,
                                                Change.displayName ? Change.displayName->c_str() : nullptr );
        const DWORD Error = Done ? ERROR_SUCCESS : GetLastError();
        if( !Password.empty() )
            SecureZeroMemory( Password.data(), Password.size() * sizeof( wchar_t ) );
        if( Error )
            return Error;
    }
    if( Change.start && ( *Change.start == StartType::Auto || *Change.start == StartType::AutoDelayed ) )
    {
        SERVICE_DELAYED_AUTO_START_INFO Delayed{ *Change.start == StartType::AutoDelayed };
        if( !ChangeServiceConfig2W( Service.value, SERVICE_CONFIG_DELAYED_AUTO_START_INFO, &Delayed ) )
            return GetLastError();
    }
    if( Change.description )
    {
        std::wstring Text = *Change.description;
        SERVICE_DESCRIPTIONW Description{ Text.data() };
        if( !ChangeServiceConfig2W( Service.value, SERVICE_CONFIG_DESCRIPTION, &Description ) )
            return GetLastError();
    }
    if( Change.failure )
    {
        std::vector<SC_ACTION> Actions;
        for( const auto& Action : Change.failure->actions )
            Actions.push_back( { SC_ACTION_TYPE( Action.type ), Action.delayMs } );
        std::wstring Reboot = Change.failure->rebootMessage, Command = Change.failure->command;
        SERVICE_FAILURE_ACTIONSW Failure{};
        Failure.dwResetPeriod = Change.failure->resetPeriodSec;
        Failure.lpRebootMsg = Reboot.data();
        Failure.lpCommand = Command.data();
        Failure.cActions = DWORD( Actions.size() );
        Failure.lpsaActions = Actions.empty() ? nullptr : Actions.data();
        if( !ChangeServiceConfig2W( Service.value, SERVICE_CONFIG_FAILURE_ACTIONS, &Failure ) )
            return GetLastError();
        SERVICE_FAILURE_ACTIONS_FLAG Flag{ Change.failure->onNonCrashFailures };
        if( !ChangeServiceConfig2W( Service.value, SERVICE_CONFIG_FAILURE_ACTIONS_FLAG, &Flag ) )
            return GetLastError();
    }
    if( Change.sidType )
    {
        SERVICE_SID_INFO Sid{ *Change.sidType };
        if( !ChangeServiceConfig2W( Service.value, SERVICE_CONFIG_SERVICE_SID_INFO, &Sid ) )
            return GetLastError();
    }
    return ERROR_SUCCESS;
}

/////////////////////////////////////////////////////////////////////////////
/// ServiceMonitor

namespace {
struct Watch
{
    SC_HANDLE       handle = nullptr;
    SERVICE_NOTIFYW notify{};
    bool            dirty  = false;
    bool            armed  = false;
};

VOID CALLBACK NotifyCallback( PVOID Parameter )
{
    auto* Notify = static_cast<SERVICE_NOTIFYW*>( Parameter );
    if( auto* Item = static_cast<Watch*>( Notify->pContext ) )
    {
        Item->dirty = true;
        Item->armed = false;
    }
}

constexpr DWORD NotifyMask = SERVICE_NOTIFY_STOPPED | SERVICE_NOTIFY_START_PENDING | SERVICE_NOTIFY_STOP_PENDING | SERVICE_NOTIFY_RUNNING
                             | SERVICE_NOTIFY_CONTINUE_PENDING | SERVICE_NOTIFY_PAUSE_PENDING | SERVICE_NOTIFY_PAUSED | SERVICE_NOTIFY_DELETE_PENDING;

// 서비스가 이미 마스크의 상태에 있으면 NotifyServiceStatusChange는 콜백을 바로 대기열에 넣는다.
// 지금 상태를 빼고 걸어야 실제로 바뀔 때만 알림이 온다(전부 넣으면 알림·재등록이 끝없이 되풀이된다).
DWORD MaskExcluding( DWORD State )
{
    return State >= SERVICE_STOPPED && State <= SERVICE_PAUSED ? NotifyMask & ~( 1u << ( State - 1 ) ) : NotifyMask;
}

bool Arm( Watch& Item, DWORD Mask )
{
    Item.notify = {};
    Item.notify.dwVersion = SERVICE_NOTIFY_STATUS_CHANGE;
    Item.notify.pfnNotifyCallback = NotifyCallback;
    Item.notify.pContext = &Item;
    Item.armed = NotifyServiceStatusChangeW( Item.handle, Mask, &Item.notify ) == ERROR_SUCCESS;
    return Item.armed;
}
} // namespace

ServiceMonitor::~ServiceMonitor() { Stop(); }

void ServiceMonitor::Start( bool IncludeDrivers, Callback Changed )
{
    Stop();
    includeDrivers_ = IncludeDrivers;
    changed_ = std::move( Changed );
    refresh_ = true;
    thread_ = std::jthread( [this]( std::stop_token Stop ) { run( Stop ); } );
}

void ServiceMonitor::Stop()
{
    if( thread_.joinable() )
    {
        thread_.request_stop();
        thread_.join();
    }
}

void ServiceMonitor::run( std::stop_token Stop )
{
    ServiceHandle Manager( OpenSCManagerW( nullptr, nullptr, SC_MANAGER_CONNECT | SC_MANAGER_ENUMERATE_SERVICE ) );
    // 서비스 이름별 감시. 알림 APC가 구조체를 가리키므로 다시 열거할 때도 지우지 않고 재사용하며,
    // 사라진 서비스의 구조체는 핸들만 닫아 두었다가 스레드가 끝날 때 해제한다.
    std::unordered_map<std::wstring, std::unique_ptr<Watch>> Watches;
    std::vector<std::unique_ptr<Watch>> Retired;
    Watch ManagerWatch;
    auto LastFull = std::chrono::steady_clock::now() - std::chrono::hours( 1 );
    while( !Stop.stop_requested() )
    {
        const auto Now = std::chrono::steady_clock::now();
        const bool Periodic = Now - LastFull >= std::chrono::seconds( notifications_ ? 15 : 3 );
        if( refresh_.exchange( false ) || Periodic )
        {
            std::vector<ServiceStatusInfo> List;
            if( EnumerateServices( includeDrivers_, List ) )
            {
                std::unordered_map<std::wstring, bool> Seen;
                bool AnyArmed = false;
                for( const auto& Info : List )
                {
                    Seen.emplace( Info.name, true );
                    auto& Slot = Watches[Info.name];
                    if( !Slot )
                    {
                        Slot = std::make_unique<Watch>();
                        Slot->handle = Manager ? OpenServiceW( Manager.value, Info.name.c_str(), SERVICE_QUERY_STATUS ) : nullptr;
                    }
                    // 걸려 있는 알림은 그대로 둔다. 콜백이 왔는데 아직 처리하지 않은 것은 아래에서 다시 건다.
                    if( Slot->handle && !Slot->armed && !Slot->dirty )
                        Arm( *Slot, MaskExcluding( DWORD( Info.state ) ) );
                    AnyArmed |= Slot->armed;
                }
                for( auto It = Watches.begin(); It != Watches.end(); )
                {
                    if( Seen.count( It->first ) )
                    {
                        ++It;
                        continue;
                    }
                    if( It->second->handle )
                    {
                        CloseServiceHandle( It->second->handle );
                        It->second->handle = nullptr;
                    }
                    Retired.push_back( std::move( It->second ) );
                    It = Watches.erase( It );
                }
                notifications_ = AnyArmed;
                if( Manager && !ManagerWatch.armed )
                {
                    ManagerWatch.handle = Manager.value;
                    Arm( ManagerWatch, SERVICE_NOTIFY_CREATED | SERVICE_NOTIFY_DELETED );
                }
                LastFull = Now;
                if( changed_ )
                    changed_( std::move( List ) );
            }
        }
        SleepEx( 250, TRUE );
        if( ManagerWatch.dirty )
        {
            ManagerWatch.dirty = false;
            if( ManagerWatch.notify.pszServiceNames )
            {
                LocalFree( ManagerWatch.notify.pszServiceNames );
                ManagerWatch.notify.pszServiceNames = nullptr;
            }
            refresh_ = true;
            Arm( ManagerWatch, SERVICE_NOTIFY_CREATED | SERVICE_NOTIFY_DELETED );
        }
        bool Emit = false;
        for( auto& [Name, Item] : Watches )
        {
            if( !Item->dirty )
                continue;
            Item->dirty = false;
            Emit = true;
            if( Item->notify.dwNotificationStatus == ERROR_SUCCESS )
                Arm( *Item, MaskExcluding( Item->notify.ServiceStatus.dwCurrentState ) );
            else
                refresh_ = true;   // 삭제 표시 등: 다시 열거해 정리한다
        }
        if( Emit && !refresh_ )
        {
            std::vector<ServiceStatusInfo> List;
            if( EnumerateServices( includeDrivers_, List ) && changed_ )
                changed_( std::move( List ) );
        }
    }
    // 핸들을 닫아 걸린 알림을 취소하고, 이미 대기열에 들어온 APC를 구조체가 살아 있는 동안 비운다.
    for( auto& [Name, Item] : Watches )
        if( Item->handle )
        {
            CloseServiceHandle( Item->handle );
            Item->handle = nullptr;
        }
    if( Manager.value )
    {
        CloseServiceHandle( Manager.value );
        Manager.value = nullptr;
    }
    SleepEx( 0, TRUE );
    ManagerWatch.handle = nullptr;
    Watches.clear();
    Retired.clear();
}

} // namespace hc::system
