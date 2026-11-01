#pragma once

// 서비스 제어 관리자(SCM) 래퍼. 조회는 표준 권한, 제어·설정 변경은 기본 DACL상 관리자 권한이 필요하다.

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace hc::system {

enum class ServiceState : uint8_t { Unknown, Stopped, StartPending, StopPending, Running, ContinuePending, PausePending, Paused };
enum class StartType : uint8_t { Unknown, Boot, System, Auto, AutoDelayed, Manual, Disabled };

struct ServiceStatusInfo
{
    std::wstring name;
    std::wstring displayName;
    uint32_t     type             = 0;
    ServiceState state            = ServiceState::Unknown;
    uint32_t     pid              = 0;
    uint32_t     controlsAccepted = 0;
    uint32_t     win32ExitCode    = 0;
    uint32_t     serviceExitCode  = 0;
    bool         driver           = false;
};

struct FailureAction
{
    uint32_t type    = 0;   // SC_ACTION_NONE·RESTART·REBOOT·RUN_COMMAND
    uint32_t delayMs = 0;
};

struct FailureActions
{
    uint32_t                   resetPeriodSec = 0;
    std::wstring               rebootMessage;
    std::wstring               command;
    std::vector<FailureAction> actions;
    bool                       onNonCrashFailures = false;
};

struct ServiceConfig
{
    uint32_t                  type           = 0;
    StartType                 start          = StartType::Unknown;
    bool                      triggerStart   = false;
    uint32_t                  triggerCount   = 0;
    uint32_t                  errorControl   = 0;
    std::wstring              binaryPath;
    std::wstring              loadOrderGroup;
    std::wstring              account;
    std::wstring              displayName;
    std::wstring              description;
    std::vector<std::wstring> dependencies;   // 그룹은 '+' 접두사
    uint32_t                  sidType        = 0;
    std::vector<std::wstring> privileges;
    uint32_t                  launchProtected = 0;
    FailureActions            failure;
    std::wstring              serviceDll;
    std::wstring              resolvedImage;
};

enum class ServiceControl : uint8_t { Start, Stop, Pause, Continue };

struct ServiceConfigChange
{
    std::optional<StartType>      start;          // AutoDelayed는 자동 + 지연 시작
    std::optional<std::wstring>   displayName;
    std::optional<std::wstring>   description;
    std::optional<uint32_t>       errorControl;
    std::optional<std::wstring>   account;
    std::optional<std::wstring>   password;       // account와 함께 쓴다. 기록하지 않는다
    std::optional<FailureActions> failure;
    std::optional<uint32_t>       sidType;
};

const wchar_t* ServiceStateName( ServiceState State );
ServiceState   ServiceStateFromNative( uint32_t State );
uint32_t       ServiceControlCode( ServiceControl Control );

bool EnumerateServices( bool IncludeDrivers, std::vector<ServiceStatusInfo>& Out, uint32_t* Error = nullptr );
bool QueryServiceStatusInfo( const std::wstring& Name, ServiceStatusInfo& Out, uint32_t* Error = nullptr );
bool QueryServiceConfiguration( const std::wstring& Name, ServiceConfig& Out, uint32_t* Error = nullptr );
bool QueryDependents( const std::wstring& Name, std::vector<ServiceStatusInfo>& Out, uint32_t* Error = nullptr );
// 서비스 실행 파일 경로에서 따옴표·인수를 떼고 환경 변수와 \SystemRoot\, \??\ 접두사를 해석한다.
std::wstring ResolveServiceImage( const std::wstring& BinaryPath );
std::wstring ReadServiceDll( const std::wstring& Name );

// 반환값은 Win32 오류 코드(0이면 성공). 상태 전환을 TimeoutMs까지 기다린다.
uint32_t ControlServiceByName( const std::wstring& Name, ServiceControl Control, const std::vector<std::wstring>& Arguments = {}, uint32_t TimeoutMs = 15000 );
uint32_t RestartServiceByName( const std::wstring& Name, uint32_t TimeoutMs = 30000 );
uint32_t ApplyServiceConfig( const std::wstring& Name, const ServiceConfigChange& Change );

// 상태 알림(NotifyServiceStatusChange)으로 변경을 감지한다. 알림이 실패하면 주기 열거로 대체한다.
// Callback은 감시 스레드에서 전체 목록과 함께 불린다.
class ServiceMonitor
{
public:
    using Callback = std::function<void( std::vector<ServiceStatusInfo> )>;
    ServiceMonitor() = default;
    ~ServiceMonitor();
    ServiceMonitor( const ServiceMonitor& ) = delete;
    ServiceMonitor& operator=( const ServiceMonitor& ) = delete;

    void Start( bool IncludeDrivers, Callback Changed );
    void Stop();
    void SetIncludeDrivers( bool IncludeDrivers ) { includeDrivers_ = IncludeDrivers; refresh_ = true; }
    void RequestRefresh() { refresh_ = true; }
    bool NotificationsActive() const { return notifications_; }

private:
    void run( std::stop_token Stop );
    std::jthread      thread_;
    Callback          changed_;
    std::atomic_bool  includeDrivers_{ false };
    std::atomic_bool  refresh_{ true };
    std::atomic_bool  notifications_{ false };
};

} // namespace hc::system
