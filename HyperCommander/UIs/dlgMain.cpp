#include "stdafx.h"
#include "dlgMain.hpp"

#include "colorSchemeMgr.hpp"
#include "UniqueLibs/commandUiBridge.hpp"
#include "UniqueLibs/fileJobUi.hpp"
#include "HyperCommanderUpdater/updateController.hpp"
#include <fmwidgets/WindowChrome.h>
#include <fmwidgets/CommandLine.h>
#include <fmstyle/ThemeManager.h>
#include <fmstyle/StylePaint.h>
#include <fmstyle/WatercolorChrome.h>
#include <QEvent>
#include <QProcess>
#include "Components/cmpView.hpp"
#include "UIs/dlgOpts.hpp"
#include "UIs/dlgFavoriteDir.hpp"

#include "UniqueLibs/commandMgr.hpp"
#include "UniqueLibs/cmnTypeDefs.hpp"
#include "UniqueLibs/externalEditorMgr.hpp"
#include "UniqueLibs/solTCPluginMgr.hpp"
#include "UniqueLibs/SHChangeNotify.hpp"
#include "UniqueLibs/builtInFsModel.hpp"
#include "UniqueLibs/builtInFsModel.hpp"
#include "UniqueLibs/solFavoriteDirMgr.hpp"

QMainUI::QMainUI( QWidget* parent, Qt::WindowFlags flags )
    : QMainWindow( parent, flags )
{
    ui.setupUi( this );
    setFont(fm::style::pixelFont(font(), 13));
    ui.verticalLayout->setContentsMargins(4, 4, 4, 0);
    ui.verticalLayout->setSpacing(2);
    ui.splitter->setChildrenCollapsible(false);
    auto* Header = new QWidget(this); auto* HeaderLayout = new QVBoxLayout(Header);
    HeaderLayout->setContentsMargins(0, 0, 0, 0); HeaderLayout->setSpacing(0);
    HeaderLayout->addWidget(new fm::ui::WindowChrome(this, Header));
    ui.menubar->setParent(Header); HeaderLayout->addWidget(ui.menubar);
    setMenuWidget(Header);
    ui.frame_2->hide();
    commandLine_ = new fm::ui::CommandLine(ui.centralwidget); commandLine_->setObjectName("hc.commandLine");
    ui.verticalLayout->addWidget(commandLine_);
    commandLine_->setVisible(hc::settings::AppSettings().Read(hc::settings::Section::General).toObject()["showCommandLine"].toBool(true));
    connect(commandLine_, &fm::ui::CommandLine::commandEntered, this, &QMainUI::executeCommandLine);
    updateWindowFrame();
    connect(&fm::style::ThemeManager::instance(), &fm::style::ThemeManager::changed, this, &QMainUI::updateWindowFrame);
    ui.verticalLayout_2->setContentsMargins(0, 0, 0, 0);
    ui.horizontalLayout->setContentsMargins(0, 4, 0, 0);
    ui.frame->setFrameShape(QFrame::NoFrame);
    ui.frame_2->setFrameShape(QFrame::NoFrame);

    setWindowTitle(QCoreApplication::applicationName());
    const auto Available = screen()->availableGeometry().size();
    resize(std::min(1360, Available.width() * 9 / 10), std::min(850, Available.height() * 9 / 10));
    ui.splitter->setSizes({1, 1});

    QMetaObject::invokeMethod( this, "initialize", Qt::QueuedConnection );
}

void QMainUI::updateWindowFrame()
{
    const auto& Colors = fm::style::themeColorsFor(this);
    const auto& Chrome = fm::style::watercolorChrome(Colors.variant());
    // The custom frame is inside the client area, so Windows cannot clip it on maximize.
    const int BorderWidth = isMaximized() ? 0 : 3;
    setStyleSheet(QString("QMainUI { border: %1px solid %2; }").arg(BorderWidth).arg(Chrome.frame.name()));
}

void QMainUI::changeEvent( QEvent* Event )
{
    QMainWindow::changeEvent(Event);
    if (Event->type() == QEvent::WindowStateChange)
        updateWindowFrame();
    if (Event->type() == QEvent::WindowActivate || Event->type() == QEvent::WindowUnblocked)
        requestInputFocusRestore();
}

void QMainUI::requestInputFocusRestore()
{
    if (inputFocusRestorePending_) return;
    inputFocusRestorePending_ = true;
    QTimer::singleShot(0, this, [this] { inputFocusRestorePending_ = false; restoreInputFocus(); });
}

void QMainUI::restoreInputFocus()
{
    if (!isVisible() || isMinimized() || !isEnabled() || QApplication::activeModalWidget() || QApplication::activePopupWidget()) return;
    const auto Root = reinterpret_cast<HWND>(winId());
    // Recover input only when Windows already has HC in front. Never activate
    // HC over another application or one of its still-open command windows.
    if (GetForegroundWindow() != Root) return;
    const auto Current = QApplication::focusWidget();
    const auto Native = GetFocus();
    const bool CurrentValid = Current && Current != this && Current->window() == this && Current->isVisible() && Current->isEnabled();
    if (Native && IsWindowVisible(Native) && (Native == Root || IsChild(Root, Native))) {
        DWORD Process = 0; GetWindowThreadProcessId(Native, &Process);
        if (CurrentValid || Process != GetCurrentProcessId()) return; // Preserve an embedded native viewer too.
    }
    QPointer<QWidget> Target = CurrentValid ? Current : lastInputFocus_.data();
    if (Target && Target->window() == this && Target->isVisible() && Target->isEnabled()) {
        // setFocus alone can be a no-op when Qt remembers the widget but the
        // native input queue has lost its HWND after a dialog/host disappears.
        SetFocus(reinterpret_cast<HWND>(Target->effectiveWinId()));
        if (Target) Target->setFocus(Qt::OtherFocusReason);
    } else if (auto* Panel = retrieveSrcPanel()) Panel->SetFocusView(Panel->CurrentTabIndex());
}

bool QMainUI::nativeEventFilter(const QByteArray& Type, void* Message, qintptr*)
{
    if (Type != "windows_generic_MSG") return false;
    const auto* Native = static_cast<MSG*>(Message);
    const auto Root = reinterpret_cast<HWND>(internalWinId());
    if (Root && (Native->hwnd == Root || IsChild(Root, Native->hwnd))
        && (Native->message == WM_SETFOCUS || Native->message == WM_KILLFOCUS)) requestInputFocusRestore();
    return false;
}

void QMainUI::OnDriveAdd( const QString& Root )
{
    ui.cmpLeftPanel->RefreshVolumeList(); ui.cmpRightPanel->RefreshVolumeList();
}

void QMainUI::OnDriveRemoved( const QString& Root )
{
    ui.cmpLeftPanel->NotifyDriveRemoved(Root); ui.cmpRightPanel->NotifyDriveRemoved(Root);
}

void QMainUI::OnMediaInserted( const QString& ItemIDDisplayName )
{
    qDebug() << __FUNCTION__ << " : " << ItemIDDisplayName;
}

void QMainUI::OnMediaRemoved( const QString& Root )
{
    OnDriveRemoved(Root);

}

void QMainUI::OnColorSchemeChanged( const TyColorScheme& ColorScheme )
{
    if( ColorScheme.Name.isEmpty() == true )
        return;

    QApplication::setFont( ColorScheme.Dialog_Font );
    setFont(ColorScheme.Dialog_Font);
    ui.menubar->setFont(ColorScheme.Menu_Font);

    const auto General = hc::settings::AppSettings().Read(hc::settings::Section::General).toObject();
    commandLine_->setVisible(General["showCommandLine"].toBool(true));
    if (auto* Bar = findChild<QWidget*>("hc.functionBar")) Bar->setVisible(General["showFunctionBar"].toBool(true));
    ui.cmpLeftPanel->OnColorSchemeChanged( ColorScheme );
    ui.cmpRightPanel->OnColorSchemeChanged( ColorScheme );
}

////////////////////////////////////////////////////////////////////////////
/// 명령 핸들러

DEFINE_HC_COMMAND( QMainUI, TabNew )
{
    // 탭을 추가하면서 Qt 가 자동적으로 엉뚱한 곳으로 포커스를 옮기는 행동이 일어날 때가 있다. 이에 대해 보정한다. 
    const auto Prev = currentPanelIndex;
    const auto Pane = retrieveSrcPanel();
    Q_ASSERT( Pane != nullptr );
    if( Pane == nullptr )
        return;

    Pane->AddTab();
    setActivePanel(Prev);
}

DEFINE_HC_COMMAND( QMainUI, TabPrevious )
{
    const auto Pane = retrieveSrcPanel();
    Q_ASSERT( Pane != nullptr );
    if( Pane == nullptr )
        return;

    Pane->PrevTab();
}

DEFINE_HC_COMMAND( QMainUI, TabNext )
{
    const auto Pane = retrieveSrcPanel();
    Q_ASSERT( Pane != nullptr );
    if( Pane == nullptr )
        return;

    Pane->NextTab();
}

DEFINE_HC_COMMAND( QMainUI, TabClose )
{
    const auto Prev = currentPanelIndex;
    const auto Pane = retrieveSrcPanel();
    Q_ASSERT( Pane != nullptr );
    if( Pane == nullptr )
        return;

    Pane->CloseTab();
    setActivePanel(Prev);
}

DEFINE_HC_COMMAND( QMainUI, ViewerQuick )
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

DEFINE_HC_COMMAND( QMainUI, FileCopy )
{
    const auto Src = retrieveSrcPanel();
    const auto Dst = retrieveDstPanel();
    Q_ASSERT( Src != nullptr );
    Q_ASSERT( Dst != nullptr );

    if( Src == nullptr || Dst == nullptr )
        return;

    // Src->FileCopyToOtherPanel( Dst );
}

DEFINE_HC_COMMAND( QMainUI, FileRename )
{
    const auto Pane = retrieveSrcPanel();
    Q_ASSERT( Pane != nullptr );
    if( Pane == nullptr )
        return;

    // Pane->RenameFileName( CursorIndex );
}

DEFINE_HC_COMMAND( QMainUI, ViewerOpen )
{
    const auto Pane = retrieveSrcPanel();
    Q_ASSERT( Pane != nullptr );
    if( Pane == nullptr )
        return;

    // Pane->ViewOnLister( CursorIndex );
}
DEFINE_HC_COMMAND( QMainUI, NavigateOpen )
{
    const auto Pane = retrieveSrcPanel();
    Q_ASSERT( Pane != nullptr );
    if( Pane == nullptr )
        return;

    Pane->ReturnOnCurrentTab( CursorIndex );
}

DEFINE_HC_COMMAND( QMainUI, FileMkdir )
{
    const auto Pane = retrieveSrcPanel();
    Q_ASSERT( Pane != nullptr );
    if( Pane == nullptr )
        return;

    Pane->NewFolderOnCurrentTab( CursorIndex );
}

DEFINE_HC_COMMAND( QMainUI, FileDelete )
{
    const auto Pane = retrieveSrcPanel();
    Q_ASSERT( Pane != nullptr );
    if( Pane == nullptr )
        return;

    Pane->FileDeleteOnCurrentTab( CursorIndex );
}

DEFINE_HC_COMMAND( QMainUI, FileNormalize )
{
    const auto Pane = retrieveSrcPanel();
    Q_ASSERT( Pane != nullptr );
    if( Pane == nullptr )
        return;

    // Pane->FileNormalization( CursorIndex );
}

DEFINE_HC_COMMAND( QMainUI, FileAttributes )
{
    const auto Pane = retrieveSrcPanel();
    Q_ASSERT( Pane != nullptr );
    if( Pane == nullptr )
        return;

    // Pane->FileSetAttrib( CursorIndex );
}

DEFINE_HC_COMMAND( QMainUI, SelectionInvert )
{
    const auto Pane = retrieveSrcPanel();
    Q_ASSERT( Pane != nullptr );
    if( Pane == nullptr )
        return;
    
    Pane->SelectRowOnCurrentTab( CursorIndex, true );
}

DEFINE_HC_COMMAND( QMainUI, FileBatchRename )
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

DEFINE_HC_COMMAND( QMainUI, ViewHidden )
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

DEFINE_HC_COMMAND( QMainUI, NavigateRefresh )
{
    const auto Pane = retrieveSrcPanel();
    Q_ASSERT( Pane != nullptr );
    if( Pane == nullptr )
        return;

    Pane->RefreshSource( Pane->CurrentTabIndex() );
}

DEFINE_HC_COMMAND( QMainUI, NavigateFavorites )
{
    const auto StFavoriteDirMgr = TyStFavoriteDirMgr::GetInstance();

    const auto Pane = retrieveSrcPanel();
    if (!Pane) return;
    QWidget* Anchor = Pane->GetFocusView(Pane->CurrentTabIndex());
    if (!Anchor) Anchor = Pane;

    QMenu Menu(this);
    Menu.setObjectName("hc.directoryHotList");
    StFavoriteDirMgr->ConstructDirMenu(&Menu);
    Menu.ensurePolished();
    const auto Size = Menu.sizeHint();
    const auto* Grid = qobject_cast<CGridView*>(Anchor);
    auto Position = Grid ? Grid->ListContentTopLeft() : Anchor->mapToGlobal(QPoint());
    const auto Available = Anchor->screen()->availableGeometry();
    Position.setX(std::clamp(Position.x(), Available.left(), std::max(Available.left(), Available.right() - Size.width() + 1)));
    Position.setY(std::clamp(Position.y(), Available.top(), std::max(Available.top(), Available.bottom() - Size.height() + 1)));
    QTimer::singleShot(0, &Menu, [&Menu] {
        Menu.setFocus();
        for (auto* Action : Menu.actions()) {
            if (Action->isVisible() && Action->isEnabled() && !Action->isSeparator()) {
                Menu.setActiveAction(Action);
                break;
            }
        }
    });
    Menu.exec(Position);
}

DEFINE_HC_COMMAND_EX( QMainUI, NavigateDrive )
{
    const auto Pane = retrieveSrcPanel();
    Q_ASSERT( Pane != nullptr );
    if( Pane == nullptr )
        return;

    Pane->ChangeVolume( Parameter.toString() );
}

DEFINE_HC_COMMAND_EX( QMainUI, NavigatePath )
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

DEFINE_HC_COMMAND( QMainUI, PanelSwitch )
{
    if( currentPanelIndex == 0 )
    {
        ui.cmpRightPanel->setFocus( Qt::MouseFocusReason );
        ui.cmpRightPanel->SetFocusView( ui.cmpRightPanel->CurrentTabIndex() );
        setActivePanel(1);
    }
    else if( currentPanelIndex == 1 )
    {
        ui.cmpLeftPanel->setFocus( Qt::MouseFocusReason );
        ui.cmpLeftPanel->SetFocusView( ui.cmpLeftPanel->CurrentTabIndex() );
        setActivePanel(0);
    }
}

DEFINE_HC_COMMAND( QMainUI, FileContext )
{
    const auto Pane = retrieveSrcPanel();
    Q_ASSERT( Pane != nullptr );
    if( Pane == nullptr )
        return;

    Pane->ContextMenuOnCurrentTab( CursorIndex );
}

DEFINE_HC_COMMAND( QMainUI, FileEdit )
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
    QMainOpts Opts(this);
    if (auto* Panel = retrieveSrcPanel())
        Opts.SetListView(qobject_cast<CGridView*>(Panel->GetFocusView(Panel->CurrentTabIndex())));
    Opts.exec();

}

void QMainUI::oo_notifyCurrentDirectory( const QString& CurrentPath )
{
    const auto* Panel = qobject_cast<CmpPanel*>(sender());
    if (Panel && Panel != retrieveSrcPanel()) return;
    commandLine_->setPrompt(CurrentPath);
    setWindowTitle(CurrentPath.isEmpty() ? QCoreApplication::applicationName() : CurrentPath + " — " + QCoreApplication::applicationName());
}

void QMainUI::setActivePanel(int Index)
{
    currentPanelIndex = Index == 1 ? 1 : 0;
    ui.cmpLeftPanel->setActivePanel(currentPanelIndex == 0);
    ui.cmpRightPanel->setActivePanel(currentPanelIndex == 1);
    auto* Panel = currentPanelIndex ? ui.cmpRightPanel : ui.cmpLeftPanel;
    commandLine_->setPrompt(Panel->CurrentPath());
    setWindowTitle(Panel->CurrentPath().isEmpty() ? QCoreApplication::applicationName() : Panel->CurrentPath() + " — " + QCoreApplication::applicationName());
    emit sigActivePanelChanged(Panel);
}

void QMainUI::oo_notifyPanelActivated()
{
    const auto Sender = qobject_cast< CmpPanel* >( sender() );

    if( Sender == ui.cmpLeftPanel )
    {
        qDebug() << __FUNCTION__ << " : " << "왼쪽 패널 활성화";
        setActivePanel(0);
    }
    else if( Sender == ui.cmpRightPanel )
    {
        qDebug() << __FUNCTION__ << " : " << "오른쪽 패널 활성화";
        setActivePanel(1);
    }
}

bool QMainUI::eventFilter( QObject* Object, QEvent* Event )
{
    const auto Widget = qobject_cast<QWidget*>(Object);
    if (Event->type() == QEvent::FocusIn) {
        if (Widget && Widget != this && Widget->window() == this) lastInputFocus_ = Widget;
        if (Widget && (Widget == ui.cmpLeftPanel || ui.cmpLeftPanel->isAncestorOf(Widget))) setActivePanel(0);
        else if (Widget && (Widget == ui.cmpRightPanel || ui.cmpRightPanel->isAncestorOf(Widget))) setActivePanel(1);
    }
    if ((Event->type() == QEvent::FocusOut && Widget && Widget->window() == this)
        || (Event->type() == QEvent::Hide && Widget && Widget->isWindow() && Widget != this && isAncestorOf(Widget)))
        requestInputFocusRestore();
    return QMainWindow::eventFilter(Object, Event);
}

void QMainUI::closeEvent( QCloseEvent* Event )
{
    const bool Updating = update_ && update_->RestartRequested();
    if (Updating && !canRestartForUpdate()) { Event->ignore(); return; }
    if (workspaceLoaded_ && workspace_.writable_ && !SaveWorkspace())
    {
        QMessageBox::warning(this, tr("작업공간 저장"), tr("작업공간을 저장하지 못했습니다. 기존 설정은 유지됩니다.\n%1").arg(workspace_.error_));
        if (Updating) { Event->ignore(); return; }
    }

    if (commandProcess_ && commandProcess_->state() != QProcess::NotRunning) {
        if (QMessageBox::question(this, tr("명령 실행 중"), tr("실행 중인 명령을 종료하고 창을 닫으시겠습니까?")) != QMessageBox::Yes) { Event->ignore(); return; }
        commandProcess_->kill(); commandProcess_->waitForFinished(1000);
    }
    if (Updating && !update_->LaunchPending()) { Event->ignore(); return; }
    QMainWindow::closeEvent( Event );
}

bool QMainUI::canRestartForUpdate() const
{
    if (commandProcess_ && commandProcess_->state() != QProcess::NotRunning) return false;
    if (const auto Bridge = TyStCommandMgr::GetInstance()->Bridge(); Bridge && Bridge->FileJobs())
        for (const auto& Job : Bridge->FileJobs()->Scheduler().Snapshots()) {
            using hc::operations::JobState;
            if (Job.state_ != JobState::Completed && Job.state_ != JobState::Failed && Job.state_ != JobState::Cancelled) return false;
        }
    return !hc::settings::AppSettings().Editing();
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
    update_ = new hc::update::UpdateController(this, HelpBar_Root, [this] { return canRestartForUpdate(); });

    shlChangeNotify = new QSHChangeNotify(this);
    shlChangeNotify->hide();
    connect(shlChangeNotify, &QSHChangeNotify::NetworkChanged, this, [this] {
        for (auto* Model : findChildren<CFSModel*>()) Model->NotifyNetworkChange();
    });
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
    for (auto* Panel : {ui.cmpLeftPanel, ui.cmpRightPanel})
        connect(Panel, &CmpPanel::sigViewSettingsChanged, this, [this] {
            if (!SaveWorkspace())
                QMessageBox::warning(this, tr("목록 모양 저장 실패"), tr("현재 화면에는 적용했지만 작업공간 파일을 저장하지 못했습니다.\n%1").arg(hc::settings::WorkspaceSettings().Error()));
        });

    // Track input in all panel descendants, including the path/search controls
    // and viewers. Focus outside both panels keeps the current command source.
    qApp->installEventFilter(this);
    qApp->installNativeEventFilter(this);
    setActivePanel(currentPanelIndex);

    workspace_ = hc::workspace::Load(hc::settings::WorkspaceSettings()); workspaceLoaded_ = true;
    if (workspace_.exists_ && workspace_.writable_ && hc::settings::AppSettings().Read(hc::settings::Section::General).toObject()["restoreWorkspace"].toBool(true)) RestoreWorkspace(workspace_.state_);
    else if (!workspace_.writable_) statusBar()->showMessage(tr("저장된 작업공간을 읽을 수 없어 원본을 보존합니다: %1").arg(workspace_.error_));
}

hc::workspace::State QMainUI::CaptureWorkspace() const
{
    auto State = workspace_.state_;
    State.left_ = ui.cmpLeftPanel->CaptureWorkspace(); State.right_ = ui.cmpRightPanel->CaptureWorkspace();
    State.commandHistory_ = commandLine_->history();
    State.activePane_ = currentPanelIndex; State.geometry_ = saveGeometry(); State.layout_ = ui.splitter->saveState();
    return State;
}

void QMainUI::RestoreWorkspace(const hc::workspace::State& State)
{
    commandLine_->SetHistory(State.commandHistory_);
    ui.cmpLeftPanel->RestoreWorkspace(State.left_, State.defaults_); ui.cmpRightPanel->RestoreWorkspace(State.right_, State.defaults_);
    if (!State.geometry_.isEmpty() && !restoreGeometry(State.geometry_)) statusBar()->showMessage(tr("저장된 창 위치가 올바르지 않아 기본 위치를 사용합니다."));
    if (!State.layout_.isEmpty() && !ui.splitter->restoreState(State.layout_)) statusBar()->showMessage(tr("저장된 패널 배치가 올바르지 않아 기본 배치를 사용합니다."));
    setActivePanel(State.activePane_);
    const auto Panel = currentPanelIndex ? ui.cmpRightPanel : ui.cmpLeftPanel;
    Panel->SetFocusView(Panel->CurrentTabIndex()); emit Panel->sig_NotifyPanelActivated();
}

bool QMainUI::SaveWorkspace()
{
    return workspaceLoaded_ && hc::workspace::Save(hc::settings::WorkspaceSettings(), workspace_, CaptureWorkspace());
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

void QMainUI::executeCommandLine(const QString& Command)
{
    auto* Panel = retrieveSrcPanel(); if (!Panel) return;
    if (Command.startsWith("hc.") || Command.startsWith("cm_")) {
        if (!TyStCommandMgr::GetInstance()->Bridge()->ExecuteCommand(Command))
            statusBar()->showMessage(tr("등록되지 않은 명령: %1").arg(Command));
        return;
    }
    const auto State = Panel->CaptureWorkspace();
    if (State.tabs_.isEmpty()) return;
    const auto Location = State.tabs_[State.active_].location_;
    if (Command.compare("cd", Qt::CaseInsensitive) == 0) { statusBar()->showMessage(Panel->CurrentPath()); return; }
    if (Command.startsWith("cd ", Qt::CaseInsensitive)) {
        auto Path = Command.mid(3).trimmed();
        if (Path.startsWith('"') && Path.endsWith('"')) Path = Path.mid(1, Path.size() - 2);
        if (Path.isEmpty()) { statusBar()->showMessage(tr("이동할 경로를 입력하십시오.")); return; }
        if (Location.kind_ == hc::storage::LocatorKind::Local && QDir::isRelativePath(Path)) Path = QDir(Panel->CurrentPath()).absoluteFilePath(Path);
        Panel->ChangeCurrentPath(Path, {}); return;
    }
    if (Location.kind_ != hc::storage::LocatorKind::Local) {
        statusBar()->showMessage(tr("외부 명령은 로컬 폴더에서 실행할 수 있습니다. 현재 위치: %1").arg(Panel->CurrentPath())); return;
    }
    if (commandProcess_ && commandProcess_->state() != QProcess::NotRunning) {
        statusBar()->showMessage(tr("앞서 실행한 명령이 진행 중입니다.")); return;
    }
    auto* Process = new QProcess(this); commandProcess_ = Process;
    Process->setWorkingDirectory(Panel->CurrentPath());
    Process->setProgram(qEnvironmentVariable("COMSPEC", "cmd.exe"));
    // cmd owns quoting, pipes and redirection. QProcess's argument quoting would
    // escape those a second time, so pass the entered command as a native tail.
    Process->setNativeArguments("/D /S /C " + Command);
    Process->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* Arguments) { Arguments->flags |= CREATE_NO_WINDOW; });
    Process->setProcessChannelMode(QProcess::MergedChannels);
    commandLine_->SetRunning(true);
    statusBar()->showMessage(tr("실행 중 · %1 · %2").arg(Panel->CurrentPath(), Command));
    connect(Process, &QProcess::readyReadStandardOutput, Process, [Process] {
        auto Output = Process->property("hc.output").toByteArray(); Output += Process->readAllStandardOutput();
        Process->setProperty("hc.output", Output.right(65536));
    });
    const auto Complete = [this, Process](const QString& Message) {
        commandLine_->SetRunning(false); statusBar()->showMessage(Message);
        commandLine_->setToolTip(Message + "\n" + QString::fromLocal8Bit(Process->property("hc.output").toByteArray()));
        if (commandProcess_ == Process) commandProcess_ = nullptr;
        Process->deleteLater();
    };
    connect(Process, &QProcess::errorOccurred, this, [Process, Complete](QProcess::ProcessError Error) {
        if (Error == QProcess::FailedToStart) Complete(QObject::tr("명령 시작 실패: %1").arg(Process->errorString()));
    });
    connect(Process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this, [Command, Complete](int Code, QProcess::ExitStatus Status) {
        Complete(Status == QProcess::NormalExit && Code == 0 ? QObject::tr("명령 완료 · %1").arg(Command) : QObject::tr("명령 실패 (종료 코드 %1) · %2").arg(Code).arg(Command));
    });
    Process->start();
}

// Launching a second process opens folders; it never executes arbitrary commands.
bool QMainUI::OpenStartupPaths(const QJsonArray& Paths)
{
    if (QApplication::activeModalWidget()) return false;
    auto* Left = findChild<CmpPanel*>("cmpLeftPanel"); auto* Right = findChild<CmpPanel*>("cmpRightPanel");
    if (!Left || !Right) return false;
    auto* Active = retrieveSrcPanel();
    int LeftCount = Left->CaptureWorkspace().tabs_.size(), RightCount = Right->CaptureWorkspace().tabs_.size();
    for (const auto& Value : Paths) {
        const auto Side = Value.toObject()["side"].toString();
        const bool IsLeft = Side == "left" || (Side == "active" && currentPanelIndex == 0);
        if (++(IsLeft ? LeftCount : RightCount) > 64) return false;
    }
    for (const auto& Value : Paths) {
        const auto Record = Value.toObject(); const auto Side = Record["side"].toString();
        auto* Panel = Side == "left" ? Left : Side == "right" ? Right : Active;
        Panel->AddTab(); Panel->ChangeCurrentPath(Record["path"].toString(), {});
    }
    if (isMinimized()) showNormal(); else show();
    raise(); activateWindow();
    return true;
}
