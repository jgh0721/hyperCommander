#include "stdafx.h"
#include "dlgExternalConfigure.hpp"

#include "UIs/dlgExternalConfigure_Item.hpp"

#include "UniqueLibs/externalEditorMgr.hpp"

CExternalConfigure::CExternalConfigure( QWidget* parent /* = nullptr*/, Qt::WindowFlags f /* = Qt::Dialog*/ )
    : QDialog( parent, f )
{
    ui.setupUi( this );
    
    Refresh();
}

void CExternalConfigure::Refresh()
{
    const auto StExternalMgr = TyStExternalEditorMgr::GetInstance();
    const auto VecSettings = StExternalMgr->RetrieveSettings();

    ui.tblEditor->clearContents();
    ui.tblEditor->setRowCount( 0 );

    for( const auto& Item : VecSettings )
    {
        const auto Row = ui.tblEditor->rowCount();
        ui.tblEditor->setRowCount( Row + 1 );
        
        if( Item.Icon.isNull() == false )
            ui.tblEditor->setItem( Row, COL_ICON, new QTableWidgetItem( Item.Icon, "" ));
        else
            ui.tblEditor->setItem( Row, COL_ICON, {} );
        ui.tblEditor->setItem( Row, COL_NAME,       new QTableWidgetItem( Item.Name ) );
        ui.tblEditor->setItem( Row, COL_PROGRAM,    new QTableWidgetItem( Item.FilePath ) );
        ui.tblEditor->setItem( Row, COL_CMDLINE,    new QTableWidgetItem( Item.CMDLine ) );
        ui.tblEditor->setItem( Row, COL_DETECT,     new QTableWidgetItem( Item.Detect ) );
        ui.tblEditor->setItem( Row, COL_EXTS,       new QTableWidgetItem( Item.SetExtensions.values().join( '|' ) ) );
        ui.tblEditor->item( Row, COL_NAME )->setData( Qt::UserRole, QVariant::fromValue( Item ) );
    }
}

QSet< int > CExternalConfigure::SelectedRow() const
{
    QSet< int > SelectedRows;
    for( const auto Sel : ui.tblEditor->selectedItems() )
        SelectedRows.insert( Sel->row() );
    return SelectedRows;
}

void CExternalConfigure::on_btnAdd_clicked( bool checked )
{
    CExternalConfigureItem AddModify;

    if( AddModify.exec() == Accepted )
    {
        const auto StExternalMgr = TyStExternalEditorMgr::GetInstance();
        StExternalMgr->AddItem( AddModify.GetItem() );
        
        QTimer::singleShot( 1, this, &CExternalConfigure::Refresh );
    }
}

void CExternalConfigure::on_btnModify_clicked( bool checked )
{
    QSet< int > SelectedRows = SelectedRow();
    if( SelectedRows.isEmpty() == true )
    {
        return;
    }

    if( SelectedRows.size() > 1 )
    {
        return;
    }

    // SelectedRows 에는 하나만 있어야 한다. 
    const auto SelectedRow = *SelectedRows.begin();
    ui.tblEditor->item( SelectedRow, COL_NAME );

    CExternalConfigureItem Editor;

    const auto Result = Editor.exec();
    if( Result == QDialog::Accepted )
    {
        const auto StExternalMgr = TyStExternalEditorMgr::GetInstance();
        StExternalMgr->ModifyItem( SelectedRow, Editor.GetItem() );

        QTimer::singleShot( 1, this, &CExternalConfigure::Refresh );
    }
}

void CExternalConfigure::on_btnRemove_clicked( bool checked )
{
    const auto StExternalMgr = TyStExternalEditorMgr::GetInstance();
    QTimer::singleShot( 1, this, &CExternalConfigure::Refresh );
}
