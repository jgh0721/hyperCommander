#pragma once

#include "cmnConfig.hpp"
#include "cmnSystemDetection.hpp"
#include "cmnCompilerDetection.hpp"

#include <optional>
#include <vector>
#include <string>
#include <cstdint>

#define CMN_MAKE_STR2(x) #x
#define CMN_MAKE_STR(x) CMN_MAKE_STR2(x)

#define CMN_MAKE_CONCAT_L2(x) L ## x
#define CMN_MAKE_CONCAT_L(x) CMN_MAKE_CONCAT_L2(x)

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

#define __FILENAME__ (strrchr(__FILE__, '/') ? strrchr(__FILE__, '/') + 1 : strrchr(__FILE__, '\\') ? strrchr(__FILE__, '\\') + 1 : __FILE__)
#define __FILENAMEW__ (wcsrchr(__FILEW__, L'/') ? wcsrchr(__FILEW__, L'/') + 1 : wcsrchr(__FILEW__, L'\\') ? wcsrchr(__FILEW__, L'\\') + 1 : __FILEW__)

#define __FUNCNAME__  ( strrchr(__FUNCTION__, ':' ) ? strrchr( __FUNCTION__, ':' ) + 1 : __FUNCTION__ )
#define __FUNCNAMEW__ ( wcsrchr(__FUNCTIONW__, L':' ) ? wcsrchr( __FUNCTIONW__, L':' ) + 1 : __FUNCTIONW__ )

#define MAKEINT64(low, high) ((((int64_t)(high))<<32)|((int64_t)low))

#define ONE_SECOND_MS 1000

struct TyOsError
{
    enum TyEnOsError { OS_WIN32_ERROR, OS_NT_ERROR, OS_COM_ERROR };
    TyEnOsError Type = OS_WIN32_ERROR;
    uint32_t    ErrorCode = ERROR_SUCCESS;
};

template< typename Ty >
struct TyOsValue
{
    std::optional< Ty > Value;
    TyOsError           ErrorCode;
};

#define MAKE_WIN32_VALUE( Value ) { Value, {} }
#define MAKE_WIN32_LAST_ERROR { std::nullopt, { TyOsError::OS_WIN32_ERROR, ::GetLastError() } }

inline bool IsOsError( const TyOsError& Error )
{
    if( Error.Type == TyOsError::OS_WIN32_ERROR )
        return Error.ErrorCode != ERROR_SUCCESS;

    if( Error.Type == TyOsError::OS_NT_ERROR )
        return static_cast< LONG >( Error.ErrorCode ) < 0;

    if( Error.Type == TyOsError::OS_COM_ERROR )
        return static_cast< HRESULT >( Error.ErrorCode ) < 0;
    
    return false;
}