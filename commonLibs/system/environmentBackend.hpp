#pragma once
#include "environmentTypes.hpp"
#include <mutex>

namespace hc::environment {
enum class WriteStatus { Applied, Conflict, ReadOnly, Invalid };
class IEnvironmentBackend
{
public:
    virtual ~IEnvironmentBackend() = default;
    virtual std::vector<Value> ReadScope( Scope Source ) const = 0;
    virtual std::optional<Value> ReadValue( Scope Source, const std::wstring& Name ) const = 0;
    // One named value, including an absent before/after. Rename is a higher-level batch.
    virtual WriteStatus CompareAndWrite( Scope Source, const std::wstring& Name,
        const std::optional<Value>& Before, const std::optional<Value>& After ) = 0;
};
class MemoryEnvironmentBackend final : public IEnvironmentBackend
{
public:
    explicit MemoryEnvironmentBackend( const std::vector<Value>& Seed = {} )
    {
        for( const auto& Item : Seed ) values_[Item.scope_][Item.name_] = Item;
    }
    std::vector<Value> ReadScope( Scope Source ) const override
    {
        std::lock_guard Lock( mutex_ );
        std::vector<Value> Result;
        if( const auto Found = values_.find( Source ); Found != values_.end() )
            for( const auto& [Name, Item] : Found->second ) Result.push_back( Item );
        return Result;
    }
    std::optional<Value> ReadValue( Scope Source, const std::wstring& Name ) const override
    {
        std::lock_guard Lock( mutex_ ); return readValue( Source, Name );
    }
    WriteStatus CompareAndWrite( Scope Source, const std::wstring& Name,
        const std::optional<Value>& Before, const std::optional<Value>& After ) override
    {
        std::lock_guard Lock( mutex_ );
        if( Source != Scope::User && Source != Scope::System ) return WriteStatus::ReadOnly;
        if( Name.empty() || Name.find( L'=' ) != std::wstring::npos || Name.find( L'\0' ) != std::wstring::npos ) return WriteStatus::Invalid;
        if( After && ( After->scope_ != Source || !SameName( After->name_, Name ) || ValidateValue( *After ) ) ) return WriteStatus::Invalid;
        if( readValue( Source, Name ) != Before ) return WriteStatus::Conflict;
        if( After ) values_[Source][Name] = *After;
        else values_[Source].erase( Name );
        return WriteStatus::Applied;
    }
private:
    std::optional<Value> readValue( Scope Source, const std::wstring& Name ) const
    {
        const auto Group = values_.find( Source );
        if( Group == values_.end() ) return {};
        const auto Found = Group->second.find( Name );
        return Found == Group->second.end() ? std::nullopt : std::optional<Value>( Found->second );
    }
    mutable std::mutex mutex_;
    std::map<Scope, std::map<std::wstring, Value, NameLess>> values_;
};
} // namespace hc::environment
