#ifndef __HDR_CMN_OS_ABSTACTION_LAYER_DEFS__
#define __HDR_CMN_OS_ABSTACTION_LAYER_DEFS__

#include "cmnSystemDetection.hpp"

#if defined(_MSC_VER)
#   pragma execution_character_set( "utf-8" )
#endif

namespace nsCmn
{
    namespace nsOAL
    {
        namespace nsDetail
        {
        } // nsDetail

        typedef enum
        {
            OS_TYPE_UNKNOWN = 0

            // 리눅스
            , OS_TYPE_LINUX

            // MAC
            , OS_TYPE_MAC

            // WINDOWS
            , OS_TYPE_WINDOWS

        } TyEnOSType;

        //////////////////////////////////////////////////////////////////////////
        /// Files and Directories

        namespace nsDetail
        {
#if defined(CMN_OS_WIN)
            const wchar_t nativeSep = L'\\';
#else
            const wchar_t nativeSep = L'/';
#endif
            const wchar_t win32Sep = L'\\';
            const wchar_t commonSep = L'/';

        } // nsDetail

        // 현재 디렉토리를 구한다. SHELL 지원을 활성화하지 않으면 _getcwd 함수를 사용하여 현재 디렉토리를 반환한다
        // 항상 \\ 붙인다. 

        template< typename CHAR >
        const CHAR*                     GetNativePathSep();

        template<>
        inline const char*              GetNativePathSep()
        {
#if defined(CMN_OS_WIN)
            return "\\";
#else
            return "/";
#endif
        }

        template<>
        inline const wchar_t*           GetNativePathSep()
        {
#if defined(CMN_OS_WIN)
            return L"\\";
#else
            return L"/";
#endif
        }


        //////////////////////////////////////////////////////////////////////////
        /// Process, Thread

#if defined(CMN_OS_WIN)
        using oalPID = DWORD;
        using oalTID = DWORD;
#else
    #if defined(CMN_OS_LINUX)
        using oalPID = pid_t;
        using oalTID = pid_t;
    #elif defined(CMN_OS_MAC)
        using oalPID = uint64_t;
        using oalTID = uint64_t;
    #endif
#endif

        namespace nsDetail
        {
#if defined(CMN_OS_WIN)
#pragma pack(push,8)
            typedef struct tagTHREADNAME_INFO
            {
                DWORD dwType; // Must be 0x1000.
                LPCSTR szName; // Pointer to name (in user addr space).
                DWORD dwThreadID; // Thread ID (-1=caller thread).
                DWORD dwFlags; // Reserved for future use, must be zero.
            } THREADNAME_INFO;
#pragma pack(pop)
#endif
        } // nsDetail

        struct OAL_PROCESS_ITEM
        {
            oalPID                          processID;
            oalPID                          sessionID;
            std::wstring                    processName;
            std::wstring                    processPath;
            oalPID                          parentProcessID;
            std::wstring                    processCMDLine;

            OAL_PROCESS_ITEM()
                : processID( 0 )
                , parentProcessID( 0 )
                , sessionID( -1 )
            {
            }

        };

        //////////////////////////////////////////////////////////////////////////
        /// Time

        struct OAL_TIMEVAL
        {
            long tv_sec;        // time_t 와 같음
            long tv_usec;       // 초 이하의 microsecond 
        };

        struct OAL_TIMEZONE
        {
            int  tz_minuteswest; /* minutes W of Greenwich */
            int  tz_dsttime;     /* type of dst correction */
        };

        struct OAL_SYSTEMTIME
        {
            WORD wYear;
            WORD wMonth;
            WORD wDayOfWeek;
            WORD wDay;
            WORD wHour;
            WORD wMinute;
            WORD wSecond;
            WORD wMilliseconds;
        };

        //////////////////////////////////////////////////////////////////////////
        /// Language

#ifndef CMN_MAKE_LANGID
#define CMN_MAKE_LANGID(p, s) ((((WORD  )(s)) << 10) | (WORD  )(p))
#endif

#ifndef CMN_EXTRACT_PRILANGID
#define CMN_EXTRACT_PRILANGID(lgid) ((WORD  )(lgid) & 0x3ff)
#endif

#ifndef CMN_EXTRACT_SUBLANGID
#define CMN_EXTRACT_SUBLANGID(lgid) ((WORD  )(lgid) >> 10)
#endif

    } // nsOAL
} // nsCmn

#endif // __HDR_CMN_OS_ABSTACTION_LAYER_DEFS__