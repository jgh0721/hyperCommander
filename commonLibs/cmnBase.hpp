#pragma once

#include "cmnSystemDetection.hpp"
#include "cmnCompilerDetection.hpp"

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
