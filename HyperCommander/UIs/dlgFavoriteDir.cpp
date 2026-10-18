#include "stdafx.h"
#include "dlgFavoriteDir.hpp"

#include "cmnHCUtils.hpp"
#include "solFavoriteDirMgr.hpp"

DECLARE_CMNLIBSV2_NAMESPACE

QFavoriteDirConfigure::QFavoriteDirConfigure( QWidget* Parent, Qt::WindowFlags Flags )
    : nsHC::QBaseUI( Parent, Flags )
{
    ui.setupUi( this );

    const auto Favorites = TyStFavoriteDirMgr::GetInstance();
    for( qsizetype Index = 0; Index < Favorites->GetCount(); ++Index )
        items_.append( Favorites->GetItem( Index ) );
    drivesOn_ = Favorites->GetDrivesOn();
    // Populate controls without invoking persistence through toggled signals.
    const QSignalBlocker None( ui.rdoDrivesOnNone ), Top( ui.rdoDrivesOnTop ), Bottom( ui.rdoDrivesOnBottom );
    ui.rdoDrivesOnNone->setChecked( drivesOn_ == CFavoriteDirMgr::DRIVES_ON_NONE );
    ui.rdoDrivesOnTop->setChecked( drivesOn_ == CFavoriteDirMgr::DRIVES_ON_TOP );
    ui.rdoDrivesOnBottom->setChecked( drivesOn_ == CFavoriteDirMgr::DRIVES_ON_BOTTOM );
    connect( ui.edtCommand, &QLineEdit::textEdited, this, [this] { on_edtCommand_editingFinished(); } );
    connect( ui.edtPath, &QLineEdit::textEdited, this, [this] { on_edtPath_editingFinished(); } );
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

    const auto Name = Current ? Current->text() : QString{};
    refreshItem( Name );
}

QString QFavoriteDirConfigure::promptName( const QString& Initial, int ExcludedRow )
{
    QString Candidate = Initial;
    for( ;; )
    {
        bool Accepted = false;
        Candidate = QInputDialog::getText( this, tr( "즐겨찾기 이름" ), tr( "항목의 이름을 지정해 주세요:" ),
            QLineEdit::Normal, Candidate, &Accepted ).trimmed();
        if( !Accepted ) return {};
        bool Valid = !Candidate.isEmpty() && !Candidate.contains( QRegularExpression( "[\\r\\n\\x{2028}\\x{2029}]" ) );
        for( qsizetype Index = 0; Index < items_.size(); ++Index )
            if( Index != ExcludedRow && items_[ Index ].Name.compare( Candidate, Qt::CaseInsensitive ) == 0 ) Valid = false;
        if( Valid ) return Candidate;
        ::ShowMSGBox( this, QMessageBox::Information, tr( "빈 이름, 여러 줄 이름 또는 이미 있는 이름은 사용할 수 없습니다." ) );
    }
}

void QFavoriteDirConfigure::on_btnAdd_clicked( bool checked )
{
    const auto Name = promptName( tr( "새 항목" ) );
    if( Name.isEmpty() ) return;
    items_.append( { Name, "", "" } );
    ui.lstDir->addItem( Name );
    ui.lstDir->setCurrentRow( ui.lstDir->count() - 1 );
}

void QFavoriteDirConfigure::on_btnRemove_clicked( bool checked )
{
    const auto Current = ui.lstDir->currentItem();
    if( Current == nullptr )
        return;

    items_.removeAt( ui.lstDir->row( Current ) );

    refresh();
    refreshItem( "" );
}

void QFavoriteDirConfigure::on_btnRename_clicked( bool checked )
{
    const auto Current = ui.lstDir->currentItem();
    if( Current == nullptr ) return;
    on_edtCommand_editingFinished();
    const auto Name = promptName( Current->text(), ui.lstDir->currentRow() );
    if( Name.isEmpty() ) return;
    items_[ ui.lstDir->currentRow() ].Name = Name;
    Current->setText( Name );
}

void QFavoriteDirConfigure::on_edtCommand_editingFinished()
{
    CFavoriteDirMgr::TyFavoriteDir Item;

    const auto Current = ui.lstDir->currentItem();
    if( Current == nullptr ) return;

    Item.Name       = Current->text();
    Item.Command    = ui.edtCommand->text();
    Item.Path       = ui.edtPath->text();

    items_[ ui.lstDir->currentRow() ] = Item;
}

void QFavoriteDirConfigure::on_edtPath_editingFinished()
{
    on_edtCommand_editingFinished();
}

void QFavoriteDirConfigure::on_btnOK_clicked( bool checked )
{
    on_edtCommand_editingFinished();

    if( !TyStFavoriteDirMgr::GetInstance()->ApplyConfiguration( items_, drivesOn_ ) )
    {
        ::ShowMSGBox( this, QMessageBox::Warning, tr( "즐겨찾기를 저장하지 못했습니다. 설정 파일의 쓰기 권한을 확인한 뒤 다시 시도하세요." ) );
        return;
    }
    done( Accepted );
}

void QFavoriteDirConfigure::on_btnCancel_clicked( bool checked )
{
    done( Rejected );
}

void QFavoriteDirConfigure::on_rdoDrivesOnNone_toggled( bool checked )
{

    if( checked ) drivesOn_ = CFavoriteDirMgr::DRIVES_ON_NONE;
}

void QFavoriteDirConfigure::on_rdoDrivesOnTop_toggled( bool checked )
{

    if( checked ) drivesOn_ = CFavoriteDirMgr::DRIVES_ON_TOP;
}

void QFavoriteDirConfigure::on_rdoDrivesOnBottom_toggled( bool checked )
{

    if( checked ) drivesOn_ = CFavoriteDirMgr::DRIVES_ON_BOTTOM;
}

void QFavoriteDirConfigure::refresh()
{

    QSignalBlocker Blocker( ui.lstDir );
    ui.lstDir->clear();

    for( const auto& Item : items_ )
    {
        ui.lstDir->addItem( Item.Name );
    }

    ui.lstDir->setCurrentItem( nullptr );
    refreshItem( {} );
}

void QFavoriteDirConfigure::refreshItem( const QString& Name )
{
    const bool HasItem = !Name.isEmpty();
    ui.edtCommand->setEnabled( HasItem );
    ui.edtPath->setEnabled( HasItem );
    ui.btnSelectCMD->setEnabled( HasItem );
    ui.btnRemove->setEnabled( HasItem );
    ui.btnRename->setEnabled( HasItem );

    ui.edtCommand->setText( "" );
    ui.edtPath->setText( "" );

    if( Name.isEmpty() == true )
        return;

    const auto Item = items_.value( ui.lstDir->currentRow() );

    ui.edtCommand->setText( Item.Command );
    ui.edtPath->setText( Item.Path );
}
