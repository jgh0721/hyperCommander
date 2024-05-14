#include "stdafx.h"
#include "HyperCommander.hpp"
#include <QtWidgets/QApplication>

int main( int argc, char* argv[] )
{
    int Ret = 0;
    HyperCommanderApp app( argc, argv );

    do
    {
        Ret = app.exec();

    } while( false );

    return Ret;
}
