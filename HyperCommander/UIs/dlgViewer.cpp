#include "stdafx.h"
#include "dlgViewer.hpp"

#include "UniqueLibs/solTCPluginMgr.hpp"

#include <WinUser.h>

QDlgViewer::QDlgViewer( QWidget* parent, Qt::WindowFlags f )
    : QDialog( parent, f )
{
    ui.setupUi( this );

    setAttribute( Qt::WA_DeleteOnClose );

    QMetaObject::invokeMethod( this, "initialize", Qt::QueuedConnection );
}

void QDlgViewer::SetFileName( const QString& FilePath )
{
    currentFilePath = FilePath;
    ui.pgWLXViewer->SetLoadToFile( FilePath );
}

void QDlgViewer::SetInternalViewer( TySpWLX Lister )
{
    ui.pgWLXViewer->SetWLX( Lister );
}

void QDlgViewer::on_btnClose_clicked( bool checked )
{
    done( Accepted );
}

void QDlgViewer::keyPressEvent( QKeyEvent* Ev )
{
    if( Ev->key() == Qt::Key_Escape )
    {
        done( QDialog::Accepted );
        return;
    }

    QDialog::keyPressEvent( Ev );
}

void QDlgViewer::initialize()
{

}

bool QDlgViewer::nativeEvent( const QByteArray& eventType, void* message, qintptr* result )
{
    if( eventType.compare( QByteArrayLiteral( "windows_generic_MSG" ) ) != 0 )
        return QDialog::nativeEvent( eventType, message, result );

    MSG* msg = ( MSG* )message;

    if( msg->message == WM_LBUTTONDOWN )
    {
        int a = 0;
    }

    return QDialog::nativeEvent( eventType, message, result );
}

LRESULT QDlgViewer::cbWndProc( HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam )
{
    LRESULT Ret = 0;
    WNDPROC Prev = nullptr;
    if( uMsg == WM_LBUTTONDOWN )
    {
        int a = 0;
    }

    Prev = (WNDPROC)GetPropW( hWnd, ( LPCWSTR )GlobalAddAtomW( L"HyperCommander" ) );
    if( Prev )
        Ret = CallWindowProcW( Prev, hWnd, uMsg, wParam, lParam );
    else
        Ret = DefWindowProcW( hWnd, uMsg, wParam, lParam );

    if( Ret == 0 && uMsg == WM_COMMAND && lParam != 0 )
    {
        int a = 0;
        //    Lister:= TControl(GetLCLOwnerObject(hWnd));
        //    if Assigned(Lister) then Lister.Perform(Msg, wParam, lParam);
    }

    return Ret;

    //  if (Result = 0) and (Msg = WM_COMMAND) and (lParam <> 0) then
    //  begin
    //    Lister:= TControl(GetLCLOwnerObject(hWnd));
    //    if Assigned(Lister) then Lister.Perform(Msg, wParam, lParam);
    //  end;
    //end;

}

LRESULT QDlgViewer::cbWndProcChild( HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam )
{
    LRESULT Ret = 0;
    WNDPROC Prev = nullptr;
    if( uMsg == WM_LBUTTONDOWN )
    {
        int a = 0;
    }

    Prev = ( WNDPROC )GetPropW( hWnd, ( LPCWSTR )GlobalAddAtomW( L"HyperCommander" ) );
    if( Prev )
        Ret = CallWindowProcW( Prev, hWnd, uMsg, wParam, lParam );
    else
        Ret = DefWindowProcW( hWnd, uMsg, wParam, lParam );

    if( Ret == 0 && uMsg == WM_KEYDOWN )
        PostMessageW( GetParent( hWnd ), uMsg, wParam, lParam );

    return Ret;
}

