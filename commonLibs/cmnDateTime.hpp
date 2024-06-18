#pragma once

namespace nsCmn
{
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

    QDateTime                           ConvertTo( const FILETIME& Time, bool IsUTC, bool* ok = nullptr );

    void                                GetTimeOfDay( OAL_TIMEVAL* tv, OAL_TIMEZONE* tz );
} // nsCmn
