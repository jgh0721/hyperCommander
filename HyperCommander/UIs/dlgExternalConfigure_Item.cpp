#include "stdafx.h"
#include "dlgExternalConfigure_Item.hpp"

#include "UniqueLibs/qtDialogFrame.hpp"

#include <objbase.h>

CExternalConfigureItem::CExternalConfigureItem( QWidget* parent, Qt::WindowFlags f )
    : QDialog( parent, f )
{
    ui.setupUi( this );
    // Same frame as the other dialogs: the legacy title frame goes, OK · Cancel move to the footer band.
    ui.frmTitleBar->hide(); setWindowTitle( ui.lblTitleBar->text() );
    auto* Footer = hc::ui::ApplyDialogFrame( this, { {}, tr( "편집기 실행 파일과 명령행, 이 편집기로 열 확장자를 지정합니다." ), fm::ui::glyph::Rename } );
    ui.frame_3->setFrameShape( QFrame::NoFrame );
    for( auto* Label : { ui.label_2, ui.label_7, ui.label_3, ui.label_4, ui.label_5, ui.label_6 } ) Label->setMinimumWidth( 64 );
    hc::ui::MoveToFooter( Footer, ui.btnOK, true ); hc::ui::MoveToFooter( Footer, ui.btnCancel ); ui.btnOK->setDefault( true );
    ui.frame->hide();
}

void CExternalConfigureItem::SetItem( const TyExternalEditor& Item )
{
    if( Item.Icon.isNull() == false )
        ui.lblIcon->setPixmap( Item.Icon.pixmap( 32, 32 ) );
    else
        setProgramIcon( ui.lblIcon, Item.IconPath );

    ui.edtIconPath->setText( Item.IconPath );
    ui.edtName->setText( Item.Name );
    ui.edtFilePath->setText( Item.FilePath );
    ui.edtCMDLine->setText( Item.CMDLine );
    ui.edtDetect->setText( Item.Detect );
    ui.edtExts->setText( Item.VecExtensions.toList().join( '|' ) );
}

TyExternalEditor CExternalConfigureItem::GetItem() const
{
    TyExternalEditor Item;

    Item.IconPath           = ui.edtIconPath->text();
    Item.Icon               = ui.lblIcon->pixmap();
    Item.Name               = ui.edtName->text();
    Item.FilePath           = ui.edtFilePath->text();
    Item.CMDLine            = ui.edtCMDLine->text();
    Item.Detect             = ui.edtDetect->text();
    Item.VecExtensions      = ui.edtExts->text().split( '|', Qt::SkipEmptyParts ).toVector();
    
    return Item;
}

void CExternalConfigureItem::on_btnClose_clicked( bool checked )
{
    on_btnCancel_clicked( checked );
}

void CExternalConfigureItem::on_btnBrowseIcon_clicked( bool checked )
{
    CoInitializeEx( 0, COINIT_APARTMENTTHREADED );

    do
    {
        const auto FilePath = QFileDialog::getOpenFileName( this, tr( "아이콘 선택" ), qApp->applicationDirPath(), "아이콘(*.ico *.exe)" );
        ui.edtIconPath->setText( FilePath );
        setProgramIcon( ui.lblIcon, FilePath );

    } while( false );

    CoUninitialize();
}

void CExternalConfigureItem::on_btnBrowseFile_clicked( bool checked )
{
    CoInitializeEx( 0, COINIT_APARTMENTTHREADED );

    do
    {
        const auto FilePath = QFileDialog::getOpenFileName( this, tr( "실행파일 선택" ), qApp->applicationDirPath(), "*.exe"  );
        ui.edtFilePath->setText( FilePath );

        if( ui.edtName->text().isEmpty() == true )
            ui.edtName->setText( QFileInfo( FilePath ).baseName() );

        if( ui.edtIconPath->text().isEmpty() == true && ui.lblIcon->pixmap().isNull() == true )
        {
            if( setProgramIcon( ui.lblIcon, FilePath ) == true )
                ui.edtIconPath->setText( FilePath );
        }

    } while( false );

    CoUninitialize();
}

void CExternalConfigureItem::on_btnOK_clicked( bool checked )
{
    done( Accepted );
}

void CExternalConfigureItem::on_btnCancel_clicked( bool checked )
{
    done( Rejected );
}

bool CExternalConfigureItem::setProgramIcon( QLabel* lbl, const QString& IconPath )
{
    if( IconPath.section( '.', -1 ).compare( "exe", Qt::CaseInsensitive ) == 0 )
    {
        QFileIconProvider IconProvider;
        ui.lblIcon->setPixmap( IconProvider.icon( QFileInfo( IconPath ) ).pixmap( 32, 32 ) );
    }
    else
    {
        ui.lblIcon->setPixmap( QIcon( IconPath ).pixmap( 32, 32 ) );
    }

    return ui.lblIcon->pixmap().isNull() == false;
}
