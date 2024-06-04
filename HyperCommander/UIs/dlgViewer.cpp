#include "stdafx.h"
#include "dlgViewer.hpp"

#include "UniqueLibs/internalViewerMgr.hpp"

#include <WinUser.h>

QDlgViewer::QDlgViewer( QWidget* parent, Qt::WindowFlags f )
    : QDialog( parent, f )
{
    ui.setupUi( this );

    QMetaObject::invokeMethod( this, "initialize", Qt::QueuedConnection );
}

void QDlgViewer::SetFileName( const QString& FilePath )
{
    currentFilePath = FilePath;
}

void QDlgViewer::SetInternalViewer( const TyInternalViewer& IV )
{
    viewer = IV;
}

void QDlgViewer::on_btnClose_clicked( bool checked )
{
    done( Accepted );
}

void QDlgViewer::initialize()
{
    wchar_t Buffer[ MAX_PATH ] = { 0, };
    GetEnvironmentVariableW( L"COMMANDER_PATH", Buffer, MAX_PATH );
    const auto WLX = QString( viewer.FileFullPath ).replace( "%COMMANDER_PATH%", QString::fromWCharArray( Buffer ) );

    CInternalViewerMgr::searchViewerFuncs( WLX, &viewer.Funcs );

    HWND hParent2 = reinterpret_cast< HWND >( ui.wViewer->winId() );
    RtlZeroMemory( Buffer, sizeof( WCHAR ) * MAX_PATH );
    wcscat_s( Buffer, currentFilePath.toStdWString().c_str() );

    const auto Parent = new QWidget;

    auto ly = new QVBoxLayout;
    ui.wViewer->setLayout( ly );
    ly->addWidget( Parent );
    
    hParent = ( HWND )Parent->winId();
    hViewer = viewer.Funcs.ListLoadW( hParent, Buffer, 0 );
    if( hViewer != nullptr )
    {

        //SetParent( hViewer, ( HWND )Parent->winId() );
        QWindow* child = QWindow::fromWinId( reinterpret_cast< WId >( hViewer ) );
        child->setFlags( Qt::FramelessWindowHint );
        QWidget* child_widget = QWidget::createWindowContainer( child, Parent );

        auto Ret = SetWindowLongPtrW( hParent, GWLP_WNDPROC, (LONG_PTR)&QDlgViewer::cbWndProc);
        SetPropW( hParent, (LPCWSTR)GlobalAddAtomW( L"HyperCommander" ), (HANDLE)Ret );

        Ret = SetWindowLongPtrW( hViewer, GWLP_WNDPROC, ( LONG_PTR )&QDlgViewer::cbWndProcChild );
        SetPropW( hViewer, ( LPCWSTR )GlobalAddAtomW( L"HyperCommander" ), ( HANDLE )Ret );

        // SetParent( hViewer, hParent );
        //SetWindowLongPtrW( hViewer, GWLP_HWNDPARENT, (LONG_PTR)hParent );

        auto ly2 = new QVBoxLayout;
        Parent->setLayout( ly2 );
        ly2->addWidget( child_widget );

        child_widget->show();
    }

    // if( viewer.Funcs.ListLoad(  ) )

    int a = 0;
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

    if( msg->hwnd == hViewer )
    {
        return true;
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

