﻿#include "stdafx.h"
#include "cmpView.hpp"

#include "builtInFsModel.hpp"
#include "cmnHCUtils.hpp"
#include "commandMgr.hpp"
#include "Modules/mdlFileModel.hpp"

#include <QtitanGrid.h>

#include "QtitanMSVSDebug.h"

DECLARE_CMNLIBSV2_NAMESPACE

//////////////////////////////////////////////////////////////////////////////
///

CViewT::CViewT( QWidget* Parent, Qt::WindowFlags f )
    : QWidget( Parent, f )
{
}

void CViewT::ApplyColorScheme( const TyColorScheme& ColorScheme )
{
    
}

bool CViewT::eventFilter( QObject* Watched, QEvent* Event )
{
    if( Event->type() == QEvent::FocusIn )
    {
        emit sig_NotifyViewActivated();

        //const auto Grid = qobject_cast< Qtitan::Grid* >( Watched );
        //if( Grid != nullptr && dynamic_cast< QFocusEvent* >( Event )->gotFocus() == true )
        //    emit sig_NotifyPanelActivated();
    }

    return QWidget::eventFilter( Watched, Event );
}

//////////////////////////////////////////////////////////////////////////////
///

CGridView::CGridView( QWidget* Parent, Qt::WindowFlags f )
    : CViewT( Parent, f )
{
    ( void )winId();
    setAttribute( Qt::WA_KeyCompression, true );
    setAttribute( Qt::WA_InputMethodEnabled, true );

    viewMode_ = VM_GRID;

    vbox_ = new QVBoxLayout;
    grid_ = new Qtitan::Grid;

    grid_->setViewType( Qtitan::Grid::BandedTableView );
    grid_->setSizePolicy( QSizePolicy( QSizePolicy::Policy::Expanding, QSizePolicy::Policy::Expanding ) );
    grid_->setAttribute( Qt::WA_KeyCompression, true );
    grid_->setAttribute( Qt::WA_InputMethodEnabled, true );
    grid_->viewport()->setAttribute( Qt::WA_KeyCompression, true );
    grid_->viewport()->setAttribute( Qt::WA_InputMethodEnabled, true );
    grid_->viewport()->setFocusPolicy( Qt::StrongFocus );

    grid_->installEventFilter( this );

    grid_->setContextMenuPolicy( Qt::CustomContextMenu );

    view_ = grid_->view< GridBandedTableView >();

    view_->beginUpdate();
    auto& BaseOpts = view_->options();
    auto& TableOpts = view_->tableOptions();
    auto& BandedOpts = view_->bandedOptions();

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
    BaseOpts.setBackgroundColor( QColor( 0, 0, 0 ) );
    BaseOpts.setSelectedBackgroundColor( QColor( 255, 112, 43 ) );
    BaseOpts.setSelectedInverseColor( true );
    BaseOpts.setModelDecorationOpacityRole( CFsModelT::USR_ROLE_HIDDENOPACITY );
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

    view_->removeBands();
    view_->addBand( tr( "FS" ) );

    // 모든 단축키 등 해제
    for( const auto& act : view_->actions() )
    {
        ( void )act->disconnect();
        act->setShortcut( {} );
    }

    connect( view_, &Qtitan::GridViewBase::contextMenu,         this, &CGridView::oo_grid_contextMenu );
    connect( view_, &Qtitan::GridViewBase::cellClicked,         this, &CGridView::oo_grid_cellClicked );
    connect( view_, &Qtitan::GridViewBase::rowDblClicked,       this, &CGridView::oo_grid_rowDblClicked );
    connect( view_, &Qtitan::GridViewBase::focusRowChanged,     this, &CGridView::oo_grid_rowFocusChanged );
    connect( view_, &Qtitan::GridViewBase::editorStarted,       this, &CGridView::oo_grid_editorStarted );
    connect( view_, &Qtitan::GridViewBase::editorPosting,       this, &CGridView::oo_grid_editorPosting );
    connect( view_, &Qtitan::GridViewBase::selectionChanged,    this, &CGridView::oo_grid_selectionChanged );

    view_->endUpdate();

    vbox_->setContentsMargins( 0, 9, 0, 0 );
    vbox_->addWidget( grid_ );
    setLayout( vbox_ );
    setSizePolicy( QSizePolicy( QSizePolicy::Policy::Expanding, QSizePolicy::Policy::Expanding ) );

    view_->bestFit( FitToHeaderPercent );
}

void CGridView::SetModel( CFsModelT* Source, QAbstractProxyModel* Proxy, ColumnView* Columns )
{
    if( view_ == nullptr )
        return;

    if( Source == nullptr || Columns == nullptr )
        return;

    model_ = Source;
    proxy_ = Proxy;
    model_->SetColumnView( *Columns );

    if( Proxy != nullptr )
    {
        proxy_->setSourceModel( model_ );
        view_->setModel( proxy_ );
    }
    else
    {
        view_->setModel( model_ );
    }

    model_->SetColumnView( *Columns );

    view_->beginUpdate();
    Qtitan::GridBandedTableColumn* GridColumn = nullptr;
    
    for( const auto& Column : Columns->VecColumns )
    {
        GridColumn = view_->addColumn( Column.Index, Column.Name );
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
                GridColumn->setSortLocaleAware( true );
                GridColumn->setSortOrder( Qtitan::SortAscending, true );
                GridColumn->dataBinding()->setSortRole( Qt::UserRole );
                
            }
            GridColumn->setFilterButtonVisible( false );
        }
    }

    view_->bestFit( FitToHeaderPercent );
    view_->endUpdate();
}

void CGridView::ApplyColorScheme( const TyColorScheme& ColorScheme )
{
    if( ColorScheme.Name.isEmpty() == true )
        return;

    if( view_ == nullptr )
        return;

    if( model_ != nullptr )
        model_->SetColorScheme( ColorScheme );

    // RowFocus 색상 등 적용
    view_->beginUpdate();
    auto& BaseOpts = view_->options();

    if( ColorScheme.FileList_BGColor.isValid() == true )
        BaseOpts.setBackgroundColor( ColorScheme.FileList_BGColor );

    if( ColorScheme.FileList_Selected.isValid() == true )
        BaseOpts.setSelectedBackgroundColor( ColorScheme.FileList_Selected );

    if( ColorScheme.FileList_Cursor.isValid() == true )
        BaseOpts.setRowFocusColor( ColorScheme.FileList_Cursor );

    view_->endUpdate();
}

bool CGridView::IsEditing() const
{
    if( view_ == nullptr )
        return false;

    return view_->activeEditor() != nullptr;
}

void CGridView::AdjustSizeToContents() const
{
    if( view_ != nullptr )
        view_->bestFit( FitToHeaderPercent );

    CViewT::AdjustSizeToContents();
}

void CGridView::EnsureKeyboardFocusOnView() const
{
    if( view_ == nullptr )
        return;

    grid_->setFocus( Qt::MouseFocusReason );
    grid_->viewport()->setFocus( Qt::MouseFocusReason );
    grid_->viewport()->raise();
    grid_->viewport()->activateWindow();

    if( view_->getRowCount() > 0 )
    {
        if( view_->focusedRowIndex() == view_->modelController()->getGridRowCount() - 1 )
        {
            // 가장 마지막임
            view_->navigateUp();
            view_->navigateDown();
        }
        else
        {
            view_->navigateDown();
            view_->navigateUp();
        }
    }

    CViewT::EnsureKeyboardFocusOnView();
}

nsHC::TySpFileSource CGridView::SelectRowOnCurrentTab( const QModelIndex& SrcIndex, bool IsMoveDown, bool& IsSelected )
{
    // TODO: 각 패널의 하단에 선택된 항목 수 및 크기 등에 대해 갱신해야 한다.
    // State 에 저장된 항목들 갱신
    // TODO: 선택된 항목이 디렉토리이고, EverythingSDK 를 쓰지 않고, 디렉토리 계산 기능이 사용 중이라면 디렉토리 크기를 계산한다. 

    if( view_ == nullptr )
        return {};
    
    const auto Row = view_->getRow( SrcIndex );
    const auto ModelIndex = proxy_ != nullptr ? proxy_->mapToSource( SrcIndex ) : SrcIndex;
    nsHC::TySpFileSource Info = model_->GetFileInfo( ModelIndex );

    view_->selectRow( Row.rowIndex(), Qtitan::Invert );
    IsSelected = view_->modelController()->isRowSelected( Row );
    
    if( IsMoveDown == true )
        view_->navigateDown( Qt::NoModifier );

    return Info;
}

void CGridView::ContextMenuOnCurrentTab( const QModelIndex& SrcIndex )
{
    QRect Rect;
    PopupMenu menu( grid_ );
    const auto Row = view_->getRow( SrcIndex );

    for( const auto Info : grid_->hitInfoAll() )
    {
        if( Info.info() != GridHitInfo::Cell )
            continue;

        if( Info.row().rowIndex() == Row.rowIndex() )
        {
            Rect = Info.rect();
            break;
        }
    }
    
    GridHitInfo HitInfo( GridHitInfo::Cell, view_, Rect, Row.rowIndex(), Row.cell( 0 ).columnIndex() );
    ContextMenuEventArgs Args( view_, &menu, HitInfo );
    oo_grid_contextMenu( &Args );
}

Qtitan::GridViewBase* CGridView::BaseView() const
{
    return view_;
}

CFsModelT* CGridView::BaseModel() const
{
    return model_;
}

//////////////////////////////////////////////////////////////////////////////
///

void CGridView::oo_grid_contextMenu( Qtitan::ContextMenuEventArgs* Args )
{
    const auto Info = Args->hitInfo();
    const auto Type = Info.info();
    const auto ModelIndex = proxy_ != nullptr ? proxy_->mapToSource( Info.modelIndex() ) : Info.modelIndex();

    viewClickTimer_.stop();

    switch( Type )
    {
        case GridHitInfo::Cell:
        {

            // NOTE: Qt 5 에서 서로 다른 해상도를 가진 다중 모니터 환경에서 정확한 커서 위치를 얻어올 수 없음.
            // 따라서, Win32 API 인 GetCursorPos 로 대체한다. 
            POINT Pos = {};
            GetCursorPos( &Pos );

            OpenShellContextMenuForObject( ( ( CFSModel* )model_ )->GetFileFullPath( ModelIndex ).toStdWString(), Pos.x, Pos.y,
                                           reinterpret_cast< HWND >( Args->view()->grid()->winId() ) );
        } break;
        case GridHitInfo::Rows:
        {
            POINT Pos = {};
            GetCursorPos( &Pos );

            OpenShellContextMenuForObject( model_->GetCurrentPathWithRoot().toStdWString(), Pos.x, Pos.y,
                                           reinterpret_cast< HWND >( Args->view()->grid()->winId() ) );
        } break;
    }

    Args->setHandled( true );
}

void CGridView::oo_grid_cellClicked( Qtitan::CellClickEventArgs* Args )
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

    ////if( cell.columnIndex() != TBL_LOCAL_HDX_FILENAME.index )
    ////{
    ////    lastFocusRowIndex_ = spLocalView_->focusedRowIndex();
    ////    viewClickTimer_.stop();
    ////    return;
    ////}

    //if( retrieveCurrentIndex() >= 0 )
    //{
    //    if( Cell.columnIndex() != 0 )
    //    {
    //        viewClickTimer.stop();
    //    }

    //    const auto Modifiers = QGuiApplication::queryKeyboardModifiers();
    //    if( Modifiers.testFlag( Qt::ControlModifier ) )
    //    {
    //        SelectRowOnCurrentTab( Args->cell().modelIndex(), false );
    //        Args->setHandled( true );
    //    }

    //    if( Cell.columnIndex() == 0 && 
    //        Modifiers == Qt::NoModifier )
    //    {
    //        const auto State = retrieveFocusState();
    //        const auto View = State->View;

    //        if( State->LastFocusedRowIndex != View->focusedRowIndex() )
    //        {
    //            State->LastFocusedRowIndex = View->focusedRowIndex();
    //            viewClickTimer.stop();
    //            return;
    //        }

    //        static const auto doubleClickInterval = qApp->styleHints()->mouseDoubleClickInterval();
    //        viewClickTimer.setSingleShot( true );
    //        viewClickTimer.start( doubleClickInterval );
    //    }
    //}
}

void CGridView::oo_grid_rowDblClicked( Qtitan::RowClickEventArgs* Args )
{
    const auto Row = Args->row();
    const auto Index = Row.modelIndex( 0 );
    const auto StCommandMgr = TyStCommandMgr::GetInstance();

    if( view_->activeEditor() != nullptr )
        view_->closeEditor();

    viewClickTimer_.stop();

    QKeyEvent Event( QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier );

    Args->setHandled( StCommandMgr->ProcessKeyPressEvent( &Event, Index ) );
}

void CGridView::oo_grid_rowFocusChanged( int OldRowIndex, int NewRowIndex )
{
    UNREFERENCED_PARAMETER( OldRowIndex );
    UNREFERENCED_PARAMETER( NewRowIndex );

    ////qDebug() << qMakePair( OldRowIndex, NewRowIndex );

    viewClickTimer_.stop();

    //for( const auto& State : vecTabStates )
    //    State->LastFocusedRowIndex = -1;
    //
    //retrieveFocusState()->LastFocusedRowIndex = OldRowIndex;
}

void CGridView::oo_grid_editorStarted( Qtitan::EditorEventArgs* Args )
{
    //if( Args->column()->index() != 0 )
    //    return;

    //const auto View = retrieveFocusView();
    //const auto Editor = View->activeEditor();
    //const auto LineEdit = qobject_cast< QLineEdit* >( Editor->getCellWidget() );

    //// 파일이름의 경우
    //LineEdit->setTextMargins( 2, 0, 2, 0 );
    //LineEdit->setObjectName( "edtFieldOnList" );
    //LineEdit->installEventFilter( this );
    //LineEdit->setStyleSheet( "margin-left: 28px;" );        // 파일 이름 옆에 나타나는 아이콘의 크기를 측정하여 지정해야한다. 

    //// 색상 설정
    //auto Pal = QGuiApplication::palette();
    //Pal.setColor( QPalette::Base, QColor( "black" ) );
    //Pal.setColor( QPalette::WindowText, QColor( "silver" ) );
    //Pal.setColor( QPalette::Text, QColor( "silver" ) );
    //LineEdit->setPalette( Pal );

    //auto Text = LineEdit->text();

    //if( Text.contains( '.' ) == true )
    //{
    //    LineEdit->setSelection( 0, static_cast< int >( Text.lastIndexOf( '.' ) ) );
    //}
    //else
    //{
    //    LineEdit->selectAll();
    //}
}

void CGridView::oo_grid_editorPosting( Qtitan::EditorEventArgs* Args )
{
    //if( Args->column()->index() != 0 )
    //    return;

    //const auto State = retrieveFocusState();
    //const auto View = State->View;
    //const auto Editor = View->activeEditor();
    //const auto LineEdit = qobject_cast< QLineEdit* >( Editor->getCellWidget() );

    //const auto Ret = State->Model->Rename( State->ProxyModel->mapToSource( Args->row().modelIndex( 0 ) ), LineEdit->text() );

    //Args->setHandled( Ret == ERROR_SUCCESS );
}

void CGridView::oo_grid_selectionChanged( Qtitan::GridSelection* NewSelection, Qtitan::GridSelection* OldSelection )
{
    UNREFERENCED_PARAMETER( NewSelection );
    UNREFERENCED_PARAMETER( OldSelection );

    viewClickTimer_.stop();

    //for( const auto& State : vecTabStates )
    //    State->LastFocusedRowIndex = -1;
}

bool CGridView::eventFilter( QObject* Watched, QEvent* Event )
{
    if( Event->type() == QEvent::KeyPress )
    {
        if( IsEditing() == true )
            return false;

        const auto KeyEvent = dynamic_cast< QKeyEvent* >( Event );
        const auto StCommandMgr = TyStCommandMgr::GetInstance();

        qDebug() << __FUNCTION__ << " : " << KeyEvent << KeyEvent->text();

        if( StCommandMgr->ProcessKeyPressEvent( KeyEvent, retrieveCursorIndex() ) == true )
            return true;
    }

    if( Event->type() == QEvent::FocusOut )
    {
        const auto Grid = qobject_cast< Qtitan::Grid* >( Watched );
        if( Grid != nullptr )
        {
            viewClickTimer_.stop();
            lastFocusedRowIndex_ = -1;
        }
    }

    return CViewT::eventFilter( Watched, Event );
}

void CGridView::resizeEvent( QResizeEvent* event )
{
    if( view_ != nullptr )
        view_->bestFit( FitToHeaderPercent );

    CViewT::resizeEvent( event );
}

bool CGridView::nativeEvent( const QByteArray& eventType, void* message, qintptr* result )
{
    int a = 0;
    if( eventType != "windows_generic_MSG" )
        return CViewT::nativeEvent( eventType, message, result );

    MSG* msg = static_cast< MSG* >( message );
    if( msg->message == WM_CHAR )
    {
        qDebug() << __FUNCTION__ << " : " << QChar( ushort( msg->wParam ) );
    }

    return false;
}

QModelIndex CGridView::retrieveCursorIndex() const
{
    if( view_ == nullptr )
        return {};

    const auto& Row = view_->focusedRow();
    if( Row.isValid() == false )
        return {};

    return Row.modelIndex( 0 );
}

//////////////////////////////////////////////////////////////////////////////
///

constexpr auto WLX_PROC_ATOM = L"HyperCommander";

CInternalViewer::CInternalViewer( QWidget* Parent, Qt::WindowFlags f )
    : CViewBase( Parent, f )
{
}

CExternalViewer::CExternalViewer( QWidget* Parent, Qt::WindowFlags f )
    : CViewBase( Parent, f )
{
    // NOTE: 삭제 금지
    ( void )winId();
    atom = GlobalAddAtomW( WLX_PROC_ATOM );

    vbox_ = new QVBoxLayout;
    setLayout( vbox_ );
    vbox_->setContentsMargins( 0, 0, 0, 0 );
}

CExternalViewer::~CExternalViewer()
{
    if( viewer_ == nullptr )
        return;

    const auto Funcs = viewer_->Funcs();
    if( Funcs.ListCloseWindow != nullptr && hChild != nullptr )
        Funcs.ListCloseWindow( hChild );

    GlobalDeleteAtom( atom );
}

void CExternalViewer::SetWLX( TySpWLX Viewer )
{
    viewer_ = Viewer;
}

bool CExternalViewer::Initialize()
{
    bool IsInitialized = false;

    do
    {
        if( viewer_ == nullptr )
            break;

        setDefaultParams();

        wParent = new QWidget;
        hParent = reinterpret_cast< HWND >( wParent->winId() );

        hChild = ListLoad( hParent, GetFilePath(), 0 );
        if( hChild == nullptr )
            break;

        QWindow* Child_W = QWindow::fromWinId( reinterpret_cast< WId >( hChild ) );
        Child_W->setFlag( Qt::FramelessWindowHint );
        wChild = QWidget::createWindowContainer( Child_W, wParent, Qt::FramelessWindowHint );

        // Subclass viewer window to catch WM_COMMAND message.
        HWND Result = (HWND)SetWindowLongPtrW( hParent, GWLP_WNDPROC, ( LONG_PTR )cbWndProc );
        SetPropW( hParent, WLX_PROC_ATOM, Result );

        // Subclass plugin window to catch some hotkeys like 'n' or 'p'.
        Result = ( HWND )SetWindowLongPtrW( hChild, GWLP_WNDPROC, ( LONG_PTR )cbWndProcChild );
        SetPropW( hChild, WLX_PROC_ATOM, Result );

#if QT_VERSION_MAJOR == 5
        SetWindowLongPtrW( hChild, GWLP_HWNDPARENT, reinterpret_cast< LONG_PTR >( hParent ) );
#endif
        vbox_->addWidget( wChild );
        wChild->show();
        
        viewer_->SetWinId( hChild );

        IsInitialized = true;

    } while( false );

    return IsInitialized;
}

HWND CExternalViewer::ListLoad( HWND ParentWin, const QString& FilePath, int ShowFlags )
{
    if( viewer_ == nullptr )
        return {};

    const auto Funcs = viewer_->Funcs();
    if( Funcs.ListLoadW != nullptr )
        return Funcs.ListLoadW( hParent, const_cast< WCHAR* >( FilePath.toStdWString().c_str() ), 0 );

    if( Funcs.ListLoad != nullptr )
        Funcs.ListLoad( hParent, const_cast< CHAR* >( FilePath.toStdString().c_str() ), 0 );

    return {};
}

void CExternalViewer::setDefaultParams()
{
    const auto Funcs = viewer_->Funcs();
    if( Funcs.ListSetDefaultParams == nullptr )
        return;

    typedef struct
    {
        int size;
        DWORD PluginInterfaceVersionLow;
        DWORD PluginInterfaceVersionHi;
        char DefaultIniName[ MAX_PATH ];
    } ListDefaultParamStruct;

    ListDefaultParamStruct dps = { sizeof( ListDefaultParamStruct ), 2, 0 };

    const auto ini = QDir::toNativeSeparators( QCoreApplication::applicationDirPath() + "/lsplugins.ini" ).toLocal8Bit();
    strncpy_s( dps.DefaultIniName, ini.data(), ini.size() );

    Funcs.ListSetDefaultParams( &dps );
}

LRESULT CExternalViewer::cbWndProc( HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam )
{
    LRESULT Ret = 0;
    WNDPROC Prev = nullptr;
    if( uMsg == WM_LBUTTONDOWN )
    {
        int a = 0;
    }

    Prev = ( WNDPROC )GetPropW( hWnd, WLX_PROC_ATOM );
    if( Prev )
        Ret = CallWindowProcW( Prev, hWnd, uMsg, wParam, lParam );
    else
        Ret = DefWindowProcW( hWnd, uMsg, wParam, lParam );

    if( Ret == 0 && uMsg == WM_COMMAND && lParam != 0 )
    {
        const auto Window = QWindow::fromWinId( reinterpret_cast< WId >( hWnd ) );
        // Window->

        //    Lister:= TControl(GetLCLOwnerObject(hWnd));
        //    if Assigned(Lister) then Lister.Perform(Msg, wParam, lParam);
    }

    return Ret;

    //  if (Result = 0) and (Msg = WM_COMMAND) and (lParam <> 0) then
    //  begin
    //    Lister:= TControl(GetLCLOwnerObject(hWnd));
    //    if Assigned(Lister) then Lister.Perform(Msg, wParam, lParam);
    //  end;
    //end;
}

LRESULT CExternalViewer::cbWndProcChild( HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam )
{
    LRESULT Ret = 0;
    WNDPROC Prev = nullptr;
    if( uMsg == WM_LBUTTONDOWN )
    {
        int a = 0;
    }

    Prev = ( WNDPROC )GetPropW( hWnd, WLX_PROC_ATOM );
    if( Prev )
        Ret = CallWindowProcW( Prev, hWnd, uMsg, wParam, lParam );
    else
        Ret = DefWindowProcW( hWnd, uMsg, wParam, lParam );

    if( Ret == 0 && uMsg == WM_KEYDOWN )
        PostMessageW( GetParent( hWnd ), uMsg, wParam, lParam );

    return Ret;
}

//////////////////////////////////////////////////////////////////////////////
///

CQuickView::CQuickView( QWidget* Parent, Qt::WindowFlags f )
    : CViewT( Parent, f )
{
    vbox_ = new QVBoxLayout;
    viewMode_ = VM_QUICK;

    setLayout( vbox_ );
    vbox_->setContentsMargins( 0, 0, 0, 0 );
}

void CQuickView::SetFilePath( const QString& FilePath )
{
    filePath_ = FilePath;
}

void CQuickView::showEvent( QShowEvent* event )
{
    do
    {
        if( event->spontaneous() == false && isInitOnce_ == false )
        {
            const auto StPlugInMgr = TyStPlugInMgr::GetInstance();
            const auto Menu = new QMenu;
            Menu->setFocus( Qt::ActiveWindowFocusReason );
            Menu->setFocusPolicy( Qt::StrongFocus );
            Menu->setFont( QFont( "Sarasa Mono K Light", 14 ) );
            Menu->deleteLater();

            StPlugInMgr->ConstructWLXMenu( Menu, filePath_ );

            bool IsLoadWLX = false;
            for( const auto& Ac : Menu->actions() )
            {
                TySpWLX Viewer = Ac->data().value< TySpWLX >();

                externalViewer_ = new CExternalViewer;
                externalViewer_->SetFilePath( filePath_ );
                externalViewer_->SetWLX( Viewer );

                if( externalViewer_->Initialize() == false )
                {
                    externalViewer_->deleteLater();
                    continue;
                }

                vbox_->addWidget( externalViewer_ );
                IsLoadWLX = true;
            }

            if( IsLoadWLX == false )
            {
                internalViewer_ = new CInternalViewer;
                internalViewer_->SetFilePath( filePath_ );
                vbox_->addWidget( internalViewer_ );
            }
            
            isInitOnce_ = true;
        }

    } while( false );

    CViewT::showEvent( event );
}

////int CmpPanel::InitializeGrid()
////{
////    Model->SetRoot( "C:" );
////    Model->SetCurrentPath( "\\" );
////    connect( Model, &FSModel::sigChangedDirectory, this, &CmpPanel::oo_ChangedDirectory );
////
////    Model->ChangeDirectory( QModelIndex() );
////    
////    return currentIndex;
////}
////


////void CmpPanel::ViewOnLister( const QModelIndex& SrcIndex )
////{
////    auto DlgViewer = new QDlgViewer;
////    DlgViewer->SetFileName( FileFullPath );
////    DlgViewer->SetInternalViewer( Viewer );
////    DlgViewer->show();
////}
////