#include "stdafx.h"
#include "mdlFileOperation.hpp"

/*!

클래스와 QT UI 상호작용 방안

1. SIGNAL 에 구조체 포인터를 전달한다.
    구조체 포함 : QVariant(반환값), QEvent



문제점 : 어떠한 선택 사항에 대해 추가 선택사항이 필요할 수 있음.
UI 가 해야할 일

#. 진행상태 표시
#. 오류가 발생했음을 알림
##. 사용자 선택
###. 사용자 선택에 대한 추가 선택
    @ 종류 : 취소(중지), 재시도, 이름변경( 추가 선택 ), 관리자 권한으로, 모두 관리자 권한으로, 해당 파일 무시, 모두 무시

#. 일시중지 / 재개
#. 취소
#. 백그라운드로 전환

 */

class TFileOperationUI
{
    // virtual int ErrorOccured( quint32 ErrorCode, QVector< QString >  );
    
};

////////////////////////////////////////////////////////////////////////////////
///

void CFileOperation::SetMode( TyEnOperation Mode )
{
    QWaitCondition cond;
    
    mode_ = Mode;
}

void CFileOperation::SetBase( const QString& Parent )
{
    base_ = Parent;
}

void CFileOperation::SetSource( const QVector<Node>& Src, const QVector<QString>& SrcMiddle )
{
    vecSrc_ = Src;
    vecSrcMiddle_ = SrcMiddle;
}

void CFileOperation::SetDestination( const QString& Dst )
{
    dst_ = Dst;
    if( dst_.endsWith( "\\" ) )
        dst_ = dst_.left( dst_.length() - 1 );
}

void CFileOperation::ChangeState( int State )
{
    // TODO: 이전 상태를 참고하여 현재 상태 변경
    state_ = static_cast< decltype(state_) >( State );
}

void CFileOperation::run()
{
    Q_ASSERT( state_ == FILE_STATE_NOT_STARTED );
    Q_ASSERT( vecSrc_.size() == vecSrcMiddle_.size() );

    if( mode_ != FILE_OP_DELETE  )
        Q_ASSERT( dst_.isEmpty() == false );

    if( isInterruptionRequested() == true )
        return;

    if( vecSrc_.isEmpty() == true )
        return;

    emit NotifyChangedState( FILE_STATE_RUNNING );

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
    emit NotifyChangedState( FILE_STATE_STOPPING );
    emit NotifyChangedState( FILE_STATE_NOT_STARTED );
}

void CFileOperation::workCopy()
{
    QDir dir;

    qint64 writtenTotalSize = 0;
    qint32 itemProgress = 0;
    qint32 totalProgress = 0;
    qsizetype totalCount = vecSrc_.size();

    for( int idx = 0; idx < vecSrc_.size(); ++idx )
    {
        const auto Src = vecSrcMiddle_[ idx ];
        const auto SrcInfo = vecSrc_[ idx ];
        auto Dst = retrieveDstItem( dst_, Src, FlagOn( SrcInfo.Attiributes, FILE_ATTRIBUTE_DIRECTORY ) ? "" : SrcInfo.Name );

        if( waitRunningState() == false )
            break;

        // 경고창 띄우고 종료
        if( Src == Dst )
            ;

        // 새 항목을 시작하면서 통지 시작
        itemProgress = 0;
        bool IsItemSuccess = false;

        emit NotifyChangedItem( Src, Dst );
        emit NotifyChangedProgress( itemProgress, totalProgress );

        if( FlagOn( SrcInfo.Attiributes, FILE_ATTRIBUTE_DIRECTORY ) )
        {
            if( dir.exists( Dst ) == true )
            {
                IsItemSuccess = true;
            }
            else
            {
                IsItemSuccess = dir.mkpath( Dst );
                if( IsItemSuccess == false )
                {
                    if( isErrorOccuredConnected() == false )
                    {
                        // UI 와 연결되어 있지 않다면 기본값으로 취소를 사용한다. 
                    }
                    else
                    {
                        auto User = new TyUserInteract;
                        TyEnUserInteracts Flags( USER_SELECT_CANCEL| USER_SELECT_RENAME );

                        User->Lock.lock();
                        emit NotifyErrorOccured( ::GetLastError(), Flags, User );
                        while( User->Wait.wait( &User->Lock, 1000 ) == false )
                        {
                            // UI 가 어떠한 오류로 인해 종료되었을 수도 있다. 
                            if( isErrorOccuredConnected() == true )
                                continue;

                            break;
                        }
                    }

                    // TODO: 오류창을 표시해야 한다.
                    // 사용자가 무시를 선택했다면 그대로 진행한다.
                    // 취소를 선택했다면 취소 절차를 시작해야 한다.
                }
            }

            if( IsItemSuccess == true )
            {
                emit NotifyChangedProgress( 100, totalProgress );
                emit NotifyChangedStatus( idx + 1, writtenTotalSize );
            }
        }
        else
        {
            // 파일이 존재하는지 확인한 후, 존재한다면 이에 대해 처리한다. 
            if( QFile::exists( Dst ) == true )
            {
                // TODO: 파일이 존재하므로 어떻게 처리할지 사용자에게 물어야 함. 
            }
            else
            {
                // TODO: CopyFileEx 를 통해 파일을 복사하고 진행상태 표시 
            }
        }

        // (전체크기 - 지금까지 전송크기) * 현재 소요시간 / 지금까지의 전송크기 = 앞으로의 잔여시간
        // https://ko.wikihow.com/%EB%8D%B0%EC%9D%B4%ED%84%B0-%EC%A0%84%EC%86%A1-%EC%86%8D%EB%8F%84%EB%A5%BC-%EA%B3%84%EC%82%B0%ED%95%98%EB%8A%94-%EB%B0%A9%EB%B2%95
        // 전체 백분율 계산
        totalProgress = ( ( double )( idx + 1 ) / ( double )totalCount ) * 100.0;
    }
}

void CFileOperation::workMove()
{
}

void CFileOperation::workDelete()
{
}

bool CFileOperation::isErrorOccuredConnected() const
{
    static auto Method = QMetaMethod::fromSignal( &CFileOperation::NotifyChangedProgress );
    return isSignalConnected( Method );
}

QString CFileOperation::retrieveDstItem( const QString& Dst, const QString& Middle, const QString& Name )
{
    if( Name.isEmpty() == false )
        return Dst + Middle + "\\" + Name;

    return Dst + Middle;
}

bool CFileOperation::waitRunningState()
{
    while( state_ == FILE_STATE_PAUSED )
    {
        if( isInterruptionRequested() == true )
            break;

        const auto dispatcher = eventDispatcher();
        dispatcher->processEvents( QEventLoop::AllEvents );
    }

    return !isInterruptionRequested();
}
