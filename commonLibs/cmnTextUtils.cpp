#include "stdafx.h"
#include "cmnTextUtils.hpp"

namespace nsCmn
{
    int stricmp( const char* lsh, const char* rsh )
    {
        return _stricmp( lsh, rsh );
    }

    int stricmp( const wchar_t* lsh, const wchar_t* rsh )
    {
        return _wcsicmp( lsh, rsh );
    }

    int stricmp( const std::string& lhs, const std::string& rhs )
    {
        return _stricmp( lhs.c_str(), rhs.c_str() );
    }

    int stricmp( const std::wstring& lhs, const std::wstring& rhs )
    {
        return _wcsicmp( lhs.c_str(), rhs.c_str() );
    }
} // nsCmn