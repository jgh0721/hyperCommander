#pragma once
#include "environmentMerge.hpp"
#include <atomic>

namespace hc::environment {
enum class ProbeStatus { Exists, Missing, Denied, Timeout, Cancelled };
struct DirectoryProbe
{
    ProbeStatus status_ = ProbeStatus::Missing;
    std::vector<std::wstring> files_;
    std::optional<bool> writableByUsers_;
    bool appAlias_ = false;
    std::set<std::wstring, NameLess> appAliases_;
};
class IPathProbe
{
public:
    virtual ~IPathProbe() = default;
    // Implementations must bound I/O and distinguish unavailable from missing.
    virtual DirectoryProbe Inspect( const std::wstring& Path ) = 0;
};
enum class PathIssue { Missing, Duplicate, Shadowed, Undefined, ExpansionOrder, Quote, Empty, Writable, SetxCopy };
struct PathEntry
{
    Scope scope_ = Scope::User;
    size_t index_ = 0;
    std::wstring raw_, expanded_, normalized_;
    std::set<PathIssue> issues_;
    std::optional<size_t> duplicateOf_;
    std::map<std::wstring, size_t, NameLess> shadowedBy_;
    DirectoryProbe probe_;
};
struct ParsedList
{
    std::vector<std::wstring> entries_;
    bool trailingSeparator_ = false;
};
inline ParsedList ParseList( const std::wstring& Text )
{
    ParsedList Result;
    if( Text.empty() ) return Result;
    Result.trailingSeparator_ = Text.back() == L';';
    size_t Begin = 0;
    for( size_t Index = 0; Index <= Text.size(); ++Index )
        if( Index == Text.size() || Text[Index] == L';' )
        {
            if( Index < Text.size() || Begin < Text.size() ) Result.entries_.push_back( Text.substr( Begin, Index - Begin ) );
            Begin = Index + 1;
        }
    return Result;
}
inline std::wstring NormalizePath( std::wstring Path )
{
    // Only comparison normalization. Never write this string back to the registry.
    Path.erase( std::remove( Path.begin(), Path.end(), L'"' ), Path.end() );
    std::replace( Path.begin(), Path.end(), L'/', L'\\' );
    // Keep the root separator: C: (drive-relative) is not C:\ (absolute).
    while( Path.size() > 1 && Path.back() == L'\\' && !( Path.size() == 3 && Path[1] == L':' ) ) Path.pop_back();
    return Path;
}
struct AnalysisInput
{
    std::vector<std::wstring> system_, user_;
    ValueMap systemExpansion_, userExpansion_, allDefinitions_;
    ValueType systemType_ = ValueType::ExpandString, userType_ = ValueType::ExpandString;
    bool executablePath_ = true;
};
inline std::vector<PathEntry> AnalyzePath( const AnalysisInput& Input, IPathProbe& Probe, const std::atomic_bool* Cancel = nullptr )
{
    std::vector<PathEntry> Result;
    std::map<std::wstring, size_t, NameLess> Seen, FirstFile;
    const auto Add = [&]( const std::vector<std::wstring>& Entries, Scope Source, const ValueMap& Available, ValueType Type ) {
        for( size_t Index = 0; Index < Entries.size(); ++Index )
        {
            PathEntry Entry; Entry.scope_ = Source; Entry.index_ = Index; Entry.raw_ = Entries[Index];
            Entry.expanded_ = Type == ValueType::ExpandString ? ExpandAtStage( Entry.raw_, Available, [&]( const auto& Name ) {
                Entry.issues_.insert( Input.allDefinitions_.contains( Name ) ? PathIssue::ExpansionOrder : PathIssue::Undefined );
            } ) : Entry.raw_;
            if( Entry.raw_.empty() ) Entry.issues_.insert( PathIssue::Empty );
            if( Entry.raw_.find( L'"' ) != Entry.raw_.npos ) Entry.issues_.insert( PathIssue::Quote );
            Entry.normalized_ = NormalizePath( Entry.expanded_ );
            const bool Resolved = !Entry.issues_.contains( PathIssue::Undefined ) && !Entry.issues_.contains( PathIssue::ExpansionOrder );
            if( Resolved && !Entry.normalized_.empty() )
            {
                const auto [Found, Inserted] = Seen.emplace( Entry.normalized_, Result.size() );
                if( !Inserted ) { Entry.issues_.insert( PathIssue::Duplicate ); Entry.duplicateOf_ = Found->second; }
                if( Cancel && Cancel->load() ) Entry.probe_.status_ = ProbeStatus::Cancelled;
                else Entry.probe_ = Entry.duplicateOf_ ? Result[*Entry.duplicateOf_].probe_ : Probe.Inspect( Entry.normalized_ );
                if( Entry.probe_.status_ == ProbeStatus::Missing ) Entry.issues_.insert( PathIssue::Missing );
                if( Source == Scope::System && Entry.probe_.writableByUsers_ == true ) Entry.issues_.insert( PathIssue::Writable );
                if( Input.executablePath_ && Entry.probe_.status_ == ProbeStatus::Exists )
                    for( const auto& File : Entry.probe_.files_ )
                    {
                        const auto [First, New] = FirstFile.emplace( File, Result.size() );
                        if( !New && !Entry.duplicateOf_ ) { Entry.shadowedBy_[File] = First->second; Entry.issues_.insert( PathIssue::Shadowed ); }
                    }
            }
            Result.push_back( std::move( Entry ) );
        }
    };
    Add( Input.system_, Scope::System, Input.systemExpansion_, Input.systemType_ );
    Add( Input.user_, Scope::User, Input.userExpansion_, Input.userType_ );
    // A contiguous copied sequence is evidence, not proof of setx use.
    for( size_t User = Input.system_.size(); User < Result.size(); ++User )
        for( size_t System = 0; System < Input.system_.size(); ++System )
        {
            size_t Length = 0;
            while( System + Length < Input.system_.size() && User + Length < Result.size() &&
                !Result[System + Length].normalized_.empty() &&
                SameName( Result[System + Length].normalized_, Result[User + Length].normalized_ ) ) ++Length;
            if( Length >= 2 ) for( size_t Offset = 0; Offset < Length; ++Offset ) Result[User + Offset].issues_.insert( PathIssue::SetxCopy );
        }
    return Result;
}
struct SearchContext
{
    std::vector<std::wstring> path_;
    std::optional<std::wstring> pathExt_; // absent and empty stay distinguishable
    std::optional<std::wstring> currentDirectory_;
};
struct CommandCandidate
{
    std::wstring path_;
    size_t directoryIndex_ = 0;
    bool first_ = false, duplicate_ = false, alias_ = false;
};
struct CommandSearchResult
{
    std::vector<CommandCandidate> candidates_;
    std::vector<std::pair<std::wstring, ProbeStatus>> unavailable_;
    bool pathExtMissing_ = false;
    std::string error_;
};
inline CommandSearchResult FindCommand( const std::wstring& Name, const SearchContext& Context,
    bool UseExtensions, bool CurrentFirst, IPathProbe& Probe, const std::atomic_bool* Cancel = nullptr )
{
    CommandSearchResult Result;
    if( Name.empty() || Name.find_first_of( L"\r\n\0", 0, 3 ) != Name.npos ) { Result.error_ = "invalid-command"; return Result; }
    auto Directories = Context.path_;
    std::wstring FileName = Name;
    if( const auto Cut = Name.find_last_of( L"\\/" ); Cut != Name.npos )
    {
        auto Directory = Name.substr( 0, Cut + 1 ); FileName = Name.substr( Cut + 1 );
        if( FileName.empty() ) { Result.error_ = "invalid-command"; return Result; }
        Directories = { Directory };
    }
    else if( CurrentFirst )
    {
        if( !Context.currentDirectory_ ) { Result.error_ = "current-directory-unavailable"; return Result; }
        Directories.insert( Directories.begin(), *Context.currentDirectory_ );
    }
    std::vector<std::wstring> Extensions{ L"" };
    if( UseExtensions && FileName.find_last_of( L'.' ) == FileName.npos )
    {
        Extensions.clear();
        Result.pathExtMissing_ = !Context.pathExt_;
        if( Context.pathExt_ ) for( const auto& Extension : ParseList( *Context.pathExt_ ).entries_ )
            if( !Extension.empty() && Extension.front() == L'.' && Extension.find_first_of( L"\\/\r\n" ) == Extension.npos ) Extensions.push_back( Extension );
    }
    std::set<std::wstring, NameLess> Seen;
    for( size_t Index = 0; Index < Directories.size(); ++Index )
    {
        if( Cancel && Cancel->load() ) { Result.error_ = "cancelled"; break; }
        auto Directory = NormalizePath( Directories[Index] );
        // Empty PATH entries are diagnostic entries, not an implicit current-directory option.
        if( Directory.empty() ) continue;
        const auto Data = Probe.Inspect( Directory );
        if( Data.status_ != ProbeStatus::Exists ) { Result.unavailable_.emplace_back( Directory, Data.status_ ); continue; }
        for( const auto& Extension : Extensions )
        {
            const auto Found = std::find_if( Data.files_.begin(), Data.files_.end(), [&]( const auto& File ) { return SameName( File, FileName + Extension ); } );
            if( Found == Data.files_.end() ) continue;
            const auto Full = Directory + ( Directory.back() == L'\\' || Directory.back() == L':' ? L"" : L"\\" ) + *Found;
            Result.candidates_.push_back( { Full, Index, Result.candidates_.empty(), !Seen.insert( Full ).second, Data.appAlias_ || Data.appAliases_.contains(*Found) } );
            // Keep all PATHEXT alternatives so the view can explain same-directory precedence.
        }
    }
    return Result;
}
} // namespace hc::environment
