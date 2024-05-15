#include "stdafx.h"
#include "HyperCommander.hpp"

#include "dlgMain.hpp"
#include "../uniqueLibs/SHChangeNotify.hpp"
#include "UniqueLibs/commandMgr.hpp"
#include "UniqueLibs/shortcutMgr.hpp"

HyperCommanderApp::HyperCommanderApp( int& argc, char** argv )
    : QApplication( argc, argv )
{
    QCoreApplication::setOrganizationName( "MyHouse" );
    QCoreApplication::setOrganizationDomain( "devtester.com" );
    QCoreApplication::setApplicationName( "HyperCommander" );

    QSettings::setDefaultFormat( QSettings::IniFormat );
    QSettings::setPath( QSettings::IniFormat, QSettings::UserScope, applicationDirPath() );

    const auto StSettings = TyStSettings::GetInstance();
    

    QSettings f( QString("%1/111.ini").arg( applicationDirPath() ), QSettings::IniFormat );
    f.beginGroup( "fsdfdsfds" );
    f.setValue( "fsdfdsds1", "rew" );
    f.endGroup();
    const auto qw  = f.fileName();

    shlChangeNotify = new QSHChangeNotify;
    shlChangeNotify->StartWatching();

    auto ui = new QMainUI;
    ui->show();

    TyStCommandMgr::GetInstance()->Refresh();

    TyStShortcutMgr::GetInstance()->SetShortcut( QKeySequence( "Shift+Return", QKeySequence::PortableText ), "ContextMenu" );

}
