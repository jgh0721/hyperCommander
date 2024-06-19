#include "stdafx.h"
#include "cmnUiUtils.hpp"

DECLARE_CMNLIBSV2_NAMESPACE

namespace nsHC
{
    QBaseUI::QBaseUI( QWidget* Parent, Qt::WindowFlags Flags )
        : QDialog( Parent, Flags )
    {}

    void QBaseUI::SetMovableUI( bool isMovable )
    {
        isDragableWindow_ = isMovable;
    }

    bool QBaseUI::GetMovableUI()
    {
        return isDragableWindow_;
    }

    void QBaseUI::SetResizableUI( bool isResizable )
    {
        isResizableWindow_ = isResizable;
    }

    bool QBaseUI::GetResizableUI() const
    {
        return isResizableWindow_;
    }

    void QBaseUI::OnColorSchemeChanged( const TyColorScheme& ColorScheme )
    {
    }

    void QBaseUI::OnLanguageChanged()
    {
    }

    void QBaseUI::mousePressEvent( QMouseEvent* ev )
    {
        if( ev->button() == Qt::LeftButton )
        {
            if( ev->y() <= nDragableHeightPx_ )
            {
                isDragWindow_ = true;
                ptDragPosition = ev->globalPos() - frameGeometry().topLeft();
                ev->accept();
            }
        }
    }

    void QBaseUI::mouseMoveEvent( QMouseEvent* ev )
    {
        if( ( ev->buttons() & Qt::LeftButton ) && ( isDragWindow_ == true ) )
        {
            if( isDragableWindow_ == true )
                move( ev->globalPos() - ptDragPosition );
            ev->accept();
        }
    }

    void QBaseUI::mouseReleaseEvent( QMouseEvent* ev )
    {
        if( isDragWindow_ == true )
        {
            isDragWindow_ = false;
            ev->accept();
        }
    }

    void QBaseUI::showEvent( QShowEvent* event )
    {
        static bool isInitOnce = false;
        if( event->spontaneous() == false && isInitOnce == false )
        {
            if( titleBar_ != nullptr )
                nDragableHeightPx_ = titleBar_->height();
                
            emit NotifyUiReady();
            isInitOnce = true;
        }

        QDialog::showEvent( event );
    }
}
