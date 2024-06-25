#ifndef __HDR_CMN_TYPE__
#define __HDR_CMN_TYPE__

#include "cmnBase.hpp"
#include "cmnSystemDetection.hpp"
#include "cmnCompilerDetection.hpp"

#if defined(CMN_OS_WIN)
    #define CMN_EOL_A "\r\n"
    #define CMN_EOL_W L"\r\n"
#else
    #define CMN_EOL_A "\n"
    #define CMN_EOL_W L"\n"
#endif

#if CMN_MSC_VER >= _MSVC2017_VER
#if _MSVC_LANG >= 201703L
namespace std
{
    template<class arg, class result>
    struct unary_function
    {
        typedef arg argument_type;
        typedef result result_type;
    };

    template<class arg1, class arg2, class result>
    struct binary_function
    {
        typedef arg1 first_argument_type;
        typedef arg2 second_argument_type;
        typedef result result_type;
    };

    template <typename T>
    struct return_type : return_type<decltype( &T::operator() )>
    {};
    // For generic types, directly use the result of the signature of its 'operator()'

    template <typename ClassType, typename ReturnType, typename... Args>
    struct return_type<ReturnType( ClassType::* )( Args... ) const>
    {
        using type = ReturnType;
    };
}
#endif
#endif

#endif // __HDR_CMN_TYPE__