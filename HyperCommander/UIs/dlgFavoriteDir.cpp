#include "stdafx.h"
#include "dlgFavoriteDir.hpp"

#include "cmnHCUtils.hpp"
#include "solFavoriteDirMgr.hpp"

DECLARE_CMNLIBSV2_NAMESPACE

QFavoriteDirConfigure::QFavoriteDirConfigure( QWidget* Parent, Qt::WindowFlags Flags )
    : nsHC::QBaseUI( Parent, Flags )
{
    ui.setupUi( this );

    refresh();
}

void QFavoriteDirConfigure::on_btnClose_clicked( bool checked )
{
    on_btnCancel_clicked( checked );
}

void QFavoriteDirConfigure::on_lstDir_currentItemChanged( QListWidgetItem* Current, QListWidgetItem* Previous )
{
    UNREFERENCED_PARAMETER( Current );
    UNREFERENCED_PARAMETER( Previous );

    const auto Name = Current->text();
    refreshItem( Name );
}

void QFavoriteDirConfigure::on_btnAdd_clicked( bool checked )
{
    bool Is = false;
    const auto StFavorites = TyStFavoriteDirMgr::GetInstance();

    do
    {
        QInputDialog Ui;

        Ui.setLabelText( tr( "항목의 이름을 지정해 주세요:" ) );
        Ui.setOkButtonText( tr( "확인(&O)" ) );
        Ui.setCancelButtonText( tr( "취소(&C)" ) );
        Ui.setTextValue( tr( "새 항목" ) );
        Ui.setOptions( QInputDialog::UsePlainTextEditForTextInput );
        Ui.setInputMode( QInputDialog::TextInput );
        if( Ui.exec() == QDialog::Rejected )
            return;

        const auto Name = Ui.textValue();

        if( StFavorites->GetNames().contains( Name, Qt::CaseInsensitive ) == true )
        {
            ::ShowMSGBox( this, QMessageBox::Information, tr( "이미 있는 이름입니다." ) );
            Is = true;
        }
        else
        {
            ui.lstDir->addItem( Name );
            StFavorites->SetItem( { Name, "", "" } );
            Is = false;
        }

    } while( Is == true );
}

void QFavoriteDirConfigure::on_btnRemove_clicked( bool checked )
{
    const auto Current = ui.lstDir->currentItem();
    if( Current == nullptr )
        return;

    // 해당 이름으로 Remove, 그후 SaveSettings 및 Refresh, UI 새로고침

    const auto StFavorites = TyStFavoriteDirMgr::GetInstance();
    StFavorites->RemoveItem( Current->text() );

    refresh();
    refreshItem( "" );
}

void QFavoriteDirConfigure::on_btnRename_clicked( bool checked )
{
    const auto Current = ui.lstDir->currentItem();
    if( Current == nullptr )
        return;

    const auto StFavorites = TyStFavoriteDirMgr::GetInstance();

    // TODO: QInputDialog 로 변경할 이름 입력 받음
    // TODO: Rename 함수 필요함
}

void QFavoriteDirConfigure::on_edtCommand_editingFinished()
{
    CFavoriteDirMgr::TyFavoriteDir Item;
    const auto StFavorites = TyStFavoriteDirMgr::GetInstance();

    Item.Name       = ui.lstDir->currentItem()->text();
    Item.Command    = ui.edtCommand->text();
    Item.Path       = ui.edtPath->text();

    StFavorites->SetItem( Item );
}

void QFavoriteDirConfigure::on_edtPath_editingFinished()
{
    on_edtCommand_editingFinished();
}

void QFavoriteDirConfigure::on_btnOK_clicked( bool checked )
{
    on_edtCommand_editingFinished();

    done( Accepted );
}

void QFavoriteDirConfigure::on_btnCancel_clicked( bool checked )
{
    done( Rejected );
}

void QFavoriteDirConfigure::on_rdoDrivesOnNone_toggled( bool checked )
{
    const auto StFavorites = TyStFavoriteDirMgr::GetInstance();
    StFavorites->SetDrivesOn( CFavoriteDirMgr::DRIVES_ON_NONE );
    StFavorites->SaveSettings();
}

void QFavoriteDirConfigure::on_rdoDrivesOnTop_toggled( bool checked )
{
    const auto StFavorites = TyStFavoriteDirMgr::GetInstance();
    StFavorites->SetDrivesOn( CFavoriteDirMgr::DRIVES_ON_TOP );
    StFavorites->SaveSettings();
}

void QFavoriteDirConfigure::on_rdoDrivesOnBottom_toggled( bool checked )
{
    const auto StFavorites = TyStFavoriteDirMgr::GetInstance();
    StFavorites->SetDrivesOn( CFavoriteDirMgr::DRIVES_ON_BOTTOM );
    StFavorites->SaveSettings();
}

void QFavoriteDirConfigure::refresh()
{
    const auto StFavorites = TyStFavoriteDirMgr::GetInstance();

    QSignalBlocker Blocker( ui.lstDir );
    ui.lstDir->clear();

    for( int idx = 0; idx < StFavorites->GetCount(); ++idx )
    {
        const auto& Item = StFavorites->GetItem( idx );
        ui.lstDir->addItem( Item.Name );
    }

    ui.lstDir->setCurrentItem( nullptr );
}

void QFavoriteDirConfigure::refreshItem( const QString& Name )
{
    ui.edtCommand->setText( "" );
    ui.edtPath->setText( "" );

    if( Name.isEmpty() == true )
        return;

    const auto StFavorites = TyStFavoriteDirMgr::GetInstance();

    const auto DrivesOn = StFavorites->GetDrivesOn();

    if( DrivesOn == CFavoriteDirMgr::DRIVES_ON_NONE )
        ui.rdoDrivesOnNone->setChecked( true );
    if( DrivesOn == CFavoriteDirMgr::DRIVES_ON_TOP )
        ui.rdoDrivesOnTop->setChecked( true );
    if( DrivesOn == CFavoriteDirMgr::DRIVES_ON_BOTTOM )
        ui.rdoDrivesOnBottom->setChecked( true );

    const auto Item = StFavorites->GetItem( Name );

    ui.edtCommand->setText( Item.Command );
    ui.edtPath->setText( Item.Path );
}
