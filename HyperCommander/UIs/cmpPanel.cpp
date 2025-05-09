﻿#include "stdafx.h"
#include "cmpPanel.hpp"
#include "dlgGridStyle.hpp"
#include "dlgViewer.hpp"

#include "Modules/mdlFileSystem.hpp"
#include "Modules/mdlFileModel.hpp"
#include "Components/cmpView.hpp"

#include "Cate - FileOperation/dlgFileCopy.hpp"
#include "Cate - FileOperation/dlgCreateDirectory.hpp"
#include "Cate - FileOperation/dlgFileDelete.hpp"
#include "Cate - FileOperation/dlgFileAttrib.hpp"

#include "UniqueLibs/columnMgr.hpp"
#include "UniqueLibs/commandMgr.hpp"
#include "UniqueLibs/builtInFsModel.hpp"
#include "UniqueLibs/externalEditorMgr.hpp"
#include "UniqueLibs/solTCPluginMgr.hpp"
#include "UniqueLibs/colorSchemeMgr.hpp"
#include "UniqueLibs/cmnHCUtils.hpp"

#include <shtypes.h>
#include <ShlObj.h>
#include <ShObjIdl.h>

#include <QtitanGrid.h>

#include "Modules/mdlFileEngine.hpp"

DECLARE_CMNLIBSV2_NAMESPACE

//////////////////////////////////////////////////////////////////////////////
///

CmpPanel::CmpPanel( QWidget* parent, Qt::WindowFlags f )
    : QWidget( parent, f )
{
    ui.setupUi( this );

    initializeUIs();

    const auto StColorSchemeMgr = TyStColorSchemeMgr::GetInstance();
    OnColorSchemeChanged( StColorSchemeMgr->GetColorScheme( StColorSchemeMgr->GetCurrentColorScheme() ) );

    QMetaObject::invokeMethod( this, "RefreshVolumeList", Qt::QueuedConnection );
    QTimer::singleShot( 100, [this]() {
        if( ui.cbxVolume->GetCount() == 0 )
            return;

        ui.cbxVolume->setCurrentIndex( 0 );
    } );
}

void CmpPanel::RefreshVolumeList() const
{
    // NOTE: 선택 복원용
    // TODO: 만약 수량이 변경되었다면( 드라이브 추가/삭제일 때, 색인으로 복원하면 다른 드라이브가 선택될 수 있다. )

    const auto Current = ui.cbxVolume->currentIndex();

    /*!
    * 1. 실제 파일시스템 추가
    * 2. 네트워크 환경 추가
    */

    QSignalBlocker Blocker( ui.cbxVolume );
    ui.cbxVolume->clear();

    QFileIconProvider QFIP;
    const auto DriveIcon = QFIP.icon( QAbstractFileIconProvider::Drive );

    // TODO: 색상 구성표 적용

    DWORD Drives = GetLogicalDrives();
    for( char idx = 0; idx < 26; ++idx )
    {
        if( ( Drives & ( 1 << idx ) ) != 0 )
        {
            CHAR driveName[] = { static_cast< CHAR >( 'A' + idx ), ':', '\\', '\0' };

            const auto Fs = nsHC::TySpFileSystem( nsHC::CFileSystemT::MakeFileSystem( QString::fromStdString( driveName ) ) );
            if( Fs != nullptr )
            {
                const auto Item = ui.cbxVolume->CreateItem();
                const auto Label = QString::fromStdString( fmt::format( "{:^3c} | {:20s}", 'A' + idx, Fs->GetVolumeName().toStdString() ) );
                
                Item->setFixedHeight( 32 );
                Item->SetText( QChar( 'A' + idx ) );
                Item->GetItem()->setForeground( QColor( "black" ) );
                Item->GetItem()->setBackground( QColor( "yellow" ) );
                Item->GetItem()->setIcon( DriveIcon );
                Item->AddLabel()->setText( Label );
                Item->AddLabel()->adjustSize();
                Item->SetUserData( Qt::UserRole, QVariant::fromValue( Fs ) );

                ui.cbxVolume->AppendItem( Item->GetItem(), Item );
            }
        }
    }

    {
        const auto FsUNC = nsHC::TySpFileSystem( nsHC::CFileSystemT::MakeFileSystem( R"(\\)" ) );
        if( FsUNC != nullptr )
        {
            const auto Item = ui.cbxVolume->CreateItem();
            const auto Label = tr("\\ | 네트워크 환경" );

            Item->setFixedHeight( 32 );
            Item->SetText( tr("네트워크 환경") );
            Item->GetItem()->setForeground( QColor( "black" ) );
            Item->GetItem()->setBackground( QColor( "yellow" ) );
            Item->GetItem()->setIcon( DriveIcon );
            Item->AddLabel()->setText( Label );
            Item->AddLabel()->adjustSize();
            Item->SetUserData( Qt::UserRole, QVariant::fromValue( FsUNC ) );

            ui.cbxVolume->AppendItem( Item->GetItem(), Item );
        }
    }

    ui.cbxVolume->setCurrentIndex( Current );

    //    //if( ui.cbxVolume->count() > 0 )
    //    //    processVolumeStatusText( ui.cbxVolume->currentText()[ 0 ] );
    //}
    
}

void CmpPanel::AddTab()
{
    const auto TabCount = ui.tabWidget->count();
    const auto TabCurrent = ui.tabWidget->currentIndex();

    // TODO: 중복제거 필요함
    // 탭에 어떠한 뷰도 없으므로, 기보값으로 GridView 를 추가한다.
    const auto StColumnMgr = TyStColumnMgr::GetInstance();
    auto ColumnView = StColumnMgr->GetColumnView( 0 );

    const auto DefaultView = static_cast< CGridView* >( createGridView() );
    if( DefaultView == nullptr )
        return;

    ui.tabWidget->blockSignals( true );
    const auto TabIndex = ui.tabWidget->addTab( DefaultView, "" );
    ui.tabWidget->setCurrentIndex( -1 );
    ui.tabWidget->blockSignals( false );

    connect( DefaultView, &CViewT::sig_NotifyViewActivated, this, &CmpPanel::sig_NotifyPanelActivated );

    const auto FsModel = new CFSModel;
    const auto ProxyModel = new CFSProxyModel;
    connect( FsModel, &CFSModel::sigChangedDirectory, this, &CmpPanel::OnChangedDirectory );

    DefaultView->SetModel( FsModel, ProxyModel, &ColumnView );

    const auto State = std::make_shared< TyTabState >( TabIndex, DefaultView, nullptr, FsModel, ProxyModel );
    vecTabStates.push_back( State );

    const auto StColorSchemeMgr = TyStColorSchemeMgr::GetInstance();
    OnColorSchemeChanged( StColorSchemeMgr->GetColorScheme( StColorSchemeMgr->GetCurrentColorScheme() ) );

    const auto Fs = ui.cbxVolume->GetItem( ui.cbxVolume->currentIndex() )->GetUserData( Qt::UserRole ).value< nsHC::TySpFileSystem >();

    ui.tabWidget->setCurrentIndex( TabCount );

    State->Model->SetRoot( Fs );
    State->Model->SetCurrentPath( "\\" );
    State->Model->ChangeDirectory( QModelIndex() );

    State->View->AdjustSizeToContents();
    SetFocusView( TabCount );

    QTimer::singleShot( 0, [State]() {State->View->EnsureKeyboardFocusOnView(); } );
}

void CmpPanel::PrevTab()
{
    // 탭이 하나라면 아무 것도 하지 않는다. 
    if( ui.tabWidget->count() <= 1 )
        return;

    // TODO: 볼륨 목록 동기화

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

    // TODO: 볼륨 목록 동기화

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

    auto Current = ui.tabWidget->currentIndex();

    const auto State = retrieveFocusState();
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

    // TODO: 볼륨 목록 동기화

    ui.tabWidget->setCurrentIndex( New );
    const auto w = ui.tabWidget->widget( Current );
    ui.tabWidget->removeTab( Current );
    delete w;
    SetFocusView( New );
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

    if( View->GetViewMode() == CViewT::VM_GRID )
        View->EnsureKeyboardFocusOnView();
}

CViewT* CmpPanel::GetFocusView( int TabIndex ) const
{
    if( TabIndex < 0 || vecTabStates.isEmpty() == true )
        return {};

    const auto& State = vecTabStates.value( TabIndex );
    if( State->QuickView != nullptr )
        return State->QuickView;

    return State->View;
}

void CmpPanel::OpenQuickView( int TabIndex, const QString& FilePath )
{
    if( TabIndex < 0 || vecTabStates.isEmpty() == true )
        return;

    {
        QSignalBlocker Blocker( ui.tabWidget );
        const auto& State = vecTabStates.value( TabIndex );
        const auto& TabTitle = ui.tabWidget->tabText( TabIndex );

        ui.tabWidget->removeTab( TabIndex );
        State->QuickView = new CQuickView;
        ( ( CQuickView* )State->QuickView )->SetFilePath( FilePath );
        ui.tabWidget->insertTab( 0, State->QuickView, TabTitle );
        ui.tabWidget->setCurrentIndex( TabIndex );
    }
}

void CmpPanel::CloseQuickView( int TabIndex )
{
    Q_ASSERT( TabIndex >= 0 && TabIndex < ui.tabWidget->count() );
    if( TabIndex < 0 || TabIndex >= ui.tabWidget->count() )
        return;

    {
        QSignalBlocker Blocker( ui.tabWidget );
        const auto& State = vecTabStates[ TabIndex ];
        const auto& TabTitle = ui.tabWidget->tabText( TabIndex );

        if( State->QuickView == nullptr )
            return;

        State->QuickView->CloseView();
        ui.tabWidget->removeTab( TabIndex );
        State->QuickView->deleteLater();
        State->QuickView = nullptr;
        ui.tabWidget->insertTab( 0, State->View, TabTitle );
        ui.tabWidget->setCurrentIndex( TabIndex );
    }
}

void CmpPanel::RefreshSource( int TabIndex )
{
    if( TabIndex < 0 || TabIndex >= vecTabStates.size() )
        return;

    vecTabStates[ TabIndex ]->Model->Refresh();
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

    const auto FileEngine = TyStFileEngine::GetInstance();
    const auto State = retrieveFocusState();

    const auto TaskItem = nsHC::TySpFETaskItem( new nsHC::CFETaskItem );
    TaskItem->Id;
    TaskItem->Type = nsHC::FE_MODE_MKDIR;
    TaskItem->Src = nsHC::TySpFETaskItemSource( new nsHC::CFETaskItemSource );
    TaskItem->Src->Fs = State->Model->GetRoot();

    const auto Src = nsHC::TySpFileSource( new nsHC::CFileSourceT );

    Src->Flags_ = 0;
    Src->Attributes_ = FILE_ATTRIBUTE_DIRECTORY;
    Src->Name_ = State->Model->GetCurrentPath();

    TaskItem->Src->Items.push_back( Src );

    TaskItem->Input = NewName;

    TyOsValue<nsHC::TyEnFEResult> Result;
    if( FileEngine->ExecuteTask( TaskItem, Result ) == false )
        return;

    if( Result.Value.value_or( nsHC::FE_RESULT_FAILED ) == nsHC::FE_RESULT_SUCCEED )
        ( ( CFSModel* )State->Model )->InsertChildItem( NewName );
}

void CmpPanel::FileDeleteOnCurrentTab( const QModelIndex& SrcIndex )
{
    UNREFERENCED_PARAMETER( SrcIndex );

    const auto Src_State = retrieveFocusState();
    QVector< QModelIndex > SrcModel = makeSrcModel( Src_State );

    if( SrcModel.isEmpty() == true )
        return;

    QFileDeleteUI Ui;
    Ui.SetSourcePath( Src_State->Model->GetCurrentPathWithRoot() );
    Ui.SetSourceModel( SrcModel );

    if( Ui.exec() == QDialog::Accepted )
        ;

        //Src_State->View->deselectAll();
}

void CmpPanel::SelectRowOnCurrentTab( const QModelIndex& SrcIndex, bool IsMoveDown )
{
    const auto State = retrieveFocusState();
    const auto View = retrieveFocusView();
    Q_ASSERT( View != nullptr );
    if( View == nullptr )
        return;

    if( View->GetViewMode() != CViewT::VM_GRID )
        return;

    bool IsSelected = false;

    const auto& Info = ( ( CGridView* )View )->SelectRowOnCurrentTab( SrcIndex, IsMoveDown, IsSelected );

    if( Info == nullptr )
        return;

    if( Info->Name_ == ".." )
        return;

    if( IsSelected == true )
    {
        if( FlagOn( Info->Attributes_, FILE_ATTRIBUTE_DIRECTORY ) )
            State->SelectedDirectoryCount += 1;
        else
            State->SelectedFileCount += 1;

        State->SelectedSize += Info->Size_;
    }
    else
    {
        if( FlagOn( Info->Attributes_, FILE_ATTRIBUTE_DIRECTORY ) )
            State->SelectedDirectoryCount -= 1;
        else
            State->SelectedFileCount -= 1;

        State->SelectedSize -= Info->Size_;
    }

    QMetaObject::invokeMethod( this, "processPanelStatusText", Qt::QueuedConnection );
}

void CmpPanel::ReturnOnCurrentTab( const QModelIndex& SrcIndex )
{
    auto State = retrieveFocusState();
    if( State == nullptr )
        return;

    const auto ModelIndex = State->ConvertToSrcIndex( SrcIndex );
    const auto EntryInfo = State->Model->GetFileInfo( ModelIndex );

    if( EntryInfo->Attributes_ & FILE_ATTRIBUTE_DIRECTORY )
    {
        State->Model->ChangeDirectory( ModelIndex );
    }
    else
    {
        // 해당 확장자를 처리할 수 있다고 주장하는 플러그인이 있다면 처리를 맡긴다. 
        const auto StPlugInMgr = TyStPlugInMgr::GetInstance();
        std::wstring Src = State->Model->GetFileFullPath( ModelIndex ).toStdWString();
        const auto VecWCXs = StPlugInMgr->RetrieveWCXByExts( EntryInfo->Ext_ );

        if( VecWCXs.count() > 0 )
        {
            State->Model->ChangeDirectory( ModelIndex );
            return;
        }

        // TODO: 해당 항목이 내부 진입이 가능한 파일인지 확인한 후 아닐 때 실행한다.
        // NOTE: 토탈 커맨더의 경우 .DLL 등 명시적인 실행파일이 아닌 경우 레지스트리를 뒤져 실행가능하지 않다면 ShellExecuteExW 를 호출하지 않고, 오류창을 표시한다. 
        CoInitializeEx( NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE );

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
    const auto State = retrieveFocusState();
    const auto View = retrieveFocusView();
    Q_ASSERT( View != nullptr );
    if( View == nullptr )
        return;

    if( View->GetViewMode() != CViewT::VM_GRID )
        return;

    ( ( CGridView* )View )->ContextMenuOnCurrentTab( SrcIndex );
}

void CmpPanel::ExternalEditorMenu( const QModelIndex& SrcIndex )
{
    /*!
        View 가 QuickView 인지 아닌지 확인한다. 
        해당 모델의 최상위 Fs 를 획득한다.
        Fs 가 직접적으로 파일을 획득할 수 있는지 확인한다. ( PACKER 여부 )
            PACKER 라면 임시 디렉토리에 파일을 해제할 수 있는지 확인한다. 
        선택한 것이 있는가? 없다면 현재 커서가 위치한 항목 사용
        선택항목이 하나 인가? 하나라면 해당 항목 사용
        선택항목이 여러 개라면 설정에 따라 목록을 전송하거나 첫째 항목 전송
     */

    const auto StExternalMgr = TyStExternalEditorMgr::GetInstance();
    const auto ColorScheme = TyStColorSchemeMgr::GetInstance()->GetColorScheme( TyStColorSchemeMgr::GetInstance()->GetCurrentColorScheme() );
    const auto State = retrieveFocusState();
    const auto View = State->View;
    const auto Fs = State->Model->GetRoot();

    if( View->GetViewMode() != CViewT::VM_GRID )
        return;

    const auto& VecSrcModel = makeSrcModel( State );

    if( VecSrcModel.isEmpty() == true )
    {
        // 선택된 항목이 없음
        return;
    }

    if( FlagOn( Fs->GetFeatures(), nsHC::FS_FEA_PACK ) )
    {
        // TODO: 선택된 항목들의 압축을 해제한 후, 임시 파일에 대한 목록을 가져온다. 

        Q_ASSERT( false );
    }

    QMenu Menu;
    Menu.setFocus( Qt::ActiveWindowFocusReason );
    Menu.setFocusPolicy( Qt::StrongFocus );
    Menu.setFont( QFont( ColorScheme.Dialog_Font.family(), 14 ) );

    const auto FileFullPath = State->Model->GetFileFullPath( VecSrcModel[0] );
    StExternalMgr->ConstructExternalMenu( &Menu, FileFullPath );

    // TODO: 글로벌 위치를 획득한 후, 해당 위치가 항목(SrcIndex) 영역 내에 위치하는 지 확인한다. 
    // 영역내에 위치한다면 해당 위치에 표시하고, 커서가 전혀 다른 곳에 위치한다면, 항목의 영역을 구하고 해당 위치내에서 표시한다. 
    POINT Pos = {};
    GetCursorPos( &Pos );

    qDebug() << QCursor::pos() << QPoint( Pos.x, Pos.y );

    QMetaObject::invokeMethod( &Menu, "setFocus", Qt::QueuedConnection );
    auto ArrowDownEvent = new QKeyEvent( QEvent::KeyPress, Qt::Key_Down, Qt::NoModifier );
    qApp->postEvent( &Menu, ArrowDownEvent );   // 이벤트의 소유권은 Qt 에게로 이전되며, 자동으로 삭제된다. 
    const auto ac = Menu.exec( retrieveMenuPoint( QCursor::pos(), SrcIndex ) );

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

void CmpPanel::ChangeVolume( const QString& Drive )
{
    int FsIndex = -1;

    for( int idx = 0; idx < ui.cbxVolume->GetCount(); ++idx )
    {
        const auto& Fs = ui.cbxVolume->GetItem( idx )->GetUserData( Qt::UserRole ).value< nsHC::TySpFileSystem >();
        if( Fs == nullptr )
            continue;

        // TODO: Drive 가 UNC 인 경우에도 추가처리가 필요하다. 
        const auto Root = Fs->GetRoot();
        if( Root.compare( Drive, Qt::CaseInsensitive ) != 0 )
            continue;

        FsIndex = idx;
        break;
    }

    if( FsIndex >= 0 )
        ui.cbxVolume->setCurrentIndex( FsIndex );
}

void CmpPanel::ChangeCurrentPath( const QString& Path, const QString& Parameter )
{
    int FsIndex = -1;
    QString Child;

    {
        QSignalBlocker Blocker( ui.cbxVolume );

        for( int idx = 0; idx < ui.cbxVolume->GetCount(); ++idx )
        {
            const auto& Fs = ui.cbxVolume->GetItem( idx )->GetUserData( Qt::UserRole ).value< nsHC::TySpFileSystem >();
            if( Fs == nullptr )
                continue;

            // TODO: Drive 가 UNC 인 경우에도 추가처리가 필요하다. 
            const auto Root = Fs->GetRoot();
            if( Path.startsWith( Root, Qt::CaseInsensitive ) == false )
                continue;

            FsIndex = idx;
            Child = Path.mid( Root.size() );
            break;
        }

        if( FsIndex >= 0 )
            ui.cbxVolume->setCurrentIndex( FsIndex );
    }

    if( FsIndex >= 0 )
        on_cbxVolume_currentIndexChanged( FsIndex, Child );
}

//////////////////////////////////////////////////////////////////////////////
/// Slot Handler 

void CmpPanel::OnColorSchemeChanged( const TyColorScheme& ColorScheme )
{
    for( const auto& State : vecTabStates )
    {
        if( State->View == nullptr )
            continue;

        State->View->ApplyColorScheme( ColorScheme );
    }

    int a = 0;
}

void CmpPanel::OnChangedDirectory( const QString& CurrentPath )
{
    qDebug() << __FUNCTION__ << " : " << CurrentPath;

    if( CurrentPath.isEmpty() == true )
    {
        // TODO: Model 에서 해당 이름을 전달해줄 수 있어야 한다. 
        ui.tabWidget->setTabText( retrieveCurrentIndex(), "\\내 PC" );
        return;
    }

    QDir dir( CurrentPath );
    if( dir.exists() == true )
    {
        const auto Current = ui.cbxVolume->GetItem( ui.cbxVolume->currentIndex() );
        const auto Fs = Current->GetUserData( Qt::UserRole ).value< nsHC::TySpFileSystem >();

        if( CurrentPath.contains( Fs->GetRoot() ) == false )
        {
            // NOTE: 볼륨이 설정과 달라졌다면 cbxVolume 을 찾아서 변경해야 한다.
            for( int idx = 0; idx < ui.cbxVolume->GetCount(); ++idx )
            {
                const auto Item = ui.cbxVolume->GetItem( idx );
                const auto ItemFs = Item->GetUserData( Qt::UserRole ).value< nsHC::TySpFileSystem >();
                if( CurrentPath.contains( ItemFs->GetRoot() ) == false )
                    continue;

                QSignalBlocker locker( ui.cbxVolume );
                ui.cbxVolume->setCurrentIndex( idx );
                processVolumeStatusText( ItemFs );
                break;
            }
        }

        ui.tabWidget->setTabText( ui.tabWidget->currentIndex(), CurrentPath.left( 2 ) + dir.dirName() );
    }
    else
    {
        ui.tabWidget->setTabText( ui.tabWidget->currentIndex(), CurrentPath );
    }

    processPanelStatusText();

    emit sig_NotifyCurrentDirectory( CurrentPath );
}

void CmpPanel::on_cbxVolume_currentIndexChanged( int index, const QString& Path )
{
    // TODO: 실제 파일시스템 외의 것들은 예외처리 필요함
    const auto State = retrieveFocusState();
    if( State == nullptr )
        return;

    // 해당 탭에서 QuickView 를 표시하고 있었다면 닫고 GridView 로 전환한다.
    if( State->GetViewMode() == CViewT::VM_QUICK )
        CloseQuickView( retrieveCurrentIndex() );

    const auto ViewMode = State->View->GetViewMode();
    Q_ASSERT( ViewMode == CViewT::VM_GRID );

    const auto Item = ui.cbxVolume->GetItem( index );
    if( Item == nullptr )
        return;

    const auto Fs = Item->GetUserData( Qt::UserRole ).value< nsHC::TySpFileSystem >();

    State->Model->SetRoot( Fs );
    State->Model->SetCurrentPath( "\\" );
    if( Path.isEmpty() == true )
        State->Model->ChangeDirectory( QModelIndex() );
    else
        State->Model->ChangeDirectory( Path );

    State->View->AdjustSizeToContents();
    QTimer::singleShot( 0, [State]() {State->View->EnsureKeyboardFocusOnView(); } );

    processVolumeStatusText( Fs );
}

void CmpPanel::on_tabWidget_currentChanged( int Index )
{
    // TODO: 모든 탭을 검사하여 기존에 탭 중에 QuickView 모드인 것이 있다면 닫아야 한다.

    if( Index < 0 )
        return;
    
    if( vecTabStates.isEmpty() == true )
        return;

    if( Index >= vecTabStates.size() )
        return;

    // TODO: 해당 탭에 설정된 Root 의 볼륨으로 cbxVolume
    // TODO: VolumeStatus
    // TODO: DirectoryStatus

    const auto View = vecTabStates.value( Index )->View;

    // retrieveFocusState();

    //QTimer::singleShot( 0, [View]() {
    //    View->grid()->setFocus();
    //    View->grid()->viewport()->setFocus( Qt::MouseFocusReason );
    //    View->grid()->viewport()->raise();
    //    View->grid()->viewport()->activateWindow();

    //    if( View->getRowCount() > 0 )
    //    {
    //        if( View->focusedRowIndex() == View->modelController()->getGridRowCount() - 1 )
    //        {
    //            // 가장 마지막임
    //            View->navigateUp();
    //            View->navigateDown();
    //        }
    //        else
    //        {
    //            View->navigateDown();
    //            View->navigateUp();
    //        }
    //    }
    //} );
}

//////////////////////////////////////////////////////////////////////////////
/// Private

bool CmpPanel::eventFilter( QObject* Object, QEvent* Event )
{
////
////    // 이름 변경 편집기가 활성화된 상태에서 위/아래 화살표를 누르면 이동한다.
////    if( Event->type() == QEvent::KeyPress && 
////        Object->objectName() == "edtFieldOnList" )
////    {
////        const auto KeyEvent = dynamic_cast< QKeyEvent* >( Event );
////        const auto Pressed = KeyEvent->keyCombination().key();
////        if( Pressed == Qt::Key_Up || Pressed == Qt::Key_Down )
////        {
////            const auto View = retrieveFocusView();
////            if( View->activeEditor() != nullptr )
////                View->closeEditor();
////
////            QMetaObject::invokeMethod( this, "RenameFileName", Qt::QueuedConnection, Q_ARG( const QModelIndex&, QModelIndex() ) );
////            return true;
////        }
////    }
////

    return false;
}

void CmpPanel::resizeEvent( QResizeEvent* event )
{
    QWidget::resizeEvent( event );
}

void CmpPanel::initializeUIs()
{
    ui.cbxVolume->setMinimumHeight( 32 );
    ui.cbxVolume->view()->setMinimumWidth( 160 );

    while( ui.tabWidget->count() > 0 )
        ui.tabWidget->removeTab( 0 );

    // 탭에 어떠한 뷰도 없으므로, 기보값으로 GridView 를 추가한다.
    const auto StColumnMgr = TyStColumnMgr::GetInstance();
    auto ColumnView = StColumnMgr->GetColumnView( 0 );

    const auto DefaultView = static_cast< CGridView* >( createGridView() );
    if( DefaultView == nullptr )
        return;

    ui.tabWidget->blockSignals( true );
    const auto TabIndex = ui.tabWidget->addTab( DefaultView, "" );
    ui.tabWidget->setCurrentIndex( -1 );
    ui.tabWidget->blockSignals( false );

    connect( DefaultView, &CViewT::sig_NotifyViewActivated, this, &CmpPanel::sig_NotifyPanelActivated );

    const auto FsModel = new CFSModel;
    const auto ProxyModel = new CFSProxyModel;
    connect( FsModel, &CFSModel::sigChangedDirectory, this, &CmpPanel::OnChangedDirectory );

    DefaultView->SetModel( FsModel, ProxyModel, &ColumnView );

    const auto State = std::make_shared< TyTabState >( TabIndex, DefaultView, nullptr, FsModel, ProxyModel );
    vecTabStates.push_back( State );
}

CViewT* CmpPanel::createGridView()
{
    CGridView* View = new CGridView;

    return View;
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

CViewT* CmpPanel::retrieveFocusView() const
{
    const auto Current = retrieveCurrentIndex();
    if( Current < 0 || vecTabStates.isEmpty() == true )
        return {};

    const auto& State = vecTabStates.value( Current );
    if( State->QuickView != nullptr )
        State->QuickView;

    return State->View;
}

QPoint CmpPanel::retrieveMenuPoint( const QPoint& GlobalCursor, QModelIndex SrcIndex )
{
    QPoint Pos( GlobalCursor );
    const auto View = retrieveFocusView();

    do
    {
        if( View->GetViewMode() != CViewT::VM_GRID )
            break;
        
        const auto GridViewBase = qobject_cast< CGridView* >( View )->BaseView();
        const auto Row = GridViewBase->getRow( SrcIndex );

        if( SrcIndex.isValid() == false || Row.isValid() == false )
            return Pos;

        QRect GlobalRowRect;
        const auto Grid = GridViewBase->grid();
       
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

            if( Info.columnIndex() == GridViewBase->getColumnCount() - 1 )
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

    } while( false );

    return Pos;
}

QVector<QModelIndex> CmpPanel::makeSrcModel( const TySpTabState& SrcState )
{
    QVector<QModelIndex> SrcModel;
    const auto View = qobject_cast< CGridView* >( SrcState->View );

    do
    {
        if( View == nullptr )
            break;

        const auto BaseGrid = View->BaseView();
        if( BaseGrid == nullptr )
            break;

        for( const auto Row : BaseGrid->selection()->selectedRowIndexes() )
        {
            const auto Index = SrcState->ConvertToSrcIndex( Row );
            if( SrcState->Model->GetName( Index ) == ".." )
                continue;

            SrcModel.push_back( Index );
        }

        // 선택된 항목이 없다면 현재 커서가 위치한 항목을 복사한다. 
        if( SrcModel.isEmpty() == true )
        {
            const auto Row = BaseGrid->focusedRow();
            if( Row.isValid() == true )
            {
                const auto Index = SrcState->ConvertToSrcIndex( Row.modelIndex( 0 ) );
                if( SrcState->Model->GetName( Index ) != ".." )
                {
                    SrcModel.push_back( Index );
                }
            }
        }

    } while( false );

    return SrcModel;
}

void CmpPanel::processVolumeStatusText( const nsHC::TySpFileSystem& SpFileSystem ) const
{
    // 기본 설정
    ui.lblVolumeStatus->setText( "[_없음_] 0 / 0 (남음/전체)" );

    if( SpFileSystem == nullptr )
        return;

    // NOTE: UNC(SMB) 의 경우에는 \\MyServer\\Share\\ 의 형태로 마지막에 \\ 로 끝나야 한다.

    if( SpFileSystem->GetCate() == nsHC::FS_CATE_LOCAL )
    {
        const auto Root = QString( "%1\\" ).arg( SpFileSystem->GetRoot() ).toStdWString();

        ULARGE_INTEGER Avail, TotalBytes, TotalFree;
        if( GetDiskFreeSpaceExW( Root.c_str(), &Avail, &TotalBytes, &TotalFree ) != FALSE )
        {
            const auto SizeStyle = static_cast< TyEnSizeStyle >( StSettings->value( "Configuration/SizeInHeader" ).toInt() );
            const auto AvailText = GetFormattedSizeText( Avail.QuadPart, SizeStyle );
            const auto TotalText = GetFormattedSizeText( TotalBytes.QuadPart, SizeStyle );

            auto VolumeName = SpFileSystem->GetVolumeName();
            if( VolumeName.isEmpty() == true )
                VolumeName = tr( "_없음_" );

            ui.lblVolumeStatus->setText( QString( "[%1] %2 / %3 (남음/전체)" ).arg( VolumeName ).arg(AvailText).arg(TotalText));
        }
    }
    else if( SpFileSystem->GetCate() == nsHC::FS_CATE_REMOTE )
    {
        //Q_ASSERT( false );
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

//void CmpPanel::Initialize()
//{
//    //ui.cbxVolume->setEditable( true );
//    //ui.cbxVolume->lineEdit()->setReadOnly( true );
//
//    const auto ColorScheme = TyStColorSchemeMgr::GetInstance()->GetColorScheme( TyStColorSchemeMgr::GetInstance()->GetCurrentColorScheme() );
//
//    sltColorSchemeChanged( ColorScheme );
//
//    //connect( &viewClickTimer, &QTimer::timeout, [this]() {
//    //    const auto View = retrieveFocusView();
//    //    if( View == nullptr )
//    //        return;
//
//    //    View->showEditor( GridEditor::ActivateByClick );
//    //} );
//
//    initializeVolumeList();
//
//
//    //AddTab();
//}
//
//void CmpPanel::sltColorSchemeChanged( const TyColorScheme& ColorScheme )
//{
//    auto Palette = ui.cbxVolume->palette();
//    ui.cbxVolume->setFont( ColorScheme.Dialog_Font );
//
//    Palette.setColor( ui.cbxVolume->backgroundRole(), ColorScheme.Dialog_BGColor );
//    Palette.setColor( QPalette::Base, ColorScheme.Dialog_BGColor );
//    Palette.setColor( ui.cbxVolume->foregroundRole(), ColorScheme.Dialog_FGColor );
//
//    // TODO: 현재 있는 그리드들에 색상 / 글꼴 적용
//}
//

//
////Qtitan::GridBandedTableView* CmpPanel::GetFocusView() const
////{
////    return retrieveFocusView();
////}
////
////


////void CmpPanel::FileCopyToOtherPanel( CmpPanel* Dst )
////{
////    Q_ASSERT( Dst != nullptr );
////    if( Dst == nullptr )
////        return;
////
////    const auto Src_State = retrieveFocusState();
////    const auto Dst_State = Dst->retrieveFocusState();
////
////    // 선택된 항목들을 추가한다.
////    QVector< QModelIndex > SrcModel = makeSrcModel( Src_State );
////
////    if( SrcModel.isEmpty() == true )
////    {
////        // TODO: 메시지 출력, 복사할 파일 없음
////        return;
////    }
////
////    QFileCopyUI Ui;
////    Ui.SetSourcePath( Src_State->Model->GetFileFullPath( "" ) );
////    Ui.SetSourceModel( SrcModel );
////    Ui.SetDestinationPath( Dst_State->Model->GetFileFullPath( "" ) );
////
////    Ui.exec();
////}
////
////void CmpPanel::FileNormalization( const QModelIndex& SrcIndex )
////{
////    const auto State = retrieveFocusState();
////    const auto ModelIndex = State->ProxyModel->mapToSource( SrcIndex );
////    const auto Node = State->Model->GetFileInfo( ModelIndex );
////
////    if( Node.IsNormalizedByNFD == FALSE )
////        return;
////
////    const DWORD Ret = State->Model->Rename( ModelIndex, Node.Name.normalized( QString::NormalizationForm_C ) );
////    if( Ret != ERROR_SUCCESS )
////    {
////        // TODO: 오류처리, 오류창 표시
////        // 관리자 권한으로 재시도할 수 있음
////        return;
////    }
////
////    State->Model->setData( ModelIndex, Node.Name.normalized( QString::NormalizationForm_C ), Qt::EditRole );
////}
////
////void CmpPanel::FileSetAttrib( const QModelIndex& SrcIndex )
////{
////    UNREFERENCED_PARAMETER( SrcIndex );
////
////    const auto Src_State = retrieveFocusState();
////    QVector< QModelIndex > SrcModel = makeSrcModel( Src_State );
////
////    QFileAttribUI Ui;
////    Ui.SetSourcePath( Src_State->Model->GetFileFullPath( "" ) );
////    Ui.SetSourceModel( SrcModel );
////
////    if( SrcModel.isEmpty() == true )
////    {
////        // TODO: 메시지 출력, 복사할 파일 없음
////        return;
////    }
////
////    if( Ui.exec() == QDialog::Accepted )
////        Src_State->View->deselectAll();
////}
////
////void CmpPanel::RenameFileName( const QModelIndex& SrcIndex )
////{
////    UNREFERENCED_PARAMETER( SrcIndex );
////
////    viewClickTimer.stop();
////
////    for( const auto& State : vecTabStates )
////        State->LastFocusedRowIndex = -1;
////
////    const auto View = retrieveFocusView();
////    View->setFocusedColumnIndex( 0 );
////    View->showEditor( GridEditor::ActivateByKeyPress_F2 );
////}
////

////void CmpPanel::ViewOnLister( const QModelIndex& SrcIndex )
////{
////    const auto State = retrieveFocusState();
////    const auto FileFullPath = State->Model->GetFileFullPath( State->ProxyModel->mapToSource( SrcIndex ) );
////    const auto StPlugInMgr = TyStPlugInMgr::GetInstance();
////
////    auto Menu = new QMenu;
////    Menu->setFocus( Qt::ActiveWindowFocusReason );
////    Menu->setFocusPolicy( Qt::StrongFocus );
////    Menu->setFont( QFont( "Sarasa Mono K Light", 14 ) );
////    Menu->deleteLater();
////
////    const auto ViewerCount = StPlugInMgr->ConstructWLXMenu( Menu, FileFullPath );
////    if( ViewerCount == 0 )
////        return;
////
////    TySpWLX Viewer;
////
////    if( ViewerCount == 1 )
////    {
////        const auto ac = Menu->actions()[ 0 ];
////        Viewer = ac->data().value< TySpWLX >();
////    }
////
////    if( ViewerCount > 1 )
////    {
////        Menu->setActiveAction( Menu->actions()[ 0 ] );
////        QMetaObject::invokeMethod( Menu, "setFocus", Qt::QueuedConnection );
////        const auto ac = Menu->exec( retrieveMenuPoint( QCursor::pos(), SrcIndex ) );
////
////        // 취소하거나 등등
////        if( ac == nullptr )
////            return;
////
////        Viewer = ac->data().value< TySpWLX >();
////    }
////    
////    auto DlgViewer = new QDlgViewer;
////    DlgViewer->SetFileName( FileFullPath );
////    DlgViewer->SetInternalViewer( Viewer );
////    DlgViewer->show();
////}
////
////int CmpPanel::InitializeGrid()
////{
////    const auto Page = new QWidget;
////    const auto Vertical = new QVBoxLayout;
////    Page->setLayout( Vertical );
////    Page->setSizePolicy( QSizePolicy( QSizePolicy::Policy::Expanding, QSizePolicy::Policy::Expanding ) );
////
////    const auto Grid = new Qtitan::Grid;
////    Grid->setViewType( Qtitan::Grid::BandedTableView );
////    Grid->setSizePolicy( QSizePolicy( QSizePolicy::Policy::Expanding, QSizePolicy::Policy::Expanding ) );
////    Grid->setAttribute( Qt::WA_KeyCompression );
////    Grid->setAttribute( Qt::WA_InputMethodEnabled );
////    Grid->viewport()->setAttribute( Qt::WA_KeyCompression );
////    Grid->viewport()->setAttribute( Qt::WA_InputMethodEnabled );
////
////    Vertical->addWidget( Grid );
////
////    // TODO: 탭 이름 => 현재 디렉토리, 만약 루트 디렉토리라면 드라이브 문자 함께 표시
////    const auto currentIndex = ui.tabWidget->addTab( Page, tr("D:/") );
////    ui.tabWidget->setCurrentIndex( currentIndex );
////
////    Grid->installEventFilter( this );
////    Grid->setContextMenuPolicy( Qt::CustomContextMenu );
////
////    const auto View = Grid->view< GridBandedTableView >();
////    Grid->viewport()->setFocusPolicy( Qt::StrongFocus );
////
////    View->beginUpdate();
////    auto& BaseOpts = View->options();
////    auto& TableOpts = View->tableOptions();
////    auto& BandedOpts = View->bandedOptions();
////
////    BaseOpts.setShowWaitCursor( true );
////    BaseOpts.setGroupsHeader( false );
////    BaseOpts.setScrollBarsTransparent( true );
////    BaseOpts.setMainMenuDisabled( false );              // true 이면 
////    BaseOpts.setAutoCreateColumns( false );
////    BaseOpts.setColumnMovingEnabled( false );
////    BaseOpts.setAlternatingRowColors( false );
////    BaseOpts.setScrollRowStyle( Qtitan::ScrollByPixel );
////    BaseOpts.setGridLines( Qtitan::LinesNone );
////    BaseOpts.setModelDecoration( true );
////    BaseOpts.setModelItemsDragEnabled( true );
////    BaseOpts.setRowStyle( Qtitan::GridViewOptions::RowStyleNormal );
////    BaseOpts.setSelectedDecorationOpacity( 0.2 );
////    BaseOpts.setSelectionPolicy( Qtitan::GridViewOptions::IgnoreSelection );
////    BaseOpts.setFieldChooserEnabled( false );
////    BaseOpts.setRubberBandSelection( false );
////    BaseOpts.setTransparentBackground( true );
////    BaseOpts.setBackgroundColor( QColor( "cyan" ) );
////    BaseOpts.setSelectedBackgroundColor( QColor( 255, 112, 43 ) );
////    BaseOpts.setSelectedInverseColor( true );
////    BaseOpts.setModelDecorationOpacityRole( FSModel::USR_ROLE_HIDDENOPACITY );
////    BaseOpts.setRowAutoHeight( true );
////
////    BaseOpts.setGroupsHeaderText( "Fdsfds" );
////    BaseOpts.setColumnHeaderHints( true );
////    
////    BaseOpts.setFocusFollowsMouse( false );
////    BaseOpts.setFocusFrameEnabled( false );
////    BaseOpts.setShowFocusDecoration( true );
////    BaseOpts.setKeepFocusedRow( true );
////    BaseOpts.setRowFocusMode( true );
////    BaseOpts.setRowFocusColor( QColor( 51, 79, 102 ) );
////    
////    BaseOpts.setDragEnabled( true );
////    BaseOpts.setDropEnabled( true );
////    BaseOpts.setFieldChooserEnabled( false );
////    BaseOpts.setGroupsHeaderTextColor( "white" );
////    
////    TableOpts.setRowsQuickSelection( false );
////    TableOpts.setRowFrozenButtonVisible( false );
////    TableOpts.setColumnsQuickCustomization( false );
////    
////    BandedOpts.setBandsHeader( false );
////    BandedOpts.setBandsQuickCustomization( false );
////    
////
////    const auto StColumnMgr = new CColumnMgr;
////    StColumnMgr->Initialize();
////    const auto ColumnView = StColumnMgr->GetColumnView( 0 );
////
////    const auto Model = new FSModel;
////    const auto ProxyModel = new FSProxyModel;
////
////    Model->SetRoot( "C:" );
////    Model->SetCurrentPath( "\\" );
////    Model->SetColumnView( ColumnView );
////    connect( Model, &FSModel::sigChangedDirectory, this, &CmpPanel::oo_ChangedDirectory );
////
////    ProxyModel->setSourceModel( Model );
////
////    View->removeBands();
////    View->addBand( tr( "FS" ) );
////
////    // TODO: CColumnMgr 은 당연히 전역 싱글턴으로 분리되어야 한다. 
////    View->setModel( ProxyModel );
////    
////    BaseOpts.setBackgroundColor( "black" );
////    
////    // 모든 단축키 등 해제
////    for( const auto& act : View->actions() )
////    {
////        (void)act->disconnect();
////        act->setShortcut( {} );
////    }
////    
////    Qtitan::GridBandedTableColumn* GridColumn = nullptr;
////    
////    // GridColumn = View->addColumn( 0, )
////    
////    for( const auto& Column : ColumnView.VecColumns )
////    {
////        GridColumn = View->addColumn( Column.Index, Column.Name );
////        if( GridColumn != nullptr )
////        {
////            GridColumn->setBandIndex( 0 );
////            GridColumn->setAutoWidth( true );
////            GridColumn->setRowIndex( Column.Row );
////            GridColumn->setMinWidth( 30 );
////            // GridColumn->setWidth( 100 );
////            GridColumn->setTextAlignment( Column.Align );
////            GridColumn->setTextColor( "silver" );
////            GridColumn->setDecorationColor( "blue" );   // 컬럼 배경 색
////            GridColumn->editorRepository()->setEditorActivationPolicy( GridEditor::ActivationPolicy() );
////            if( Column.Content.contains( "HC.Fs.name", Qt::CaseInsensitive ) == true )
////            {
////                GridColumn->setSortLocaleAware( true );
////                GridColumn->setSortOrder( Qtitan::SortAscending, true );
////                GridColumn->dataBinding()->setSortRole( Qt::UserRole );
////                
////            }
////            GridColumn->setFilterButtonVisible( false );
////        }
////    }
////    
////    connect( View, &Qtitan::GridViewBase::contextMenu, this, &CmpPanel::oo_grdLocal_contextMenu );
////    connect( View, &Qtitan::GridViewBase::cellClicked, this, &CmpPanel::oo_grdLocal_cellClicked );
////    connect( View, &Qtitan::GridViewBase::rowDblClicked, this, &CmpPanel::oo_grdLocal_rowDblClicked );
////    connect( View, &Qtitan::GridViewBase::focusRowChanged, this, &CmpPanel::oo_grdLocal_rowFocusChanged );
////    connect( View, &Qtitan::GridViewBase::editorStarted, this, &CmpPanel::oo_grdLocal_editorStarted );
////    connect( View, &Qtitan::GridViewBase::editorPosting, this, &CmpPanel::oo_grdLocal_editorPosting );
////    connect( View, &Qtitan::GridViewBase::selectionChanged, this, &CmpPanel::oo_grdLocal_selectionChanged );
////
////    View->endUpdate();
////    
////    auto State = std::make_shared<TyTabState>( currentIndex, View, Model, ProxyModel );
////    vecTabStates.push_back( State );
////
////    Model->ChangeDirectory( QModelIndex() );
////    View->bestFit( FitToHeaderPercent );
////    
////    return currentIndex;
////}
////
//
////void CmpPanel::on_btnGridStyle_clicked( bool checked )
////{
////    const auto View = retrieveFocusView();
////    if( View == nullptr )
////        return;
////
////    QDlgGridStyle Styler;
////    //Styler.setStyleText( mapTabToStats[ currentIndex ].View->grid()->styleSheet() );
////    Styler.setStyleText( qApp->styleSheet() );
////    Styler.exec();
////
////    View->beginUpdate();
////    qApp->setStyleSheet( Styler.styleText() );
////    View->grid()->setStyleSheet( Styler.styleText() );
////    View->endUpdate();
////}
////

////QModelIndex CmpPanel::retrieveFocusViewCursorIndex() const
////{
////    const auto View = retrieveFocusView();
////    if( View == nullptr )
////        return {};
////
////    const auto& Row = View->focusedRow();
////    if( Row.isValid() == false )
////        return {};
////
////    return Row.modelIndex( 0 );
////}
