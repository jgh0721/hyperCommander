#include "stdafx.h"
#include "cmpPanel.hpp"
#include "UniqueLibs/qtWorkspaceRestore.hpp"
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
    QTimer::singleShot( 100, this, [this]() {
        if( workspaceRestored_ || ui.cbxVolume->GetCount() == 0 )
            return;

        // Start in the user's home; restored tabs keep their saved location.
        // Never probe a mapped drive just to populate the first window.
        hc::workspace::PaneState Initial;
        hc::workspace::TabState Tab; Tab.id_ = QUuid::createUuid().toString(QUuid::WithoutBraces);
        Tab.location_ = {hc::storage::LocatorKind::Local, QDir::toNativeSeparators(QDir::homePath()).toStdString()};
        Initial.tabs_.append(Tab);
        RestoreWorkspace(Initial);
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


void CmpPanel::AddTab(bool Navigate)
{
    if (vecTabStates.size() >= 64) return;
    const auto Previous = CaptureWorkspace();
    auto Columns = TyStColumnMgr::GetInstance()->GetColumnView(0);
    auto* View = static_cast<CGridView*>(createGridView());
    auto* Model = new CFSModel; auto* Proxy = new CFSProxyModel;
    View->SetModel(Model, Proxy, &Columns);
    const auto State = std::make_shared<TyTabState>(int(vecTabStates.size()), View, nullptr, Model, Proxy);
    State->workspace_.id_ = QUuid::createUuid().toString(QUuid::WithoutBraces);
    Model->setProperty("hc.tabId", State->workspace_.id_);
    vecTabStates.append(State);
    connect(View, &CViewT::sig_NotifyViewActivated, this, &CmpPanel::sig_NotifyPanelActivated);
    connect(Model, &CFSModel::sigChangedDirectory, this, &CmpPanel::OnChangedDirectory);
    connect(Model, &CFSModel::MetadataChanged, Proxy, [Proxy] { Proxy->SetSearch(Proxy->Search()); });
    connect(Model, &CFSModel::RetryRequested, this, [this, Model] {
        for (int Index = 0; Index < vecTabStates.size(); ++Index)
            if (vecTabStates[Index]->Model == Model) { restoreTab(Index); break; }
    });
    connect(Model, &CFSModel::sigRefreshCompleted, this, [this, Model] {
        for (const auto& Tab : vecTabStates) {
            if (Tab->Model != Model) continue;
            if (Tab->restoreSelection_ && !Model->LastNavigationError()) {
                static_cast<CGridView*>(Tab->View)->RestoreSelection(Tab->workspace_.selection_, Tab->workspace_.focus_);
                Tab->restoreSelection_ = false;
            }
            const auto Selection = static_cast<CGridView*>(Tab->View)->CaptureSelection();
            const QSet<QString> Keys(Selection.begin(), Selection.end());
            Tab->SelectedSize = 0; Tab->SelectedFileCount = Tab->SelectedDirectoryCount = 0;
            for (int Row = 0; Row < Model->rowCount(); ++Row) {
                const auto Item = Model->GetFileInfo(Model->index(Row, 0));
                if (!Item || !Item->storageEntry_ || !Keys.contains(hc::workspace::SelectionKey(*Item->storageEntry_))) continue;
                if (Item->storageEntry_->directory_) ++Tab->SelectedDirectoryCount;
                else { ++Tab->SelectedFileCount; Tab->SelectedSize += Item->storageEntry_->size_; }
            }
            break;
        }
        updateWorkspaceStatus();
    });
    { QSignalBlocker Blocker(ui.tabWidget);
      ui.tabWidget->addTab(View, tr("새 탭")); ui.tabWidget->setCurrentIndex(vecTabStates.size() - 1); }
    // uic assigns the pane objectName after its constructor returns. Defer the
    // initial M3 profile lookup until that stable pane identity is available.
    if (objectName().isEmpty()) QTimer::singleShot(0, View, [View] { View->RestoreWorkspaceView({}); });
    else View->RestoreWorkspaceView({});
    const auto Color = TyStColorSchemeMgr::GetInstance();
    View->ApplyColorScheme(Color->GetColorScheme(Color->GetCurrentColorScheme()));
    if (Navigate) {
        if (!Previous.tabs_.isEmpty()) {
            auto Saved = Previous.tabs_.value(Previous.active_); Saved.id_ = State->workspace_.id_;
            State->workspace_ = Saved; State->restoreSelection_ = true;
            View->RestoreWorkspaceView(Saved.view_); restoreTab(vecTabStates.size() - 1);
        } else if (const auto Item = ui.cbxVolume->GetItem(ui.cbxVolume->currentIndex())) {
            Model->SetRoot(Item->GetUserData(Qt::UserRole).value<nsHC::TySpFileSystem>());
            Model->SetCurrentPath("\\"); Model->Refresh();
        }
    }
    on_tabWidget_currentChanged(ui.tabWidget->currentIndex());
    View->AdjustSizeToContents();
}

hc::workspace::PaneState CmpPanel::CaptureWorkspace() const
{
    hc::workspace::PaneState Result; Result.active_ = std::max(0, ui.tabWidget->currentIndex());
    for (const auto& State : vecTabStates) {
        auto Tab = State->workspace_;
        const auto Model = static_cast<CFSModel*>(State->Model);
        const auto View = static_cast<CGridView*>(State->View);
        if (!State->pending_) {
            Tab.location_ = Model->WorkspaceLocation();
            if (!Model->GetRoot() && !Model->HasPlaceholder() && Tab.location_.root_.empty())
                Tab.location_ = {hc::storage::LocatorKind::Local, QDir::toNativeSeparators(QDir::homePath()).toStdString()};
            if (Tab.location_.kind_ == hc::storage::LocatorKind::Plugin || Tab.location_.kind_ == hc::storage::LocatorKind::Archive) {
                Tab.provider_ = Model->property("hc.workspaceProvider").toString();
                Tab.nativeRoot_ = Model->property("hc.workspaceNativeRoot").toString();
                if (const auto Pack = dynamic_cast<nsHC::CFSPack*>(Model->GetRoot().get()); Pack && Pack->GetWCX())
                    for (const auto& Choice : TyStPlugInMgr::GetInstance()->RetrieveWCXByExts(QFileInfo(Pack->GetRoot()).suffix()))
                        if (Choice.FileFullPath.compare(Pack->GetWCX()->PluginPath(), Qt::CaseInsensitive) == 0) { Tab.provider_ = Choice.hostManifest_; break; }
            } else { Tab.provider_.clear(); Tab.nativeRoot_.clear(); }
            if (!State->restoreSelection_) { Tab.selection_ = View->CaptureSelection(); Tab.focus_ = View->CaptureFocus(); }
        }
        if (Tab.location_.kind_ == hc::storage::LocatorKind::Plugin) {
            const auto Path = hc::workspace::PluginPath(Tab.location_);
            if (!Path.isEmpty()) Tab.location_.root_ = Path.toStdString();
        }
        Tab.view_ = View->CaptureWorkspaceView(); Result.tabs_.append(std::move(Tab));
    }
    return Result;
}

void CmpPanel::RestoreWorkspace(const hc::workspace::PaneState& Pane, const QJsonObject& Defaults)
{
    if (Pane.tabs_.isEmpty() || Pane.tabs_.size() > 64 || Pane.active_ < 0 || Pane.active_ >= Pane.tabs_.size()) return;
    workspaceRestored_ = true;
    // A preview temporarily replaces its grid in QTabWidget. Restore that grid
    // first so replacing a workspace also destroys its hidden model/watchers.
    for (int Index = 0; Index < vecTabStates.size(); ++Index)
        if (vecTabStates[Index]->QuickView) CloseQuickView(Index);
    { QSignalBlocker Blocker(ui.tabWidget);
      while (ui.tabWidget->count()) { auto* Widget = ui.tabWidget->widget(0); ui.tabWidget->removeTab(0); delete Widget; }
      vecTabStates.clear(); }
    for (const auto& Tab : Pane.tabs_) {
        AddTab(false); const auto State = vecTabStates.back();
        State->workspace_ = Tab; State->pending_ = true; State->restoreSelection_ = true;
        State->Model->setProperty("hc.tabId", Tab.id_);
        static_cast<CGridView*>(State->View)->RestoreWorkspaceView(hc::workspace::MergeView(Defaults, Tab.view_));
        ui.tabWidget->setTabText(vecTabStates.size() - 1, QString::fromStdString(Tab.location_.root_));
    }
    { QSignalBlocker Blocker(ui.tabWidget); ui.tabWidget->setCurrentIndex(Pane.active_); }
    on_tabWidget_currentChanged(Pane.active_);
}

void CmpPanel::restoreTab(int Index)
{
    if (Index < 0 || Index >= vecTabStates.size()) return;
    const auto State = vecTabStates[Index]; State->pending_ = false;
    // Reuse discovered volumes, retaining their cached labels and capabilities
    // without another filesystem-information query during workspace restore.
    nsHC::TySpFileSystem Native;
    const auto Path = QDir::toNativeSeparators(QString::fromStdString(State->workspace_.location_.root_));
    for (int Volume = 0; Volume < ui.cbxVolume->GetCount(); ++Volume) {
        const auto Fs = ui.cbxVolume->GetItem(Volume)->GetUserData(Qt::UserRole).value<nsHC::TySpFileSystem>();
        if (Fs && !Fs->GetRoot().isEmpty() && Path.startsWith(Fs->GetRoot(), Qt::CaseInsensitive)) { Native = Fs; break; }
    }
    RestoreWorkspaceLocation(static_cast<CFSModel*>(State->Model), State->workspace_, Native);
}

void CmpPanel::updateWorkspaceStatus()
{
    if (!workspaceStatus_ || ui.tabWidget->currentIndex() < 0 || ui.tabWidget->currentIndex() >= vecTabStates.size()) return;
    const auto Model = static_cast<CFSModel*>(vecTabStates[ui.tabWidget->currentIndex()]->Model);
    const auto Location = Model->WorkspaceLocation();
    int Volume = -1;
    if (Location.kind_ == hc::storage::LocatorKind::Local || Location.kind_ == hc::storage::LocatorKind::Smb || Location.kind_ == hc::storage::LocatorKind::Archive) {
        const auto Path = QDir::toNativeSeparators(QString::fromStdString(Location.root_));
        for (int Index = 0; Index < ui.cbxVolume->GetCount(); ++Index) {
            const auto Fs = ui.cbxVolume->GetItem(Index)->GetUserData(Qt::UserRole).value<nsHC::TySpFileSystem>();
            if (Fs && !Fs->GetRoot().isEmpty() && Path.startsWith(Fs->GetRoot(), Qt::CaseInsensitive)) { Volume = Index; break; }
        }
    }
    if (ui.cbxVolume->currentIndex() != Volume) {
        QSignalBlocker Blocker(ui.cbxVolume); ui.cbxVolume->setCurrentIndex(Volume);
        ui.lblVolumeStatus->setText(Model->GetRoot() ? Model->GetRoot()->GetRoot() : QString{});
    }
    const auto Error = Model->LastNavigationError();
    workspaceStatus_->setText(Error ? tr("%1 · %2").arg(QString::fromStdString(Model->WorkspaceLocation().root_), QString::fromStdString(Error->code_)) : QString{});
    workspaceStatus_->setVisible(bool(Error)); retry_->setVisible(bool(Error));
    workspaceStatus_->parentWidget()->setVisible(bool(Error));
    retry_->setEnabled(!Model->property("hc.restorePending").toBool());
    processPanelStatusText();
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

    const auto Current = ui.tabWidget->currentIndex();
    if (Current < 0 || Current >= vecTabStates.size()) return;
    // Restore the original grid before deleting a preview tab so neither its
    // hidden model nor an active viewer session survives tab closure.
    if (vecTabStates[Current]->QuickView) CloseQuickView(Current);
    const auto Closing = ui.tabWidget->widget(Current);
    {
        QSignalBlocker Blocker(ui.tabWidget);
        ui.tabWidget->removeTab(Current);
        vecTabStates.removeAt(Current);
        ui.tabWidget->setCurrentIndex(std::min(Current, ui.tabWidget->count() - 1));
    }
    delete Closing;
    on_tabWidget_currentChanged(ui.tabWidget->currentIndex());
    SetFocusView(ui.tabWidget->currentIndex());
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
    if( TabIndex < 0 || TabIndex >= vecTabStates.size() )
        return {};

    const auto& State = vecTabStates.value( TabIndex );
    if( State->QuickView != nullptr )
        return State->QuickView;

    return State->View;
}

void CmpPanel::OpenQuickView( int TabIndex, const QString& FilePath )
{
    if( TabIndex < 0 || TabIndex >= vecTabStates.size() )
        return;

    if (vecTabStates[TabIndex]->QuickView) CloseQuickView(TabIndex);

    {
        QSignalBlocker Blocker( ui.tabWidget );
        const auto& State = vecTabStates.value( TabIndex );
        const auto& TabTitle = ui.tabWidget->tabText( TabIndex );

        ui.tabWidget->removeTab( TabIndex );
        State->QuickView = new CQuickView;
        ( ( CQuickView* )State->QuickView )->SetFilePath( FilePath );
        ui.tabWidget->insertTab( TabIndex, State->QuickView, TabTitle );
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
        ui.tabWidget->insertTab( TabIndex, State->View, TabTitle );
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


void CmpPanel::OnChangedDirectory(const QString& CurrentPath)
{
    int Index = ui.tabWidget->currentIndex();
    if (sender()) for (int Candidate = 0; Candidate < vecTabStates.size(); ++Candidate)
        if (vecTabStates[Candidate]->Model == sender()) { Index = Candidate; break; }
    if (Index < 0 || Index >= vecTabStates.size()) return;
    const auto State = vecTabStates[Index]; const auto Model = static_cast<CFSModel*>(State->Model);
    auto Location = Model->WorkspaceLocation();
    if (Location.kind_ == hc::storage::LocatorKind::Plugin) Location.root_ = hc::workspace::PluginPath(Location).toStdString();
    if (State->workspace_.history_.isEmpty() || State->workspace_.history_.back() != Location) {
        State->workspace_.history_.append(Location);
        if (State->workspace_.history_.size() > 64) State->workspace_.history_.removeFirst();
    }
    const auto Current = CaptureWorkspace().tabs_[Index];
    State->workspace_.historyConnections_.insert(hc::workspace::LocationKey(Location), QJsonObject{{"provider", Current.provider_}, {"nativeRoot", Current.nativeRoot_}});
    QSet<QString> Retained;
    for (const auto& Item : State->workspace_.history_) Retained.insert(hc::workspace::LocationKey(Item));
    for (const auto& Key : State->workspace_.historyConnections_.keys())
        if (!Retained.contains(Key)) State->workspace_.historyConnections_.remove(Key);
    // Labels are lexical: no existence/volume probe on the UI thread.
    auto Label = QDir::fromNativeSeparators(CurrentPath).section('/', -1);
    ui.tabWidget->setTabText(Index, Label.isEmpty() ? CurrentPath : Label); ui.tabWidget->setTabToolTip(Index, CurrentPath);
    if (Index != ui.tabWidget->currentIndex()) return;
    if (history_) {
        QSignalBlocker Blocker(history_); history_->clear();
        for (const auto& Item : State->workspace_.history_) history_->addItem(QString::fromStdString(Item.root_));
        history_->setCurrentIndex(history_->count() - 1);
    }
    if (Model->GetRoot() && (Model->GetRoot()->GetCate() == nsHC::FS_CATE_LOCAL || Model->GetRoot()->IsRemote()))
        processVolumeStatusText(Model->GetRoot());
    processPanelStatusText(); updateWorkspaceStatus(); emit sig_NotifyCurrentDirectory(CurrentPath);
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


void CmpPanel::on_tabWidget_currentChanged(int Index)
{
    if (Index < 0 || Index >= vecTabStates.size()) return;
    emit sig_NotifyPanelActivated();
    const auto State = vecTabStates[Index];
    if (State->pending_) restoreTab(Index);
    for (int Candidate = 0; Candidate < vecTabStates.size(); ++Candidate)
        static_cast<CFSModel*>(vecTabStates[Candidate]->Model)->SetWatchEnabled(Candidate == Index);
    if (search_) { QSignalBlocker Blocker(search_); search_->setText(State->ProxyModel->Search()); }
    if (history_) {
        QSignalBlocker Blocker(history_); history_->clear();
        for (const auto& Location : State->workspace_.history_) history_->addItem(QString::fromStdString(Location.root_));
        history_->setCurrentIndex(history_->count() - 1);
    }
    updateWorkspaceStatus();
}



void CmpPanel::resizeEvent( QResizeEvent* event )
{
    QWidget::resizeEvent( event );
}

bool CmpPanel::eventFilter(QObject* Object, QEvent* Event)
{
    return QWidget::eventFilter(Object, Event);
}


void CmpPanel::initializeUIs()
{
    ui.verticalLayout->setContentsMargins(6, 4, 6, 4);
    ui.verticalLayout->setSpacing(6);
    ui.lblVolumeStatus->setMinimumWidth(0);
    ui.lblVolumeStatus->setMaximumWidth(QWIDGETSIZE_MAX);
    ui.lblVolumeStatus->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    ui.horizontalLayout->setStretch(1, 1);
    for (auto* Button : ui.frmHDR->findChildren<QPushButton*>())
        if (Button->text() == "Grid Style") Button->setText(tr("목록 모양"));
    ui.cbxVolume->setMinimumHeight(32); ui.cbxVolume->view()->setMinimumWidth(160);
    while (ui.tabWidget->count()) { auto* Widget = ui.tabWidget->widget(0); ui.tabWidget->removeTab(0); delete Widget; }
    auto* Controls = new QWidget(this); auto* Layout = new QHBoxLayout(Controls); Layout->setContentsMargins(0, 0, 0, 0);
    history_ = new QComboBox(Controls); history_->setObjectName("hc.workspaceHistory"); history_->setToolTip(tr("방문 기록"));
    history_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon); history_->setMinimumContentsLength(12);
    search_ = new QLineEdit(Controls); search_->setObjectName("hc.workspaceSearch"); search_->setClearButtonEnabled(true);
    search_->setPlaceholderText(tr("현재 폴더 검색 · name:문자 ext:확장자"));
    search_->setToolTip(tr("공백으로 구분한 조건을 모두 만족하는 항목을 표시합니다. 플러그인에서 조회한 정보도 필드명:값으로 검색할 수 있습니다."));
    Layout->addWidget(history_, 1); Layout->addWidget(search_, 2); ui.verticalLayout->insertWidget(1, Controls);
    auto* Notice = new QWidget(this); auto* NoticeLayout = new QHBoxLayout(Notice); NoticeLayout->setContentsMargins(0, 0, 0, 0);
    workspaceStatus_ = new QLabel(Notice); workspaceStatus_->setWordWrap(true); workspaceStatus_->setTextFormat(Qt::PlainText);
    retry_ = new QPushButton(tr("다시 시도"), Notice); retry_->setObjectName("hc.workspaceRetry");
    NoticeLayout->addWidget(workspaceStatus_, 1); NoticeLayout->addWidget(retry_); ui.verticalLayout->insertWidget(2, Notice);
    workspaceStatus_->hide(); retry_->hide(); Notice->hide();
    connect(search_, &QLineEdit::textChanged, this, [this](const QString& Text) {
        emit sig_NotifyPanelActivated();
        const auto Index = ui.tabWidget->currentIndex(); if (Index >= 0 && Index < vecTabStates.size()) vecTabStates[Index]->ProxyModel->SetSearch(Text);
    });
    connect(retry_, &QPushButton::clicked, this, [this] { RefreshSource(ui.tabWidget->currentIndex()); });
    connect(history_, &QComboBox::activated, this, [this](int Index) {
        emit sig_NotifyPanelActivated();
        const auto State = retrieveFocusState(); if (Index < 0 || Index >= State->workspace_.history_.size()) return;
        auto Saved = CaptureWorkspace().tabs_[ui.tabWidget->currentIndex()]; Saved.location_ = State->workspace_.history_[Index];
        const auto Connection = Saved.historyConnections_.value(hc::workspace::LocationKey(Saved.location_)).toObject();
        Saved.provider_ = Connection.value("provider").toString(); Saved.nativeRoot_ = Connection.value("nativeRoot").toString();
        Saved.selection_.clear(); Saved.focus_.clear(); State->workspace_ = Saved; restoreTab(ui.tabWidget->currentIndex());
    });
    AddTab(false);
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
    if( Current < 0 || Current >= vecTabStates.size() )
        return {};

    const auto& State = vecTabStates.value( Current );
    if( State->QuickView != nullptr )
        return State->QuickView;

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

    // Network capacity/labels require server I/O, including mapped X: drives.
    // Keep selection and startup responsive even when that server is offline.
    if (SpFileSystem->IsRemote())
    {
        ui.lblVolumeStatus->setText(tr("[%1] 네트워크 드라이브").arg(SpFileSystem->GetRoot()));
        return;
    }

    // NOTE: UNC(SMB) 의 경우에는 \\MyServer\\Share\\ 의 형태로 마지막에 \\ 로 끝나야 한다.

    if( SpFileSystem->GetCate() == nsHC::FS_CATE_LOCAL )
    {
        const auto Root = QString( "%1\\" ).arg( SpFileSystem->GetRoot() ).toStdWString();

        ULARGE_INTEGER Avail, TotalBytes, TotalFree;
        if( GetDiskFreeSpaceExW( Root.c_str(), &Avail, &TotalBytes, &TotalFree ) != FALSE )
        {
            const auto SizeStyle = static_cast< TyEnSizeStyle >( hc::settings::AppSettings().SizeStyle(true) );
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
    const auto SizeStyle = static_cast< TyEnSizeStyle >( hc::settings::AppSettings().SizeStyle(false) );

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
