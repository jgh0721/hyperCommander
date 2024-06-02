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
    ui.edtExts->setText( Item.SetExtensions.values().join( '|' ) );
}

TyExternalEditor CExternalConfigureItem::GetItem() const
{
    return {};
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

    } while( false );

    CoUninitialize();
}

void CExternalConfigureItem::on_btnBrowseFile_clicked( bool checked )
{
    CoInitializeEx( 0, COINIT_APARTMENTTHREADED );

    do
    {

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
