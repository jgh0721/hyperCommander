#pragma once

#include <string>

#include "cmnBase.hpp"

namespace nsCmn
{
    namespace nsCmnPath
    {
        int64_t                         CalculateDirSize( const std::wstring& Path );
        QString                         GetFileExtension( const QString& FileName );

        // 지정한 경로가 SymLink, SymLinkD, Juntion 일 경우 실제 파일/디렉토리를 반환
        QString                         GetReparsePointTo( const QString& Path );

        TyOsValue< int64_t >            GetFileSize( HANDLE hFile );
        TyOsValue< int64_t >            GetFileSize( const std::wstring& FilePath );

        TyOsValue< uint32_t >           GetHardLinkCount( HANDLE hFile );

        TyOsValue< bool >               IsFileExists( const std::wstring& FilePath );
        TyOsValue< bool >               CopyACL( const std::wstring& Src, const std::wstring& Dst, SECURITY_INFORMATION Si );

    } // nsCmnPath
} // nsCmn
