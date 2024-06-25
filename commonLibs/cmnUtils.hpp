#ifndef __HDR_CMN_UTILS__
#define __HDR_CMN_UTILS__

#include <set>
#include <map>
#include <string>

#include "cmnTypes.hpp"
#include "cmnTextUtils.hpp"

#ifndef FlagOn
    #define FlagOn(_F,_SF)        ((_F) & (_SF))
#endif

#ifndef BooleanFlagOn
    #define BooleanFlagOn(F,SF)   ((BOOLEAN)(((F) & (SF)) != 0))
#endif

#ifndef SetFlag
    #define SetFlag(_F,_SF)       ((_F) |= (_SF))
#endif

#ifndef ClearFlag
    #define ClearFlag(_F,_SF)     ((_F) &= ~(_SF))
#endif

#ifndef Add2Ptr
    #define Add2Ptr(P,I) ((PVOID)((PUCHAR)(P) + (I)))
#endif

namespace nsCmn
{
    namespace nsDetail
    {

    } // nsDetail

    class eq_nocaseW
        : public std::binary_function < const std::wstring, const std::wstring, bool >
    {
    public:
        bool operator()( const std::wstring& lsh, const std::wstring& rsh ) const
        {
            return _wcsicmp( lsh.c_str(), rsh.c_str() ) == 0;
        }
    };

    class eq_nocaseA
        : public std::binary_function < const std::string, const std::string, bool >
    {
    public:
        bool operator()( const std::string& lsh, const std::string& rsh ) const
        {
            return _stricmp( lsh.c_str(), rsh.c_str() ) == 0;
        }
    };

    class lt_nocaseW
        : public std::binary_function < const std::wstring, const std::wstring, bool >
    {
    public:
        bool operator()( const std::wstring& lsh, const std::wstring& rsh ) const
        {
            return _wcsicmp( lsh.c_str(), rsh.c_str() ) < 0;
        }
    };

#if defined(USE_QT_SUPPORT)
    class lt_nocaseQW
        : public std::binary_function < const QString, const QString, bool >
    {
    public:
        bool operator()( const QString& lsh, const QString& rsh ) const
        {
            return lsh.compare( rsh, Qt::CaseInsensitive ) < 0;
        }
    };
#endif

    class lt_nocaseA
        : public std::binary_function < char*, char*, bool >
    {
    public:
        bool operator()( std::string lsh, std::string rsh ) const
        {
            return _stricmp( lsh.c_str(), rsh.c_str() ) < 0;
        }
    };
    
    template< typename Char >
    class lt_nocase
        : public std::binary_function< std::basic_string< Char >, std::basic_string< Char >, bool >
    {
    public:
        bool operator()( const std::basic_string< Char >& lsh, const std::basic_string< Char >& rsh ) const
        {
            return nsCmn::stricmp( lsh, rsh ) < 0;
        }
    };
} // nsCmn

#endif // __HDR_CMN_UTILS__