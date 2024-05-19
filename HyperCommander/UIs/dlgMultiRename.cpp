#include "stdafx.h"
#include "dlgMultiRename.hpp"

QMultiRenameUI::QMultiRenameUI( QWidget* Parent, Qt::WindowFlags Flags )
    : QDialog( Parent, Flags )
{
    Ui.setupUi( this );

    View = Ui.grdPreview->view< GridBandedTableView >();

    View->beginUpdate();
    auto& BaseOpts = View->options();
    auto& TableOpts = View->tableOptions();
    auto& BandedOpts = View->bandedOptions();

    BaseOpts.setAutoCreateColumns( false );

    View->removeBands();
    View->addBand( tr( "FS" ) );

    Model = new QMultiRenameModel;
    View->setModel( Model );

    Qtitan::GridBandedTableColumn* GridColumn = nullptr;
    GridColumn = View->addColumn( 0, tr("원본 파일이름") );
    GridColumn->setBandIndex( 0 );

    GridColumn = View->addColumn( 1, tr( "원본 파일이름" ) );
    GridColumn->setBandIndex( 0 );

    GridColumn = View->addColumn( 2, tr( "크기" ) );
    GridColumn->setBandIndex( 0 );

    GridColumn = View->addColumn( 3, tr( "날짜" ) );
    GridColumn->setBandIndex( 0 );

    GridColumn = View->addColumn( 4, tr( "대상 파일이름" ) );
    GridColumn->setBandIndex( 0 );
    GridColumn->setRowIndex( 1 );

    GridColumn = View->addColumn( 5, tr( "대상 확장자" ) );
    GridColumn->setBandIndex( 0 );
    GridColumn->setRowIndex( 1 );

    View->endUpdate();

}
