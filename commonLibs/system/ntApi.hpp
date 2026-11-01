#pragma once

// ntdll 함수는 실행 시점에 연결한다. 정의는 SYSTEM_PROCESS_INFORMATION 등 공개 헤더가
// 일부만 드러내는 구조를 Windows 7 이후 x64 배치 그대로 옮긴 것이다.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <cstdint>

namespace hc::system::nt {

using Status = LONG;
inline bool Succeeded( Status Value ) { return Value >= 0; }
constexpr Status InfoLengthMismatch = static_cast<Status>( 0xC0000004L );
constexpr Status BufferTooSmall     = static_cast<Status>( 0xC0000023L );
constexpr Status BufferOverflow     = static_cast<Status>( 0x80000005L );

constexpr ULONG SystemProcessInformationClass            = 5;
constexpr ULONG SystemProcessorPerformanceInformationClass = 8;
constexpr ULONG ProcessCommandLineInformationClass       = 60;

struct UnicodeString
{
    USHORT Length;
    USHORT MaximumLength;
    PWSTR  Buffer;
};

struct ClientId
{
    HANDLE UniqueProcess;
    HANDLE UniqueThread;
};

struct SystemThreadInformation
{
    LARGE_INTEGER KernelTime;
    LARGE_INTEGER UserTime;
    LARGE_INTEGER CreateTime;
    ULONG         WaitTime;
    PVOID         StartAddress;
    ClientId      ClientIdentity;
    LONG          Priority;
    LONG          BasePriority;
    ULONG         ContextSwitches;
    ULONG         ThreadState;
    ULONG         WaitReason;
};

constexpr ULONG ThreadStateWaiting   = 5;
constexpr ULONG WaitReasonSuspended  = 5;

struct SystemProcessInformation
{
    ULONG         NextEntryOffset;
    ULONG         NumberOfThreads;
    LARGE_INTEGER WorkingSetPrivateSize;
    ULONG         HardFaultCount;
    ULONG         NumberOfThreadsHighWatermark;
    ULONGLONG     CycleTime;
    LARGE_INTEGER CreateTime;
    LARGE_INTEGER UserTime;
    LARGE_INTEGER KernelTime;
    UnicodeString ImageName;
    LONG          BasePriority;
    HANDLE        UniqueProcessId;
    HANDLE        InheritedFromUniqueProcessId;
    ULONG         HandleCount;
    ULONG         SessionId;
    ULONG_PTR     UniqueProcessKey;
    SIZE_T        PeakVirtualSize;
    SIZE_T        VirtualSize;
    ULONG         PageFaultCount;
    SIZE_T        PeakWorkingSetSize;
    SIZE_T        WorkingSetSize;
    SIZE_T        QuotaPeakPagedPoolUsage;
    SIZE_T        QuotaPagedPoolUsage;
    SIZE_T        QuotaPeakNonPagedPoolUsage;
    SIZE_T        QuotaNonPagedPoolUsage;
    SIZE_T        PagefileUsage;
    SIZE_T        PeakPagefileUsage;
    SIZE_T        PrivatePageCount;
    LARGE_INTEGER ReadOperationCount;
    LARGE_INTEGER WriteOperationCount;
    LARGE_INTEGER OtherOperationCount;
    LARGE_INTEGER ReadTransferCount;
    LARGE_INTEGER WriteTransferCount;
    LARGE_INTEGER OtherTransferCount;
    // SystemThreadInformation Threads[NumberOfThreads] 가 뒤따른다.
};

struct SystemProcessorPerformanceInformation
{
    LARGE_INTEGER IdleTime;
    LARGE_INTEGER KernelTime;
    LARGE_INTEGER UserTime;
    LARGE_INTEGER DpcTime;
    LARGE_INTEGER InterruptTime;
    ULONG         InterruptCount;
};

using QuerySystemInformationFn   = Status( NTAPI* )( ULONG, PVOID, ULONG, PULONG );
using QuerySystemInformationExFn = Status( NTAPI* )( ULONG, PVOID, ULONG, PVOID, ULONG, PULONG );
using QueryInformationProcessFn  = Status( NTAPI* )( HANDLE, ULONG, PVOID, ULONG, PULONG );
using ProcessControlFn           = Status( NTAPI* )( HANDLE );

struct Api
{
    QuerySystemInformationFn   QuerySystemInformation   = nullptr;
    QuerySystemInformationExFn QuerySystemInformationEx = nullptr;
    QueryInformationProcessFn  QueryInformationProcess  = nullptr;
    ProcessControlFn           SuspendProcess           = nullptr;
    ProcessControlFn           ResumeProcess            = nullptr;
};

// 처음 호출할 때 한 번 연결한다. 실패한 항목은 nullptr로 남는다.
const Api& Functions();

} // namespace hc::system::nt
