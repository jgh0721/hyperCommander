#pragma once
#include "environmentTypes.hpp"
#include <functional>

namespace hc::environment {
inline std::vector<std::wstring> References( std::wstring_view Text )
{
    std::vector<std::wstring> Result;
    size_t Position = 0;
    while( ( Position = Text.find( L'%', Position ) ) != Text.npos )
    {
        const auto End = Text.find( L'%', Position + 1 );
        if( End == Text.npos ) break;
        if( End > Position + 1 ) Result.emplace_back( Text.substr( Position + 1, End - Position - 1 ) );
        Position = End + 1;
    }
    return Result;
}
inline std::wstring ExpandAtStage( const std::wstring& Raw, const ValueMap& Available,
    const std::function<void( const std::wstring& )>& Missing = {} )
{
    std::wstring Result;
    size_t Position = 0;
    while( Position < Raw.size() )
    {
        const auto Begin = Raw.find( L'%', Position );
        if( Begin == Raw.npos ) { Result.append( Raw, Position ); break; }
        Result.append( Raw, Position, Begin - Position );
        const auto End = Raw.find( L'%', Begin + 1 );
        if( End == Raw.npos ) { Result.append( Raw, Begin ); break; }
        const auto Name = Raw.substr( Begin + 1, End - Begin - 1 );
        const auto Found = Available.find( Name );
        if( !Name.empty() && Found != Available.end() ) Result += Found->second;
        else { Result.append( Raw, Begin, End - Begin + 1 ); if( Missing && !Name.empty() ) Missing( Name ); }
        Position = End + 1;
    }
    return Result;
}
inline MergeResult BuildEnvironment( const MergeInput& Input )
{
    struct Definition { Value value_; int stage_; };
    std::vector<Definition> Definitions;
    const auto Add = [&]( const std::vector<Value>& Items, Scope Source, int Plain, int Expand ) {
        for( auto Item : Items ) { Item.scope_ = Source; Definitions.push_back( { Item, Item.type_ == ValueType::ExpandString ? Expand : Plain } ); }
    };
    Add( Input.coreSystem_, Scope::CoreSystem, 1, 1 ); Add( Input.system_, Scope::System, 2, 3 );
    Add( Input.coreUser_, Scope::CoreUser, 4, 4 ); Add( Input.user_, Scope::User, 5, 6 ); Add( Input.account_, Scope::Account, 7, 7 );
    std::stable_sort( Definitions.begin(), Definitions.end(), []( const auto& Left, const auto& Right ) { return Left.stage_ < Right.stage_; } );
    MergeResult Result;
    ValueMap Current;
    const auto Issue = [&]( const Definition& Owner, const std::wstring& Name ) {
        bool Found = false, Same = false;
        for( const auto& Other : Definitions ) if( SameName( Other.value_.name_, Name ) ) { Found = true; Same |= Other.stage_ == Owner.stage_; }
        std::set<std::wstring, NameLess> Visited;
        std::function<bool( const std::wstring& )> Reaches = [&]( const std::wstring& Node ) {
            if( SameName( Node, Owner.value_.name_ ) ) return true;
            if( !Visited.insert( Node ).second ) return false;
            for( const auto& Other : Definitions )
                if( Other.stage_ == Owner.stage_ && Other.value_.type_ == ValueType::ExpandString && SameName( Other.value_.name_, Node ) )
                    for( const auto& Ref : References( Other.value_.raw_ ) ) if( Reaches( Ref ) ) return true;
            return false;
        };
        return Reaches( Name ) ? ReferenceIssue::Cycle : Same ? ReferenceIssue::SameStage : Found ? ReferenceIssue::LaterStage : ReferenceIssue::Undefined;
    };
    for( int Stage = 1; Stage <= 7; ++Stage )
    {
        auto Available = Current;
        if( Stage == 3 )
            for( const auto& Item : Input.coreUser_ )
                if( Input.availableToSystemExpansion_.contains( Item.name_ ) && !Available.contains( Item.name_ ) ) Available[Item.name_] = Item.raw_;
        if( Stage == 3 ) Result.afterSystemStrings_ = Available;
        if( Stage == 6 ) Result.beforeUserExpansion_ = Available;
        for( const auto& Item : Definitions )
        {
            if( Item.stage_ != Stage ) continue;
            Origin Source{ Item.value_, Stage, Item.value_.raw_ };
            if( Item.value_.type_ == ValueType::ExpandString )
                Source.expanded_ = ExpandAtStage( Item.value_.raw_, Available, [&]( const auto& Name ) { Source.unresolved_.push_back( { Name, Issue( Item, Name ) } ); } );
            auto& Target = Result.values_[Item.value_.name_];
            if( Target.name_.empty() ) Target.name_ = Item.value_.name_;
            const bool Append = Item.value_.scope_ == Scope::User &&
                ( SameName( Item.value_.name_, L"Path" ) || SameName( Item.value_.name_, L"LibPath" ) || SameName( Item.value_.name_, L"Os2LibPath" ) );
            const bool HasSystem = std::any_of( Target.origins_.begin(), Target.origins_.end(), []( const auto& Origin ) { return Origin.value_.scope_ == Scope::System; } );
            Source.appended_ = Append && HasSystem;
            auto Text = Source.expanded_;
            if( Source.appended_ && !Target.value_.empty() && !Text.empty() )
                Text = Target.value_ + ( Target.value_.back() == L';' ? L"" : L";" ) + Text;
            else if( Source.appended_ && Text.empty() ) Text = Target.value_;
            Target.value_ = Text; Target.origins_.push_back( std::move( Source ) ); Current[Item.value_.name_] = Text;
        }
    }
    return Result;
}
} // namespace hc::environment
