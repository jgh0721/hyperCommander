#include "stdafx.h"
#include "cmnUiUtils.hpp"

#include "colorSchemeMgr.hpp"

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

            initializeColorScheme();

            emit NotifyUiReady();
            isInitOnce = true;
        }

        QDialog::showEvent( event );
    }

    void QBaseUI::initializeColorScheme()
    {
        const auto StColorSchemeMgr = TyStColorSchemeMgr::GetInstance();
        const auto ColorScheme = StColorSchemeMgr->GetColorScheme( StColorSchemeMgr->GetCurrentColorScheme() );

        auto Palette = palette();
        setFont( ColorScheme.Dialog_Font );
        //
        //Palette.setColor( QPalette::ColorRole::Window, ColorScheme.Dialog_BGColor );
        //Palette.setColor( QPalette::ColorRole::Button, ColorScheme.Dialog_BGColor );
        //Palette.setColor( QPalette::ColorRole::WindowText, ColorScheme.Dialog_FGColor );
        //Palette.setColor( QPalette::ColorRole::ButtonText, ColorScheme.Dialog_FGColor );

        //if( ColorScheme.IsDarkMode == true )
        //{
        //    setStyleSheet( R"(
        //        * { border-radius : 2px; border-color: cyan; border-width: 1px; };
        //    )" );
        //}

        //setPalette( Palette );
    }

    //////////////////////////////////////////////////////////////////////////

    QSet<int> SelectedRowsFromTbl( QTableWidget* Widget )
    {
        QSet< int > SelectedRows;
        for( auto& Item : Widget->selectedItems() )
            SelectedRows.insert( Item->row() );

        return SelectedRows;
    }
}
