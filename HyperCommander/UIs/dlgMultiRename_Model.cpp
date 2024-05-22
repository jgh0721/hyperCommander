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

void QMultiRenameModel::ProcessPattern()
{
    if( rowCount() == 0 )
        return;

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
