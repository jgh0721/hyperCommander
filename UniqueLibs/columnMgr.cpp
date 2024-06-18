#include "stdafx.h"
#include "columnMgr.hpp"

#include "builtInFsModel.hpp"
#include "cmnHCUtils.hpp"

void CColumnMgr::Initialize()
{
    VecColumnViews.push_back( BUILTIN_COLVIEW_DETAILS );
}

ColumnView CColumnMgr::GetColumnView( int Index )
{
    if( Index < 0 || Index >= VecColumnViews.size() )
        return {};

    return VecColumnViews[ Index ];
}

bool CColumnMgr::Parse( wchar_t*& Fmt, ColumnParseResult& Result, QString& Content )
{
    bool IsContinue = false;
    wchar_t* Pos = nullptr;

    while( IsContinue == false && Fmt && *Fmt != L'\0' )
    {
        while( *Fmt && ( *Fmt != '[' && *Fmt != ']' ) )
            Content.push_back( *Fmt++ );

        do
        {
            if( *Fmt == L'\0' )
                break;

            if( ( Fmt[ 0 ] == '[' && Fmt[ 1 ] == '[' ) ||
                ( Fmt[ 0 ] == ']' && Fmt[ 1 ] == ']' ) )
            {
                Content.push_back( Fmt[ 0 ] );
                Fmt = Fmt + 2;
                break;
            }

            ++Fmt;
            // = 으로 시작하지 않으면 해당 [] 는 무시한다. 
            if( Fmt[ 0 ] != L'=' )
            {
                while( *Fmt && *Fmt != L']' )
                    ++Fmt;

                break;
            }
            
            ++Fmt;
            // . 구분자를 찾지 못하면 역시 무시
            Pos = wcschr( Fmt, L'.' );
            if( Pos == nullptr )
            {
                while( *Fmt && *Fmt != L']' )
                    ++Fmt;

                break;
            }

            Result.Module = QStringView( Fmt, Pos - Fmt );
            Fmt = Pos;
            ++Fmt;

            Pos = wcschr( Fmt, L'.' );
            if( Pos == nullptr )
            {
                while( *Fmt && *Fmt != L']' )
                    ++Fmt;

                break;
            }

            Result.Cate = QStringView( Fmt, Pos - Fmt );
            Fmt = Pos;
            ++Fmt;

            Pos = wcspbrk( Fmt, L".]");
            if( Pos == nullptr )
            {
                // Type 은 기본값을 사용한다. 
                Pos = wcschr( Fmt, L']' );
                if( Pos != nullptr )
                {
                    Result.Name = QStringView( Fmt, Pos - Fmt );
                    Fmt = Pos;
                    ++Fmt;
                    IsContinue = true;
                    break;
                }

                // ] 가 보이지 않는다. 올바르지 않은 문장임. 
                break;
            }
            
            
            Result.Name = QStringView( Fmt, Pos - Fmt );
            Fmt = Pos;
            ++Fmt;

            if( *Pos == L']' )
            {
                IsContinue = true;
                break;
            }

            // Type 을 찾아보자... 
            int a = 0;

        } while( false );
    }

    return IsContinue;
}

void CColumnMgr::CreateColumnContent( const ColumnParseResult& Result, Node* Info, QString& Content )
{
    if( Result.Module.compare( QLatin1StringView( "HC" ), Qt::CaseInsensitive ) == 0 )
    {
        if( Result.Cate.compare( QLatin1StringView( "Fs" ), Qt::CaseInsensitive ) == 0 )
        {
            builtInFsColumn( Result.Name, Result.Convert, Info, Content );
            return;
        }
    }
}

void CColumnMgr::builtInFsColumn( QStringView Name, QStringView Type, Node* Info, QString& Content )
{
    if( Info == nullptr )
        return;

    // TODO: Type 을 고려해야 한다. 

    if( Name.compare( QLatin1String( "name" ), Qt::CaseInsensitive ) == 0 )
        Content.push_back( Info->Name );
    else if( Name.compare( QLatin1String( "size" ), Qt::CaseInsensitive ) == 0 )
    {
        if( FlagOn( Info->Attiributes, FILE_ATTRIBUTE_DIRECTORY ) )
        {
            if( Info->Size > 0 )
                Content.push_back( QString::number( Info->Size ) );
            else
            {
                if( FlagOn( Info->Attiributes, FILE_ATTRIBUTE_REPARSE_POINT ) )
                {
                    Content.push_back( QObject::tr( "<링크>" ) );
                }
                else
                {
                    Content.push_back( QObject::tr( "<폴더>" ) );
                }
            }
        }
        else
            Content.push_back( QString::number( Info->Size ) );
    }
    else if( Name.compare( QLatin1String( "created" ), Qt::CaseInsensitive ) == 0 )
    {
        Content.push_back( Info->Created.toString( "yyyy-MM-dd hh:mm:ss" ) );
    }
    else if( Name.compare( QLatin1String( "attribText" ), Qt::CaseInsensitive ) == 0 )
    {
        Content.push_back( GetFormattedAttrText( Info->Attiributes, false ) );
    }
}
