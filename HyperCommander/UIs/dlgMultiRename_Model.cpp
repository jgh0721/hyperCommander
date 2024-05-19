#include "stdafx.h"
#include "dlgMultiRename_Model.hpp"

int QMultiRenameModel::rowCount( const QModelIndex& Parent ) const
{
    if( Parent.isValid() == true )
        return 0;

    return 0;
}

int QMultiRenameModel::columnCount( const QModelIndex& Parent ) const
{
    return 6;
}

QVariant QMultiRenameModel::data( const QModelIndex& Index, int Role ) const
{
    return QVariant();
}
