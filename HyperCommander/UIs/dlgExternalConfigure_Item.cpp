#include "stdafx.h"
#include "dlgExternalConfigure_Item.hpp"

#include <objbase.h>

CExternalConfigureItem::CExternalConfigureItem( QWidget* parent, Qt::WindowFlags f )
    : QDialog( parent, f )
{
    ui.setupUi( this );
}

void CExternalConfigureItem::SetItem( const TyExternalEditor& Item )
{
    if( Item.Icon.isNull() == false )
        ui.lblIcon->setPixmap( Item.Icon.pixmap( 32, 32 ) );
    
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
        if( FilePath.section( '.', -1 ).compare( "exe", Qt::CaseInsensitive ) == 0 )
        {
            QFileIconProvider IconProvider;
            ui.lblIcon->setPixmap( IconProvider.icon( QFileInfo( FilePath ) ).pixmap( 32, 32 ) );
        }
        else
        {
            ui.lblIcon->setPixmap( QIcon( FilePath ).pixmap( 32, 32 ) );
        }

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
