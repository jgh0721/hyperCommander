#include "stdafx.h"
#include "dlgDefaultCMD.hpp"

QDefaultCMDUI::QDefaultCMDUI( QWidget* Parent, Qt::WindowFlags Flags )
    : QBaseUI( Parent, Flags )
{
    ui.setupUi( this );

    ui.tblShortCut->setSelectionMode( QAbstractItemView::SelectionMode::SingleSelection );
    ui.tblShortCut->setSelectionBehavior( QAbstractItemView::SelectionBehavior::SelectRows );

    ui.tblShortCut->clearContents();
    ui.tblShortCut->setRowCount( 0 );

    for( const auto& Default : GlobalBuiltInCMDs )
    {
        const auto Row = ui.tblShortCut->rowCount();

        ui.tblShortCut->setRowCount( Row + 1 );
        ui.tblShortCut->setItem( Row, 0, new QTableWidgetItem( Default.Name ) );
        ui.tblShortCut->setItem( Row, 1, new QTableWidgetItem( QKeySequence( Default.Default ).toString() ) );
        ui.tblShortCut->setItem( Row, 2, new QTableWidgetItem( Default.Desc ) );
    }
}

TyHC_COMMAND QDefaultCMDUI::GetSelectedCMD() const
{
    QSet< int > SelectedRows = nsHC::SelectedRowsFromTbl( ui.tblShortCut );
    if( SelectedRows.isEmpty() == true )
        return {};

    const auto SelectedRow = *SelectedRows.begin();
    const auto SelectedCMD = ui.tblShortCut->item( SelectedRow, 0 )->text();

    for( const auto& Default : GlobalBuiltInCMDs )
    {
        if( Default.Name.compare( SelectedCMD, Qt::CaseInsensitive ) != 0 )
            continue;

        return Default;
    }

    return TyHC_COMMAND();
}

void QDefaultCMDUI::on_btnClose_clicked( bool checked )
{
    on_btnCancel_clicked( checked );
}

void QDefaultCMDUI::on_btnOK_clicked( bool checked )
{
    done( Accepted );
}

void QDefaultCMDUI::on_btnCancel_clicked( bool checked )
{
    done( Rejected );
}
