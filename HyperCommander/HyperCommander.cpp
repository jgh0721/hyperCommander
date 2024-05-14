#include "stdafx.h"
#include "HyperCommander.hpp"

#include "dlgMain.hpp"
#include "../uniqueLibs/SHChangeNotify.hpp"
#include "UniqueLibs/commandMgr.hpp"
#include "UniqueLibs/shortcutMgr.hpp"

HyperCommanderApp::HyperCommanderApp( int& argc, char** argv )
    : QApplication( argc, argv )
{

    auto q = new QSHChangeNotify;

    q->StartWatching();

    auto ui = new QMainUI;
    ui->show();

    TyStCommandMgr::GetInstance()->Refresh();

    TyStShortcutMgr::GetInstance()->SetShortcut( QKeySequence( "Shift+Return", QKeySequence::PortableText ), "ContextMenu" );

}
