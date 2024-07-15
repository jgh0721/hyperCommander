#include "stdafx.h"
#include "dlgFavoriteDir.hpp"

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
    // TODO: 이름 중복 체크
}

void QFavoriteDirConfigure::on_btnRemove_clicked( bool checked )
{
    const auto Current = ui.lstDir->currentItem();
    if( Current == nullptr )
        return;

    // 해당 이름으로 Remove, 그후 SaveSettings 및 Refresh, UI 새로고침
}

void QFavoriteDirConfigure::on_btnRename_clicked( bool checked )
{
    const auto Current = ui.lstDir->currentItem();
    if( Current == nullptr )
        return;

    // TODO: QInputDialog 로 변경할 이름 입력 받음
    // TODO: Rename 함수 필요함
}

void QFavoriteDirConfigure::on_btnOK_clicked( bool checked )
{
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
