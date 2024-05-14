#include "stdafx.h"
#include "cmnDateTime.hpp"

namespace nsCmn
{
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
} // nsCmn
