#include "stdafx.h"
#include "dlgMultiRename.hpp"

QMultiRenameUI::QMultiRenameUI( QWidget* Parent, Qt::WindowFlags Flags )
    : QDialog( Parent, Flags )
{
    Ui.setupUi( this );

    Ui.cbxSRApplyRange->setEditable( true );
    Ui.cbxSRApplyRange->lineEdit()->setReadOnly( true );
    Ui.cbxSRApplyRange->lineEdit()->setAlignment( Qt::AlignCenter );

    View = Ui.grdPreview->view< GridBandedTableView >();

    View->beginUpdate();
    auto& BaseOpts = View->options();
    auto& TableOpts = View->tableOptions();
    auto& BandedOpts = View->bandedOptions();

    BaseOpts.setAutoCreateColumns( false );
    BaseOpts.setGroupsHeader( false );
    BaseOpts.setFocusFrameEnabled( false );
    BaseOpts.setAllowFilterEditor( false );
    BaseOpts.setFieldChooserEnabled( false );
    BaseOpts.setMainMenuDisabled( true );
    BaseOpts.setScrollBarsTransparent( true );

    TableOpts.setColumnAutoWidth( true );
    TableOpts.setColumnsQuickMenuVisible( false );
    TableOpts.setColumnsQuickCustomization( false );
    TableOpts.setRowsQuickSelection( false );
    TableOpts.setRowFrozenButtonVisible( false );

    BandedOpts.setBandsHeader( false );
    BandedOpts.setBandsQuickCustomization( false );
    
    View->removeBands();
    View->addBand( tr( "FS" ) );

    Model = new QMultiRenameModel;
    View->setModel( Model );
    
    Qtitan::GridBandedTableColumn* GridColumn = nullptr;
    GridColumn = View->addColumn( 0, tr("원본 파일이름") );
    GridColumn->setBandIndex( 0 );
    GridColumn->setTextAlignment( Qt::AlignCenter );
    GridColumn->editorRepository()->setAlignment( Qt::AlignLeft );
    GridColumn->setAutoWidth( true );
    
    GridColumn = View->addColumn( 1, tr( "원본 확장자" ) );
    GridColumn->setBandIndex( 0 );
    GridColumn->setWidth( 220 );
    GridColumn->setMinWidth( 220 );
    GridColumn->setMaxWidth( 220 );
    GridColumn->setTextAlignment( Qt::AlignCenter );
    GridColumn->editorRepository()->setAlignment( Qt::AlignLeft );
    GridColumn->setAutoWidth( false );

    GridColumn = View->addColumn( 2, tr( "크기" ) );
    GridColumn->setBandIndex( 0 );
    GridColumn->setWidth( 160 );
    GridColumn->setMinWidth( 160 );
    GridColumn->setMaxWidth( 160 );
    GridColumn->setTextAlignment( Qt::AlignCenter );
    GridColumn->setAutoWidth( false );

    GridColumn = View->addColumn( 3, tr( "날짜" ) );
    GridColumn->setBandIndex( 0 );
    GridColumn->setWidth( 160 );
    GridColumn->setMinWidth( 160 );
    GridColumn->setMaxWidth( 160 );
    GridColumn->setTextAlignment( Qt::AlignCenter );
    GridColumn->setAutoWidth( false );

    GridColumn = View->addColumn( 4, tr( "대상 파일이름" ) );
    GridColumn->setBandIndex( 0 );
    GridColumn->setRowIndex( 1 );
    GridColumn->setTextAlignment( Qt::AlignCenter );
    GridColumn->editorRepository()->setAlignment( Qt::AlignLeft );
    GridColumn->setAutoWidth( true );

    GridColumn = View->addColumn( 5, tr( "대상 확장자" ) );
    GridColumn->setBandIndex( 0 );
    GridColumn->setRowIndex( 1 );
    GridColumn->setWidth( 220 );
    GridColumn->setMinWidth( 220 );
    GridColumn->setMaxWidth( 220 );
    GridColumn->setTextAlignment( Qt::AlignCenter );
    GridColumn->editorRepository()->setAlignment( Qt::AlignLeft );
    GridColumn->setAutoWidth( false );

    GridColumn = View->addColumn( 6, tr( "위치" ) );
    GridColumn->setBandIndex( 0 );
    GridColumn->setRowIndex( 1 );
    GridColumn->setWidth( 333 );
    GridColumn->setMinWidth( 333 );
    GridColumn->setMaxWidth( 333 );
    GridColumn->setTextAlignment( Qt::AlignCenter );
    GridColumn->setAutoWidth( false );

    View->endUpdate();

    View->bestFit( Qtitan::BestFitModeFlag::FitToHeader );
}

void QMultiRenameUI::SetSourceFiles( const QVector<QString>& VecFiles )
{
    Model->SetSourceFiles( VecFiles );

    {
        QSignalBlocker Blocker( Ui.edtFileNamePattern );
        Ui.edtFileNamePattern->setText( "[N]" );
    }

    {
        QSignalBlocker Blocker( Ui.edtFileExtPattern );
        Ui.edtFileExtPattern->setText( "[E]" );
    }

    Model->SetRenamePattern( "[N]", "[E]" );
    Model->ProcessPattern();
}

void QMultiRenameUI::on_edtFileNamePattern_textChanged( const QString& Text )
{
    Model->SetRenamePattern( Text, Ui.edtFileExtPattern->text() );

    Model->ProcessPattern();
}

void QMultiRenameUI::on_edtFileExtPattern_textChanged( const QString& Text )
{
    Model->SetRenamePattern( Ui.edtFileNamePattern->text(), Text );
    Model->ProcessPattern();
}

void QMultiRenameUI::on_edtSearchString_textChanged( const QString& Text )
{
    Model->SetSearchReplace( Text, Ui.edtReplaceString->text(), Ui.chkIsCaseSensitive->isChecked(), Ui.chkIsOnlyOnce->isChecked(), Ui.cbxSRApplyRange->currentIndex() );
    Model->ProcessPattern();
}

void QMultiRenameUI::on_edtReplaceString_textChanged( const QString& Text )
{
    Model->SetSearchReplace( Ui.edtSearchString->text(), Text, Ui.chkIsCaseSensitive->isChecked(), Ui.chkIsOnlyOnce->isChecked(), Ui.cbxSRApplyRange->currentIndex() );
    Model->ProcessPattern();
}

void QMultiRenameUI::on_chkIsCaseSensitive_checkStateChanged( Qt::CheckState state )
{
    Model->SetSearchReplace( Ui.edtSearchString->text(), Ui.edtReplaceString->text(), state == Qt::Checked, Ui.chkIsOnlyOnce->isChecked(), Ui.cbxSRApplyRange->currentIndex() );
    Model->ProcessPattern();
}

void QMultiRenameUI::on_chkIsOnlyOnce_checkStateChanged( Qt::CheckState state )
{
    Model->SetSearchReplace( Ui.edtSearchString->text(), Ui.edtReplaceString->text(), Ui.chkIsCaseSensitive->isChecked(), state == Qt::Checked, Ui.cbxSRApplyRange->currentIndex() );
    Model->ProcessPattern();
}

void QMultiRenameUI::on_cbxSRApplyRange_currentIndexChanged( int index )
{
    Model->SetSearchReplace( Ui.edtSearchString->text(), Ui.edtReplaceString->text(), Ui.chkIsCaseSensitive->isChecked(), Ui.chkIsOnlyOnce->isChecked(), index );
    Model->ProcessPattern();
}

/// Footer
void QMultiRenameUI::on_btnRedo_clicked( bool checked )
{
}

void QMultiRenameUI::on_btnRun_clicked( bool checked )
{
    /*!
     * 1. 지정한 패턴대로 이름을 변경하였을 때 이름이 겹치지 않는지 확인
     * 2. 이름이 겹친다면 자동으로 이름을 변경할 것인지 확인
     * 3. 이름이 겹치지 않거나, 자동으로 변경하기로 하였다면 변경 시작. 
     */




}

void QMultiRenameUI::on_btnCancel_clicked( bool checked )
{
}

void QMultiRenameUI::on_btnResultList_clicked( bool checked )
{
}

void QMultiRenameUI::on_btnClose_clicked( bool checked )
{
    done( Accepted );
}
