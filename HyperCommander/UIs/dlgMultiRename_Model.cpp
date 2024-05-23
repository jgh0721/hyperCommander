#include "stdafx.h"
#include "dlgMultiRename_Model.hpp"

void QMultiRenameModel::SetSourceFiles( const QVector<QString>& VecFiles )
{
    QVector< Item > VecItems;

    for( const auto& File : VecFiles )
    {
        struct Item Item;
        QFileInfo Info( File );

        Item.Src        = File;
        Item.Src_Name   = Info.completeBaseName();
        Item.Src_Ext    = Info.suffix();
        if( Info.isDir() == false )
            Item.Src_Size = Info.size();
        Item.Src_Created = Info.birthTime();
        Item.Src_Modified = Info.lastModified();

        Item.Dst_Name   = Item.Src_Name;
        Item.Dst_Ext    = Item.Src_Ext;

        VecItems.push_back( Item );
    }
    
    beginResetModel();
    qSwap( VecItems, vecItems );
    endResetModel();

}

void QMultiRenameModel::SetRenamePattern( const QString& Name, const QString& Ext, const QString& Search, const QString& Replace )
{
    Pattern_Name = Name;
    Pattern_Ext = Ext;
    Pattern_Search = Search;
    Pattern_Replace = Replace;
}

void QMultiRenameModel::ProcessPattern()
{
    if( rowCount() == 0 )
        return;

    for( int RowIdx = 0; RowIdx < rowCount(); ++RowIdx )
    {
        auto& Item = vecItems[ RowIdx ];
        Item.Src_Name;
    }

    QChar* Tok = nullptr;

    /// 이름 처리
    Tok = Pattern_Name.data();
    while( Tok && Tok->isNull() == false )
    {

    }
    
    /// 확장자 처리
    
    /// 카운터 처리
    
    /// 이름 및 확장자에서 찾아 바꾸기

    // NOTE: 대상 파일이름 / 확장자 데이터 변경 알림
    emit dataChanged( index( 0, 4 ), index( rowCount() - 1, 5 ), QList<int>() << Qt::DisplayRole );
}

int QMultiRenameModel::rowCount( const QModelIndex& Parent ) const
{
    if( Parent.isValid() == true )
        return 0;

    return vecItems.size();
}

int QMultiRenameModel::columnCount( const QModelIndex& Parent ) const
{
    if( Parent.isValid() == true )
        return 0;

    return 6;
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
        }
    }

    return QVariant();
}
