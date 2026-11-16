#pragma once
#include <Windows.h>
#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace hc::environment {
struct NameLess
{
    bool operator()( const std::wstring& Left, const std::wstring& Right ) const
    {
        if( Left.empty() || Right.empty() ) return Left.size() < Right.size();
        return CompareStringOrdinal( Left.data(), int( Left.size() ), Right.data(), int( Right.size() ), TRUE ) == CSTR_LESS_THAN;
    }
};
inline bool SameName( const std::wstring& Left, const std::wstring& Right )
{
    return !NameLess{}( Left, Right ) && !NameLess{}( Right, Left );
}
enum class Scope { CoreSystem, System, CoreUser, User, Account, Process };
enum class ValueType : uint32_t { String = REG_SZ, ExpandString = REG_EXPAND_SZ };
enum class ReferenceIssue { Undefined, LaterStage, SameStage, Cycle };
struct Value
{
    std::wstring name_;
    std::wstring raw_;
    ValueType type_ = ValueType::String;
    Scope scope_ = Scope::User;
    bool operator==( const Value& Other ) const = default;
};
struct Reference
{
    std::wstring name_;
    ReferenceIssue issue_ = ReferenceIssue::Undefined;
};
struct Origin
{
    Value value_;
    int stage_ = 0;
    std::wstring expanded_;
    std::vector<Reference> unresolved_;
    bool appended_ = false;
};
struct EffectiveValue
{
    std::wstring name_, value_;
    std::vector<Origin> origins_;
};
using ValueMap = std::map<std::wstring, std::wstring, NameLess>;
using EffectiveMap = std::map<std::wstring, EffectiveValue, NameLess>;
struct MergeInput
{
    std::vector<Value> coreSystem_, system_, coreUser_, user_, account_;
    // VM-verified exceptions only; display provenance still belongs to coreUser.
    std::set<std::wstring, NameLess> availableToSystemExpansion_;
};
struct MergeResult
{
    EffectiveMap values_;
    ValueMap afterSystemStrings_, beforeUserExpansion_;
};
inline bool SensitiveName( const std::wstring& Name )
{
    for( const auto* Part : { L"TOKEN", L"KEY", L"SECRET", L"PASSWORD" } )
        for( size_t Index = 0, Count = wcslen( Part ); Index + Count <= Name.size(); ++Index )
            if( SameName( Name.substr( Index, Count ), Part ) ) return true;
    return false;
}
inline std::optional<std::string> ValidateValue( const Value& Item )
{
    if( Item.name_.empty() || Item.name_.size() > 32767 || Item.name_.find_first_of( L"=\0", 0, 2 ) != std::wstring::npos ) return "invalid-name";
    if( Item.raw_.size() > 32767 || Item.raw_.find( L'\0' ) != std::wstring::npos ) return "invalid-value";
    if( Item.type_ != ValueType::String && Item.type_ != ValueType::ExpandString ) return "invalid-type";
    return {};
}
} // namespace hc::environment
