#pragma once
#include "environmentTypes.hpp"
#include "processSnapshot.hpp"
#include "processInfo.hpp"
#include "ntApi.hpp"
#include <span>
#include <cstring>
#include <limits>

namespace hc::environment {
enum class ProcessReadStatus { Ok, InvalidTarget, Denied, Exited, Unsupported, Unstable, InvalidBlock, TooLarge, ReadFailed };
struct ProcessEnvironment
{
    ProcessReadStatus status_ = ProcessReadStatus::ReadFailed;
    uint32_t nativeError_ = 0;
    bool wow64_ = false;
    ValueMap values_;
    std::optional<std::wstring> currentDirectory_;
};
inline ProcessEnvironment ParseEnvironmentBlock( std::span<const wchar_t> Block )
{
    ProcessEnvironment Result;
    Result.status_ = ProcessReadStatus::InvalidBlock;
    ValueMap Parsed; // Publish only a complete, validated snapshot.
    size_t Position = 0;
    if( Block.size() < 2 ) return Result;
    if( Block[0] == 0 ) { if( Block[1] == 0 ) Result.status_ = ProcessReadStatus::Ok; return Result; }
    while( Position < Block.size() )
    {
        const auto Begin = Position;
        while( Position < Block.size() && Block[Position] != 0 ) ++Position;
        if( Position == Block.size() ) return Result;
        const std::wstring Item( Block.data() + Begin, Position - Begin );
        const auto Equals = Item.find( L'=', Item.front() == L'=' ? 1 : 0 );
        if( Equals == Item.npos || Equals == 0 ) return Result;
        const auto Name = Item.substr( 0, Equals );
        if( !Parsed.emplace( Name, Item.substr( Equals + 1 ) ).second ) return Result;
        ++Position;
        if( Position < Block.size() && Block[Position] == 0 ) { Result.status_ = ProcessReadStatus::Ok; Result.values_ = std::move( Parsed ); return Result; }
    }
    return Result;
}
class IEnvironmentMemory
{
public:
    virtual ~IEnvironmentMemory() = default;
    virtual bool Read( uint64_t Address, void* Output, size_t Bytes ) const = 0;
};
inline bool ReadEnvironmentPointer( const IEnvironmentMemory& Memory, uint64_t Address, bool Wow64, uint64_t& Out )
{
    if( Wow64 ) { uint32_t Value = 0; if( !Memory.Read( Address, &Value, sizeof( Value ) ) ) return false; Out = Value; return true; }
    return Memory.Read( Address, &Out, sizeof( Out ) );
}
inline ProcessReadStatus ReadEnvironmentBytes( const IEnvironmentMemory& Memory, uint64_t Address,
    std::vector<wchar_t>& Out, size_t Limit )
{
    if( Address == 0 || Address % 2 != 0 || Limit < 2 ) return ProcessReadStatus::InvalidBlock;
    // Read on page boundaries, so an unreadable next page cannot hide a valid terminator.
    while( Out.size() < Limit )
    {
        if( Address > std::numeric_limits<uint64_t>::max() - 4096 ) return ProcessReadStatus::InvalidBlock;
        const size_t Count = std::min<size_t>( ( 4096 - ( Address % 4096 ) ) / sizeof( wchar_t ), Limit - Out.size() );
        std::vector<wchar_t> Chunk( Count );
        if( !Memory.Read( Address, Chunk.data(), Count * sizeof( wchar_t ) ) ) return ProcessReadStatus::ReadFailed;
        for( const auto Character : Chunk )
        {
            Out.push_back( Character );
            if( Out.size() >= 2 && Character == 0 && Out[Out.size() - 2] == 0 ) return ProcessReadStatus::Ok;
        }
        Address += Count * sizeof( wchar_t );
    }
    return ProcessReadStatus::TooLarge;
}
inline ProcessEnvironment ReadEnvironmentAtPeb( const IEnvironmentMemory& Memory, uint64_t Peb, bool Wow64,
    size_t Limit = 2 * 1024 * 1024 )
{
    ProcessEnvironment Result; Result.wow64_ = Wow64;
    if( !Peb || Peb > std::numeric_limits<uint64_t>::max() - 0x100 || Limit > 2 * 1024 * 1024 )
    { Result.status_ = ProcessReadStatus::InvalidTarget; return Result; }
    for( int Attempt = 0; Attempt < 3; ++Attempt )
    {
        uint64_t Parameters = 0, Environment = 0, AfterParameters = 0, AfterEnvironment = 0;
        if( !ReadEnvironmentPointer( Memory, Peb + ( Wow64 ? 0x10 : 0x20 ), Wow64, Parameters ) ||
            !Parameters || Parameters > std::numeric_limits<uint64_t>::max() - 0x100 ||
            !ReadEnvironmentPointer( Memory, Parameters + ( Wow64 ? 0x48 : 0x80 ), Wow64, Environment ) ) return Result;
        std::vector<wchar_t> First, Second;
        Result.status_ = ReadEnvironmentBytes( Memory, Environment, First, Limit );
        if( Result.status_ != ProcessReadStatus::Ok ) return Result;
        Result.status_ = ReadEnvironmentBytes( Memory, Environment, Second, Limit );
        if( Result.status_ != ProcessReadStatus::Ok ) return Result;
        Result.status_ = ProcessReadStatus::ReadFailed;
        if( !ReadEnvironmentPointer( Memory, Peb + ( Wow64 ? 0x10 : 0x20 ), Wow64, AfterParameters ) ||
            !ReadEnvironmentPointer( Memory, Parameters + ( Wow64 ? 0x48 : 0x80 ), Wow64, AfterEnvironment ) ) return Result;
        if( Parameters != AfterParameters || Environment != AfterEnvironment || First != Second ) continue;
        Result = ParseEnvironmentBlock( First ); Result.wow64_ = Wow64;
        if( Result.status_ != ProcessReadStatus::Ok ) { Result.values_.clear(); return Result; }
        // CURDIR starts with a UNICODE_STRING. Copy only a validated bounded string.
        const auto Directory = Parameters + ( Wow64 ? 0x24 : 0x38 );
        uint16_t Length = 0, Maximum = 0; uint64_t Buffer = 0;
        if( Memory.Read( Directory, &Length, sizeof( Length ) ) && Memory.Read( Directory + 2, &Maximum, sizeof( Maximum ) ) &&
            Length % 2 == 0 && Length <= Maximum && Length <= 32766 &&
            ReadEnvironmentPointer( Memory, Directory + ( Wow64 ? 4 : 8 ), Wow64, Buffer ) && Buffer && Buffer % 2 == 0 )
        {
            std::wstring Text( Length / 2, L'\0' );
            if( Memory.Read( Buffer, Text.data(), Length ) && Text.find( L'\0' ) == Text.npos ) Result.currentDirectory_ = std::move( Text );
        }
        return Result;
    }
    Result.status_ = ProcessReadStatus::Unstable; Result.values_.clear(); return Result;
}
inline ProcessEnvironment ReadProcessEnvironment( const hc::system::ProcessKey& Target )
{
    ProcessEnvironment Result;
    if( !Target.pid || !Target.createTime ) { Result.status_ = ProcessReadStatus::InvalidTarget; return Result; }
    struct ProcessMemory final : IEnvironmentMemory
    {
        HANDLE handle_ = nullptr;
        ~ProcessMemory() { if( handle_ ) CloseHandle( handle_ ); }
        bool Read( uint64_t Address, void* Output, size_t Bytes ) const override
        {
            if( !Address || Address > std::numeric_limits<uintptr_t>::max() || Bytes > std::numeric_limits<uintptr_t>::max() - uintptr_t( Address ) ) return false;
            SIZE_T ReadBytes = 0;
            return ReadProcessMemory( handle_, reinterpret_cast<LPCVOID>( uintptr_t( Address ) ), Output, Bytes, &ReadBytes ) && ReadBytes == Bytes;
        }
    } Memory;
    Memory.handle_ = OpenProcess( PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | SYNCHRONIZE, FALSE, Target.pid );
    if( !Memory.handle_ ) { Result.nativeError_ = GetLastError(); Result.status_ = Result.nativeError_ == ERROR_ACCESS_DENIED ? ProcessReadStatus::Denied : ProcessReadStatus::Exited; return Result; }
    if( hc::system::ProcessCreateTime( Memory.handle_ ) != Target.createTime ) { Result.status_ = ProcessReadStatus::InvalidTarget; return Result; }
    if( WaitForSingleObject( Memory.handle_, 0 ) == WAIT_OBJECT_0 ) { Result.status_ = ProcessReadStatus::Exited; return Result; }
    const auto Query = hc::system::nt::Functions().QueryInformationProcess;
    if( !Query || sizeof( void* ) != 8 ) { Result.status_ = ProcessReadStatus::Unsupported; return Result; }
    USHORT Machine = 0, Native = 0;
    using IsWow64Fn = BOOL( WINAPI* )( HANDLE, USHORT*, USHORT* );
    const auto IsWow64 = reinterpret_cast<IsWow64Fn>( GetProcAddress( GetModuleHandleW( L"kernel32.dll" ), "IsWow64Process2" ) );
    if( !IsWow64 || !IsWow64( Memory.handle_, &Machine, &Native ) || Native != IMAGE_FILE_MACHINE_AMD64 ||
        ( Machine != IMAGE_FILE_MACHINE_UNKNOWN && Machine != IMAGE_FILE_MACHINE_I386 ) ) { Result.status_ = ProcessReadStatus::Unsupported; return Result; }
    uint64_t Peb = 0;
    const bool Wow64 = Machine == IMAGE_FILE_MACHINE_I386;
    if( Wow64 )
    {
        const auto Status = Query( Memory.handle_, 26, &Peb, sizeof( Peb ), nullptr );
        if( Status < 0 ) { Result.nativeError_ = uint32_t( Status ); return Result; }
    }
    else
    {
        struct BasicInfo { void* reserved1_; void* peb_; void* reserved2_[2]; uintptr_t pid_; void* reserved3_; } Basic{};
        const auto Status = Query( Memory.handle_, 0, &Basic, sizeof( Basic ), nullptr );
        if( Status < 0 ) { Result.nativeError_ = uint32_t( Status ); return Result; }
        Peb = uint64_t( reinterpret_cast<uintptr_t>( Basic.peb_ ) );
    }
    Result = ReadEnvironmentAtPeb( Memory, Peb, Wow64 );
    if( hc::system::ProcessCreateTime( Memory.handle_ ) != Target.createTime ) { Result.values_.clear(); Result.status_ = ProcessReadStatus::InvalidTarget; }
    return Result;
}
} // namespace hc::environment
