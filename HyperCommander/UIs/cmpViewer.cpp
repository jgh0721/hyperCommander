#include "stdafx.h"
#include "cmpViewer.hpp"

CmpViewer::CmpViewer( QWidget* parent, Qt::WindowFlags f )
    : QWidget( parent, f )
{
    setWindowFlag( Qt::FramelessWindowHint );

    // NOTE: 삭제 금지
    ( void )winId();

    ui.setupUi( this );

    ly_ = new QVBoxLayout;
    setLayout( ly_ );
    ly_->setContentsMargins( 0, 0, 0, 0 );

    QMetaObject::invokeMethod( this, "initialize", Qt::QueuedConnection );
}

CmpViewer::~CmpViewer()
{
    cleanup();
}

void CmpViewer::SetWLX( TySpWLX view )
{
    viewer_ = view;
}

void CmpViewer::SetLoadToFile( const QString& FilePath )
{
    currentFilePath = FilePath;
}

void CmpViewer::closeEvent( QCloseEvent* Ev )
{
    //viewer_->CloseWLX();
    //Ev->accept();
}

void CmpViewer::initialize()
{
    if( viewer_ == nullptr )
        return;

    wlx_ = new QWidget;
    HWND hParent = (HWND)wlx_->winId();
    const auto Funcs = viewer_->Funcs();

    HWND Child = Funcs.ListLoadW( hParent, (WCHAR*) currentFilePath.toStdWString().c_str(), 0 );
    QWindow* Child_W = QWindow::fromWinId( reinterpret_cast< WId >( Child ) );
    Child_W->setFlag( Qt::FramelessWindowHint );
    QWidget* Child_O = QWidget::createWindowContainer( Child_W, wlx_, Qt::FramelessWindowHint );
    
    ly_->addWidget( Child_O );
    Child_O->show();

    viewer_->SetWinId( Child );
}

void CmpViewer::cleanup()
{
    if( viewer_ == nullptr )
        return;

}
