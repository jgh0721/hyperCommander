#include "stdafx.h"
#include "tstEnvironmentCore.hpp"

/// 모듈
/// UIs
/// 기타 헤더파일
/// 고유 라이브러리
/// 공통 라이브러리
#include "commonLibs/system/environmentBackend.hpp"
#include "commonLibs/system/environmentAnalysis.hpp"
#include "commonLibs/system/processEnvironment.hpp"
/// 외부 라이브러리
/// 선언 헤더파일들
/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <cstring>

using namespace hc::environment;
namespace {
Value Make( const wchar_t* Name, const wchar_t* Raw, ValueType Type = ValueType::String ) { return { Name, Raw, Type }; }
struct FakeProbe final : IPathProbe
{
    std::map<std::wstring, DirectoryProbe, NameLess> data_;
    int calls_ = 0;
    DirectoryProbe Inspect( const std::wstring& Path ) override { ++calls_; const auto Found = data_.find( Path ); return Found == data_.end() ? DirectoryProbe{} : Found->second; }
};
struct FakeMemory final : IEnvironmentMemory
{
    std::vector<unsigned char> bytes_ = std::vector<unsigned char>( 0x8000 );
    mutable int environmentReads_ = 0;
    bool unstable_ = false;
    template<class T> void Put( size_t Address, T Value ) { memcpy( bytes_.data() + Address, &Value, sizeof( Value ) ); }
    bool Read( uint64_t Address, void* Output, size_t Bytes ) const override
    {
        if( Address > bytes_.size() || Bytes > bytes_.size() - size_t( Address ) ) return false;
        memcpy( Output, bytes_.data() + size_t( Address ), Bytes );
        if( unstable_ && Address == 0x3000 ) { ++environmentReads_; static_cast<wchar_t*>( Output )[2] = environmentReads_ % 2 ? L'A' : L'B'; }
        return true;
    }
    void Setup( bool Wow )
    {
        if( Wow ) { Put<uint32_t>( 0x1010, 0x2000 ); Put<uint32_t>( 0x2048, 0x3000 ); Put<uint32_t>( 0x2028, 0x4000 ); }
        else { Put<uint64_t>( 0x1020, 0x2000 ); Put<uint64_t>( 0x2080, 0x3000 ); Put<uint64_t>( 0x2040, 0x4000 ); }
        Put<uint16_t>( Wow ? 0x2024 : 0x2038, 6 ); Put<uint16_t>( Wow ? 0x2026 : 0x203a, 8 );
        const wchar_t Block[] = L"A=one\0Path=C:\\Synthetic\0\0";
        memcpy( bytes_.data() + 0x3000, Block, sizeof( Block ) );
        const wchar_t Directory[] = L"C:\\"; memcpy( bytes_.data() + 0x4000, Directory, sizeof( Directory ) );
    }
};
}
void EnvironmentCoreTests::mergeSevenStages()
{
    MergeInput Input;
    Input.coreSystem_ = { Make( L"SystemRoot", L"C:\\SyntheticWindows" ) };
    Input.system_ = { Make( L"Tool", L"system" ), Make( L"A", L"%SystemRoot%\\tools", ValueType::ExpandString ), Make( L"USERNAME", L"SYSTEM" ) };
    Input.coreUser_ = { Make( L"USERPROFILE", L"C:\\SyntheticUser" ) };
    Input.user_ = { Make( L"Tool", L"user" ), Make( L"B", L"%USERPROFILE%\\bin", ValueType::ExpandString ) };
    Input.account_ = { Make( L"USERNAME", L"synthetic" ) };
    const auto Result = BuildEnvironment( Input );
    QCOMPARE( Result.values_.at( L"Tool" ).value_, std::wstring( L"user" ) );
    QCOMPARE( Result.values_.at( L"Tool" ).origins_.size(), size_t( 2 ) );
    QCOMPARE( Result.values_.at( L"A" ).value_, std::wstring( L"C:\\SyntheticWindows\\tools" ) );
    QCOMPARE( Result.values_.at( L"B" ).value_, std::wstring( L"C:\\SyntheticUser\\bin" ) );
    QCOMPARE( Result.values_.at( L"USERNAME" ).origins_.back().stage_, 7 );
}
void EnvironmentCoreTests::mergeAppendTypes()
{
    for( const auto Type : { ValueType::String, ValueType::ExpandString } )
        for( const auto* Name : { L"Path", L"LibPath", L"Os2LibPath" } )
        {
            MergeInput Input; Input.system_ = { Make( Name, L"C:\\System;" ) }; Input.user_ = { Make( Name, L"C:\\User", Type ) };
            const auto Result = BuildEnvironment( Input );
            QCOMPARE( Result.values_.at( Name ).value_, std::wstring( L"C:\\System;C:\\User" ) );
            QVERIFY( Result.values_.at( Name ).origins_.back().appended_ );
        }
    MergeInput Input; Input.system_ = { Make( L"PSModulePath", L"system" ) }; Input.user_ = { Make( L"PSModulePath", L"user" ) };
    QCOMPARE( BuildEnvironment( Input ).values_.at( L"PSModulePath" ).value_, std::wstring( L"user" ) );
}
void EnvironmentCoreTests::sameStageReference()
{
    MergeInput Input; Input.user_ = { Make( L"A", L"%B%", ValueType::ExpandString ), Make( L"B", L"defined", ValueType::ExpandString ) };
    for( int Index = 0; Index < 2; ++Index )
    {
        const auto Result = BuildEnvironment( Input );
        QCOMPARE( Result.values_.at( L"A" ).value_, std::wstring( L"%B%" ) );
        QCOMPARE( Result.values_.at( L"A" ).origins_.back().unresolved_.front().issue_, ReferenceIssue::SameStage );
        std::reverse( Input.user_.begin(), Input.user_.end() );
    }
}
void EnvironmentCoreTests::undefinedAndCycles()
{
    MergeInput Input; Input.user_ = { Make( L"A", L"%B%", ValueType::ExpandString ), Make( L"B", L"%A%", ValueType::ExpandString ) };
    Input.system_ = { Make( L"Late", L"%A%", ValueType::ExpandString ), Make( L"Missing", L"%NOWHERE%", ValueType::ExpandString ) };
    const auto Result = BuildEnvironment( Input );
    QCOMPARE( Result.values_.at( L"A" ).origins_.back().unresolved_[0].issue_, ReferenceIssue::Cycle );
    QCOMPARE( Result.values_.at( L"Late" ).origins_.back().unresolved_[0].issue_, ReferenceIssue::LaterStage );
    QCOMPARE( Result.values_.at( L"Missing" ).origins_.back().unresolved_[0].issue_, ReferenceIssue::Undefined );
}
void EnvironmentCoreTests::caseInsensitiveNames()
{
    QVERIFY( !SameName( L"", L"Path" ) ); QVERIFY( SameName( L"", L"" ) );
    MergeInput Input; Input.system_ = { Make( L"Tool", L"system" ) }; Input.user_ = { Make( L"TOOL", L"user" ), Make( L"Literal", L"%TOOL%" ) };
    const auto Result = BuildEnvironment( Input );
    QCOMPARE( Result.values_.size(), size_t( 2 ) );
    QCOMPARE( Result.values_.at( L"tool" ).value_, std::wstring( L"user" ) );
    QCOMPARE( Result.values_.at( L"Literal" ).value_, std::wstring( L"%TOOL%" ) );
}
void EnvironmentCoreTests::availabilityExceptions()
{
    MergeInput Input; Input.coreUser_ = { Make( L"ProgramFiles", L"C:\\SyntheticPrograms" ) };
    Input.system_ = { Make( L"Probe", L"%ProgramFiles%\\Modules", ValueType::ExpandString ) };
    QCOMPARE( BuildEnvironment( Input ).values_.at( L"Probe" ).value_, std::wstring( L"%ProgramFiles%\\Modules" ) );
    Input.availableToSystemExpansion_.insert( L"ProgramFiles" );
    QCOMPARE( BuildEnvironment( Input ).values_.at( L"Probe" ).value_, std::wstring( L"C:\\SyntheticPrograms\\Modules" ) );
}
void EnvironmentCoreTests::backendConflictAndTypes()
{
    MemoryEnvironmentBackend Backend;
    auto Item = Make( L"Example", L"", ValueType::ExpandString );
    QCOMPARE( Backend.CompareAndWrite( Scope::User, L"Example", {}, Item ), WriteStatus::Applied );
    QCOMPARE( Backend.CompareAndWrite( Scope::User, L"EXAMPLE", {}, Item ), WriteStatus::Conflict );
    const auto Before = Backend.ReadValue( Scope::User, L"example" );
    QVERIFY( Before.has_value() ); QCOMPARE( Before->type_, ValueType::ExpandString ); QVERIFY( Before->raw_.empty() );
    QCOMPARE( Backend.CompareAndWrite( Scope::User, L"EXAMPLE", Before, {} ), WriteStatus::Applied );
    QVERIFY( !Backend.ReadValue( Scope::User, L"Example" ) );
    QCOMPARE( Backend.CompareAndWrite( Scope::Account, L"Example", {}, Item ), WriteStatus::ReadOnly );
}
void EnvironmentCoreTests::valueValidation()
{
    QVERIFY( ValidateValue( Make( L"", L"x" ) ) ); QVERIFY( ValidateValue( Make( L"A=B", L"x" ) ) );
    auto Item = Make( L"A", L"x" ); Item.raw_.assign( 32767, L'x' ); QVERIFY( !ValidateValue( Item ) );
    Item.raw_.push_back( L'x' ); QVERIFY( ValidateValue( Item ) );
    Item.raw_ = std::wstring( L"a\0b", 3 ); QVERIFY( ValidateValue( Item ) );
}
void EnvironmentCoreTests::pathNineChecks()
{
    FakeProbe Probe;
    Probe.data_[L"C:\\First"] = { ProbeStatus::Exists, { L"tool.exe" }, false };
    Probe.data_[L"C:\\Second"] = { ProbeStatus::Exists, { L"tool.exe" }, true };
    AnalysisInput Input;
    Input.system_ = { L"C:\\First", L"C:\\Second", L"C:\\Absent", L"\"C:\\First\"", L"", L"%MISSING%", L"%LATER%" };
    Input.user_ = { L"c:/first/", L"C:\\Second" }; Input.allDefinitions_[L"LATER"] = L"defined-later";
    const auto Entries = AnalyzePath( Input, Probe );
    QVERIFY( Entries[1].issues_.contains( PathIssue::Shadowed ) ); QVERIFY( Entries[1].issues_.contains( PathIssue::Writable ) );
    QVERIFY( Entries[2].issues_.contains( PathIssue::Missing ) ); QVERIFY( Entries[3].issues_.contains( PathIssue::Quote ) );
    QVERIFY( Entries[3].issues_.contains( PathIssue::Duplicate ) ); QVERIFY( Entries[4].issues_.contains( PathIssue::Empty ) );
    QVERIFY( Entries[5].issues_.contains( PathIssue::Undefined ) ); QVERIFY( Entries[6].issues_.contains( PathIssue::ExpansionOrder ) );
    QVERIFY( Entries[7].issues_.contains( PathIssue::SetxCopy ) ); QVERIFY( Entries[8].issues_.contains( PathIssue::SetxCopy ) );
    QCOMPARE( Entries[1].shadowedBy_.at( L"TOOL.EXE" ), size_t( 0 ) );
}
void EnvironmentCoreTests::pathRootNormalization()
{
    QCOMPARE( NormalizePath( L"C:/" ), std::wstring( L"C:\\" ) );
    QVERIFY( !SameName( NormalizePath( L"C:" ), NormalizePath( L"C:\\" ) ) );
    QVERIFY( SameName( NormalizePath( L"\\\\host\\share\\" ), NormalizePath( L"//host/share" ) ) );
    auto Parsed = ParseList( L"a;;b;" ); QCOMPARE( Parsed.entries_.size(), size_t( 3 ) ); QVERIFY( Parsed.trailingSeparator_ );
    QCOMPARE( ParseList( L"a;;" ).entries_.size(), size_t( 2 ) );
}
void EnvironmentCoreTests::pathUnknownAndCancellation()
{
    FakeProbe Probe; Probe.data_[L"C:\\Denied"] = { ProbeStatus::Denied };
    AnalysisInput Input; Input.user_ = { L"C:\\Denied", L"C:\\Later" };
    const auto Result = AnalyzePath( Input, Probe ); QVERIFY( !Result[0].issues_.contains( PathIssue::Missing ) );
    std::atomic_bool Cancel{ true }; Probe.calls_ = 0;
    QCOMPARE( AnalyzePath( Input, Probe, &Cancel )[0].probe_.status_, ProbeStatus::Cancelled ); QCOMPARE( Probe.calls_, 0 );
}
void EnvironmentCoreTests::commandSearchContext()
{
    FakeProbe Probe; Probe.data_[L"C:\\Old"] = { ProbeStatus::Exists, { L"tool.cmd" } }; Probe.data_[L"C:\\New"] = { ProbeStatus::Exists, { L"tool.exe" } };
    SearchContext Old{ { L"C:\\Old" }, L".CMD", {} }, New{ { L"C:\\New" }, L".EXE", {} };
    QCOMPARE( FindCommand( L"tool", Old, true, false, Probe ).candidates_[0].path_, std::wstring( L"C:\\Old\\tool.cmd" ) );
    QCOMPARE( FindCommand( L"tool", New, true, false, Probe ).candidates_[0].path_, std::wstring( L"C:\\New\\tool.exe" ) );
    Old.pathExt_.reset(); const auto Missing = FindCommand( L"tool", Old, true, false, Probe ); QVERIFY( Missing.pathExtMissing_ ); QVERIFY( Missing.candidates_.empty() );
    QCOMPARE( FindCommand( L"tool.cmd", Old, true, false, Probe ).candidates_.size(), size_t( 1 ) );
}
void EnvironmentCoreTests::pathextOrderAndCwd()
{
    FakeProbe Probe; Probe.data_[L"C:\\Bin"] = { ProbeStatus::Exists, { L"tool.exe", L"tool.cmd" } }; Probe.data_[L"C:\\Work"] = { ProbeStatus::Exists, { L"tool.cmd" } };
    SearchContext Context{ { L"C:\\Bin", L"c:/bin/" }, L".CMD;.EXE", L"C:\\Work" };
    const auto Result = FindCommand( L"tool", Context, true, true, Probe );
    QCOMPARE( Result.candidates_.size(), size_t( 5 ) ); QCOMPARE( Result.candidates_[0].path_, std::wstring( L"C:\\Work\\tool.cmd" ) );
    QVERIFY( Result.candidates_[0].first_ ); QVERIFY( Result.candidates_[3].duplicate_ );
    Context.currentDirectory_.reset(); QVERIFY( !FindCommand( L"tool", Context, true, true, Probe ).error_.empty() );
}
void EnvironmentCoreTests::processBlockBounds()
{
    const wchar_t Valid[] = L"A=one\0B=two\0\0"; QCOMPARE( ParseEnvironmentBlock( Valid ).status_, ProcessReadStatus::Ok );
    const wchar_t Broken[] = { L'A', L'=', L'b', 0 }; QCOMPARE( ParseEnvironmentBlock( Broken ).status_, ProcessReadStatus::InvalidBlock );
    const wchar_t Duplicate[] = L"A=one\0a=two\0\0"; QCOMPARE( ParseEnvironmentBlock( Duplicate ).status_, ProcessReadStatus::InvalidBlock );
    QVERIFY( ParseEnvironmentBlock( Broken ).values_.empty() ); QVERIFY( ParseEnvironmentBlock( Duplicate ).values_.empty() );
    const wchar_t Empty[] = { 0, 0 }; QVERIFY( ParseEnvironmentBlock( Empty ).values_.empty() );
}
void EnvironmentCoreTests::processHiddenNames()
{
    const wchar_t Block[] = L"=C:=C:\\Synthetic\0=ExitCode=00000000\0A=one=two\0\0";
    const auto Result = ParseEnvironmentBlock( Block ); QCOMPARE( Result.status_, ProcessReadStatus::Ok );
    QCOMPARE( Result.values_.at( L"=C:" ), std::wstring( L"C:\\Synthetic" ) ); QCOMPARE( Result.values_.at( L"A" ), std::wstring( L"one=two" ) );
}
void EnvironmentCoreTests::processPebLayouts()
{
    for( bool Wow : { false, true } )
    {
        FakeMemory Memory; Memory.Setup( Wow ); const auto Result = ReadEnvironmentAtPeb( Memory, 0x1000, Wow );
        QCOMPARE( Result.status_, ProcessReadStatus::Ok ); QCOMPARE( Result.wow64_, Wow );
        QCOMPARE( Result.values_.at( L"A" ), std::wstring( L"one" ) ); QVERIFY( Result.currentDirectory_ ); QCOMPARE( *Result.currentDirectory_, std::wstring( L"C:\\" ) );
    }
}
void EnvironmentCoreTests::processUnstableAndLimits()
{
    FakeMemory Memory; Memory.Setup( false ); Memory.unstable_ = true;
    QCOMPARE( ReadEnvironmentAtPeb( Memory, 0x1000, false ).status_, ProcessReadStatus::Unstable );
    QCOMPARE( ReadEnvironmentAtPeb( Memory, 0x1000, false, 3 ).status_, ProcessReadStatus::TooLarge );
    QCOMPARE( ReadEnvironmentAtPeb( Memory, 0, false ).status_, ProcessReadStatus::InvalidTarget );
    std::vector<wchar_t> Data; QCOMPARE( ReadEnvironmentBytes( Memory, 0x3001, Data, 1024 ), ProcessReadStatus::InvalidBlock );
}
void EnvironmentCoreTests::secretNames()
{
    for( const auto* Name : { L"demo_TOKEN", L"Api_Key", L"SECRET_VALUE", L"password" } ) QVERIFY( SensitiveName( Name ) );
    QVERIFY( !SensitiveName( L"Path" ) );
}
QTEST_GUILESS_MAIN( EnvironmentCoreTests )
