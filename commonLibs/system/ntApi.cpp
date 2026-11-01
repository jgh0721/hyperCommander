#include "ntApi.hpp"

namespace hc::system::nt {

const Api& Functions()
{
    static const Api Value = [] {
        Api Result;
        if( const auto Module = GetModuleHandleW( L"ntdll.dll" ) )
        {
            Result.QuerySystemInformation   = reinterpret_cast<QuerySystemInformationFn>( GetProcAddress( Module, "NtQuerySystemInformation" ) );
            Result.QuerySystemInformationEx = reinterpret_cast<QuerySystemInformationExFn>( GetProcAddress( Module, "NtQuerySystemInformationEx" ) );
            Result.QueryInformationProcess  = reinterpret_cast<QueryInformationProcessFn>( GetProcAddress( Module, "NtQueryInformationProcess" ) );
            Result.SuspendProcess           = reinterpret_cast<ProcessControlFn>( GetProcAddress( Module, "NtSuspendProcess" ) );
            Result.ResumeProcess            = reinterpret_cast<ProcessControlFn>( GetProcAddress( Module, "NtResumeProcess" ) );
        }
        return Result;
    }();
    return Value;
}

} // namespace hc::system::nt
