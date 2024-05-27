#pragma once

#include <string>

namespace nsCmn
{
    namespace nsCmnPath
    {
        int64_t                         CalculateDirSize( const std::wstring& Path );
        QString                         GetFileExtension( const QString& FileName );
    } // nsCmnPath
} // nsCmn
