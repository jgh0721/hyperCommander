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

TyOsError CFileOperation::ResultCode() const
{
    return resultCode_;
}

void CFileOperation::SetDeleteIsDelHiddenSys( bool IsDelHiddenSys )
{
    isDelHiddenSys_ = IsDelHiddenSys;
}

void CFileOperation::SetDeleteIsDelReadOnly( bool IsDelReadOnly )
{
    isDelReadonly_ = IsDelReadOnly;
}

void CFileOperation::SetDeleteIsUseRecycleBin( bool IsUseRecycleBin )
{
    isUseRecycleBin = IsUseRecycleBin;
}

void CFileOperation::SetDeleteIsUseAutoAdminRights( bool IsUseAutoAdminRights )
{
    isUseAutoAdminRights = IsUseAutoAdminRights;
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
    const qsizetype totalCount = vecSrc_.size();

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
/*!
    MoveFile 은 CopyFile 과 달리 원본 또는 대상이 디렉토리이면 여러 가지 제약이 걸린다.
    따라서, 이를 구분하여 직접 복사/삭제 동작을 수행해야 할 수 있다.

    Src 와 Dst 의 드라이브(루트) 가 같은지 확인한다.
        UNC 경로는 서버 이름까지 확인한다.  

    Src 가 디렉토리이고 루트가 다르면 직접 복사/삭제를 수행해야 한다.

 */


}

void CFileOperation::workDelete()
{
    qint32 itemProgress = 0;
    qint32 itemCount = 0;
    qint32 totalProgress = 0;
    qint64 totalSize = 0;
    const qsizetype totalCount = vecSrc_.size();

    bool IsAlwaysYes_ReadOnly = isDelReadonly_;
    bool IsAlwaysYes_HiddenSys = isDelHiddenSys_;

    SHFILEOPSTRUCTW SHFileOP = { 0, };
    RtlZeroMemory( &SHFileOP, sizeof( SHFileOP ) );
    SHFileOP.hwnd = nullptr;
    SHFileOP.wFunc = FO_DELETE;
    SHFileOP.fFlags |= FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_NO_UI | FOF_SILENT;

    SHFileOperationW( &SHFileOP );

    // 파일 및 디렉토리를 삭제할 때에는 디렉토리가 비어 있어야 삭제가 가능하다.
    // 그러므로, 벡터를 역순으로 순회하며 가장 깊은 곳의 파일/디렉토리부터 순차적으로 삭제한다.
    for( int idx = vecSrc_.size() - 1; idx >= 0; --idx )
    {
        const auto Src      = vecSrcMiddle_[ idx ];
        const auto SrcInfo  = vecSrc_[ idx ];
        auto Dst = retrieveDstItem( base_, Src, FlagOn( SrcInfo.Attiributes, FILE_ATTRIBUTE_DIRECTORY ) ? "" : SrcInfo.Name );
        const auto DstW = Dst.contains( ":" ) == true && isUseRecycleBin == false ? ( R"(\\?\)" + Dst ).toStdWString() : Dst.toStdWString();

        if( waitRunningState() == false )
            break;

        if( FlagOn( SrcInfo.Attiributes, FILE_ATTRIBUTE_HIDDEN ) ||
            FlagOn( SrcInfo.Attiributes, FILE_ATTRIBUTE_SYSTEM ) )
        {
            if( isDelHiddenSys_ == false || IsAlwaysYes_HiddenSys == false )
            {
                // 숨김 파일 삭제를 체크하지 않았거나, 한 번 사용자에게 알린 후, 사용자가 모두 삭제를 선택하지 않았다면 알린다. 
            }
        }

        if( FlagOn( SrcInfo.Attiributes, FILE_ATTRIBUTE_READONLY ) )
        {
            if( isDelReadonly_ == false || IsAlwaysYes_ReadOnly == false )
            {
                // 읽기전용 파일 삭제를 체크하지 않았거나, 한 번 사용자에게 알린 후, 사용자가 모두 삭제를 선택하지 않았다면 알린다. 
            }
        }

        emit NotifyChangedItem( Dst, "" );
        emit NotifyChangedProgress( 0, totalProgress );
        emit NotifyChangedStatus( ++itemCount, totalSize );

        if( !FlagOn( SrcInfo.Attiributes, FILE_ATTRIBUTE_DIRECTORY ) )
        {
            do
            {
                SetFileAttributesW( DstW.c_str(), FILE_ATTRIBUTE_NORMAL );
                const BOOL bRet = DeleteFileW( Dst.toStdWString().c_str() );
                if( bRet != FALSE )
                    break;

                const auto ErrorCode = ::GetLastError();
                if( ErrorCode == ERROR_FILE_NOT_FOUND )     // 성공으로 간주
                    break;

                // 삭제가 실패했으므로, 속성을 원상 복구한다. 
                SetFileAttributesW( DstW.c_str(), SrcInfo.Attiributes );
                if( ErrorCode == ERROR_ACCESS_DENIED )
                {
                    if( isUseAutoAdminRights == true )
                    {
                        // 관리자 권한으로 시도 후, 실패하면 아래로 이동하여 오류 창 표시
                        // 성공하면 break
                    }
                }

                // 오류창 통지

            } while( false );
        }
        else
        {
            // 디렉토리를 삭제하려하는데 오류가 NOT_EMPTY 가 반환됨. 하지만, isDelHiddenSys_ 가 false 이면 오류창을 표시하지 않고 무시.
            const BOOL bRet = RemoveDirectoryW( Dst.toStdWString().c_str() );
            if( bRet == FALSE )
            {
                const auto ErrorCode = ::GetLastError();
                if( ( ErrorCode != ERROR_DIR_NOT_EMPTY ) ||
                    ( ErrorCode == ERROR_DIR_NOT_EMPTY && isDelHiddenSys_ == true ) )
                {
                    // 오류 통지 후 수행
                }
                else
                {
                    // 무시
                }

                if( isDelHiddenSys_ == true )
                {

                }
            }
        }

        // (전체크기 - 지금까지 전송크기) * 현재 소요시간 / 지금까지의 전송크기 = 앞으로의 잔여시간
        // https://ko.wikihow.com/%EB%8D%B0%EC%9D%B4%ED%84%B0-%EC%A0%84%EC%86%A1-%EC%86%8D%EB%8F%84%EB%A5%BC-%EA%B3%84%EC%82%B0%ED%95%98%EB%8A%94-%EB%B0%A9%EB%B2%95
        // 전체 백분율 계산
        totalProgress = ( static_cast< double >( itemCount ) / static_cast< double >( totalCount ) ) * 100.0;
        emit NotifyChangedProgress( 100, totalProgress );

        totalSize += SrcInfo.Size;
        emit NotifyChangedStatus( itemCount, totalSize );
    }
}

bool CFileOperation::isErrorOccuredConnected() const
{
    static auto Method = QMetaMethod::fromSignal( &CFileOperation::NotifyChangedProgress );
    return isSignalConnected( Method );
}

QString CFileOperation::retrieveDstItem( const QString& Dst, const QString& Middle, const QString& Name )
{
    if( Name.isEmpty() == false )
        return Dst + Middle + Name;

    return Dst + Middle.left( Middle.length() - 1 );
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
