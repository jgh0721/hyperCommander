#include "stdafx.h"
#include "SHChangeNotify.hpp"

#include <ShlObj.h>

QSHChangeNotify::QSHChangeNotify( QWidget* parent, Qt::WindowFlags f )
    : QWidget( parent, f )
{
    hWnd = winId();
}

HRESULT QSHChangeNotify::StartWatching()
{
    HRESULT Ret = S_OK;
    LPITEMIDLIST pidl = nullptr;

    do
    {
        Ret = SHGetKnownFolderIDList( FOLDERID_ComputerFolder, 0, nullptr, &pidl );
        if( Ret != S_OK )
            break;

        SHChangeNotifyEntry shCNE = { pidl, TRUE };
        ulRegister = SHChangeNotifyRegister( reinterpret_cast< HWND >( hWnd ), 
                                             SHCNRF_ShellLevel | SHCNRF_InterruptLevel | SHCNRF_NewDelivery, 
                                             SHCNE_DRIVEADD | SHCNE_DRIVEREMOVED | SHCNE_MEDIAINSERTED | SHCNE_MEDIAREMOVED, SHELL_NOTIFY_MESSAGE, 1, &shCNE );

        Ret = ulRegister != 0 ? S_OK : E_FAIL;

    } while( false );

    return Ret;
}

void QSHChangeNotify::StopWatching()
{
    if( ulRegister > 0 )
        SHChangeNotifyDeregister( ulRegister );
    ulRegister = 0;
}

bool QSHChangeNotify::nativeEvent( const QByteArray& eventType, void* message, qintptr* result )
{
    do
    {
        if( eventType != "windows_generic_MSG" )
            break;

        const auto msg = static_cast< MSG* >( message );
        if( msg->message != SHELL_NOTIFY_MESSAGE )
            break;

        long lEvent = 0;
        PIDLIST_ABSOLUTE* rgpidl = nullptr;
        HANDLE hNotifyLock = SHChangeNotification_Lock( reinterpret_cast< HANDLE >( msg->wParam ), static_cast< DWORD >( msg->lParam ), &rgpidl, &lEvent );

        do
        {
            if( hNotifyLock == nullptr )
                break;

            if( isItemNotificationEvent( lEvent ) == false )
                break;

            IShellItem2* Left = nullptr;
            IShellItem2* Right = nullptr;

            if( rgpidl[ 0 ] )
                SHCreateItemFromIDList( rgpidl[ 0 ], IID_PPV_ARGS( &Left ) );

            if( rgpidl[ 1 ] )
                SHCreateItemFromIDList( rgpidl[ 1 ], IID_PPV_ARGS( &Right ) );

            if( lEvent == SHCNE_DRIVEADD || lEvent == SHCNE_DRIVEREMOVED )
            {
                PWSTR LeftDisplayName = nullptr;
                if( Left->GetDisplayName( SIGDN_FILESYSPATH, &LeftDisplayName ) == S_OK )
                {
                    if( lEvent == SHCNE_DRIVEADD )
                        emit OnDriveAdd( QString::fromWCharArray( LeftDisplayName ) );

                    if( lEvent == SHCNE_DRIVEREMOVED )
                        emit OnDriveRemoved( QString::fromWCharArray( LeftDisplayName ) );

                    CoTaskMemFree( LeftDisplayName );
                }
            }

            if( lEvent == SHCNE_MEDIAINSERTED || lEvent == SHCNE_MEDIAREMOVED )
            {
                PWSTR LeftDisplayName = nullptr;
                if( Left->GetDisplayName( SIGDN_DESKTOPABSOLUTEPARSING, &LeftDisplayName ) == S_OK )
                {
                    if( lEvent == SHCNE_DRIVEADD )
                        emit OnMediaInserted( QString::fromWCharArray( LeftDisplayName ) );

                    if( lEvent == SHCNE_DRIVEREMOVED )
                        emit OnMediaRemoved( QString::fromWCharArray( LeftDisplayName ) );

                    CoTaskMemFree( LeftDisplayName );
                }
            }

            if( Left != nullptr )
            {
                Left->Release();
                Left = nullptr;
            }

            if( Right != nullptr )
            {
                Right->Release();
                Right = nullptr;
            }

        } while( false );

        SHChangeNotification_Unlock( hNotifyLock );

    } while( false );

    return QWidget::nativeEvent( eventType, message, result );
}

bool QSHChangeNotify::isItemNotificationEvent( long lEvent ) const
{
    return !( lEvent & ( SHCNE_UPDATEIMAGE | SHCNE_ASSOCCHANGED | SHCNE_EXTENDED_EVENT | SHCNE_FREESPACE | SHCNE_DRIVEADDGUI | SHCNE_SERVERDISCONNECT ) );
}
