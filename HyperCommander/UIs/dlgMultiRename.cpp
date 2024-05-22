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
    BaseOpts.setGroupsHeader( false );
    BaseOpts.setFocusFrameEnabled( false );
    TableOpts.setColumnAutoWidth( true );
    TableOpts.setColumnsQuickCustomization( false );
    TableOpts.setRowsQuickSelection( false );
    BandedOpts.setBandsHeader( false );

    View->removeBands();
    View->addBand( tr( "FS" ) );

    Model = new QMultiRenameModel;
    View->setModel( Model );

    Qtitan::GridBandedTableColumn* GridColumn = nullptr;
    GridColumn = View->addColumn( 0, tr("원본 파일이름") );
    GridColumn->setBandIndex( 0 );
    GridColumn->setTextAlignment( Qt::AlignLeft );

    GridColumn = View->addColumn( 1, tr( "원본 확장자" ) );
    GridColumn->setBandIndex( 0 );
    GridColumn->setTextAlignment( Qt::AlignLeft );

    GridColumn = View->addColumn( 2, tr( "크기" ) );
    GridColumn->setBandIndex( 0 );
    GridColumn->setTextAlignment( Qt::AlignCenter );

    GridColumn = View->addColumn( 3, tr( "날짜" ) );
    GridColumn->setBandIndex( 0 );
    GridColumn->setTextAlignment( Qt::AlignCenter );

    GridColumn = View->addColumn( 4, tr( "대상 파일이름" ) );
    GridColumn->setBandIndex( 0 );
    GridColumn->setRowIndex( 1 );
    GridColumn->setTextAlignment( Qt::AlignLeft );

    GridColumn = View->addColumn( 5, tr( "대상 확장자" ) );
    GridColumn->setBandIndex( 0 );
    GridColumn->setRowIndex( 1 );
    GridColumn->setTextAlignment( Qt::AlignLeft );

    View->endUpdate();

    View->bestFit( Qtitan::BestFitModeFlag::FitToHeaderAndViewContent );
}

void QMultiRenameUI::SetSourceFiles( const QVector<QString>& VecFiles )
{
    Model->SetSourceFiles( VecFiles );

    Ui.edtFileNamePattern->setText( "[N]" );
    Ui.edtFileExtPattern->setText( "[E]" );
}

void QMultiRenameUI::on_edtFileNamePattern_textChanged( const QString& Text )
{

}

void QMultiRenameUI::on_edtFileExtPattern_textChanged( const QString& Text )
{}

void QMultiRenameUI::on_edtSearchString_textChanged( const QString& Text )
{}

void QMultiRenameUI::on_edtReplaceString_textChanged( const QString& Text )
{}
