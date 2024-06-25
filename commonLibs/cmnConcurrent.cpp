#include "stdafx.h"
#include "cmnConcurrent.hpp"

#if defined(USE_OS_NATIVE_IMPLEMENTATION) 
    #if defined(CMN_OS_WIN)
    #else
    #endif
#else
    #if defined(USE_QT_SUPPORT)
        #include <QtCore>
    #else
    #endif
#endif

#if defined(_MSC_VER)
#   pragma execution_character_set( "utf-8" )
#endif

namespace nsCmn
{
    namespace nsConcurrent
    {

    } // nsConcurrent

} // nsCmn
