#include "stdafx.h"
#include "cmnHCModelUtils.hpp"

#include "builtInFsModel.hpp"

DECLARE_CMNLIBSV2_NAMESPACE

////////////////////////////////////////////////////////////////////////////////
///

bool QTraverseSrcModel::IsFinished() const
{
    return isFinished_;
}

void QTraverseSrcModel::run()
{
    emit notifyStats( "", false, 0, 0, 0 );

    if( src.isEmpty() == false )
        model_ = const_cast< FSModel* >( qobject_cast< const FSModel* >( src[ 0 ].model() ) );

    if( filter.isEmpty() == false )
        regexp = QRegularExpression( QRegularExpression::wildcardToRegularExpression( filter ) );

    auto Parent = model_->GetFileFullPath( "" );
    if( Parent.endsWith( "\\" ) )
        Parent = Parent.left( Parent.length() - 1 );

    base = Parent;
    baseLength = base.length();

    for( int idx = 0; idx < src.size(); ++idx )
    {
        if( isInterruptionRequested() == true )
            break;

        const auto& Node = model_->GetFileInfo( src[ idx ] );
        if( FlagOn( Node.Attiributes, FILE_ATTRIBUTE_DIRECTORY ) )
        {
            traverseSubDirectory( model_->GetFileFullPath( src[ idx ] ), Node );
        }
        else
        {
            processFile( Parent, Node );
        }
    }

    isFinished_ = true;
    emit notifyResultReady();
}

void QTraverseSrcModel::traverseSubDirectory( const QString& Path, const Node& Item )
{
    QStack< QPair< QString, Node > > Stack;
    Stack.push_back( qMakePair( Path, Item ) );

    while( Stack.isEmpty() == false )
    {
        const auto P = Stack.pop();

        dirCount_ += 1;
        vecItems_.push_back( P.second );
        vecItemMiddle_.emplace_back( P.first.mid( baseLength ) + "\\" );

        emit notifyStats( vecItemMiddle_.last(), true, totalSize_, fileCount_, dirCount_);

        if( isRecursive == false )
            continue;

        for( const auto& Child : model_->GetChildItems( P.first ) )
        {
            if( isInterruptionRequested() == true )
                break;

            if( FlagOn( Child.Attiributes, FILE_ATTRIBUTE_DIRECTORY ) )
            {
                Stack.emplace_back( qMakePair( P.first + "\\" + Child.Name, Child ) );
            }
            else
            {
                processFile( P.first, Child );
            }
        }
    }
}

void QTraverseSrcModel::processFile( const QString& Parent, const Node& Item )
{
    bool IsMatch = false;

    if( filter.isEmpty() == false && regexp.isValid() == true )
        IsMatch = regexp.match( Item.Name ).hasMatch();
    else
        IsMatch = true;

    if( IsMatch == true )
    {
        totalSize_ += Item.Size;
        fileCount_ += 1;

        vecItems_.emplace_back( Item );
        if( Parent.length() == baseLength )
            vecItemMiddle_.emplace_back( "\\" );
        else
            vecItemMiddle_.emplace_back( Parent.mid( baseLength ) + "\\" );

        emit notifyStats( vecItemMiddle_.last() + Item.Name, false, totalSize_, fileCount_, dirCount_ );
    }
}
