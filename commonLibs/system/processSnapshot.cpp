#include "processSnapshot.hpp"

#include "ntApi.hpp"

#include <psapi.h>

#include <algorithm>
#include <cmath>

namespace hc::system {

namespace {
uint64_t ToUnsigned( const LARGE_INTEGER& Value ) { return Value.QuadPart > 0 ? uint64_t( Value.QuadPart ) : 0; }
uint64_t FileTimeValue( const FILETIME& Value ) { return ( uint64_t( Value.dwHighDateTime ) << 32 ) | Value.dwLowDateTime; }

bool ProcessorPerformance( uint64_t& Interrupt, uint64_t& Busy )
{
    // 인터럽트·DPC 몫 = Δ(Interrupt+Dpc) ÷ Δ(Kernel+User). Kernel에는 유휴가 포함된다.
    const auto& Api = nt::Functions();
    Interrupt = Busy = 0;
    const WORD Groups = GetActiveProcessorGroupCount();
    bool Any = false;
    for( WORD Group = 0; Group < std::max<WORD>( Groups, 1 ); ++Group )
    {
        const DWORD Count = std::max<DWORD>( GetActiveProcessorCount( Group ), 1 );
        std::vector<nt::SystemProcessorPerformanceInformation> Values( Count );
        ULONG Length = 0;
        nt::Status Status = -1;
        if( Api.QuerySystemInformationEx )
        {
            USHORT Input = Group;
            Status = Api.QuerySystemInformationEx( nt::SystemProcessorPerformanceInformationClass, &Input, sizeof( Input ), Values.data(),
                                                    ULONG( Values.size() * sizeof( Values[0] ) ), &Length );
        }
        else if( Group == 0 && Api.QuerySystemInformation )
            Status = Api.QuerySystemInformation( nt::SystemProcessorPerformanceInformationClass, Values.data(), ULONG( Values.size() * sizeof( Values[0] ) ), &Length );
        if( !nt::Succeeded( Status ) )
            continue;
        const size_t Filled = std::min<size_t>( Values.size(), Length / sizeof( Values[0] ) );
        for( size_t Index = 0; Index < Filled; ++Index )
        {
            Interrupt += ToUnsigned( Values[Index].InterruptTime ) + ToUnsigned( Values[Index].DpcTime );
            Busy += ToUnsigned( Values[Index].KernelTime ) + ToUnsigned( Values[Index].UserTime );
        }
        Any = true;
    }
    return Any;
}
} // namespace

uint32_t ActiveProcessorCount()
{
    const DWORD Count = GetActiveProcessorCount( ALL_PROCESSOR_GROUPS );
    return Count ? Count : 1;
}

bool IdleProcessorCycles( uint64_t& Total )
{
    Total = 0;
    const WORD Groups = std::max<WORD>( GetActiveProcessorGroupCount(), 1 );
    for( WORD Group = 0; Group < Groups; ++Group )
    {
        ULONG Length = 0;
        if( !QueryIdleProcessorCycleTimeEx( Group, &Length, nullptr ) && GetLastError() != ERROR_INSUFFICIENT_BUFFER && Length == 0 )
            return false;
        if( Length == 0 )
            return false;
        std::vector<ULONG64> Values( Length / sizeof( ULONG64 ) );
        if( Values.empty() || !QueryIdleProcessorCycleTimeEx( Group, &Length, Values.data() ) )
            return false;
        for( const auto Value : Values )
            Total += Value;
    }
    return true;
}

std::wstring ProcessDisplayName( const ProcessSample& Sample )
{
    if( !Sample.imageName.empty() )
        return Sample.imageName;
    if( Sample.key.pid == 0 )
        return L"System Idle Process";
    return L"[" + std::to_wstring( Sample.key.pid ) + L"]";
}

double CpuPercentByCycles( uint64_t DeltaCycles, const CpuInterval& Interval )
{
    const double Total = double( Interval.processCycles ) + double( Interval.idleCycles );
    if( !Interval.cyclesValid || Total <= 0.0 )
        return -1.0;
    const double Share = double( DeltaCycles ) / Total;
    return std::clamp( Share * ( 1.0 - std::clamp( Interval.interruptShare, 0.0, 1.0 ) ) * 100.0, 0.0, 100.0 );
}

double CpuPercentByTime( uint64_t DeltaTime100ns, const CpuInterval& Interval )
{
    if( Interval.elapsed100ns <= 0.0 || Interval.processors == 0 )
        return -1.0;
    return std::clamp( double( DeltaTime100ns ) / ( Interval.elapsed100ns * Interval.processors ) * 100.0, 0.0, 100.0 );
}

bool CycleIntervalPlausible( const CpuInterval& Interval, double IdleShareByTime )
{
    const double Total = double( Interval.processCycles ) + double( Interval.idleCycles );
    if( !Interval.cyclesValid || Total <= 0.0 )
        return false;
    if( IdleShareByTime < 0.0 )
        return true;
    // 유휴 몫이 시간 기준과 30 %p 이상 어긋나면 사이클 카운터의 의미를 믿지 않는다.
    const double IdleShare = double( Interval.idleCycles ) / Total;
    return std::fabs( IdleShare - IdleShareByTime ) <= 0.30;
}

void ProcessCollector::Reset()
{
    previous_.clear();
    hasPrevious_ = false;
}

bool ProcessCollector::Sample( Snapshot& Out, uint32_t* Error )
{
    const auto& Api = nt::Functions();
    Out = {};
    if( !Api.QuerySystemInformation )
    {
        if( Error ) *Error = ERROR_PROC_NOT_FOUND;
        return false;
    }
    if( buffer_.empty() )
        buffer_.resize( 512 * 1024 );
    nt::Status Status = 0;
    for( int Attempt = 0; Attempt < 8; ++Attempt )
    {
        ULONG Needed = 0;
        Status = Api.QuerySystemInformation( nt::SystemProcessInformationClass, buffer_.data(), ULONG( buffer_.size() ), &Needed );
        if( Status != nt::InfoLengthMismatch && Status != nt::BufferTooSmall && Status != nt::BufferOverflow )
            break;
        buffer_.resize( std::max<size_t>( buffer_.size() * 2, size_t( Needed ) + 64 * 1024 ) );
    }
    if( !nt::Succeeded( Status ) )
    {
        if( Error ) *Error = uint32_t( Status );
        return false;
    }

    LARGE_INTEGER Counter{}, Frequency{};
    QueryPerformanceCounter( &Counter );
    QueryPerformanceFrequency( &Frequency );
    FILETIME SampleClock{};
    GetSystemTimeAsFileTime( &SampleClock );

    uint64_t IdleCycles = 0;
    const bool CyclesAvailable = IdleProcessorCycles( IdleCycles );
    uint64_t PerfInterrupt = 0, PerfBusy = 0;
    const bool PerfAvailable = ProcessorPerformance( PerfInterrupt, PerfBusy );
    FILETIME SysIdle{}, SysKernel{}, SysUser{};
    const bool SystemTimes = GetSystemTimes( &SysIdle, &SysKernel, &SysUser ) != FALSE;

    CpuInterval Interval;
    Interval.processors = ActiveProcessorCount();
    const bool Rates = hasPrevious_;
    if( Rates && Frequency.QuadPart > 0 )
        Interval.elapsed100ns = double( Counter.QuadPart - previousCounter_ ) * 1.0e7 / double( Frequency.QuadPart );

    std::unordered_map<ProcessKey, Previous, ProcessKeyHash> Current;
    Current.reserve( previous_.size() + 64 );
    Out.processes.reserve( previous_.size() + 64 );

    uint64_t ProcessCycles = 0;
    const auto* Cursor = buffer_.data();
    const auto* End = buffer_.data() + buffer_.size();
    for( ;; )
    {
        if( Cursor + sizeof( nt::SystemProcessInformation ) > End )
            break;
        const auto* Info = reinterpret_cast<const nt::SystemProcessInformation*>( Cursor );
        ProcessEntry Entry;
        auto& Sample = Entry.sample;
        Sample.key.pid        = uint32_t( reinterpret_cast<ULONG_PTR>( Info->UniqueProcessId ) );
        Sample.key.createTime = ToUnsigned( Info->CreateTime );
        Sample.parentPid      = uint32_t( reinterpret_cast<ULONG_PTR>( Info->InheritedFromUniqueProcessId ) );
        Sample.sessionId      = Info->SessionId;
        Sample.threadCount    = Info->NumberOfThreads;
        Sample.handleCount    = Info->HandleCount;
        Sample.basePriority   = Info->BasePriority;
        Sample.kernelTime     = ToUnsigned( Info->KernelTime );
        Sample.userTime       = ToUnsigned( Info->UserTime );
        Sample.cycleTime      = Info->CycleTime;
        Sample.workingSet     = Info->WorkingSetSize;
        Sample.privateBytes   = Info->PagefileUsage;
        Sample.ioBytes        = ToUnsigned( Info->ReadTransferCount ) + ToUnsigned( Info->WriteTransferCount ) + ToUnsigned( Info->OtherTransferCount );
        if( Info->ImageName.Buffer && Info->ImageName.Length )
            Sample.imageName.assign( Info->ImageName.Buffer, Info->ImageName.Length / sizeof( wchar_t ) );
        // 모든 스레드가 "일시 중단 사유로 대기"이면 일시 중단된 프로세스다.
        if( Info->NumberOfThreads > 0 && Sample.key.pid > 4 )
        {
            const auto* Threads = reinterpret_cast<const nt::SystemThreadInformation*>( Cursor + sizeof( nt::SystemProcessInformation ) );
            if( reinterpret_cast<const unsigned char*>( Threads + Info->NumberOfThreads ) <= End )
            {
                bool All = true;
                for( ULONG Index = 0; Index < Info->NumberOfThreads && All; ++Index )
                    All = Threads[Index].ThreadState == nt::ThreadStateWaiting && Threads[Index].WaitReason == nt::WaitReasonSuspended;
                Sample.suspended = All;
            }
        }
        Previous Now{ Sample.kernelTime + Sample.userTime, Sample.cycleTime, Sample.ioBytes };
        if( Sample.key.pid != 0 && Rates )
        {
            const auto Found = previous_.find( Sample.key );
            // 직전 표본 이후에 생성된 프로세스는 이전 값을 0으로 본다. 생성 후 사용량이 모두 이 구간 안에 있다.
            if( Found != previous_.end() || Sample.key.createTime >= previousSystemTime_ )
            {
                const uint64_t Before = Found != previous_.end() ? Found->second.cycles : 0;
                if( Now.cycles >= Before )
                    ProcessCycles += Now.cycles - Before;
            }
        }
        Current.emplace( Sample.key, Now );
        Out.totals.threadCount += Sample.threadCount;
        Out.totals.handleCount += Sample.handleCount;
        Out.processes.push_back( std::move( Entry ) );
        if( Info->NextEntryOffset == 0 )
            break;
        Cursor += Info->NextEntryOffset;
    }
    Out.totals.processCount = uint32_t( Out.processes.size() );
    Out.totals.processors = Interval.processors;

    double IdleShareByTime = -1.0;
    if( SystemTimes && Rates )
    {
        const uint64_t Idle = FileTimeValue( SysIdle ) - previousSysIdle_;
        const uint64_t Kernel = FileTimeValue( SysKernel ) - previousSysKernel_;
        const uint64_t User = FileTimeValue( SysUser ) - previousSysUser_;
        if( Kernel + User > 0 )
        {
            IdleShareByTime = double( Idle ) / double( Kernel + User );
            Out.totals.cpu = std::clamp( ( 1.0 - IdleShareByTime ) * 100.0, 0.0, 100.0 );
        }
    }
    if( PerfAvailable && Rates && PerfBusy > previousPerfBusy_ )
    {
        Interval.interruptShare = std::clamp( double( PerfInterrupt - previousPerfInterrupt_ ) / double( PerfBusy - previousPerfBusy_ ), 0.0, 1.0 );
        Out.totals.interruptCpu = Interval.interruptShare * 100.0;
    }
    Interval.processCycles = ProcessCycles;
    Interval.idleCycles = CyclesAvailable && Rates && IdleCycles >= previousIdleCycles_ ? IdleCycles - previousIdleCycles_ : 0;
    Interval.cyclesValid = method_ == CpuMethod::Auto && CyclesAvailable && Rates;
    const bool UseCycles = CycleIntervalPlausible( Interval, IdleShareByTime );
    Out.totals.cycleBased = UseCycles;

    if( Rates )
    {
        for( auto& Entry : Out.processes )
        {
            const auto& Sample = Entry.sample;
            const auto Found = previous_.find( Sample.key );
            const bool Known = Found != previous_.end();
            const uint64_t TimeBefore = Known ? Found->second.time : 0;
            const uint64_t CyclesBefore = Known ? Found->second.cycles : 0;
            const uint64_t IoBefore = Known ? Found->second.io : 0;
            const uint64_t TimeNow = Sample.kernelTime + Sample.userTime;
            if( Sample.key.pid == 0 )
            {
                // 유휴는 사이클 방식이면 유휴 사이클 몫, 아니면 유휴 프로세스의 커널 시간이다.
                Entry.cpu = UseCycles ? CpuPercentByCycles( Interval.idleCycles, Interval )
                                      : ( Known && TimeNow >= TimeBefore ? CpuPercentByTime( TimeNow - TimeBefore, Interval ) : -1.0 );
                continue;
            }
            // 직전 표본에 없던 프로세스는 그 뒤에 생성된 경우에만 0에서부터 계산한다.
            if( Known || Sample.key.createTime >= previousSystemTime_ )
            {
                if( UseCycles && Sample.cycleTime >= CyclesBefore )
                    Entry.cpu = CpuPercentByCycles( Sample.cycleTime - CyclesBefore, Interval );
                else if( TimeNow >= TimeBefore )
                    Entry.cpu = CpuPercentByTime( TimeNow - TimeBefore, Interval );
            }
            if( Interval.elapsed100ns > 0.0 && Sample.ioBytes >= IoBefore )
                Entry.ioPerSec = double( Sample.ioBytes - IoBefore ) * 1.0e7 / Interval.elapsed100ns;
        }
        Out.baseline = false;
    }

    MEMORYSTATUSEX Memory{ sizeof( Memory ) };
    if( GlobalMemoryStatusEx( &Memory ) )
    {
        Out.totals.physicalTotal = Memory.ullTotalPhys;
        Out.totals.physicalUsed = Memory.ullTotalPhys - Memory.ullAvailPhys;
    }
    PERFORMANCE_INFORMATION Performance{ sizeof( Performance ) };
    if( GetPerformanceInfo( &Performance, sizeof( Performance ) ) )
    {
        Out.totals.commitLimit = uint64_t( Performance.CommitLimit ) * Performance.PageSize;
        Out.totals.commitUsed = uint64_t( Performance.CommitTotal ) * Performance.PageSize;
    }

    previous_ = std::move( Current );
    previousCounter_ = Counter.QuadPart;
    previousSystemTime_ = FileTimeValue( SampleClock );
    previousIdleCycles_ = IdleCycles;
    if( SystemTimes )
    {
        previousSysIdle_ = FileTimeValue( SysIdle );
        previousSysKernel_ = FileTimeValue( SysKernel );
        previousSysUser_ = FileTimeValue( SysUser );
    }
    if( PerfAvailable )
    {
        previousPerfInterrupt_ = PerfInterrupt;
        previousPerfBusy_ = PerfBusy;
    }
    hasPrevious_ = true;
    return true;
}

} // namespace hc::system
