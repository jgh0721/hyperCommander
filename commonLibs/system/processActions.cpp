#include "processActions.hpp"

#include "ntApi.hpp"

#include <shellapi.h>

#include <algorithm>
#include <cwctype>

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

bool SamePath( const std::wstring& Left, const std::wstring& Right )
{
    return Left.size() == Right.size() && _wcsicmp( Left.c_str(), Right.c_str() ) == 0;
}

ActionResult Fail( Outcome Value, uint32_t Code, std::string Reason = {} ) { return { Value, Code, std::move( Reason ) }; }

// 대상 프로세스를 다시 열고 같은 프로세스인지 확인한다.
ActionResult OpenVerified( const ProcessTarget& Target, DWORD Access, Handle& Out )
{
    Out.value = OpenProcess( Access | PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, Target.pid );
    if( !Out.value )
    {
        const DWORD Error = GetLastError();
        if( Error == ERROR_INVALID_PARAMETER )
            return Fail( Outcome::AlreadyGone, Error );
        return Fail( Error == ERROR_ACCESS_DENIED ? Outcome::AccessDenied : Outcome::Failed, Error );
    }
    if( ProcessCreateTime( Out.value ) != Target.createTime )
        return Fail( Outcome::TargetChanged, ERROR_INVALID_HANDLE, "pid-reused" );
    // 끝난 프로세스는 다른 핸들이 남아 있어도 이미지 경로를 읽을 수 없다. 종료 여부를 먼저 본다.
    if( WaitForSingleObject( Out.value, 0 ) == WAIT_OBJECT_0 )
        return Fail( Outcome::AlreadyGone, 0 );
    if( !Target.image.empty() && !SamePath( ProcessImagePath( Out.value ), Target.image ) )
        return Fail( Outcome::TargetChanged, ERROR_INVALID_HANDLE, "image-changed" );
    return { Outcome::Completed, 0, {} };
}

ActionResult Allowed( HANDLE Process, const ProcessTarget& Target, const ActionLimits& Limits )
{
    const auto Check = CheckAction( Process, Target.pid, Limits.callerPid, Limits.blocked, Limits.product );
    if( Check.verdict == Verdict::Blocked || ( Check.verdict == Verdict::Warn && !Limits.allowWarned ) )
        return Fail( Outcome::Rejected, ERROR_ACCESS_DENIED, Check.reason );
    return { Outcome::Completed, 0, {} };
}

struct WindowSearch
{
    DWORD             pid = 0;
    std::vector<HWND> windows;
};

BOOL CALLBACK CollectWindow( HWND Window, LPARAM Parameter )
{
    auto* Search = reinterpret_cast<WindowSearch*>( Parameter );
    DWORD Owner = 0;
    GetWindowThreadProcessId( Window, &Owner );
    if( Owner == Search->pid && IsWindowVisible( Window ) && !GetWindow( Window, GW_OWNER ) )
        Search->windows.push_back( Window );
    return TRUE;
}

std::vector<HWND> TopLevelWindows( uint32_t Pid )
{
    WindowSearch Search;
    Search.pid = Pid;
    EnumWindows( CollectWindow, reinterpret_cast<LPARAM>( &Search ) );
    return Search.windows;
}
} // namespace

const char* OutcomeName( Outcome Value )
{
    switch( Value )
    {
        case Outcome::Completed:     return "Completed";
        case Outcome::Rejected:      return "Rejected";
        case Outcome::AccessDenied:  return "AccessDenied";
        case Outcome::AlreadyGone:   return "AlreadyGone";
        case Outcome::TargetChanged: return "TargetChanged";
        case Outcome::Failed:        return "Failed";
        default:                     return "Unknown";
    }
}

Outcome OutcomeFromName( const std::string& Name )
{
    for( const auto Value : { Outcome::Completed, Outcome::Rejected, Outcome::AccessDenied, Outcome::AlreadyGone, Outcome::TargetChanged, Outcome::Failed } )
        if( Name == OutcomeName( Value ) )
            return Value;
    return Outcome::Unknown;
}

ActionResult TerminateProcessTarget( const ProcessTarget& Target, const ActionLimits& Limits, uint32_t WaitMs )
{
    Handle Process;
    if( auto Opened = OpenVerified( Target, PROCESS_TERMINATE | SYNCHRONIZE, Process ); Opened.outcome != Outcome::Completed )
        return Opened;
    if( auto Check = Allowed( Process.value, Target, Limits ); Check.outcome != Outcome::Completed )
        return Check;
    if( !TerminateProcess( Process.value, 1 ) )
    {
        const DWORD Error = GetLastError();
        return Fail( Error == ERROR_ACCESS_DENIED ? Outcome::AccessDenied : Outcome::Failed, Error );
    }
    return WaitForSingleObject( Process.value, WaitMs ) == WAIT_OBJECT_0 ? ActionResult{ Outcome::Completed, 0, {} } : Fail( Outcome::Unknown, WAIT_TIMEOUT );
}

bool HasTopLevelWindows( uint32_t Pid, uint32_t* Count )
{
    const auto Windows = TopLevelWindows( Pid );
    if( Count ) *Count = uint32_t( Windows.size() );
    return !Windows.empty();
}

ActionResult RequestCloseWindows( const ProcessTarget& Target )
{
    Handle Process;
    if( auto Opened = OpenVerified( Target, SYNCHRONIZE, Process ); Opened.outcome != Outcome::Completed )
        return Opened;
    const auto Windows = TopLevelWindows( Target.pid );
    if( Windows.empty() )
        return Fail( Outcome::Rejected, ERROR_NOT_FOUND, "no-window" );
    for( const auto Window : Windows )
        PostMessageW( Window, WM_CLOSE, 0, 0 );
    return { Outcome::Completed, 0, {} };
}

bool WaitForExit( const ProcessTarget& Target, uint32_t TimeoutMs )
{
    Handle Process( OpenProcess( SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, Target.pid ) );
    if( !Process.value )
        return GetLastError() == ERROR_INVALID_PARAMETER;
    if( ProcessCreateTime( Process.value ) != Target.createTime )
        return true;
    return WaitForSingleObject( Process.value, TimeoutMs ) == WAIT_OBJECT_0;
}

ActionResult SuspendProcessTarget( const ProcessTarget& Target, bool Suspend, const ActionLimits& Limits )
{
    const auto& Api = nt::Functions();
    const auto Function = Suspend ? Api.SuspendProcess : Api.ResumeProcess;
    if( !Function )
        return Fail( Outcome::Failed, ERROR_PROC_NOT_FOUND );
    Handle Process;
    if( auto Opened = OpenVerified( Target, PROCESS_SUSPEND_RESUME, Process ); Opened.outcome != Outcome::Completed )
        return Opened;
    if( auto Check = Allowed( Process.value, Target, Limits ); Check.outcome != Outcome::Completed )
        return Check;
    const auto Status = Function( Process.value );
    if( !nt::Succeeded( Status ) )
        return Fail( Outcome::Failed, uint32_t( Status ) );
    return { Outcome::Completed, 0, {} };
}

ActionResult SetPriorityTarget( const ProcessTarget& Target, uint32_t PriorityClass, const ActionLimits& Limits )
{
    Handle Process;
    if( auto Opened = OpenVerified( Target, PROCESS_SET_INFORMATION, Process ); Opened.outcome != Outcome::Completed )
        return Opened;
    if( auto Check = Allowed( Process.value, Target, Limits ); Check.outcome != Outcome::Completed )
        return Check;
    if( !SetPriorityClass( Process.value, PriorityClass ) )
    {
        const DWORD Error = GetLastError();
        return Fail( Error == ERROR_ACCESS_DENIED ? Outcome::AccessDenied : Outcome::Failed, Error );
    }
    // 실시간 요청이 권한 부족으로 높음이 되는 경우가 있다. 결과는 호출자가 다시 조회한다.
    return { Outcome::Completed, 0, {} };
}

ActionResult SetEfficiencyTarget( const ProcessTarget& Target, bool Enable, const ActionLimits& Limits )
{
    Handle Process;
    if( auto Opened = OpenVerified( Target, PROCESS_SET_INFORMATION, Process ); Opened.outcome != Outcome::Completed )
        return Opened;
    if( auto Check = Allowed( Process.value, Target, Limits ); Check.outcome != Outcome::Completed )
        return Check;
    PROCESS_POWER_THROTTLING_STATE State{};
    State.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    State.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
    State.StateMask = Enable ? PROCESS_POWER_THROTTLING_EXECUTION_SPEED : 0;
    if( !SetProcessInformation( Process.value, ProcessPowerThrottling, &State, sizeof( State ) ) )
    {
        const DWORD Error = GetLastError();
        return Fail( Error == ERROR_ACCESS_DENIED ? Outcome::AccessDenied : Outcome::Failed, Error );
    }
    // Windows 작업 관리자의 효율 모드와 같이 우선순위도 함께 낮춘다.
    SetPriorityClass( Process.value, Enable ? IDLE_PRIORITY_CLASS : NORMAL_PRIORITY_CLASS );
    return { Outcome::Completed, 0, {} };
}

bool SwitchToProcessWindow( uint32_t Pid )
{
    const auto Windows = TopLevelWindows( Pid );
    if( Windows.empty() )
        return false;
    const auto Window = Windows.front();
    if( IsIconic( Window ) )
        ShowWindow( Window, SW_RESTORE );
    return SetForegroundWindow( Window ) != FALSE;
}

void SplitCommand( const std::wstring& Command, std::wstring& File, std::wstring& Arguments )
{
    File.clear();
    Arguments.clear();
    size_t Index = 0;
    while( Index < Command.size() && std::iswspace( Command[Index] ) )
        ++Index;
    if( Index < Command.size() && Command[Index] == L'"' )
    {
        const auto Close = Command.find( L'"', Index + 1 );
        File = Command.substr( Index + 1, Close == std::wstring::npos ? std::wstring::npos : Close - Index - 1 );
        Index = Close == std::wstring::npos ? Command.size() : Close + 1;
    }
    else
    {
        const auto Space = Command.find_first_of( L" \t", Index );
        File = Command.substr( Index, Space == std::wstring::npos ? std::wstring::npos : Space - Index );
        Index = Space == std::wstring::npos ? Command.size() : Space;
    }
    while( Index < Command.size() && std::iswspace( Command[Index] ) )
        ++Index;
    Arguments = Command.substr( std::min( Index, Command.size() ) );
}

RunResult RunProcess( const RunRequest& Request )
{
    RunResult Result;
    std::wstring File, Arguments;
    SplitCommand( Request.command, File, Arguments );
    if( File.empty() )
    {
        Result.nativeCode = ERROR_INVALID_PARAMETER;
        return Result;
    }
    SHELLEXECUTEINFOW Info{};
    Info.cbSize = sizeof( Info );
    Info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI | SEE_MASK_NOASYNC;
    Info.lpVerb = Request.elevated ? L"runas" : nullptr;
    Info.lpFile = File.c_str();
    Info.lpParameters = Arguments.empty() ? nullptr : Arguments.c_str();
    Info.lpDirectory = Request.directory.empty() ? nullptr : Request.directory.c_str();
    Info.nShow = Request.show;
    if( !ShellExecuteExW( &Info ) )
    {
        Result.nativeCode = GetLastError();
        return Result;
    }
    Result.started = true;
    if( Info.hProcess )
    {
        Result.pid = GetProcessId( Info.hProcess );
        if( Request.priorityClass )
            SetPriorityClass( Info.hProcess, Request.priorityClass );
        CloseHandle( Info.hProcess );
    }
    return Result;
}

} // namespace hc::system
