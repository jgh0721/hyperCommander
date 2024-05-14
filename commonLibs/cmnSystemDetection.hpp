#ifndef __HDR_CMN_SYSTEM_DETECTION__
#define __HDR_CMN_SYSTEM_DETECTION__

/*!
  *
  * ref : Qt Source Code
  */

#if defined(__APPLE__)
#   include <TargetConditionals.h>
#   if defined(TARGET_OS_MAC) && TARGET_OS_MAC
#       define CMN_OS_DARWIN
#       define CMN_OS_BSD4
#       ifdef __LP64__
#           define CMN_OS_DARWIN64
#       else
#           define CMN_OS_DARWIN32
#       endif
#       if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
#           define QT_PLATFORM_UIKIT
#           if defined(TARGET_OS_WATCH) && TARGET_OS_WATCH
#               define CMN_OS_WATCHOS
#           elif defined(TARGET_OS_TV) && TARGET_OS_TV
#               define CMN_OS_TVOS
#           else
#               // TARGET_OS_IOS is only available in newer SDKs,
#               // so assume any other iOS-based platform is iOS for now
#               define CMN_OS_IOS
#           endif
#       else
#           // TARGET_OS_OSX is only available in newer SDKs,
#           // so assume any non iOS-based platform is macOS for now
#           define CMN_OS_MACOS
#       endif
#   else
#   endif
#elif defined(__CYGWIN__)
#   define CMN_OS_CYGWIN
#elif !defined(SAG_COM) && (!defined(WINAPI_FAMILY) || WINAPI_FAMILY==WINAPI_FAMILY_DESKTOP_APP) && (defined(WIN64) || defined(_WIN64) || defined(__WIN64__))
#    define CMN_OS_WIN32
#    define CMN_OS_WIN64
#elif !defined(SAG_COM) && (defined(WIN32) || defined(_WIN32) || defined(__WIN32__) || defined(__NT__))
#    if defined(WINAPI_FAMILY)
#       ifndef WINAPI_FAMILY_PC_APP
#           define WINAPI_FAMILY_PC_APP WINAPI_FAMILY_APP
#       endif
#       if defined(WINAPI_FAMILY_PHONE_APP) && WINAPI_FAMILY==WINAPI_FAMILY_PHONE_APP
#           define CMN_OS_WINPHONE
#           define CMN_OS_WINRT
#       elif WINAPI_FAMILY==WINAPI_FAMILY_PC_APP
#           define CMN_OS_WINRT
#       else
#           define CMN_OS_WIN32
#       endif
#   else
#       define CMN_OS_WIN32
#   endif
#elif defined(__linux__) || defined(__linux)
#   define CMN_OS_LINUX
#else
#   error "Not Supported OS"
#endif

#if defined(CMN_OS_WIN32) || defined(CMN_OS_WIN64) || defined(CMN_OS_WINRT)
#  define CMN_OS_WIN
#endif

#if defined(__linux__)
#   define CMN_OS_LINUX
#   ifndef linux
#       define linux
#   endif
#   ifndef __linux
#       define __linux
#   endif
#endif

// Compatibility synonyms
#ifdef CMN_OS_DARWIN
#define CMN_OS_MAC
#endif
#ifdef CMN_OS_DARWIN32
#define CMN_OS_MAC32
#endif
#ifdef CMN_OS_DARWIN64
#define CMN_OS_MAC64
#endif
#ifdef CMN_OS_MACOS
#define CMN_OS_MACX
#define CMN_OS_OSX
#define CMN_OS_MAC
#endif

#ifdef CMN_OS_DARWIN
#   include <Availability.h>
#   include <AvailabilityMacros.h>
#endif

#endif // __HDR_CMN_SYSTEM_DETECTION__
