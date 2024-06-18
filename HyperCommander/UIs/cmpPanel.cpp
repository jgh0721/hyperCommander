#include "stdafx.h"
#include "cmpPanel.hpp"
#include "dlgGridStyle.hpp"
#include "dlgViewer.hpp"

#include "Cate - FileOperation/dlgFileCopy.hpp"
#include "Cate - FileOperation/dlgCreateDirectory.hpp"

#include "UniqueLibs/columnMgr.hpp"
#include "UniqueLibs/commandMgr.hpp"
#include "UniqueLibs/builtInFsModel.hpp"
#include "UniqueLibs/externalEditorMgr.hpp"
#include "UniqueLibs/internalViewerMgr.hpp"

#include <shtypes.h>
#include <ShlObj.h>
#include <ShObjIdl.h>

#include <QtitanGrid.h>

#include "cmnHCUtils.hpp"
#include "externalLibs/QtitanDataGrid/src/src/base/QtnCommonStyle.h"

CmpPanel::CmpPanel( QWidget* parent, Qt::WindowFlags f )
{
    ui.setupUi( this );

    Qtitan::CommonStyle::ensureStyle();
}

void CmpPanel::Initialize()
{
    connect( &viewClickTimer, &QTimer::timeout, [this]() {
        const auto View = retrieveFocusView();
        if( View == nullptr )
            return;

        View->showEditor( GridEditor::ActivateByClick );
    } );

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

    if( ui.cbxVolume->count() > 0 )
        processVolumeStatusText( ui.cbxVolume->currentText()[ 0 ] );
}

int CmpPanel::CurrentTabIndex() const
{
    return retrieveCurrentIndex();
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
        if( View->focusedRowIndex() == View->modelController()->getGridRowCount() - 1 )
        {
            // 가장 마지막임
            View->navigateUp();
            View->navigateDown();
        }
        else
        {
            View->navigateDown();
            View->navigateUp();
        }
    }
}

void CmpPanel::SelectRowOnCurrentTab( const QModelIndex& SrcIndex, bool IsMoveDown )
{
    const auto State = retrieveFocusState();
    const auto View = retrieveFocusView();
    Q_ASSERT( View != nullptr );
    if( View == nullptr )
        return;

    // TODO: 각 패널의 하단에 선택된 항목 수 및 크기 등에 대해 갱신해야 한다.
    // State 에 저장된 항목들 갱신
    // TODO: 선택된 항목이 디렉토리이고, EverythingSDK 를 쓰지 않고, 디렉토리 계산 기능이 사용 중이라면 디렉토리 크기를 계산한다. 
    const auto Row = View->getRow( SrcIndex );
    const auto ModelIndex = State->ProxyModel->mapToSource( SrcIndex );
    const auto Node = State->Model->GetFileInfo( ModelIndex );

    View->selectRow( Row.rowIndex(), Qtitan::Invert );

    if( IsMoveDown == true )
        View->navigateDown( Qt::NoModifier );

    if( Node.Name == ".." )
        return;

    if( View->modelController()->isRowSelected( Row ) == true )
    {
        if( FlagOn( Node.Attiributes, FILE_ATTRIBUTE_DIRECTORY ) )
            State->SelectedDirectoryCount += 1;
        else
            State->SelectedFileCount += 1;

        State->SelectedSize += Node.Size;
    }
    else
    {
        if( FlagOn( Node.Attiributes, FILE_ATTRIBUTE_DIRECTORY ) )
            State->SelectedDirectoryCount -= 1;
        else
            State->SelectedFileCount -= 1;

        State->SelectedSize -= Node.Size;
    }

    QMetaObject::invokeMethod( this, "processPanelStatusText", Qt::QueuedConnection );
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
        State->Model->ChangeDirectory( ModelIndex );
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

void CmpPanel::NewFolderOnCurrentTab( const QModelIndex& SrcIndex )
{
    UNREFERENCED_PARAMETER( SrcIndex );

    QDirectoryCreateUI Ui;
    const auto Ret = Ui.exec();
    if( Ret == QDialog::Rejected )
        return;

    const auto NewName = Ui.GetInputText();
    if( NewName.isEmpty() == true )
        return;

    const auto State = retrieveFocusState();
    if( State->Model->MakeDirectory( NewName ) != ERROR_SUCCESS )
    {
        
    }
}

void CmpPanel::RefreshSource( int TabIndex )
{
    if( TabIndex < 0 || TabIndex >= vecTabStates.size() )
        return;

    vecTabStates[ TabIndex ]->Model->Refresh();
}

void CmpPanel::FileCopyToOtherPanel( CmpPanel* Dst )
{
    QFileCopyUI Ui;

    const auto Src_State = retrieveFocusState();
    const auto Dst_State = Dst->retrieveFocusState();

    // 선택된 항목들을 추가한다.
    QVector< QModelIndex > SrcModel;

    for( const auto Row : Src_State->View->selection()->selectedRowIndexes() )
    {
        const auto Index = Src_State->ProxyModel->mapToSource( Row );
        if( Src_State->Model->GetName( Index ) == ".." )
            continue;

        SrcModel.push_back( Index );
    }

    // 선택된 항목이 없다면 현재 커서가 위치한 항목을 복사한다. 
    if( SrcModel.isEmpty() == true )
    {
        const auto Row = Src_State->View->focusedRow();
        if( Row.isValid() == true )
        {
            const auto Index = Src_State->ProxyModel->mapToSource( Row.modelIndex( 0 ) );
            if( Src_State->Model->GetName( Index ) != ".." )
            {
                SrcModel.push_back( Index );
            }
        }
    }

    if( SrcModel.isEmpty() == true )
    {
        // TODO: 메시지 출력, 복사할 파일 없음
        return;
    }

    Ui.SetSourcePath( Src_State->Model->GetFileFullPath( "" ) );
    Ui.SetSourceModel( SrcModel );
    Ui.SetDestinationPath( Dst_State->Model->GetFileFullPath( "" ) );

    Ui.exec();
}

void CmpPanel::FileDeleteOnCurrentTab( const QModelIndex& SrcIndex )
{
    UNREFERENCED_PARAMETER( SrcIndex );

    const auto View = retrieveFocusView();
    const auto Rows = View->selection()->selectedRowIndexes();
    if( Rows.isEmpty() == true )
    {
        
    }
}

void CmpPanel::FileNormalization( const QModelIndex& SrcIndex )
{
    const auto State = retrieveFocusState();
    const auto ModelIndex = State->ProxyModel->mapToSource( SrcIndex );
    const auto Node = State->Model->GetFileInfo( ModelIndex );

    if( Node.IsNormalizedByNFD == FALSE )
        return;

    const DWORD Ret = State->Model->Rename( ModelIndex, Node.Name.normalized( QString::NormalizationForm_C ) );
    if( Ret != ERROR_SUCCESS )
    {
        // TODO: 오류처리, 오류창 표시
        // 관리자 권한으로 재시도할 수 있음
        return;
    }

    State->Model->setData( ModelIndex, Node.Name.normalized( QString::NormalizationForm_C ), Qt::EditRole );
}

void CmpPanel::RenameFileName( const QModelIndex& SrcIndex )
{
    UNREFERENCED_PARAMETER( SrcIndex );

    viewClickTimer.stop();

    for( const auto& State : vecTabStates )
        State->LastFocusedRowIndex = -1;

    const auto View = retrieveFocusView();
    View->setFocusedColumnIndex( 0 );
    View->showEditor( GridEditor::ActivateByKeyPress_F2 );
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

void CmpPanel::ViewOnLister( const QModelIndex& SrcIndex )
{
    const auto StInternalMgr = TyStInternalViewerMgr::GetInstance();
    const auto State = retrieveFocusState();
    const auto FileFullPath = State->Model->GetFileFullPath( State->ProxyModel->mapToSource( SrcIndex ) );

    QMenu Menu;
    Menu.setFocus( Qt::ActiveWindowFocusReason );
    Menu.setFocusPolicy( Qt::StrongFocus );
    Menu.setFont( QFont( "Sarasa Mono K Light", 14 ) );
    const auto ViewerCount = StInternalMgr->ConstructInternalMenu( &Menu, FileFullPath );
    if( ViewerCount == 0 )
        return;

    TyInternalViewer Viewer;

    if( ViewerCount == 1 )
    {
        const auto ac = Menu.actions()[ 0 ];
        Viewer = ac->data().value< TyInternalViewer >();
    }

    if( ViewerCount > 1 )
    {
        const auto ac = Menu.exec( retrieveMenuPoint( QCursor::pos(), SrcIndex ) );

        // 취소하거나 등등
        if( ac == nullptr )
            return;

        Viewer = ac->data().value< TyInternalViewer >();
    }

    auto DlgViewer = new QDlgViewer;
    DlgViewer->SetFileName( FileFullPath );
    DlgViewer->SetInternalViewer( Viewer );
    DlgViewer->show();
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
    BaseOpts.setRowAutoHeight( true );

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
        (void)act->disconnect();
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
            GridColumn->editorRepository()->setEditorActivationPolicy( GridEditor::ActivationPolicy() );
            if( Column.Content.contains( "HC.Fs.name", Qt::CaseInsensitive ) == true )
            {
                GridColumn->setSortOrder( Qtitan::SortAscending );
                GridColumn->dataBinding()->setSortRole( Qt::UserRole );
            }
            GridColumn->setFilterButtonVisible( false );
        }
    }
    
    connect( View, &Qtitan::GridViewBase::contextMenu, this, &CmpPanel::oo_grdLocal_contextMenu );
    connect( View, &Qtitan::GridViewBase::cellClicked, this, &CmpPanel::oo_grdLocal_cellClicked );
    connect( View, &Qtitan::GridViewBase::rowDblClicked, this, &CmpPanel::oo_grdLocal_rowDblClicked );
    connect( View, &Qtitan::GridViewBase::focusRowChanged, this, &CmpPanel::oo_grdLocal_rowFocusChanged );
    connect( View, &Qtitan::GridViewBase::editorStarted, this, &CmpPanel::oo_grdLocal_editorStarted );
    connect( View, &Qtitan::GridViewBase::editorPosting, this, &CmpPanel::oo_grdLocal_editorPosting );
    connect( View, &Qtitan::GridViewBase::selectionChanged, this, &CmpPanel::oo_grdLocal_selectionChanged );

    View->endUpdate();
    
    auto State = std::make_shared<TyTabState>( currentIndex, View, Model, ProxyModel );
    vecTabStates.push_back( State );

    Model->ChangeDirectory( QModelIndex() );
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
    auto State = retrieveFocusState();
    if( State == nullptr )
        return;
    
    State->Model->SetRoot( ui.cbxVolume->currentText().left( 2 ) );
    State->Model->SetCurrentPath( "/" );
    State->Model->ChangeDirectory( QModelIndex() );

    processVolumeStatusText( ui.cbxVolume->currentText()[ 0 ] );
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
            if( View->focusedRowIndex() == View->modelController()->getGridRowCount() - 1 )
            {
                // 가장 마지막임
                View->navigateUp();
                View->navigateDown();
            }
            else
            {
                View->navigateDown();
                View->navigateUp();
            }
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

    viewClickTimer.stop();

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
        case GridHitInfo::Rows: {
            POINT Pos = {};
            GetCursorPos( &Pos );

            const auto ProxyModel = qobject_cast< FSProxyModel* >( Args->view()->model() );
            const auto FsModel = qobject_cast< FSModel* >( ProxyModel->sourceModel() );

            openShellContextMenuForObject( FsModel->GetFileFullPath( "" ).toStdWString(), Pos.x, Pos.y,
                                           reinterpret_cast< HWND >( Args->view()->grid()->winId() ) );
        } break;
    }

    Args->setHandled( true );
}

void CmpPanel::oo_grdLocal_cellClicked( Qtitan::CellClickEventArgs* Args )
{
    const auto& Cell = Args->cell();

    //// 자신을 포함한 여러 행을 선택하고, 그 중에 하나를 다시 선택하면 해당 행만 남기고 나머지는 선택 해제한다.
    //// 선택 행이 하나이고, 해당 행을 다시 클릭했을 때는 이름 변경 시도를 고려해야 한다. 
    //if( spLocalView_->modelController()->isRowSelected( cell.row() ) == true )
    //{
    //    if( spLocalView_->modelController()->selection()->selectedRowIndexes().count() > 1 )
    //    {
    //        if( QGuiApplication::queryKeyboardModifiers() == Qt::NoModifier )
    //        {
    //            spLocalView_->deselectAll();
    //            spLocalView_->selectRow( cell.row().rowIndex() );
    //            return;
    //        }
    //    }
    //}

    //if( cell.columnIndex() != TBL_LOCAL_HDX_FILENAME.index )
    //{
    //    lastFocusRowIndex_ = spLocalView_->focusedRowIndex();
    //    viewClickTimer_.stop();
    //    return;
    //}

    if( retrieveCurrentIndex() < 0 )
    {
        int a = 0;
    }

    if( retrieveCurrentIndex() >= 0 )
    {
        if( Cell.columnIndex() != 0 )
        {
            viewClickTimer.stop();
        }

        const auto Modifiers = QGuiApplication::queryKeyboardModifiers();
        if( Modifiers.testFlag( Qt::ControlModifier ) )
        {
            SelectRowOnCurrentTab( Args->cell().modelIndex(), false );
            Args->setHandled( true );
        }

        if( Cell.columnIndex() == 0 && 
            Modifiers == Qt::NoModifier )
        {
            const auto State = retrieveFocusState();
            const auto View = State->View;

            if( State->LastFocusedRowIndex != View->focusedRowIndex() )
            {
                State->LastFocusedRowIndex = View->focusedRowIndex();
                viewClickTimer.stop();
                return;
            }

            static const auto doubleClickInterval = qApp->styleHints()->mouseDoubleClickInterval();
            viewClickTimer.setSingleShot( true );
            viewClickTimer.start( doubleClickInterval );
        }
    }
}

void CmpPanel::oo_grdLocal_rowDblClicked( Qtitan::RowClickEventArgs* Args )
{
    const auto Row = Args->row();
    const auto View = retrieveFocusView();
    const auto Index = Row.modelIndex( 0 );
    const auto StCommandMgr = TyStCommandMgr::GetInstance();

    if( View->activeEditor() != nullptr )
        View->closeEditor();

    viewClickTimer.stop();

    QKeyEvent Event( QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier );

    Args->setHandled( StCommandMgr->ProcessKeyPressEvent( &Event, Index ) );
}

void CmpPanel::oo_grdLocal_rowFocusChanged( int OldRowIndex, int NewRowIndex )
{
    UNREFERENCED_PARAMETER( OldRowIndex );
    UNREFERENCED_PARAMETER( NewRowIndex );

    //qDebug() << qMakePair( OldRowIndex, NewRowIndex );

    viewClickTimer.stop();

    for( const auto& State : vecTabStates )
        State->LastFocusedRowIndex = -1;
    
    retrieveFocusState()->LastFocusedRowIndex = OldRowIndex;
}

void CmpPanel::oo_grdLocal_editorStarted( Qtitan::EditorEventArgs* Args )
{
    if( Args->column()->index() != 0 )
        return;

    const auto View = retrieveFocusView();
    const auto Editor = View->activeEditor();
    const auto LineEdit = qobject_cast< QLineEdit* >( Editor->getCellWidget() );

    // 파일이름의 경우
    LineEdit->setTextMargins( 2, 0, 2, 0 );
    LineEdit->setObjectName( "edtFieldOnList" );
    LineEdit->installEventFilter( this );
    LineEdit->setStyleSheet( "margin-left: 28px;" );        // 파일 이름 옆에 나타나는 아이콘의 크기를 측정하여 지정해야한다. 

    // 색상 설정
    auto Pal = QGuiApplication::palette();
    Pal.setColor( QPalette::Base, QColor( "black" ) );
    Pal.setColor( QPalette::WindowText, QColor( "silver" ) );
    Pal.setColor( QPalette::Text, QColor( "silver" ) );
    LineEdit->setPalette( Pal );

    auto Text = LineEdit->text();

    if( Text.contains( '.' ) == true )
    {
        LineEdit->setSelection( 0, static_cast< int >( Text.lastIndexOf( '.' ) ) );
    }
    else
    {
        LineEdit->selectAll();
    }
}

void CmpPanel::oo_grdLocal_editorPosting( Qtitan::EditorEventArgs* Args )
{
    if( Args->column()->index() != 0 )
        return;

    const auto State = retrieveFocusState();
    const auto View = State->View;
    const auto Editor = View->activeEditor();
    const auto LineEdit = qobject_cast< QLineEdit* >( Editor->getCellWidget() );

    const auto Ret = State->Model->Rename( State->ProxyModel->mapToSource( Args->row().modelIndex( 0 ) ), LineEdit->text() );

    Args->setHandled( Ret == ERROR_SUCCESS );
}

void CmpPanel::oo_grdLocal_selectionChanged( Qtitan::GridSelection* NewSelection, Qtitan::GridSelection* OldSelection )
{
    viewClickTimer.stop();

    for( const auto& State : vecTabStates )
        State->LastFocusedRowIndex = -1;

    qDebug() << "Selected = " << NewSelection->selectedRowIndexes().size();

    for( const auto& Row : NewSelection->selectedRowIndexes() )
        qDebug() << Row;
}

bool CmpPanel::eventFilter( QObject* Object, QEvent* Event )
{
    if( Event->type() == QEvent::FocusIn )
    {
        const auto Grid = qobject_cast< Qtitan::Grid* >( Object );
        if( Grid != nullptr && dynamic_cast< QFocusEvent* >( Event )->gotFocus() == true )
            emit sig_NotifyPanelActivated();
    }

    if( Event->type() == QEvent::FocusOut )
    {
        const auto Grid = qobject_cast< Qtitan::Grid* >( Object );
        if( Grid != nullptr )
        {
            viewClickTimer.stop();
            for( const auto& State : vecTabStates )
                State->LastFocusedRowIndex = -1;
        }
    }

    // 이름 변경 편집기가 활성화된 상태에서 위/아래 화살표를 누르면 이동한다.
    if( Event->type() == QEvent::KeyPress && 
        Object->objectName() == "edtFieldOnList" )
    {
        const auto KeyEvent = dynamic_cast< QKeyEvent* >( Event );
        const auto Pressed = KeyEvent->keyCombination().key();
        if( Pressed == Qt::Key_Up || Pressed == Qt::Key_Down )
        {
            const auto View = retrieveFocusView();
            if( View->activeEditor() != nullptr )
                View->closeEditor();

            QMetaObject::invokeMethod( this, "RenameFileName", Qt::QueuedConnection, Q_ARG( const QModelIndex&, QModelIndex() ) );
            return true;
        }
    }

    if( Event->type() == QEvent::KeyPress )
    {
        const auto View         = retrieveFocusView();
        if( View->activeEditor() != nullptr )
            return false;

        const auto KeyEvent     = dynamic_cast< QKeyEvent* >( Event );
        const auto StCommandMgr = TyStCommandMgr::GetInstance();
        //const auto StShortcutMgr = TyStShortcutMgr::GetInstance();

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

void CmpPanel::processVolumeStatusText( QChar Drive ) const
{
    const auto Root = QString( "%1:\\" ).arg( Drive ).toStdWString();
    ULARGE_INTEGER Avail, TotalBytes, TotalFree;
    if( GetDiskFreeSpaceExW( Root.c_str(), &Avail, &TotalBytes, &TotalFree ) != FALSE )
    {
        const auto SizeStyle = static_cast< TyEnSizeStyle >( StSettings->value( "Configuration/SizeInHeader" ).toInt() );
        const auto AvailText = GetFormattedSizeText( Avail.QuadPart, SizeStyle );
        const auto TotalText = GetFormattedSizeText( TotalBytes.QuadPart, SizeStyle );

        ui.lblVolumeStatus->setText( QString( "%1 / %2 (남음/전체)" ).arg( AvailText ).arg( TotalText ) );
    }
    else
    {
        ui.lblVolumeStatus->setText( "0 / 0 (남음/전체)" );
    }
}

void CmpPanel::processPanelStatusText()
{
    const auto& State = retrieveFocusState();
    const auto SizeStyle = static_cast< TyEnSizeStyle >( StSettings->value( "Configuration/SizeInFooter" ).toInt() );

    ui.lblStatus->setText( QString( "크기: %1 / %2\t파일: %3 / %4\t폴더: %5 / %6" )
                           .arg( GetFormattedSizeText( State->SelectedSize, SizeStyle ) ).arg( GetFormattedSizeText( State->Model->GetTotalSize(), SizeStyle ) )
                           .arg( State->SelectedFileCount ).arg( State->Model->GetFileCount() )
                           .arg( State->SelectedDirectoryCount ).arg( State->Model->GetDirectoryCount() ) );
}
