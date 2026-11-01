#pragma once

// 프로세스 상세 정보와 동작 허용 정책. 핸들은 void*로 받아 Windows 헤더 의존을 줄인다.

#include <cstdint>
#include <string>
#include <vector>

namespace hc::system {

enum class Integrity : uint8_t { Unknown, Untrusted, Low, Medium, MediumPlus, High, System, Protected };

struct ProcessDetails
{
    bool         opened        = false;   // 제한 조회 권한으로 열었다
    bool         tokenRead     = false;   // 토큰(사용자·무결성)을 읽었다
    std::wstring imagePath;
    std::wstring commandLine;
    std::wstring userName;                // DOMAIN\user
    std::wstring userSid;
    Integrity    integrity     = Integrity::Unknown;
    bool         elevated      = false;
    bool         wow64         = false;   // 32비트 프로세스
    bool         packaged      = false;
    std::wstring packageName;
    uint32_t     priorityClass = 0;
    bool         critical      = false;
    bool         protectedProcess = false;
};

// Pid·CreateTime이 맞는 프로세스의 상세 정보를 채운다. 시각이 다르면 false(PID 재사용).
bool QueryProcessDetails( uint32_t Pid, uint64_t CreateTime, ProcessDetails& Out );
std::wstring ProcessImagePath( void* Process );
uint64_t     ProcessCreateTime( void* Process );
std::wstring ProcessUserSid( void* Process );
std::wstring CurrentUserSid();
std::wstring AccountName( const std::wstring& Sid );

enum class ActionPolicy { ProcessManager };
enum class Verdict { Allowed, Warn, Blocked };
struct ActionCheck
{
    Verdict      verdict = Verdict::Blocked;
    std::string  reason;   // 기계 판독용 사유: system-process, self, critical-process, protected-process, product-process, process-unverified
};

// 끝내기·일시 중단·우선순위 변경 전에 확인한다. Product는 경고 대상 이미지 파일 이름 목록이다.
ActionCheck CheckAction( void* Process, uint32_t Pid, uint32_t CallerPid, const std::vector<std::wstring>& Blocked, const std::vector<std::wstring>& Product );
// 끝내면 시스템이 즉시 멈추는 이름(대소문자 무시). 중요 프로세스 표시가 없는 경우를 보완한다.
bool IsSystemCriticalName( const std::wstring& ImageName );

} // namespace hc::system
