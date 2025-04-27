#include "stdafx.h"
#include "cmnHCModelUtils.hpp"

#include "builtInFsModel.hpp"

DECLARE_CMNLIBSV2_NAMESPACE

////////////////////////////////////////////////////////////////////////////////
///

void QTraverseModel::Run( const QVector< QModelIndex >& Src )
{
    const auto Model = const_cast< CFSModel* >( qobject_cast< const CFSModel* >( Src[ 0 ].model() ) );
    // Model->GetRoot();

    auto Parent = Model->GetCurrentPathWithRoot();
    if( Parent.endsWith( "\\" ) )
        Parent = Parent.left( Parent.length() - 1 );

    for( int idx = 0; idx < Src.size(); ++idx )
    {
        if( QThread::currentThread()->isInterruptionRequested() == true )
            break;

        const auto& FileInfo = Model->GetFileInfo( Src[ idx ] );
        if( FlagOn( FileInfo->Attributes_, FILE_ATTRIBUTE_DIRECTORY ) )
        {
            traverseSubDirectory( Model->GetFileFullPath( Src[ idx ] ), FileInfo );
        }
        else
        {
            processFile( Parent, FileInfo );
        }
    }

    emit notifyResultReady();
}

void QTraverseModel::traverseSubDirectory( const QString& Path, const nsHC::TySpFileSource& Item )
{
}

void QTraverseModel::processFile( const QString& Parent, const nsHC::TySpFileSource& Item )
{
}

//
//bool QTraverseSrcModel::IsFinished() const
//{
//    return isFinished_;
//}
//
//void QTraverseSrcModel::run()
//{
//    emit notifyStats( "", false, 0, 0, 0 );
//
//    if( src.isEmpty() == false )
//        model_ = const_cast< CFSModel* >( qobject_cast< const CFSModel* >( src[ 0 ].model() ) );
//
//    if( filter.isEmpty() == false )
//        regexp = QRegularExpression( QRegularExpression::wildcardToRegularExpression( filter ) );
//
//    auto Parent = model_->GetCurrentPathWithRoot();
//    if( Parent.endsWith( "\\" ) )
//        Parent = Parent.left( Parent.length() - 1 );
//
//    base = Parent;
//    baseLength = base.length();
//
//    for( int idx = 0; idx < src.size(); ++idx )
//    {
//        if( isInterruptionRequested() == true )
//            break;
//
//        const auto& Node = model_->GetFileInfo( src[ idx ] );
//        if( FlagOn( Node->Attributes_, FILE_ATTRIBUTE_DIRECTORY ) )
//        {
//            traverseSubDirectory( model_->GetFileFullPath( src[ idx ] ), Node );
//        }
//        else
//        {
//            processFile( Parent, Node );
//        }
//    }
//
//    isFinished_ = true;
//    emit notifyResultReady();
//}
//
//void QTraverseSrcModel::traverseSubDirectory( const QString& Path, const nsHC::TySpFileSource& Item )
//{
//    QStack< QPair< QString, nsHC::TySpFileSource > > Stack;
//    Stack.push_back( qMakePair( Path, Item ) );
//
//    while( Stack.isEmpty() == false )
//    {
//        const auto P = Stack.pop();
//
//        dirCount_ += 1;
//        vecItems_.push_back( P.second );
//        vecItemMiddle_.emplace_back( P.first.mid( baseLength ) + "\\" );
//
//        emit notifyStats( vecItemMiddle_.last(), true, totalSize_, fileCount_, dirCount_);
//
//        if( isRecursive == false )
//            continue;
//
//        for( const auto& Child : model_->GetChildItems( P.first ) )
//        {
//            if( isInterruptionRequested() == true )
//                break;
//
//            if( FlagOn( Child->Attributes_, FILE_ATTRIBUTE_DIRECTORY ) )
//            {
//                Stack.emplace_back( qMakePair( P.first + "\\" + Child->Name_, Child ) );
//            }
//            else
//            {
//                processFile( P.first, Child );
//            }
//        }
//    }
//}
//
//void QTraverseSrcModel::processFile( const QString& Parent, const nsHC::TySpFileSource& Item )
//{
//    bool IsMatch = false;
//
//    if( filter.isEmpty() == false && regexp.isValid() == true )
//        IsMatch = regexp.match( Item->Name_ ).hasMatch();
//    else
//        IsMatch = true;
//
//    if( IsMatch == true )
//    {
//        totalSize_ += Item->Size_;
//        fileCount_ += 1;
//
//        vecItems_.emplace_back( Item );
//        if( Parent.length() == baseLength )
//            vecItemMiddle_.emplace_back( "\\" );
//        else
//            vecItemMiddle_.emplace_back( Parent.mid( baseLength ) + "\\" );
//
//        emit notifyStats( vecItemMiddle_.last() + Item->Name_, false, totalSize_, fileCount_, dirCount_ );
//    }
//}
