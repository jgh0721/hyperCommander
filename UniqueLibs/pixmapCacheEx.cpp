#include "stdafx.h"
#include "pixmapCacheEx.hpp"

DECLARE_CMNLIBSV2_NAMESPACE

////////////////////////////////////////////////////////////////////////////////
///

void CIconCache::Clear()
{
    QWriteLocker Locker( &Lock );
    Values.clear();
}

QPixmap CIconCache::Value( const QString& Key, bool* ok )
{
    QReadLocker Locker( &Lock );
    if( ok ) *ok = false;

    const auto Value = Values.object( Key );
    if( Value == nullptr )
        return {};

    if( ok ) *ok = true;
    return *Value;
}

void CIconCache::Insert( const QString& Key, QPixmap* Value )
{
    QWriteLocker Locker( &Lock );
    Values.insert( Key, Value );
}
