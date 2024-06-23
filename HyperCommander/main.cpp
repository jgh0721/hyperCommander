#include "stdafx.h"
#include "HyperCommander.hpp"
#include <QtWidgets/QApplication>

#include <combaseapi.h>

int main( int argc, char* argv[] )
{
    int Ret = 0;
    CoInitializeEx( 0, COINIT_APARTMENTTHREADED );

    SetEnvironmentVariableW( L"QT_ENABLE_HIGHDPI_SCALING", L"1" );
    QCoreApplication::setAttribute( Qt::AA_EnableHighDpiScaling, true );
    QCoreApplication::setAttribute( Qt::AA_UseHighDpiPixmaps, true );

    HyperCommanderApp app( argc, argv );

    do
    {
        Ret = app.exec();

    } while( false );

    CoUninitialize();
    return Ret;
}
