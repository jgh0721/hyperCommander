#include "stdafx.h"
#include "shortcutMgr.hpp"

void CShortcutMgr::SetShortcut( const QKeySequence& KeySequence, const QString& Cmd )
{
    mapKeyToCMD[ KeySequence.toString( QKeySequence::PortableText ) ] = Cmd;
}

QString CShortcutMgr::GetCMDFromShortcut( const QKeySequence& KeySequence )
{
    return mapKeyToCMD[ KeySequence.toString( QKeySequence::PortableText ) ];
}
