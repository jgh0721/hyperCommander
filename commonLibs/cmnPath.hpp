#ifndef __HDR_CMN_PATH__
#define __HDR_CMN_PATH__

#include <string>

#include "cmnBase.hpp"

namespace nsCmn
{
    namespace nsCmnPath
    {
        int64_t                         CalculateDirSize( const std::wstring& Path );
#if defined(USE_QT_SUPPORT)
        QString                         GetFileExtension( const QString& FileName );
#endif

#if defined(USE_QT_SUPPORT)
        // 지정한 경로가 SymLink, SymLinkD, Junction 일 경우 링크에 기록된 대상을 반환한다. 그 밖에는 빈 문자열
        QString                         GetReparsePointTo( const QString& Path );
        bool                            IsContainPathSeps( const QString& Path );
#endif
        TyOsValue< uint64_t >           GetFreeDiskSpace( const std::wstring& Directory );

        TyOsValue< int64_t >            GetFileSize( HANDLE hFile );
        TyOsValue< int64_t >            GetFileSize( const std::wstring& FilePath );

        TyOsValue< uint32_t >           GetHardLinkCount( HANDLE hFile );

        struct TyReparseLink
        {
            enum TyEnKind { REPARSE_LINK_OTHER, REPARSE_LINK_SYMLINK, REPARSE_LINK_JUNCTION };

            TyEnKind                    Kind = REPARSE_LINK_OTHER;
            uint32_t                    Tag = 0;
            bool                        Relative = false;   // SYMLINK_FLAG_RELATIVE
            std::wstring                Target;             // 표시 이름. 없으면 \??\ 를 걷어낸 대체 이름
        };

        // FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS 로 연 핸들의 재분석 지점을 읽는다.
        // 심볼릭 링크와 접합 지점 밖의 태그는 Kind 가 OTHER 이고 Target 이 비어 있다.
        // 대상은 링크에 기록된 그대로다. 상대 심볼릭 링크를 풀거나 대상의 존재를 확인하지 않는다.
        TyOsValue< TyReparseLink >      ReadReparseLink( HANDLE hFile );

        TyOsValue< bool >               IsDirExists( const std::wstring& DirPath );
        TyOsValue< bool >               IsFileExists( const std::wstring& FilePath );
        TyOsValue< bool >               CopyACL( const std::wstring& Src, const std::wstring& Dst, SECURITY_INFORMATION Si );

    } // nsCmnPath
} // nsCmn

#endif
