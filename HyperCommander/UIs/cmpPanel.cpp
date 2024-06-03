#include "stdafx.h"
#include "cmpPanel.hpp"
#include "dlgGridStyle.hpp"

#include "UniqueLibs/columnMgr.hpp"
#include "UniqueLibs/builtInFsModel.hpp"
#include "UniqueLibs/externalEditorMgr.hpp"

#include <shtypes.h>
#include <ShlObj.h>
#include <ShObjIdl.h>

#include <QtitanGrid.h>
#include "externalLibs/QtitanDataGrid/src/src/base/QtnCommonStyle.h"
#include "UniqueLibs/commandMgr.hpp"
#include "UniqueLibs/shortcutMgr.hpp"

CmpPanel::CmpPanel( QWidget* parent, Qt::WindowFlags f )
{
    ui.setupUi( this );

    Qtitan::CommonStyle::ensureStyle();
}

void CmpPanel::Initialize()
{
    RefreshVolumeList();

    while( ui.tabWidget->count() > 0 )
        ui.tabWidget->removeTab( 0 );

    AddTab();
}

void CmpPanel::AddTab()
{
    qDebug() << this << "0";
    const auto Index = InitializeGrid();
    SetFocusView( Index );
    qDebug() << this << "1";
}

void CmpPanel::PrevTab()
{
    // 탭이 하나라면 아무 것도 하지 않는다. 
    if( ui.tabWidget->count() <= 1 )
        return;

    const auto Current = ui.tabWidget->currentIndex();
    Q_ASSERT( Current >= 0 );
    if( Current == 0 )
        ui.tabWidget->setCurrentIndex( ui.tabWidget->count() - 1 );
    else
        ui.tabWidget->setCurrentIndex( Current - 1 );
}

void CmpPanel::NextTab()
{
    // 탭이 하나라면 아무 것도 하지 않는다. 
    if( ui.tabWidget->count() <= 1 )
        return;

    const auto Current = ui.tabWidget->currentIndex();
    Q_ASSERT( Current >= 0 );
    if( Current == ui.tabWidget->count() - 1 )
        ui.tabWidget->setCurrentIndex( 0 );
    else
        ui.tabWidget->setCurrentIndex( Current + 1 );
}

void CmpPanel::CloseTab()
{
    // 마지막 탭이라면 닫지 않는다.
    if( ui.tabWidget->count() == 1 )
        return;

    TySpTabState State;
    auto Current = ui.tabWidget->currentIndex();

    State = retrieveFocusState();
    if( State != nullptr )
        vecTabStates.removeAt( Current );

    int New = -1;
    // 첫 번째 탭이라면 다음 탭으로
    if( Current == 0 )
        New = 1;
    // 마지막 탭이라면 이전 탭으로
    else if( Current == ui.tabWidget->count() - 1 )
        New = Current - 1;
    else
        New = Current + 1;

    ui.tabWidget->setCurrentIndex( New );
    const auto w = ui.tabWidget->widget( Current );
    ui.tabWidget->removeTab( Current );
    delete w;
    SetFocusView( New );
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

    QSignalBlocker Blocker( ui.cbxVolume );
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
    auto View = retrieveFocusView();

    if( View == nullptr )
        return;

    View->grid()->setFocus( Qt::MouseFocusReason );
    View->grid()->viewport()->setFocus( Qt::MouseFocusReason );

    EnsureKeyboardFocusOnView( View );
}

Qtitan::GridBandedTableView* CmpPanel::GetFocusView() const
{
    return retrieveFocusView();
}

void CmpPanel::EnsureKeyboardFocusOnView( Qtitan::GridBandedTableView* View )
{
    Q_ASSERT( View != nullptr );

    if( View->getRowCount() > 0 )
    {
        View->navigateDown();
        View->navigateUp();
    }
}

void CmpPanel::SelectRowOnCurrentTab( const QModelIndex& SrcIndex, bool IsMoveDown )
{
    const auto View = retrieveFocusView();
    Q_ASSERT( View != nullptr );
    if( View == nullptr )
        return;

    View->selectRow( View->getRow( SrcIndex ).rowIndex(), Qtitan::Invert );
    if( IsMoveDown == true )
        View->navigateDown( Qt::NoModifier );
}

void CmpPanel::ReturnOnCurrentTab( const QModelIndex& SrcIndex )
{
    auto State = retrieveFocusState();
    if( State == nullptr )
        return;

    const auto ModelIndex = State->ProxyModel->mapToSource( SrcIndex );
    const auto EntryInfo = State->Model->GetFileInfo( ModelIndex );

    if( EntryInfo.Attiributes & FILE_ATTRIBUTE_DIRECTORY )
    {
        State->Model->ChangeDirectory( EntryInfo.Name );
    }
    else
    {
        // TODO: 해당 항목이 내부 진입이 가능한 파일인지 확인한 후 아닐 때 실행한다.
        // NOTE: 토탈 커맨더의 경우 .DLL 등 명시적인 실행파일이 아닌 경우 레지스트리를 뒤져 실행가능하지 않다면 ShellExecuteExW 를 호출하지 않고, 오류창을 표시한다. 
        CoInitializeEx( NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE );
        std::wstring Src = State->Model->GetFileFullPath( ModelIndex ).toStdWString();

        do
        {
            SHELLEXECUTEINFOW shinfo = { 0, };
            shinfo.cbSize = sizeof( shinfo );
            shinfo.nShow = SW_NORMAL;
            shinfo.lpVerb = L"open";
            shinfo.lpFile = Src.c_str();

            ShellExecuteExW( &shinfo );

        } while( false );

        CoUninitialize();
    }
}

void CmpPanel::ContextMenuOnCurrentTab( const QModelIndex& SrcIndex )
{
    const auto View = retrieveFocusView();
    const auto Grid = View->grid();

    QRect Rect;
    PopupMenu menu( Grid );
    const auto Row = View->getRow( SrcIndex );

    for( const auto Info : Grid->hitInfoAll() )
    {
        if( Info.info() != GridHitInfo::Cell )
            continue;

        if( Info.row().rowIndex() == Row.rowIndex() )
        {
            Rect = Info.rect();
            break;
        }
    }
    
    GridHitInfo HitInfo( GridHitInfo::Cell, View, Rect, Row.rowIndex(), Row.cell( 0 ).columnIndex() );
    ContextMenuEventArgs Args( View, &menu, HitInfo );
    oo_grdLocal_contextMenu( &Args );
}

void CmpPanel::ExternalEditorMenu( const QModelIndex& SrcIndex )
{
    const auto StExternalMgr = TyStExternalEditorMgr::GetInstance();
    const auto State = retrieveFocusState();
    const auto View = State->View;

    QMenu Menu;
    Menu.setFocus( Qt::ActiveWindowFocusReason );
    Menu.setFocusPolicy( Qt::StrongFocus );
    Menu.setFont( QFont( "Sarasa Mono K Light", 14 ) );
    const auto FileFullPath = State->Model->GetFileFullPath( State->ProxyModel->mapToSource( SrcIndex ) );
    StExternalMgr->ConstructExternalMenu( &Menu, FileFullPath );

    // TODO: 글로벌 위치를 획득한 후, 해당 위치가 항목(SrcIndex) 영역 내에 위치하는 지 확인한다. 
    // 영역내에 위치한다면 해당 위치에 표시하고, 커서가 전혀 다른 곳에 위치한다면, 항목의 영역을 구하고 해당 위치내에서 표시한다. 
    POINT Pos = {};
    GetCursorPos( &Pos );

    qDebug() << QCursor::pos() << QPoint( Pos.x, Pos.y );

    QMetaObject::invokeMethod( &Menu, "setFocus", Qt::QueuedConnection );
    const auto ac = Menu.exec( retrieveMenuPoint( QCursor::pos(), SrcIndex) );

    // 취소하거나 등등
    if( ac == nullptr )
        return;
    
    const auto Editor = ac->data().value< TyExternalEditor >();
    if( Editor.FilePath.isEmpty() == true )
        return;

    // TODO: CMDLine 을 이용하여 FileFullPath 를 프로세스에 전달할 명령행으로 변환한다. 
    Editor.CMDLine;

    nsCmn::nsProcess::CreateProcessAsNormal( QDir::toNativeSeparators( Editor.FilePath ), FileFullPath, false, false );
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
    const auto currentIndex = ui.tabWidget->addTab( Page, tr("D:/") );
    ui.tabWidget->setCurrentIndex( currentIndex );

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
    BaseOpts.setSelectedDecorationOpacity( 0.2 );
    BaseOpts.setSelectionPolicy( Qtitan::GridViewOptions::IgnoreSelection );
    BaseOpts.setFieldChooserEnabled( false );
    BaseOpts.setRubberBandSelection( false );
    BaseOpts.setTransparentBackground( true );
    BaseOpts.setBackgroundColor( QColor( "cyan" ) );
    BaseOpts.setSelectedBackgroundColor( QColor( 255, 112, 43 ) );
    BaseOpts.setSelectedInverseColor( true );
    BaseOpts.setModelDecorationOpacityRole( FSModel::USR_ROLE_HIDDENOPACITY );

    BaseOpts.setGroupsHeaderText( "Fdsfds" );
    BaseOpts.setColumnHeaderHints( true );
    
    BaseOpts.setFocusFollowsMouse( false );
    BaseOpts.setFocusFrameEnabled( false );
    BaseOpts.setShowFocusDecoration( true );
    BaseOpts.setKeepFocusedRow( true );
    BaseOpts.setRowFocusMode( true );
    BaseOpts.setRowFocusColor( QColor( 51, 79, 102 ) );
    
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
    const auto ProxyModel = new FSProxyModel;

    Model->SetRoot( "C:" );
    Model->SetCurrentPath( "\\" );
    Model->SetColumnView( ColumnView );
    connect( Model, &FSModel::sigChangedDirectory, this, &CmpPanel::oo_ChangedDirectory );

    ProxyModel->setSourceModel( Model );

    View->removeBands();
    View->addBand( tr( "FS" ) );

    // TODO: CColumnMgr 은 당연히 전역 싱글턴으로 분리되어야 한다. 
    View->setModel( ProxyModel );
    
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
            GridColumn->setMinWidth( 30 );
            // GridColumn->setWidth( 100 );
            GridColumn->setTextAlignment( Column.Align );
            GridColumn->setTextColor( "silver" );
            GridColumn->setDecorationColor( "blue" );   // 컬럼 배경 색
            
            if( Column.Content.contains( "HC.Fs.name", Qt::CaseInsensitive ) == true )
            {
                GridColumn->setSortOrder( Qtitan::SortAscending );
                GridColumn->dataBinding()->setSortRole( Qt::UserRole );
            }
            GridColumn->setFilterButtonVisible( false );
        }
    }
    
    connect( View, &Qtitan::GridViewBase::contextMenu, this, &CmpPanel::oo_grdLocal_contextMenu );
    connect( View, &Qtitan::GridViewBase::cellClicked, this, [this]( CellClickEventArgs* Args ) {
        if( retrieveCurrentIndex() >= 0 )
        {
            if( GetAsyncKeyState( VK_CONTROL ) & 0x8000 )
            {
                SelectRowOnCurrentTab( Args->cell().modelIndex(), false );
                Args->setHandled( true );
            }
        }
    } );
    connect( View, &Qtitan::GridViewBase::columnClicked, this, [=]( ColumnClickEventArgs* Args ) {
        qDebug() << Args;
    } );
    connect( View, &Qtitan::GridViewBase::selectionChanged, this, [ = ]( GridSelection* selection, GridSelection* oldSelection ) {
        qDebug() << "Selected = " << selection->selectedRowIndexes().size();

        for( const auto& Row : selection->selectedRowIndexes() )
            qDebug() << Row;
    } );

    View->endUpdate();
    
    auto State = std::make_shared<TyTabState>( currentIndex, View, Model, ProxyModel );
    vecTabStates.push_back( State );

    Model->ChangeDirectory( "" );
    View->bestFit( FitToHeaderPercent );

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

    processPanelStatusText();

    emit sig_NotifyCurrentDirectory( CurrentPath );
}

void CmpPanel::on_cbxVolume_currentIndexChanged( int index )
{
    // TODO: 실제 파일시스템 외의 것들은 예외처리 필요함
    ;

    auto State = retrieveFocusState();
    if( State == nullptr )
        return;
    
    State->Model->SetRoot( ui.cbxVolume->currentText().left( 2 ) );
    State->Model->SetCurrentPath( "/" );
    State->Model->ChangeDirectory( "" );
}

void CmpPanel::on_btnGridStyle_clicked( bool checked )
{
    const auto View = retrieveFocusView();
    if( View == nullptr )
        return;

    QDlgGridStyle Styler;
    //Styler.setStyleText( mapTabToStats[ currentIndex ].View->grid()->styleSheet() );
    Styler.setStyleText( qApp->styleSheet() );
    Styler.exec();

    View->beginUpdate();
    qApp->setStyleSheet( Styler.styleText() );
    View->grid()->setStyleSheet( Styler.styleText() );
    View->endUpdate();
}

void CmpPanel::on_tabWidget_currentChanged( int Index )
{   
    if( Index < 0 )
        return;
    
    if( vecTabStates.isEmpty() == true )
        return;

    if( Index >= vecTabStates.size() )
        return;

    const auto View = vecTabStates.value( Index )->View;

    QTimer::singleShot( 0, [View]() {
        View->grid()->setFocus();
        View->grid()->viewport()->setFocus( Qt::MouseFocusReason );
        View->grid()->viewport()->raise();
        View->grid()->viewport()->activateWindow();

        if( View->getRowCount() > 0 )
        {
            View->navigateDown();
            View->navigateUp();
        }
    } );
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

            // NOTE: Qt 5 에서 서로 다른 해상도를 가진 다중 모니터 환경에서 정확한 커서 위치를 얻어올 수 없음.
            // 따라서, Win32 API 인 GetCursorPos 로 대체한다. 
            POINT Pos = {};
            GetCursorPos( &Pos );

            const auto ProxyModel = qobject_cast< FSProxyModel* >( Args->view()->model() );
            const auto FsModel = qobject_cast< FSModel* >( ProxyModel->sourceModel() );

            openShellContextMenuForObject( FsModel->GetFileFullPath( ProxyModel->mapToSource( ModelIndex ) ).toStdWString(), Pos.x, Pos.y,
                                           reinterpret_cast< HWND >( Args->view()->grid()->winId() ) );
        } break;
    }

    Args->setHandled( true );
}

bool CmpPanel::eventFilter( QObject* Object, QEvent* Event )
{
    if( Event->type() == QEvent::FocusIn )
    {
        const auto Grid = qobject_cast< Qtitan::Grid* >( Object );
        if( Grid != nullptr && static_cast< QFocusEvent* >( Event )->gotFocus() == true )
            emit sig_NotifyPanelActivated();
    }

    if( Event->type() == QEvent::KeyPress )
    {
        const auto KeyEvent     = static_cast< QKeyEvent* >( Event );
        const auto StCommandMgr = TyStCommandMgr::GetInstance();
        //const auto StShortcutMgr = TyStShortcutMgr::GetInstance();

        qDebug() << KeyEvent;

        if( StCommandMgr->ProcessKeyPressEvent( KeyEvent, retrieveFocusViewCursorIndex() ) == true )
            return true;
    }

    return false;
}

void CmpPanel::resizeEvent( QResizeEvent* event )
{
    const auto View = retrieveFocusView();
    if( View != nullptr )
        View->bestFit( FitToHeaderPercent );
    
    QWidget::resizeEvent( event );
}

int CmpPanel::retrieveCurrentIndex() const
{
    return ui.tabWidget->currentIndex();
}

CmpPanel::TySpTabState CmpPanel::retrieveFocusState()
{
    const auto Index = ui.tabWidget->currentIndex();
    Q_ASSERT( Index >= 0 );
    
    return vecTabStates[ Index ];
}

Qtitan::GridBandedTableView* CmpPanel::retrieveFocusView() const
{
    const auto Current = retrieveCurrentIndex();
    if( Current < 0 || vecTabStates.isEmpty() == true )
        return {};

    return vecTabStates.value( Current )->View;
}

QModelIndex CmpPanel::retrieveFocusViewCursorIndex() const
{
    const auto View = retrieveFocusView();
    if( View == nullptr )
        return {};

    const auto& Row = View->focusedRow();
    if( Row.isValid() == false )
        return {};

    return Row.modelIndex( 0 );
}

QPoint CmpPanel::retrieveMenuPoint( const QPoint& GlobalCursor, QModelIndex SrcIndex )
{
    QPoint Pos( GlobalCursor );

    const auto View = retrieveFocusView();
    const auto Grid = View->grid();
    const auto Row = View->getRow( SrcIndex );

    if( SrcIndex.isValid() == false || Row.isValid() == false )
        return Pos;

    QRect GlobalRowRect;

    for( const auto& Info : Grid->hitInfoAll() )
    {
        if( Info.info() != GridHitInfo::Cell )
            continue;

        if( Info.row().rowIndex() != Row.rowIndex() )
            continue;

        if( Info.columnIndex() == 0 )
        {
            GlobalRowRect.setTopLeft( Grid->mapToGlobal( Info.rect().topLeft() ) );
            GlobalRowRect.setBottomRight( Grid->mapToGlobal( Info.rect().bottomRight() ) );
        }

        if( Info.columnIndex() == View->getColumnCount() - 1 )
            GlobalRowRect.setBottomRight( Grid->mapToGlobal( Info.rect().bottomRight() ) );
    }

    do
    {
        if( Pos.isNull() == true )
        {
            Pos = GlobalRowRect.topLeft();
            break;
        }

        if( GlobalRowRect.contains( Pos ) == true )
            break;

        Pos = GlobalRowRect.topLeft();
        break;

    } while( false );

    return Pos;
}

void CmpPanel::processPanelStatusText()
{
    const auto& State = retrieveFocusState();
    

    ui.lblStatus->setText( QString( "크기: %1 / %2\t파일: %3 / %4\t폴더: %5 / %6" ).arg( 0).arg( State->Model->GetTotalSize() ).arg(0).arg( State->Model->GetFileCount()).arg(0).arg( State->Model->GetDirectoryCount()));

}
