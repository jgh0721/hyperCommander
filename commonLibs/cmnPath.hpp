#pragma once

#include <string>

namespace nsCmn
{
    namespace nsCmnPath
    {
        int64_t                         CalculateDirSize( const std::wstring& Path );
        QString                         GetFileExtension( const QString& FileName );

        // 지정한 경로가 SymLink, SymLinkD, Juntion 일 경우 실제 파일/디렉토리를 반환
        QString                         GetReparsePointTo( const QString& Path );

    } // nsCmnPath
} // nsCmn
