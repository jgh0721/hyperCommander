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
        //SHFILEINFOW SHInfo = { 0, };
        //const auto File = std::wstring( L"sln" );
        //SHGetFileInfoW( L".sln", FILE_ATTRIBUTE_NORMAL, &SHInfo, sizeof( SHFILEINFOW ),
        //                SHGFI_TYPENAME | SHGFI_USEFILEATTRIBUTES | SHGFI_ADDOVERLAYS | SHGFI_ICON | SHGFI_LARGEICON );
        //auto Icon = QPixmap::fromImage( QImage::fromHICON( SHInfo.hIcon ) );
        //if( SHInfo.hIcon != Q_NULLPTR )
        //    DestroyIcon( SHInfo.hIcon );

        Ret = app.exec();

    } while( false );

    CoUninitialize();
    return Ret;
}
