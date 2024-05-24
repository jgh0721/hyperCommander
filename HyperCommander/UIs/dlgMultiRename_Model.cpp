#include "stdafx.h"
#include "dlgMultiRename_Model.hpp"

void QMultiRenameModel::SetSourceFiles( const QVector<QString>& VecFiles )
{
    QVector< Item > VecItems;

    for( const auto& File : VecFiles )
    {
        struct Item Item;
        QFileInfo Info( File );

        Item.Src            = File;
        Item.Src_Name       = Info.completeBaseName();
        Item.Src_Ext        = Info.suffix();
        if( Info.isDir() == false )
            Item.Src_Size   = Info.size();
        Item.Src_Created    = Info.birthTime();
        Item.Src_Modified   = Info.lastModified();

        VecItems.push_back( Item );
    }
    
    beginResetModel();
    qSwap( VecItems, vecItems );
    endResetModel();

}

void QMultiRenameModel::SetRenamePattern( const QString& Name, const QString& Ext )
{
    Pattern_Name    = Name;
    Pattern_Ext     = Ext;
}

void QMultiRenameModel::SetSearchReplace( const QString& Search, const QString& Replace, bool IsCaseSensitive,
                                          bool IsOnlyOnce, int ApplyToFileExt )
{
    Pattern_Search                  = Search;
    Pattern_Replace                 = Replace;
    Pattern_SR_IsUseCaseSensitive   = IsCaseSensitive;
    Pattern_SR_IsUseOnlyOnce        = IsOnlyOnce;
    Pattern_SR_ApplyToFileExt       = ApplyToFileExt;
}

void QMultiRenameModel::ProcessPattern()
{
    if( rowCount() == 0 )
        return;

    for( int RowIdx = 0; RowIdx < rowCount(); ++RowIdx )
    {
        auto& Item = vecItems[ RowIdx ];

        Item.Dst_Name = "";
        Item.Dst_Ext = "";

        /// 이름 처리
        processPattern( Pattern_Name, Item, Item.Dst_Name );
        /// 확장자 처리
        processPattern( Pattern_Ext, Item, Item.Dst_Ext );
        /// 카운터 처리
        /// 이름 및 확장자에서 찾아 바꾸기
        processFindReplace( Pattern_Search, Pattern_Replace, Item );
    }

    // NOTE: 대상 파일이름 / 확장자 데이터 변경 알림
    emit dataChanged( index( 0, 4 ), index( rowCount() - 1, 5 ), QList<int>() << Qt::DisplayRole );
}

void QMultiRenameModel::processFindReplace( const QString& Search, const QString& Replace, Item& Info )
{
    if( Search.isEmpty() == true )
        return;

    const auto Case = Pattern_SR_IsUseCaseSensitive == true ? Qt::CaseSensitive : Qt::CaseInsensitive;

    if( Pattern_SR_IsUseOnlyOnce == true )
    {
        if( Pattern_SR_IsUseOnlyOnce == 0 || Pattern_SR_ApplyToFileExt == 1 )
        {
            const auto Index = Info.Dst_Name.indexOf( Search, 0, Case );
            if( Index >= 0 )
                Info.Dst_Name.replace( Index, Search.length(), Replace );
        }

        if( Pattern_SR_IsUseOnlyOnce == 0 || Pattern_SR_ApplyToFileExt == 2 )
        {
            const auto Index = Info.Dst_Ext.indexOf( Search, 0, Case );
            if( Index >= 0 )
                Info.Dst_Ext.replace( Index, Search.length(), Replace );
        }
    }
    else
    {
        if( Pattern_SR_IsUseOnlyOnce == 0 || Pattern_SR_ApplyToFileExt == 1 )
            Info.Dst_Name = Info.Dst_Name.replace( Search, Replace, Case );
        if( Pattern_SR_ApplyToFileExt == 0 || Pattern_SR_ApplyToFileExt == 2 )
            Info.Dst_Ext = Info.Dst_Ext.replace( Search, Replace, Case );
    }
}

int QMultiRenameModel::rowCount( const QModelIndex& Parent ) const
{
    if( Parent.isValid() == true )
        return 0;

    return static_cast< int >( vecItems.size() );
}

int QMultiRenameModel::columnCount( const QModelIndex& Parent ) const
{
    if( Parent.isValid() == true )
        return 0;

    return 7;
}

QVariant QMultiRenameModel::data( const QModelIndex& Index, int Role ) const
{
    if( checkIndex( Index, CheckIndexOption::IndexIsValid ) == false )
        return {};

    const auto Row = Index.row();
    const auto Col = Index.column();

    if( Role == Qt::DisplayRole )
    {
        const auto& Item = vecItems[ Row ];

        switch( Col )
        {
            case 0: return Item.Src_Name;
            case 1: return Item.Src_Ext;
            case 2: return Item.Src_Size >= 0 ? QString::number( Item.Src_Size ) : QString();
            case 3: return Item.Src_Created.toString( "yyyy-MM-dd hh:mm:ss" );
            case 4: return Item.Dst_Name;
            case 5: return Item.Dst_Ext;
            case 6: return QDir::toNativeSeparators( QFileInfo( Item.Src ).filePath() );
            default: return {};
        }
    }

    if( Role == Qt::BackgroundRole )
    {
        
    }

    return {};
}

QVariant QMultiRenameModel::headerData( int section, Qt::Orientation orientation, int role ) const
{
    if( orientation != Qt::Horizontal )
        return QAbstractTableModel::headerData( section, orientation, role );

    if( role == Qt::TextAlignmentRole )
        return Qt::AlignCenter;

    return QAbstractTableModel::headerData( section, orientation, role );
}

void QMultiRenameModel::processPattern( const QString& Pattern, const Item& Info, QString& Dst )
{
    const auto Pat = Pattern.toStdWString();
    auto Fmt = Pat.c_str();

    while( Fmt && *Fmt != L'\0' )
    {
        while( *Fmt && ( *Fmt != '[' && *Fmt != ']' ) )
            Dst.push_back( *Fmt++ );

        do
        {
            if( *Fmt == L'\0' )
                break;

            if( ( Fmt[ 0 ] == '[' && Fmt[ 1 ] == '[' ) || 
                ( Fmt[ 0 ] == ']' && Fmt[ 1 ] == ']' ) )
            {
                Dst.push_back( Fmt[ 0 ] );
                Fmt = Fmt + 2;
                break;
            }

            ++Fmt;
            if( *Fmt == 'N' )
            {
                ++Fmt;
                processNameExt( Fmt, Info.Src_Name, Dst );
            }
            else if( *Fmt == 'A' )
            {
                ++Fmt;
                processNameExt( Fmt, Info.Src_Ext.isEmpty() == false ? Info.Src_Name + "." + Info.Src_Ext : Info.Src_Name, Dst );
            }
            else if( *Fmt == 'E' )
            {
                ++Fmt;
                processNameExt( Fmt, Info.Src_Ext, Dst );
            }
            else if( *Fmt == 'P' )
            {
                ++Fmt;
                processNameExt( Fmt, Info.Src.section( '\\', -1 ), Dst );
            }

        } while( false );

    }
}

void QMultiRenameModel::processNameExt( const wchar_t*& Fmt, const QString& Src, QString& Dst )
{
    // NOTE: 패턴의 색인은 1부터 시작이다. 

    do
    {
        if( *Fmt == ']' )
        {
            Fmt = Fmt + 1;
            Dst += Src;
            break;
        }

        bool IsReverse = false;
        if( *Fmt == '-' )
        {
            IsReverse = true;
            ++Fmt;
        }

        wchar_t FillChar = '\0';
        if( *Fmt == '0' || *Fmt == ' ' )
        {
            FillChar = *Fmt;
            ++Fmt;
        }

        unsigned int S = 0;
        unsigned int E = 0;
        if( isdigit( *Fmt ) != 0 )
        {
            S = parse_nonnegative_int( Fmt );
        }

        if( *Fmt == '-' )
        {
            ++Fmt;
            if( isdigit( *Fmt ) != 0 )
            {
                E = parse_nonnegative_int( Fmt );
                if( E <= S )
                    break;

                // [N1-5] or [N-1-5]
                if( IsReverse == false )
                {
                    if( FillChar == '\0' )
                        Dst.push_back( Src.mid( S - 1, E - S ) );
                    else
                    {
                        const auto DstLength = E - S + 1;
                        const auto Start = S - 1;
                        const auto Content = Src.mid( Start, DstLength );
                        Dst += QString( DstLength - Content.size(), FillChar );
                        Dst += Content;
                        break;
                    }
                }
                else
                {
                    if( FillChar == '\0' )
                        Dst.push_back( Src.mid( Src.size() - E + 1, E - S ) );
                }

                if( *Fmt == ']' )
                    ++Fmt;
                break;
            }

            if( *Fmt == L']' )
            {
                // [N2-] or [N-2-]
                if( IsReverse == false )
                    Dst.push_back( Src.mid( S - 1 ) );
                else
                    Dst.push_back( Src.mid( Src.size() - S ) );

                ++Fmt;
                break;
            }
        }
        else if( *Fmt == ',' )
        {
            ++Fmt;
            if( isdigit( *Fmt ) != 0 )
            {
                E = parse_nonnegative_int( Fmt );

                // [N1,5] or [N-1,5]
                if( IsReverse == false )
                    Dst.push_back( Src.mid( S - 1, E ) );
                else
                    Dst.push_back( Src.mid( Src.size() - E + 1, E - S + 1 ) );

                if( *Fmt == ']' )
                    ++Fmt;
                break;
            }
        }
        else if( *Fmt == L']' )
        {
            ++Fmt;

            // [N2]
            if( S == 0 )
                break;

            if( S > Src.size() )
                break;

            Dst.push_back( Src[ S - 1 ] );
            break;
        }

    } while( false );
}
