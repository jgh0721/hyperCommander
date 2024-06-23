#include "stdafx.h"
#include "cmnDateTime.hpp"

// epoch time으로 변환할 상수, oalGetTimeOfDay 에서 사용
// microseconds betweeen Jan 1,1601 and Jan 1,1970 
#if defined(_MSC_VER) || defined(_MSC_EXTENSIONS)
    #define DELTA_EPOCH_IN_MICROSECS  11644473600000000Ui64
#else
    #define DELTA_EPOCH_IN_MICROSECS  11644473600000000ULL
#endif

namespace nsCmn
{
#if defined(USE_QT_SUPPORT)
    QDateTime ConvertTo( const FILETIME& Time, bool IsUTC, bool* ok /* = nullptr */ )
    {
        if( ok != nullptr )
            *ok = false;

        ULARGE_INTEGER store;
        QDateTime tempDateTime( QDate( 1601, 1, 1 ), QTime( 0, 0, 0 ), IsUTC == true ? Qt::UTC : Qt::LocalTime );

        store.QuadPart = Time.dwHighDateTime;
        store.QuadPart = store.QuadPart << 32;
        store.QuadPart += Time.dwLowDateTime;

        tempDateTime = tempDateTime.addMSecs( store.QuadPart / 10000 );
        if( ok != nullptr )
            *ok = true;

        return tempDateTime;
    }
#endif

    void GetTimeOfDay( OAL_TIMEVAL* tv, OAL_TIMEZONE* tz )
    {
        // http://alones.kr/gettimeofday%EB%A5%BC-window%EC%97%90%EC%84%9C-%EA%B5%AC%ED%98%84%ED%95%B4%EC%84%9C-%EC%82%AC%EC%9A%A9%ED%95%98%EA%B8%B0/
        // https://msdn.microsoft.com/en-us/library/windows/desktop/dn553408%28v=vs.85%29.aspx?f=255&MSPPError=-2147217396
        FILETIME ft; /* 64-bit value representing the number of 100-nanosecond intervals since January 1, 1601 00:00 UTC */
        unsigned __int64 tmpres = 0;
        static int tzflag;

        if( tv != nullptr )
        {
            // system time을 구하기
            //if( QOperatingSystemVersion::current() >= QOperatingSystemVersion::Windows8 )
            //    GetSystemTimePreciseAsFileTime( &ft );
            //else
                GetSystemTimeAsFileTime( &ft );

            // unsigned 64 bit로 만들기
            tmpres |= ft.dwHighDateTime;
            tmpres <<= 32;
            tmpres |= ft.dwLowDateTime;

            // 100nano를 1micro로 변환하기
            tmpres /= 10;

            // epoch time으로 변환하기
            tmpres -= DELTA_EPOCH_IN_MICROSECS;

            // sec와 micorsec으로 맞추기
            tv->tv_sec  = static_cast< long >( ( tmpres / 1000000UL ) );
            tv->tv_usec = ( tmpres % 1000000UL );
        }

        // timezone 처리
        if( tz != nullptr )
        {
            DWORD Ret = 0;
            TIME_ZONE_INFORMATION TZ;
            if( (Ret = GetTimeZoneInformation( &TZ )) != TIME_ZONE_ID_INVALID )
            {
                if( Ret == TIME_ZONE_ID_UNKNOWN )
                {
                    tz->tz_minuteswest = 0;
                    tz->tz_dsttime = 0;
                }
                else
                {
                    tz->tz_minuteswest = TZ.Bias;
                    tz->tz_dsttime = 0;

                    if( Ret == TIME_ZONE_ID_STANDARD )
                        tz->tz_dsttime = TZ.StandardBias;

                    if( Ret == TIME_ZONE_ID_DAYLIGHT )
                        tz->tz_dsttime = TZ.DaylightBias;
                }
            }
        }
    }
} // nsCmn
