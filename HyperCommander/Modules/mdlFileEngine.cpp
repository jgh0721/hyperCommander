#include "stdafx.h"
#include "mdlFileEngine.hpp"

DECLARE_CMNLIBSV2_NAMESPACE

///////////////////////////////////////////////////////////////////////////////

namespace nsHC
{
    bool CFileEngine::ExecuteTask( const TySpFETaskItem& Task, TyOsValue<TyEnFEResult>& Result )
    {
        Result.Value = FE_RESULT_FAILED;

        switch( Task->Type )
        {
            case FE_MODE_MKDIR: {
                processMkDir( Task, Result );
            } break;
            case FE_MODE_RMDIR: {
                processRmDir( Task, Result );
            } break;
        }

        return Result.Value == FE_RESULT_SUCCEED;
    }

    void CFileEngine::processMkDir( const TySpFETaskItem& Task, TyOsValue<TyEnFEResult>& Result )
    {
        Q_ASSERT( Task->Src != nullptr );
        Q_ASSERT( Task->Src->Fs != nullptr );
        Q_ASSERT( Task->Src->Items.size() == 1 );
        Q_ASSERT( Task->Input.isEmpty() == false );

        TyEnFEState Curr = FE_STATE_NOT_STARTED;
        TyEnFEState Prev = FE_STATE_NOT_STARTED;

        do
        {
            Prev = Curr;
            Curr = FE_STATE_STARTING;
            if( notifyChangeState( Task, Curr, Prev ) == 1 )
            {
                Result.Value = FE_RESULT_CANCELED;
                break;
            }

            bool IsExistSrc = false;
            const auto Src = QString( "%1%2" ).arg( Task->Src->Fs->GetRoot(), Task->Src->Items[ 0 ]->Name_ );
            auto IsRoot = QDir( Src ).isRoot();

            auto Dst = Task->Input;

            switch( Task->Src->Fs->GetCate() )
            {
                case FS_CATE_LOCAL:
                {
                    IsExistSrc = IsDirExists( Src.toStdWString() ).Value.value_or( false );
                } break;
                case FS_CATE_REMOTE:
                {
                    IsRoot = false;

                    if( Src.startsWith( R"(\\)" ) == false || Src.size() <= 2 )
                    {
                        IsExistSrc = false;
                    }
                    else if( Src.indexOf( '\\', 2 ) < 0 )
                    {
                        IsExistSrc = false;
                    }
                    else
                    {
                        IsExistSrc = QDir( Src ).exists();
                    }

                } break;
                default:
                {
                    Q_ASSERT( false );
                } break;
            }

            if( IsExistSrc == false )
            {
                if( Task->pfnErrorOccured != nullptr )
                    Task->pfnErrorOccured( Task->Context, tr( "" ), tr( "" ), Result.ErrorCode );

                break;
            }

            Prev = Curr;
            Curr = FE_STATE_STARTED;
            if( notifyChangeState( Task, Curr, Prev ) == 1 )
            {
                Result.Value = FE_RESULT_CANCELED;
                break;
            }

            const auto FullPath = QString( "%1%2%3" ).arg( Src ).arg( IsRoot ? "" : "\\" ).arg( Dst ).toStdWString();

            Result.ErrorCode.Type = TyOsError::OS_WIN32_ERROR;
            Result.ErrorCode.ErrorCode = SHCreateDirectoryExW( nullptr, FullPath.c_str(), nullptr );
            Result.Value = Result.ErrorCode.ErrorCode == ERROR_SUCCESS ? FE_RESULT_SUCCEED : FE_RESULT_FAILED;

            // 디렉토리가 존재한다면 성공으로 처리한다. 
            if( Result.ErrorCode.ErrorCode == ERROR_FILE_EXISTS ||
                Result.ErrorCode.ErrorCode == ERROR_ALREADY_EXISTS )
            {
                Result.Value = FE_RESULT_SUCCEED;
            }

            if( Result.Value != FE_RESULT_SUCCEED )
            {
                if( Task->pfnErrorOccured != nullptr )
                {
                    Task->pfnErrorOccured( Task->Context, tr( "" ), tr( "" ), Result.ErrorCode );
                }
            }

            Prev = Curr;
            Curr = FE_STATE_STOPPED;
            notifyChangeState( Task, Curr, Prev );

            //Task->Src->Fs->GetCate() == FS_CATE_VFS;
            //Task->Src->Fs->GetCate() == FS_CATE_VIRUAL;

        } while( false );
    }

    void CFileEngine::processRmDir( const TySpFETaskItem& Task, TyOsValue<TyEnFEResult>& Result )
    {
        Q_ASSERT( Task->Src != nullptr );
        Q_ASSERT( Task->Src->Fs != nullptr );

        TyEnFEState Curr = FE_STATE_NOT_STARTED;
        TyEnFEState Prev = FE_STATE_NOT_STARTED;

        do
        {
            Prev = Curr;
            Curr = FE_STATE_STARTING;
            if( notifyChangeState( Task, Curr, Prev ) == 1 )
            {
                Result.Value = FE_RESULT_CANCELED;
                break;
            }

            // Task->Src->Fs;
            Task->Src->Items;


        } while( false );
    }

    int CFileEngine::notifyChangeState( const TySpFETaskItem& Task, TyEnFEState Curr, TyEnFEState Prev )
    {
        if( Task->pfnChangeState != nullptr )
            return Task->pfnChangeState( Task->Context, Curr, Prev );

        return 0;
    }
}
