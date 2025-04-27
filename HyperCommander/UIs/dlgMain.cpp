#include "stdafx.h"
#include "dlgMain.hpp"

#include "colorSchemeMgr.hpp"
#include "Components/cmpView.hpp"
#include "UIs/dlgOpts.hpp"
#include "UIs/dlgFavoriteDir.hpp"

#include "UniqueLibs/commandMgr.hpp"
#include "UniqueLibs/cmnTypeDefs.hpp"
#include "UniqueLibs/externalEditorMgr.hpp"
#include "UniqueLibs/solTCPluginMgr.hpp"
#include "UniqueLibs/SHChangeNotify.hpp"
#include "UniqueLibs/builtInFsModel.hpp"
#include "UniqueLibs/solFavoriteDirMgr.hpp"

QMainUI::QMainUI( QWidget* parent, Qt::WindowFlags flags )
    : QMainWindow( parent, flags )
{
    ui.setupUi( this );

    setWindowTitle( tr( "HyperCommander - %1" ).arg( CMN_MAKE_STR( RES_CANONICAL_VERSION ) ) );

    QMetaObject::invokeMethod( this, "initialize", Qt::QueuedConnection );
}

void QMainUI::OnDriveAdd( const QString& Root )
{
    qDebug() << __FUNCTION__ << " : " << Root;
}

void QMainUI::OnDriveRemoved( const QString& Root )
{
    qDebug() << __FUNCTION__ << " : " << Root;
}

void QMainUI::OnMediaInserted( const QString& ItemIDDisplayName )
{
    qDebug() << __FUNCTION__ << " : " << ItemIDDisplayName;
}

void QMainUI::OnMediaRemoved( const QString& Root )
{
    qDebug() << __FUNCTION__ << " : " << Root;

}

void QMainUI::OnColorSchemeChanged( const TyColorScheme& ColorScheme )
{
    if( ColorScheme.Name.isEmpty() == true )
        return;

    QApplication::setFont( ColorScheme.Dialog_Font );

    ui.cmpLeftPanel->OnColorSchemeChanged( ColorScheme );
    ui.cmpRightPanel->OnColorSchemeChanged( ColorScheme );
}

////////////////////////////////////////////////////////////////////////////
/// 명령 핸들러

DEFINE_HC_COMMAND( QMainUI, cm_NewTab )
{
    // 탭을 추가하면서 Qt 가 자동적으로 엉뚱한 곳으로 포커스를 옮기는 행동이 일어날 때가 있다. 이에 대해 보정한다. 
    const auto Prev = currentPanelIndex;
    const auto Pane = retrieveSrcPanel();
    Q_ASSERT( Pane != nullptr );
    if( Pane == nullptr )
        return;

    Pane->AddTab();
    currentPanelIndex = Prev;
}

DEFINE_HC_COMMAND( QMainUI, cm_PrevTab )
{
    const auto Pane = retrieveSrcPanel();
    Q_ASSERT( Pane != nullptr );
    if( Pane == nullptr )
        return;

    Pane->PrevTab();
}

DEFINE_HC_COMMAND( QMainUI, cm_NextTab )
{
    const auto Pane = retrieveSrcPanel();
    Q_ASSERT( Pane != nullptr );
    if( Pane == nullptr )
        return;

    Pane->NextTab();
}

DEFINE_HC_COMMAND( QMainUI, cm_CloseTab )
{
    const auto Prev = currentPanelIndex;
    const auto Pane = retrieveSrcPanel();
    Q_ASSERT( Pane != nullptr );
    if( Pane == nullptr )
        return;

    Pane->CloseTab();
    currentPanelIndex = Prev;
}

DEFINE_HC_COMMAND( QMainUI, cm_SrcQuickView )
{
    const auto Src = retrieveSrcPanel();
    const auto Dst = retrieveDstPanel();
    Q_ASSERT( Src != nullptr );
    Q_ASSERT( Dst != nullptr );

    if( Src == nullptr || Dst == nullptr )
        return;

    /*!
        Dst 의 현재 뷰가 QuickView 인지 확인. 그렇다면 이전 뷰로 되돌리고 끝낸다. ( QuickView 종료 )

        Src 에서 현재 커서가 위치한 대상 파일이름 획득한다.
        해당 파일명을 Dst 에 전달
        Dst : 현재 포커스가 있는 뷰를 전환한다. 
     */

    const auto DstView = Dst->GetFocusView( Dst->CurrentTabIndex() );
    if( DstView->GetViewMode() == CViewT::VM_QUICK )
    {
        Dst->CloseQuickView( Dst->CurrentTabIndex() );
        return;
    }

    const auto SrcView = Src->GetFocusView( Src->CurrentTabIndex() );
    Q_ASSERT( SrcView->GetViewMode() == CViewT::VM_GRID );

    QModelIndex SrcIndex = CursorIndex;
    const auto Proxy = qobject_cast< const QAbstractProxyModel* >( CursorIndex.model() );
    if( Proxy != nullptr )
        SrcIndex = Proxy->mapToSource( CursorIndex );

    const auto FilePath = static_cast< CGridView* >( SrcView )->BaseModel()->GetFileFullPath( SrcIndex );

    Dst->OpenQuickView( Dst->CurrentTabIndex(), FilePath );
}

DEFINE_HC_COMMAND( QMainUI, cm_CopyOtherPanel )
{
    const auto Src = retrieveSrcPanel();
    const auto Dst = retrieveDstPanel();
    Q_ASSERT( Src != nullptr );
    Q_ASSERT( Dst != nullptr );

    if( Src == nullptr || Dst == nullptr )
        return;

    // Src->FileCopyToOtherPanel( Dst );
}

DEFINE_HC_COMMAND( QMainUI, cm_RenameSingleFile )
{
    const auto Pane = retrieveSrcPanel();
    Q_ASSERT( Pane != nullptr );
    if( Pane == nullptr )
        return;

    // Pane->RenameFileName( CursorIndex );
}

DEFINE_HC_COMMAND( QMainUI, cm_List )
{
    const auto Pane = retrieveSrcPanel();
    Q_ASSERT( Pane != nullptr );
    if( Pane == nullptr )
        return;

    // Pane->ViewOnLister( CursorIndex );
}
DEFINE_HC_COMMAND( QMainUI, cm_Return )
{
    const auto Pane = retrieveSrcPanel();
    Q_ASSERT( Pane != nullptr );
    if( Pane == nullptr )
        return;

    Pane->ReturnOnCurrentTab( CursorIndex );
}

DEFINE_HC_COMMAND( QMainUI, cm_MkDir )
{
    const auto Pane = retrieveSrcPanel();
    Q_ASSERT( Pane != nullptr );
    if( Pane == nullptr )
        return;

    Pane->NewFolderOnCurrentTab( CursorIndex );
}

DEFINE_HC_COMMAND( QMainUI, cm_Delete )
{
    const auto Pane = retrieveSrcPanel();
    Q_ASSERT( Pane != nullptr );
    if( Pane == nullptr )
        return;

    Pane->FileDeleteOnCurrentTab( CursorIndex );
}

DEFINE_HC_COMMAND( QMainUI, cm_NameNormalization )
{
    const auto Pane = retrieveSrcPanel();
    Q_ASSERT( Pane != nullptr );
    if( Pane == nullptr )
        return;

    // Pane->FileNormalization( CursorIndex );
}

DEFINE_HC_COMMAND( QMainUI, cm_SetAttrib )
{
    const auto Pane = retrieveSrcPanel();
    Q_ASSERT( Pane != nullptr );
    if( Pane == nullptr )
        return;

    // Pane->FileSetAttrib( CursorIndex );
}

DEFINE_HC_COMMAND( QMainUI, cm_SelInverse )
{
    const auto Pane = retrieveSrcPanel();
    Q_ASSERT( Pane != nullptr );
    if( Pane == nullptr )
        return;
    
    Pane->SelectRowOnCurrentTab( CursorIndex, true );
}

DEFINE_HC_COMMAND( QMainUI, cm_MultiRenameFiles )
{
    const auto Pane = retrieveSrcPanel();
    Q_ASSERT( Pane != nullptr );
    if( Pane == nullptr )
        return;

    //const auto View = Pane->retrieveFocusView();
    //const auto Selection = View->modelController()->selection();
    //if( Selection->isEmpty() == true )
    //{
    //    QMessageBox::information( nullptr, tr( "HyperCommander" ), tr( "이름을 변경할 파일을 선택해 주세요." ) );
    //    return;
    //}

    //QVector< QString > VecFiles;
    //const auto ProxyModel = qobject_cast< FSProxyModel* >( View->model() );
    //const auto FsModel = qobject_cast< FSModel* >( ProxyModel->sourceModel() );

    //for( const auto& Item : Selection->selectedRowIndexes() )
    //{
    //    qDebug() << FsModel->GetFileFullPath( ProxyModel->mapToSource( Item ) );
    //    VecFiles.push_back( FsModel->GetFileFullPath( ProxyModel->mapToSource( Item ) ) );
    //}

    //QMetaObject::invokeMethod( qApp, "ShowMultiRename", Q_ARG( const QVector< QString >&, VecFiles ) );
}

DEFINE_HC_COMMAND( QMainUI, cm_SwitchHidSys )
{
    const auto Pane = retrieveSrcPanel();
    Q_ASSERT( Pane != nullptr );
    if( Pane == nullptr )
        return;

    const auto State = Pane->retrieveFocusState();
    bool IsShow = false;

    QMetaObject::invokeMethod( ( QObject* )State->ProxyModel, "GetHiddenSystem", qReturnArg( IsShow ) );
    QMetaObject::invokeMethod( ( QObject* )State->ProxyModel, "SetHiddenSystem", !IsShow );
}

DEFINE_HC_COMMAND( QMainUI, cm_RereadSource )
{
    const auto Pane = retrieveSrcPanel();
    Q_ASSERT( Pane != nullptr );
    if( Pane == nullptr )
        return;

    Pane->RefreshSource( Pane->CurrentTabIndex() );
}

DEFINE_HC_COMMAND( QMainUI, cm_DirectoryHotList )
{
    const auto StFavoriteDirMgr = TyStFavoriteDirMgr::GetInstance();

    QMenu Menu;
    StFavoriteDirMgr->ConstructDirMenu( &Menu );

    QMetaObject::invokeMethod( &Menu, "setFocus", Qt::QueuedConnection );
    auto ArrowDownEvent = new QKeyEvent( QEvent::KeyPress, Qt::Key_Down, Qt::NoModifier );
    qApp->postEvent( &Menu, ArrowDownEvent );   // 이벤트의 소유권은 Qt 에게로 이전되며, 자동으로 삭제된다. 

    // TODO: 화면 중앙에 나타나도록 개선 필요함
    Menu.exec( QCursor::pos() );
}

DEFINE_HC_COMMAND_EX( QMainUI, cm_GotoDrive )
{
    const auto Pane = retrieveSrcPanel();
    Q_ASSERT( Pane != nullptr );
    if( Pane == nullptr )
        return;

    Pane->ChangeVolume( Parameter.toString() );
}

DEFINE_HC_COMMAND_EX( QMainUI, cm_ChangePath )
{
    const auto Pane = retrieveSrcPanel();
    Q_ASSERT( Pane != nullptr );
    if( Pane == nullptr )
        return;

    const auto Args = Parameter.toStringList();
    if( Args.isEmpty() == true )
        return;

    Pane->ChangeCurrentPath( Args[ 0 ], Args.size() == 2 ? Args[1] : "" );
}

DEFINE_HC_COMMAND( QMainUI, cm_SwitchPanel )
{
    if( currentPanelIndex == 0 )
    {
        ui.cmpRightPanel->setFocus( Qt::MouseFocusReason );
        ui.cmpRightPanel->SetFocusView( ui.cmpRightPanel->CurrentTabIndex() );
        currentPanelIndex = 1;
    }
    else if( currentPanelIndex == 1 )
    {
        ui.cmpLeftPanel->setFocus( Qt::MouseFocusReason );
        ui.cmpLeftPanel->SetFocusView( ui.cmpLeftPanel->CurrentTabIndex() );
        currentPanelIndex = 0;
    }
}

DEFINE_HC_COMMAND( QMainUI, cm_ContextMenu )
{
    const auto Pane = retrieveSrcPanel();
    Q_ASSERT( Pane != nullptr );
    if( Pane == nullptr )
        return;

    Pane->ContextMenuOnCurrentTab( CursorIndex );
}

DEFINE_HC_COMMAND( QMainUI, cm_ExternalEditorMenu )
{
    const auto Pane = retrieveSrcPanel();
    Q_ASSERT( Pane != nullptr );
    if( Pane == nullptr )
        return;

    Pane->ExternalEditorMenu( CursorIndex );
}

void QMainUI::OnAddCurrentDirToFavorite()
{
    const auto Src = retrieveSrcPanel();
    const auto View = Src->GetFocusView( Src->CurrentTabIndex() );
    
    if( View == nullptr )
        return;

    if( View->GetViewMode() != CViewT::VM_GRID )
        return;

    const auto Model = ( ( CGridView* )View )->BaseModel();
    if( Model == nullptr )
        return;

    const auto Current = Model->GetCurrentPathWithRoot();

    QInputDialog Ui;
    Ui.setLabelText( tr( "항목의 이름을 지정해 주세요:" ) );
    Ui.setOkButtonText( tr( "확인(&O)" ) );
    Ui.setCancelButtonText( tr( "취소(&C)" ) );
    Ui.setTextValue( Current );
    Ui.setOptions( QInputDialog::UsePlainTextEditForTextInput );
    Ui.setInputMode( QInputDialog::TextInput );
    if( Ui.exec() == QDialog::Rejected )
        return;

    const auto StFavorites = TyStFavoriteDirMgr::GetInstance();
    CFavoriteDirMgr::TyFavoriteDir CurrentCMD;
    CurrentCMD.Name = Ui.textValue();
    CurrentCMD.Command = QString( R"(cd %1)" ).arg( Current );
    StFavorites->SetItem( CurrentCMD );
}

void QMainUI::on_acShowMainOpts_triggered( bool checked )
{
    QMainOpts Opts;

    Opts.exec();

}

void QMainUI::oo_notifyCurrentDirectory( const QString& CurrentPath )
{
    ui.lblPrompt->setText( CurrentPath + ">" );
}

void QMainUI::oo_notifyPanelActivated()
{
    const auto Sender = qobject_cast< CmpPanel* >( sender() );

    if( Sender == ui.cmpLeftPanel )
    {
        qDebug() << __FUNCTION__ << " : " << "왼쪽 패널 활성화";
        currentPanelIndex = 0;
    }
    else if( Sender == ui.cmpRightPanel )
    {
        qDebug() << __FUNCTION__ << " : " << "오른쪽 패널 활성화";
        currentPanelIndex = 1;
    }
}

bool QMainUI::eventFilter( QObject* Object, QEvent* Event )
{
    if( Event->type() != QEvent::FocusAboutToChange )
        return false;

    const auto FocusEvent = ( QFocusEvent* )Event;
    if( FocusEvent->gotFocus() == false )
        return false;

    const auto Grid = qobject_cast< Qtitan::Grid* >( Object );
    if( Grid != nullptr )
    {

    }

    return false;
}

void QMainUI::closeEvent( QCloseEvent* Event )
{
    StSettings->beginGroup( OPT_SEC_WINDOW );
    StSettings->setValue( "geometry", saveGeometry() );
    StSettings->endGroup();

    QMainWindow::closeEvent( Event );
}

void QMainUI::initialize()
{
    const auto StCommandMgr = TyStCommandMgr::GetInstance();
    const auto StColorSchemeMgr = TyStColorSchemeMgr::GetInstance();
    const auto StFavoriteDirMgr = TyStFavoriteDirMgr::GetInstance();

    connect( StFavoriteDirMgr, &CFavoriteDirMgr::NotifyAppendCurrentPath, this, &QMainUI::OnAddCurrentDirToFavorite );

    ui.menubar->setNativeMenuBar( true );

    auto HelpBar = new QMenuBar( ui.menubar );
    auto HelpBar_Root = new QMenu( LANG_MNU_HELP, HelpBar );
    HelpBar->addMenu( HelpBar_Root );

    ui.menubar->setCornerWidget( HelpBar, Qt::TopRightCorner );

    StCommandMgr->SetMainUI( this );

    shlChangeNotify = new QSHChangeNotify;
    shlChangeNotify->StartWatching();

    connect( shlChangeNotify, &QSHChangeNotify::OnDriveAdd, this, &QMainUI::OnDriveAdd );
    connect( shlChangeNotify, &QSHChangeNotify::OnDriveRemoved, this, &QMainUI::OnDriveRemoved );
    connect( shlChangeNotify, &QSHChangeNotify::OnMediaInserted, this, &QMainUI::OnMediaInserted );
    connect( shlChangeNotify, &QSHChangeNotify::OnMediaRemoved, this, &QMainUI::OnMediaRemoved );

    connect( StColorSchemeMgr, &CColorSchemeMgr::sigColorSchemeChanged, this, &QMainUI::OnColorSchemeChanged );

    //connect( ui.cmpLeftPanel, &CmpPanel::sig_NotifyCurrentDirectory, this, &QMainUI::oo_notifyCurrentDirectory );
    //connect( ui.cmpRightPanel, &CmpPanel::sig_NotifyCurrentDirectory, this, &QMainUI::oo_notifyCurrentDirectory );
    connect( ui.cmpLeftPanel, &CmpPanel::sig_NotifyPanelActivated, this, &QMainUI::oo_notifyPanelActivated );
    connect( ui.cmpRightPanel, &CmpPanel::sig_NotifyPanelActivated, this, &QMainUI::oo_notifyPanelActivated );

    ui.cmpLeftPanel->installEventFilter( this );
    ui.cmpRightPanel->installEventFilter( this );
}

CmpPanel* QMainUI::retrieveSrcPanel() const
{
    qDebug() << __FUNCTION__ << " : 현재 패널 번호 " << currentPanelIndex;

    if( currentPanelIndex == 0 )
        return ui.cmpLeftPanel;

    if( currentPanelIndex == 1 )
        return ui.cmpRightPanel;

    return nullptr;
}

CmpPanel* QMainUI::retrieveDstPanel() const
{
    qDebug() << __FUNCTION__ << " : 현재 패널 번호 " << currentPanelIndex;

    if( currentPanelIndex == 0 )
        return ui.cmpRightPanel;

    if( currentPanelIndex == 1 )
        return ui.cmpLeftPanel;

    return nullptr;
}
