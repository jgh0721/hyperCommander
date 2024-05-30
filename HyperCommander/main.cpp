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
        QDir dir( R"(D:\Downloads)" );
        for( const auto& Info : dir.entryInfoList( QStringList() << "*.pdf" ) )
        {
            const auto Name = Info.fileName().toStdWString();

            SetLastError( ERROR_SUCCESS );
            BOOL a = IsNormalizedString( NormalizationD, Name.c_str(), -1 );
            if( a != FALSE )
            {
                int q = 0;
            }

        }

        Ret = app.exec();

    } while( false );

    CoUninitialize();
    return Ret;
}
