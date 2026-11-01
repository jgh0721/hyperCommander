#pragma once

// 프로세스 목록 표본과 CPU 사용률 계산. Qt에 의존하지 않는다.
// 사이클 기반 계산을 기본으로 하고, 사이클 정보를 쓸 수 없거나 검증에 실패하면
// 커널·사용자 시간 기반으로 바꾼다(설계 문서 7.8).

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace hc::system {

struct ProcessKey
{
    uint32_t pid        = 0;
    uint64_t createTime = 0;     // FILETIME(100 ns). PID 재사용 구분에 쓴다.
    bool operator==( const ProcessKey& Other ) const { return pid == Other.pid && createTime == Other.createTime; }
    bool operator!=( const ProcessKey& Other ) const { return !( *this == Other ); }
};

struct ProcessKeyHash
{
    size_t operator()( const ProcessKey& Key ) const noexcept
    {
        return std::hash<uint64_t>{}( Key.createTime ^ ( uint64_t( Key.pid ) * 0x9E3779B97F4A7C15ull ) );
    }
};

struct ProcessSample
{
    ProcessKey   key;
    uint32_t     parentPid     = 0;
    uint32_t     sessionId     = 0;
    uint32_t     threadCount   = 0;
    uint32_t     handleCount   = 0;
    int32_t      basePriority  = 0;
    uint64_t     kernelTime    = 0;  // 100 ns
    uint64_t     userTime      = 0;  // 100 ns
    uint64_t     cycleTime     = 0;
    uint64_t     workingSet    = 0;  // bytes
    uint64_t     privateBytes  = 0;  // bytes
    uint64_t     ioBytes       = 0;  // 읽기+쓰기+기타 누계
    bool         suspended     = false;
    std::wstring imageName;
};

struct ProcessEntry
{
    ProcessSample sample;
    double        cpu        = -1.0; // 백분율. 직전 표본이 없으면 음수
    double        ioPerSec   = 0.0;
};

struct SystemTotals
{
    double   cpu            = -1.0;  // 전체 사용률(%)
    double   interruptCpu   = 0.0;   // 인터럽트·DPC(%)
    uint64_t physicalTotal  = 0, physicalUsed = 0;
    uint64_t commitLimit    = 0, commitUsed   = 0;
    uint32_t processCount   = 0, threadCount  = 0, handleCount = 0;
    uint32_t processors     = 1;
    bool     cycleBased     = false;
};

struct Snapshot
{
    std::vector<ProcessEntry> processes;
    SystemTotals              totals;
    bool                      baseline = true;  // 직전 표본이 없어 비율을 계산하지 못한 표본
};

enum class CpuMethod
{
    Auto,   // 사이클 우선, 실패 시 시간 기반
    Time,
};

// 한 구간의 계산 입력. 단위 시험에서 고정 값으로 검증한다.
struct CpuInterval
{
    double   elapsed100ns    = 0.0;
    uint32_t processors      = 1;
    uint64_t processCycles   = 0;   // PID 0을 뺀 모든 프로세스의 사이클 증가량 합
    uint64_t idleCycles      = 0;   // 프로세서별 유휴 사이클 증가량 합
    bool     cyclesValid     = false;
    double   interruptShare  = 0.0; // 0~1. 인터럽트·DPC 몫
};

// 사이클 방식: 프로세스 몫 × (1 − 인터럽트 몫). 분모는 프로세스 사이클 합 + 유휴 사이클.
double CpuPercentByCycles( uint64_t DeltaCycles, const CpuInterval& Interval );
// 시간 방식: (Δ커널 + Δ사용자) ÷ (Δ경과 × 논리 프로세서 수).
double CpuPercentByTime( uint64_t DeltaTime100ns, const CpuInterval& Interval );
// 사이클 결과를 믿을 수 있는지 판단한다. 유휴·프로세스 사이클 비율이 시간 기준과 크게 어긋나면 false.
bool   CycleIntervalPlausible( const CpuInterval& Interval, double IdleShareByTime );

class ProcessCollector
{
public:
    void SetMethod( CpuMethod Method ) { method_ = Method; }
    CpuMethod Method() const { return method_; }
    // 직전 표본을 버린다. 다음 표본은 비율이 없는 기준점이 된다.
    void Reset();
    // 성공하면 true. 실패하면 Error에 Win32/NT 오류 코드를 넣는다.
    bool Sample( Snapshot& Out, uint32_t* Error = nullptr );

private:
    struct Previous
    {
        uint64_t time   = 0;
        uint64_t cycles = 0;
        uint64_t io     = 0;
    };
    std::unordered_map<ProcessKey, Previous, ProcessKeyHash> previous_;
    std::vector<unsigned char> buffer_;
    CpuMethod method_          = CpuMethod::Auto;
    bool      hasPrevious_     = false;
    int64_t   previousCounter_ = 0;
    uint64_t  previousSystemTime_ = 0;   // 직전 표본 시각(FILETIME)
    uint64_t  previousIdleCycles_ = 0;
    uint64_t  previousSysIdle_ = 0, previousSysKernel_ = 0, previousSysUser_ = 0;
    uint64_t  previousPerfInterrupt_ = 0, previousPerfBusy_ = 0;
};

// 프로세스 수집에 쓰는 시스템 정보 도우미
uint32_t ActiveProcessorCount();
bool     IdleProcessorCycles( uint64_t& Total );
std::wstring ProcessDisplayName( const ProcessSample& Sample );

} // namespace hc::system
