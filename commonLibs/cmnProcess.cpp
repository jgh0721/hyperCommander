#include "stdafx.h"
#include "cmnProcess.hpp"

namespace nsCmn
{
    namespace nsProcess
    {
        bool CreateProcessAsNormal( const QString& sFilePath, const QString& sCMDLine, bool isHide, bool isWait, uint32_t* dwProcessId )
        {
            bool isSuccess = false;
            STARTUPINFO si;
            PROCESS_INFORMATION pi;

            do
            {
                if( dwProcessId != Q_NULLPTR )
                    *dwProcessId = 0;

                if( sFilePath.isEmpty() == true && sCMDLine.isEmpty() == true )
                    break;

                ZeroMemory( &si, sizeof( si ) );
                ZeroMemory( &pi, sizeof( pi ) );

                si.cb = sizeof( si );

                si.dwFlags      = STARTF_USESHOWWINDOW;
                si.wShowWindow  = isHide == true ? SW_HIDE : SW_SHOW;

                std::wstring CMDLine = sFilePath.isEmpty() == false ? QString( R"("%1" "%2")" ).arg( sFilePath ).arg( sCMDLine ).toStdWString() : sCMDLine.toStdWString();

                wchar_t* wCMDLine = nullptr;
                if( CMDLine.empty() == false )
                    wCMDLine = _wcsdup( CMDLine.c_str() );

                if( CreateProcessW( nullptr,            // No module name (use command line).
                                    wCMDLine,           // Command line.
                                    NULL,               // Process handle not inheritable.
                                    NULL,               // Thread handle not inheritable.
                                    FALSE,              // Set handle inheritance to FALSE.
                                    0,                  // No creation flags.
                                    NULL,               // Use parent's environment block.
                                    NULL,               // Use parent's starting directory.
                                    &si,                // Pointer to STARTUPINFO structure.
                                    &pi                 // Pointer to PROCESS_INFORMATION structure.
                ) == FALSE )
                {
                    if( wCMDLine != nullptr )
                        free( wCMDLine );
                    break;
                }

                if( wCMDLine != nullptr )
                    free( wCMDLine );

                if( isWait == false && dwProcessId != Q_NULLPTR )
                    *dwProcessId = pi.dwProcessId;

                if( pi.hProcess != Q_NULLPTR )
                {
                    if( isWait == true )
                        WaitForSingleObject( pi.hProcess, INFINITE );
                    CloseHandle( pi.hProcess );
                }

                if( pi.hThread != Q_NULLPTR )
                    CloseHandle( pi.hThread );

                isSuccess = true;

            } while( false );

            return isSuccess;
        }

    } // nsProcess

} // nsCmn