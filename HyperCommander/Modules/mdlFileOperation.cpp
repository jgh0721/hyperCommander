#include "stdafx.h"
#include "mdlFileOperation.hpp"

////////////////////////////////////////////////////////////////////////////////
///

void CFileOperation::SetMode( TyEnOperation Mode )
{
    mode_ = Mode;
}

void CFileOperation::SetSource( const QVector<Node>& Src, const QVector<QString>& SrcFullPath )
{
    vecSrc_ = Src;
    vecSrcFullPath_ = SrcFullPath;
}

void CFileOperation::run()
{
    Q_ASSERT( vecSrc_.size() == vecSrcFullPath_.size() );

    if( isInterruptionRequested() == true )
        return;

    switch( mode_ )
    {
        case FILE_OP_COPY: workCopy();
        case FILE_OP_MOVE: workMove();
        case FILE_OP_DELETE: workDelete();
    }

    /*!
        1. COPY / MOVE 일 경우에는 대상 디렉토리에 해당 파일이 존재하는지 확인
        2. 존재하는가? 
            2.1 기존에 옵션이 설정되었다면 옵션대로 행동
            2.2 옵션이 설정되어 있지 않다면 알림창 표시
     */

    // QT 는 mainThread 가 아니면 UI 를 생성하는 것을 금지한다. QThread 에서 UI 를 생성하려면 어떻게 해야 하는가?

    // 1. 해당 스레드가 시작될 때 스레드를 생성한 UI 를 저장하고, UI 에게 알림창을 표시하도록 요청한다.
    // 2. 별도 인터페이스를 구축하여 qApp 에 UI 를 표시하도록 요청한 후, 처리 결과를 전달받는다. 
}

void CFileOperation::workCopy()
{
    for( int idx = 0; idx < vecSrc_.size(); ++idx )
    {
        const auto Src = vecSrcFullPath_[ idx ];
        const auto SrcInfo = vecSrc_[ idx ];
        auto Dst = retrieveDstItem( dst_, SrcInfo.Name );

        emit NotifyChangedItem( Src, Dst );

        if( FlagOn( SrcInfo.Attiributes, FILE_ATTRIBUTE_DIRECTORY ) )
        {
            // 디렉토리는 이미 존재한다면 그대로 진행한다.  
        }
        else
        {
            // 파일이 존재하는지 확인한 후, 존재한다면 이에 대해 처리한다. 
        }
    }
}

void CFileOperation::workMove()
{
}

void CFileOperation::workDelete()
{
}

QString CFileOperation::retrieveDstItem( const QString& Dst, const QString& Name )
{
    return "";
}
