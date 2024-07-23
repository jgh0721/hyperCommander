#include "stdafx.h"
#include "HyperCommander.hpp"
#include <QtWidgets/QApplication>

#include <combaseapi.h>

#include "ipc/ipc.hpp"
#include "win32api/W32API_NtDll.hpp"

#include "UniqueLibs/builtInVolumeModel.hpp"

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

/*!

    추상화 계층
        FileSource
            모든 항목의 기초 클래스

        FileSystem
            모든 파일시스템의 기초 클래스

        CFSNTFS : public FileSystem



 */

