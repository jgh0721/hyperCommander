#pragma once

// 프로세스 동작. 모든 동작은 PID·생성 시각·이미지 경로를 다시 확인한 뒤 실행한다.

#include "processInfo.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace hc::system {

enum class Outcome : uint8_t { Completed, Rejected, AccessDenied, AlreadyGone, TargetChanged, Failed, Unknown };
const char* OutcomeName( Outcome Value );
Outcome     OutcomeFromName( const std::string& Name );

struct ProcessTarget
{
    uint32_t     pid        = 0;
    uint64_t     createTime = 0;
    std::wstring image;       // 비어 있으면 이미지 확인을 생략한다(경로를 읽지 못한 경우)
};

struct ActionResult
{
    Outcome     outcome    = Outcome::Unknown;
    uint32_t    nativeCode = 0;
    std::string reason;
};

struct ActionLimits
{
    std::vector<std::wstring> blocked;   // 끝낼 수 없는 제품 실행 파일 이름
    std::vector<std::wstring> product;   // 경고 후 허용하는 제품 실행 파일 이름
    uint32_t                  callerPid = 0;
    bool                      allowWarned = false;  // 경고 대상을 허용한다(사용자 확인 후)
};

ActionResult TerminateProcessTarget( const ProcessTarget& Target, const ActionLimits& Limits, uint32_t WaitMs = 3000 );
// 최상위 창에 WM_CLOSE를 보낸다. 창이 없으면 Rejected.
ActionResult RequestCloseWindows( const ProcessTarget& Target );
bool         HasTopLevelWindows( uint32_t Pid, uint32_t* Count = nullptr );
bool         WaitForExit( const ProcessTarget& Target, uint32_t TimeoutMs );
ActionResult SuspendProcessTarget( const ProcessTarget& Target, bool Suspend, const ActionLimits& Limits );
ActionResult SetPriorityTarget( const ProcessTarget& Target, uint32_t PriorityClass, const ActionLimits& Limits );
ActionResult SetEfficiencyTarget( const ProcessTarget& Target, bool Enable, const ActionLimits& Limits );
bool         SwitchToProcessWindow( uint32_t Pid );

struct RunRequest
{
    std::wstring command;           // 실행 파일과 인수
    std::wstring directory;
    bool         elevated      = false;
    int          show          = 1; // SW_SHOWNORMAL
    uint32_t     priorityClass = 0; // 0이면 바꾸지 않는다
};
struct RunResult
{
    bool     started    = false;
    uint32_t pid        = 0;
    uint32_t nativeCode = 0;
};
RunResult RunProcess( const RunRequest& Request );
// "C:\a b\x.exe" -arg 형태를 파일과 인수로 나눈다.
void SplitCommand( const std::wstring& Command, std::wstring& File, std::wstring& Arguments );

} // namespace hc::system
