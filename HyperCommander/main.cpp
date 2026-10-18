#include "stdafx.h"
#include "HyperCommander.hpp"
#include "HyperCommanderUpdater/updateTransaction.hpp"
#include <QtWidgets/QApplication>

#include <combaseapi.h>

#include "ipc/ipc.hpp"
#include "win32api/W32API_NtDll.hpp"

#include "UniqueLibs/builtInVolumeModel.hpp"

int main( int argc, char* argv[] )
{
    if (!hc::update::StartupAllowed(hc::update::ExecutableRoot())) {
        MessageBoxW(nullptr, L"업데이트가 진행 중이거나 복구가 필요합니다. 모든 제품 창을 닫고 HyperCommanderUpdater.exe --recover <설치 폴더>를 실행하십시오.", L"HyperCommander 업데이트", MB_OK | MB_ICONWARNING);
        return ERROR_BUSY;
    }
    int Ret = 0;
    CoInitializeEx( 0, COINIT_APARTMENTTHREADED );

    SetEnvironmentVariableW( L"QT_ENABLE_HIGHDPI_SCALING", L"1" );
    QCoreApplication::setAttribute( Qt::AA_EnableHighDpiScaling, true );
    QCoreApplication::setAttribute( Qt::AA_UseHighDpiPixmaps, true );

    HyperCommanderApp app( argc, argv );

    do
    {
        Ret = app.ShouldRun() ? app.exec() : app.StartupExitCode();

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

