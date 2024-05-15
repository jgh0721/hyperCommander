#include "stdafx.h"
#include "HyperCommander.hpp"
#include <QtWidgets/QApplication>

#include <combaseapi.h>

int main( int argc, char* argv[] )
{
    int Ret = 0;
    CoInitializeEx( 0, COINIT_APARTMENTTHREADED );
    HyperCommanderApp app( argc, argv );

    do
    {
        Ret = app.exec();

    } while( false );

    CoUninitialize();
    return Ret;
}
