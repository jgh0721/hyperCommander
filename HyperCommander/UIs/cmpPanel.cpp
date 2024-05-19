#include "stdafx.h"
#include "cmpPanel.hpp"

#include "UniqueLibs/columnMgr.hpp"
#include "UniqueLibs/builtInFsModel.hpp"

#include <QtitanGrid.h>
#include <shtypes.h>
#include <ShlObj.h>
#include <ShObjIdl.h>

#include "UniqueLibs/commandMgr.hpp"
#include "UniqueLibs/shortcutMgr.hpp"

CmpPanel::CmpPanel( QWidget* parent, Qt::WindowFlags f )
{
    ui.setupUi( this );

    RefreshVolumeList();

    while( ui.tabWidget->count() > 0 )
        ui.tabWidget->removeTab( 0 );

    QTimer::singleShot( 1, [this]() { AddTab(); } );
}

void CmpPanel::AddTab()
{
    const auto Index = InitializeGrid();
    ui.tabWidget->setCurrentIndex( Index );
}

void CmpPanel::CloseTab()
{
    // 마지막 탭이라면 닫지 않는다.
    if( ui.tabWidget->count() == 1 )
        return;

    auto Current = ui.tabWidget->currentIndex();
    mapTabToStats.remove( Current );
    delete ui.tabWidget->widget( Current );
    ui.tabWidget->removeTab( Current );
}

void CmpPanel::RefreshVolumeList()
{
    ui.cbxVolume->clear();

    /*!
    * 1. 실제 파일시스템 추가
    * 2. 네트워크 환경 추가
    * 
    */

    QFileIconProvider QFIP;
    const auto DriveIcon = QFIP.icon( QAbstractFileIconProvider::Drive );

    DWORD Drives = GetLogicalDrives();
    for( char idx = 0; idx < 26; ++idx )
    {
        if( ( Drives & ( 1 << idx ) ) != 0 )
        {
            CHAR driveName[] = { static_cast< CHAR >( 'A' + idx ), ':', '\\', '\0' };
            ui.cbxVolume->addItem( DriveIcon, driveName );
        }
    }
}

int CmpPanel::CurrentTabIndex() const
{
    return ui.tabWidget->currentIndex();
}

void CmpPanel::SetFocusView( int TabIndex )
{
    if( mapTabToStats.contains( TabIndex ) == true )
    {
        const auto View = mapTabToStats[ TabIndex ].View;

        View->grid()->setFocus( Qt::OtherFocusReason );
        View->grid()->viewport()->setFocus(Qt::MouseFocusReason);

        if( View->focusedRow().isValid() == false )
        {
            if( View->getRowCount() > 0 )
                View->setFocusedRowIndex( 0 );
        }
    }
}

int CmpPanel::InitializeGrid()
{
    const auto Page = new QWidget;
    const auto Vertical = new QVBoxLayout;
    Page->setLayout( Vertical );
    Page->setSizePolicy( QSizePolicy( QSizePolicy::Policy::Expanding, QSizePolicy::Policy::Expanding ) );

    const auto Grid = new Qtitan::Grid;
    Grid->setViewType( Qtitan::Grid::BandedTableView );
    Grid->setSizePolicy( QSizePolicy( QSizePolicy::Policy::Expanding, QSizePolicy::Policy::Expanding ) );

    Vertical->addWidget( Grid );

    // TODO: 탭 이름 => 현재 디렉토리, 만약 루트 디렉토리라면 드라이브 문자 함께 표시
    currentIndex = ui.tabWidget->addTab( Page, tr("D:/") );
    
    Grid->installEventFilter( this );
    Grid->setContextMenuPolicy( Qt::CustomContextMenu );

    const auto View = Grid->view< GridBandedTableView >();
    Grid->viewport()->setFocusPolicy( Qt::StrongFocus );

    View->beginUpdate();
    auto& BaseOpts = View->options();
    auto& TableOpts = View->tableOptions();
    auto& BandedOpts = View->bandedOptions();
    BaseOpts.setShowWaitCursor( true );
    BaseOpts.setGroupsHeader( false );
    BaseOpts.setScrollBarsTransparent( true );
    BaseOpts.setMainMenuDisabled( false );              // true 이면 
    BaseOpts.setAutoCreateColumns( false );
    BaseOpts.setColumnMovingEnabled( false );
    BaseOpts.setAlternatingRowColors( false );
    BaseOpts.setScrollRowStyle( Qtitan::ScrollByPixel );
    BaseOpts.setGridLines( Qtitan::LinesNone );
    BaseOpts.setModelDecoration( true );
    BaseOpts.setModelItemsDragEnabled( true );
    BaseOpts.setRowStyle( Qtitan::GridViewOptions::RowStyleNormal );
    BaseOpts.setSelectedDecorationOpacity( 0.4f );
    BaseOpts.setSelectionPolicy( Qtitan::GridViewOptions::IgnoreSelection );
    BaseOpts.setFieldChooserEnabled( false );
    BaseOpts.setRubberBandSelection( false );
    BaseOpts.setTransparentBackground( true );
    BaseOpts.setGroupsHeaderText( "Fdsfds" );
    BaseOpts.setColumnHeaderHints( true );

    BaseOpts.setFocusFollowsMouse( false );
    BaseOpts.setFocusFrameEnabled( true );
    BaseOpts.setShowFocusDecoration( true );
    BaseOpts.setKeepFocusedRow( false );

    BaseOpts.setDragEnabled( true );
    BaseOpts.setDropEnabled( true );
    BaseOpts.setFieldChooserEnabled( false );
    
    BaseOpts.setGroupsHeaderTextColor( "white" );
    TableOpts.setRowsQuickSelection( false );
    TableOpts.setRowFrozenButtonVisible( false );
    TableOpts.setColumnsQuickCustomization( false );
    
    BandedOpts.setBandsHeader( false );
    BandedOpts.setBandsQuickCustomization( false );
    

    const auto StColumnMgr = new CColumnMgr;
    StColumnMgr->Initialize();
    const auto ColumnView = StColumnMgr->GetColumnView( 0 );

    const auto Model = new FSModel;
    Model->SetRoot( "C:" );
    Model->SetCurrentPath( "\\" );
    Model->SetColumnView( ColumnView );
    connect( Model, &FSModel::sigChangedDirectory, this, &CmpPanel::oo_ChangedDirectory );

    View->removeBands();
    View->addBand( tr( "FS" ) );

    // TODO: CColumnMgr 은 당연히 전역 싱글턴으로 분리되어야 한다. 
    View->setModel( Model );
    
    BaseOpts.setBackgroundColor( "black" );
    
    // 모든 단축키 등 해제
    for( const auto& act : View->actions() )
    {
        act->disconnect();
        act->setShortcut( {} );
    }
    
    Qtitan::GridBandedTableColumn* GridColumn = nullptr;
    
    // GridColumn = View->addColumn( 0, )
    
    for( const auto& Column : ColumnView.VecColumns )
    {
        GridColumn = View->addColumn( Column.Index, Column.Name );
        if( GridColumn != nullptr )
        {
            GridColumn->setBandIndex( 0 );
            GridColumn->setAutoWidth( true );
            GridColumn->setRowIndex( Column.Row );
            GridColumn->setWidth( 100 );
            GridColumn->setTextAlignment( Column.Align );
            GridColumn->setTextColor( "silver" );
            GridColumn->setDecorationColor( "blue" );   // 컬럼 배경 색
            
            if( Column.Content.contains( "HC.name", Qt::CaseInsensitive ) == true )
            {
                GridColumn->setSortOrder( Qtitan::SortAscending );
                GridColumn->dataBinding()->setSortRole( Qt::UserRole );
            }
            GridColumn->setFilterButtonVisible( false );
        }
    }
    
    connect( View, &Qtitan::GridViewBase::contextMenu, this, &CmpPanel::oo_grdLocal_contextMenu );
    connect( View, &Qtitan::GridViewBase::columnClicked, this, [=]( ColumnClickEventArgs* Args ) {
        qDebug() << Args;
    } );
    connect( View, &Qtitan::GridViewBase::selectionChanged, this, [ = ]( GridSelection* selection, GridSelection* oldSelection ) {
        qDebug() << "Selected = " << selection->selectedRowIndexes().size();

        for( const auto& Row : selection->selectedRowIndexes() )
            qDebug() << Row;
        
    } );

    View->endUpdate();

    TyTabState TabState{ currentIndex, View };
    mapTabToStats[ currentIndex ] = TabState;

    Model->ChangeDirectory( "" );
    View->bestFit( FitToHeaderAndContent );

    return currentIndex;
}

void CmpPanel::oo_ChangedDirectory( const QString& CurrentPath )
{
    QDir dir( CurrentPath );
    if( dir.exists() == true )
    {
        ui.tabWidget->setTabText( ui.tabWidget->currentIndex(), CurrentPath.left( 2 ) + dir.dirName() );
    }
    else
    {
        ui.tabWidget->setTabText( ui.tabWidget->currentIndex(), CurrentPath );
    }

    emit sig_NotifyCurrentDirectory( CurrentPath );
}

void CmpPanel::on_cbxVolume_currentIndexChanged( int index )
{
    // TODO: 실제 파일시스템 외의 것들은 예외처리 필요함
    ;

    if( mapTabToStats.contains( ui.tabWidget->currentIndex() ) == true )
    {
        const auto View = mapTabToStats.value( ui.tabWidget->currentIndex() ).View;
        const auto Model = ( FSModel* )View->model();
        Model->SetRoot( ui.cbxVolume->currentText().left( 2 ) );
        Model->SetCurrentPath( "/" );
        Model->ChangeDirectory( "" );
    }
}

void CmpPanel::on_tabWidget_currentChanged( int Index )
{   
    if( mapTabToStats.contains( Index ) == false )
        return;

    const auto View = mapTabToStats.value( Index ).View;

    QTimer::singleShot( 0, [View]() {
        View->grid()->setFocus();
        View->grid()->viewport()->setFocus();
        View->grid()->viewport()->raise();
        View->grid()->viewport()->activateWindow();
                        } );


    //Current = ui.tabWidget->currentIndex();
    //if( mapTabToStats.contains( Current ) == true )
    //{
    //    QTimer::singleShot( 10, [this, Current]() { mapTabToStats.value( Current ).View->grid()->setFocus(); } );
    //    ;
    //}
        //QTimer::singleShot( 10, [this, Index]() { 
    //    mapTabToStats.value( Index ).View->grid()->setFocus(); 
    //    mapTabToStats.value( Index ).View->grid()->viewport()->setFocus();
    //                    } );

}

bool openShellContextMenuForObject( const std::wstring& path, int xPos, int yPos, void* parentWindow )
{
    //assert( parentWindow );
    ITEMIDLIST* id = nullptr;
    IShellFolder* ifolder = nullptr;
    IContextMenu* imenu = nullptr;
    std::wstring windowsPath = path;
    bool IsSuccess = false;

    do
    {
        //std::replace( windowsPath.begin(), windowsPath.end(), '/', '\');
        HRESULT result = SHParseDisplayName( windowsPath.c_str(), 0, &id, 0, 0 );
        if( !SUCCEEDED( result ) || !id )
            break;

        LPCITEMIDLIST idChild = 0;
        result = SHBindToParent( id, IID_IShellFolder, ( void** )&ifolder, &idChild );
        if( !SUCCEEDED( result ) || !ifolder )
            break;

        result = ifolder->GetUIObjectOf( ( HWND )parentWindow, 1, &idChild, IID_IContextMenu, 0, reinterpret_cast< void** >( &imenu ) );
        if( !SUCCEEDED( result ) || !ifolder )
            break;

        HMENU hMenu = CreatePopupMenu();
        if( !hMenu )
            break;

        if( SUCCEEDED( imenu->QueryContextMenu( hMenu, 0, 1, 0x7FFF, CMF_NORMAL ) ) )
        {
            UINT Flags = TPM_RETURNCMD ;
            if( GetSystemMetrics( SM_MENUDROPALIGNMENT ) != 0 )
                Flags |= TPM_RIGHTALIGN | TPM_HORNEGANIMATION;
            else
                Flags |= TPM_LEFTALIGN | TPM_HORPOSANIMATION;

            int iCmd = TrackPopupMenuEx( hMenu, Flags, xPos, yPos, ( HWND )parentWindow, NULL );
            if( iCmd > 0 )
            {
                CMINVOKECOMMANDINFOEX info = { 0 };
                info.cbSize = sizeof( info );
                info.fMask = CMIC_MASK_UNICODE ;
                info.hwnd = ( HWND )parentWindow;
                info.lpVerb = MAKEINTRESOURCEA( iCmd - 1 );
                info.lpVerbW = MAKEINTRESOURCEW( iCmd - 1 );
                info.nShow = SW_SHOWNORMAL;
                imenu->InvokeCommand( ( LPCMINVOKECOMMANDINFO )&info );
            }
        }
        DestroyMenu( hMenu );

        IsSuccess = true;

    } while( false );

    if( imenu )
        imenu->Release();

    if( ifolder )
        ifolder->Release();

    if( id )
        CoTaskMemFree( id );

    return IsSuccess;
}

void CmpPanel::oo_grdLocal_contextMenu( Qtitan::ContextMenuEventArgs* Args )
{
    const auto ContextMenu = dynamic_cast< PopupMenu* >( Args->contextMenu() );

    const auto Info = Args->hitInfo();
    const auto Type = Info.info();
    const auto ModelIndex = Info.modelIndex();
    
    switch( Type )
    {
        case GridHitInfo::Cell: {

            // TODO: 현재 보이는 뷰의 내용에 따라 Model 클래스 변경
            const auto Grid = Args->view()->grid();
            const auto Pos = QCursor::pos() * Grid->screen()->devicePixelRatio();

            auto Model = reinterpret_cast< FSModel* >( Args->view()->model() );
            openShellContextMenuForObject( Model->GetFileFullPath( ModelIndex ).toStdWString(), Pos.x(), Pos.y(),
                                           reinterpret_cast< HWND >( Args->view()->grid()->winId() ) );
        };
    }

    Args->setHandled( true );
}

bool CmpPanel::eventFilter( QObject* Object, QEvent* Event )
{
    if( Event->type() == QEvent::KeyPress )
    {
        const auto StCommandMgr = TyStCommandMgr::GetInstance();
        const auto StShortcutMgr = TyStShortcutMgr::GetInstance();
        
        const auto Grid = qobject_cast< Qtitan::Grid* >( Object );
        const auto KeyEvent = static_cast< QKeyEvent* >( Event );
        
        if( KeyEvent->key() == Qt::Key_T && KeyEvent->modifiers() == Qt::ControlModifier )
        {
            AddTab();
            return true;
        }

        if( KeyEvent->key() == Qt::Key_W && KeyEvent->modifiers() == Qt::ControlModifier  )
        {
            CloseTab();
            return true;
        }

        if( Grid != nullptr )
        {
            const auto View = Grid->view< Qtitan::GridBandedTableView >();
            const auto Row = View->focusedRow();

            const auto CmdText = StShortcutMgr->GetCMDFromShortcut( KeyEvent->keyCombination() );

            if( KeyEvent->key() == Qt::Key_R && KeyEvent->modifiers() == ( Qt::ControlModifier | Qt::AltModifier ) )
            {
                StCommandMgr->CMD_MultiRename( View, QCursor::pos(), Row.modelIndex( 0 ) );
                return true;
            }

            if( ( KeyEvent->key() == Qt::Key_Enter || KeyEvent->key() == Qt::Key_Return ) )
            {
                StCommandMgr->CMD_Return( View, QCursor::pos(), Row.modelIndex( 0 ) );
            }
            else if( KeyEvent->key() == Qt::Key_Space )
            {
                StCommandMgr->CMD_Space( View, QCursor::pos(), Row.modelIndex( 0 ) );
            }
            else if( KeyEvent->key() == Qt::Key_Tab )
            {
                StCommandMgr->CMD_TabSwitch( View, QCursor::pos(), Row.modelIndex( 0 ) );
                return true;
            }

            QRect Rect;
            PopupMenu menu( Grid );

            //for( const auto Info : ui.grdLocal->hitInfoAll() )
            //{
            //    if( Info.info() != GridHitInfo::Cell )
            //        continue;

            //    if( Info.columnIndex() != TBL_LOCAL_HDX_MODIFIED.index )
            //        continue;

            //    if( Info.row().rowIndex() == Row.rowIndex() )
            //    {
            //        Rect = Info.rect();
            //        break;
            //    }
            //}

            //GridHitInfo HitInfo( GridHitInfo::Cell, spLocalView_, Rect, Row.rowIndex(), Row.cell( 0 ).columnIndex() );
            //ContextMenuEventArgs Args( spLocalView_, &menu, HitInfo );
            //oo_grdLocal_contextMenu( &Args );

            // TODO: Tab 키 일 때 옆으로 포커스 이동
            // qDebug() << Object << Object->objectName() << Event << CmdText;
            int a = 0;
        }
    }

    return false;
}

void CmpPanel::focusInEvent( QFocusEvent* event )
{
    QWidget::focusInEvent( event );
}

/*!

    QtitanDataGrid 
        SetModel -> FSModel
        CurrentView 에 따라 헤더 생성






*/
