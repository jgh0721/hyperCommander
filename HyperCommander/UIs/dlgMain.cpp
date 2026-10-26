#include "stdafx.h"
#include "dlgMain.hpp"
#include "startupTrace.hpp"
#include "UniqueLibs/qtMessageDialog.hpp"

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
#include <QFocusEvent>
#include <QLineEdit>
#include <QLabel>
#include "UniqueLibs/commandLineExecution.hpp"
#include "UniqueLibs/commandUiConfig.hpp"
#include <QToolButton>
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
    // Caption clicks must keep the input widget, just like native title buttons.
    // Retain keyboard access to the custom caption controls through Tab.
    for (const auto* Name : {"fm.window.minimize", "fm.window.maximize", "fm.window.close"})
        if (auto* Button = Header->findChild<QToolButton*>(Name)) Button->setFocusPolicy(Qt::TabFocus);
    auto* MenuRow = new QWidget(Header);
    auto* MenuLayout = new QHBoxLayout(MenuRow);
    MenuLayout->setContentsMargins(0, 0, 0, 0); MenuLayout->setSpacing(4);
    ui.menubar->setParent(MenuRow); MenuLayout->addWidget(ui.menubar, 1);
    HeaderLayout->addWidget(MenuRow);
    setMenuWidget(Header);
    ui.frame_2->hide();
    commandLine_ = new fm::ui::CommandLine(ui.centralwidget); commandLine_->setObjectName("hc.commandLine");
    ui.verticalLayout->addWidget(commandLine_);
    commandLine_->setVisible(hc::settings::AppSettings().Read(hc::settings::Section::General).toObject()["showCommandLine"].toBool(true));
    connect(commandLine_, &fm::ui::CommandLine::commandEntered, this, [this](const QString& Command) { executeCommandLine(Command, false); });
    const auto RefreshCommandHelp = [this] {
        const auto Keys = TyStCommandMgr::GetInstance()->Retrieve();
        const auto Overrides = hc::settings::AppSettings().Read(hc::settings::Section::Shortcuts).toObject();
        auto Candidates = Keys.keys();
        Candidates.append(QKeyCombination(Qt::Key_Return));
        Candidates.append(QKeyCombination(Qt::ShiftModifier, Qt::Key_Return));
        QStringList Run, Keep;
        for (const auto Key : Candidates) {
            const QKeyEvent Event(QEvent::KeyPress, Key.key(), Key.keyboardModifiers());
            const auto Id = hc::ui::CommandLineShortcut(Event, Keys, Overrides);
            const auto Label = QKeySequence(Key).toString(QKeySequence::NativeText);
            if (Id == "hc.commandLine.execute") Run.append(Label);
            if (Id == "hc.commandLine.keepOpen") Keep.append(Label);
        }
        Run.removeDuplicates(); Keep.removeDuplicates(); Run.sort(); Keep.sort();
        const auto None = tr("단축키 없음");
        commandLine_->lineEdit()->setPlaceholderText(tr("명령 입력 — %1 실행 · %2 창 유지 · Ctrl+↓ 기록")
            .arg(Run.isEmpty() ? None : Run.join(", "), Keep.isEmpty() ? None : Keep.join(", ")));
        commandLine_->setToolTip(tr("외부 명령은 별도 창에서 독립 실행합니다. cmd는 셸 하나를 열고 명시한 /C·/K 옵션을 따릅니다. Ctrl+C는 입력 텍스트를 복사합니다."));
    };
    connect(TyStCommandMgr::GetInstance(), &CCommandMgr::ShortcutsChanged, this, RefreshCommandHelp);
    RefreshCommandHelp();
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
    // Only Watercolor draws the thick blue window frame. Paper Fluent keeps the
    // Windows 11 border, which ThemeManager tints with the title wash.
    if (!Colors.isWatercolor()) { setStyleSheet({}); return; }
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
    const auto Previous = lastInputFocus_.data();
    const bool RestorePrevious = activationFocusRestorePending_ && Previous && Previous->window() == this
        && Previous->isVisible() && Previous->isEnabled();
    activationFocusRestorePending_ = false;
    if (Native && IsWindowVisible(Native) && (Native == Root || IsChild(Root, Native))) {
        DWORD Process = 0; GetWindowThreadProcessId(Native, &Process);
        if ((CurrentValid && !RestorePrevious) || Process != GetCurrentProcessId()) return; // Preserve an embedded native viewer too.
    }
    // Activation may have selected a valid fallback (the command line) while
    // restoring a native panel child. Prefer the user's last input widget.
    QPointer<QWidget> Target = RestorePrevious ? Previous : CurrentValid ? Current : Previous;
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

    hc::ui::InputDialog Ui;
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
    updateCommandLinePrompt(CurrentPath);
    setWindowTitle(CurrentPath.isEmpty() ? QCoreApplication::applicationName() : CurrentPath + " — " + QCoreApplication::applicationName());
}

void QMainUI::setActivePanel(int Index)
{
    currentPanelIndex = Index == 1 ? 1 : 0;
    ui.cmpLeftPanel->setActivePanel(currentPanelIndex == 0);
    ui.cmpRightPanel->setActivePanel(currentPanelIndex == 1);
    auto* Panel = currentPanelIndex ? ui.cmpRightPanel : ui.cmpLeftPanel;
    updateCommandLinePrompt(Panel->CurrentPath());
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
    if (commandLine_ && Object == commandLine_->lineEdit()
        && (Event->type() == QEvent::ShortcutOverride || Event->type() == QEvent::KeyPress)) {
        const auto* Key = static_cast<QKeyEvent*>(Event);
        const auto Overrides = hc::settings::AppSettings().Read(hc::settings::Section::Shortcuts).toObject();
        const auto Id = hc::ui::CommandLineShortcut(*Key, TyStCommandMgr::GetInstance()->Retrieve(), Overrides);
        const bool Execute = Id == "hc.commandLine.execute" || Id == "hc.commandLine.keepOpen";
        if (Event->type() == QEvent::ShortcutOverride) {
            // Keep editing shortcuts (especially Ctrl+C) inside the line edit.
            Event->accept(); return true;
        }
        if (Execute) { SubmitCommandLine(Id == "hc.commandLine.keepOpen"); return true; }
        if (Key->key() == Qt::Key_Return || Key->key() == Qt::Key_Enter) return true;
    }
    // QWidget dispatches activation/unblocking through event(), not changeEvent().
    if (Object == this && (Event->type() == QEvent::WindowActivate || Event->type() == QEvent::WindowUnblocked)) {
        if (Event->type() == QEvent::WindowActivate) activationFocusRestorePending_ = true;
        requestInputFocusRestore();
    }
    const auto Widget = qobject_cast<QWidget*>(Object);
    if (Event->type() == QEvent::FocusIn) {
        if (Widget && Widget != this && Widget->window() == this) {
            // Qt's activation fallback is not a user request to switch input.
            const auto Reason = static_cast<QFocusEvent*>(Event)->reason();
            if (Reason != Qt::ActiveWindowFocusReason || !lastInputFocus_) {
                lastInputFocus_ = Widget;
                activationFocusRestorePending_ = false;
            }
        }
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
        hc::ui::MessageDialog::warning(this, tr("작업공간 저장"), tr("작업공간을 저장하지 못했습니다. 기존 설정은 유지됩니다.\n%1").arg(workspace_.error_));
        if (Updating) { Event->ignore(); return; }
    }

    if (Updating && !update_->LaunchPending()) { Event->ignore(); return; }
    QMainWindow::closeEvent( Event );
}

bool QMainUI::canRestartForUpdate() const
{
    if (const auto Bridge = TyStCommandMgr::GetInstance()->Bridge(); Bridge && Bridge->FileJobs())
        for (const auto& Job : Bridge->FileJobs()->Scheduler().Snapshots()) {
            using hc::operations::JobState;
            if (Job.state_ != JobState::Completed && Job.state_ != JobState::Failed && Job.state_ != JobState::Cancelled) return false;
        }
    return !hc::settings::AppSettings().Editing();
}

void QMainUI::initialize()
{
    hc::startup::Span Trace("workspace.initialize");
    const auto StCommandMgr = TyStCommandMgr::GetInstance();
    const auto StColorSchemeMgr = TyStColorSchemeMgr::GetInstance();
    const auto StFavoriteDirMgr = TyStFavoriteDirMgr::GetInstance();

    connect( StFavoriteDirMgr, &CFavoriteDirMgr::NotifyAppendCurrentPath, this, &QMainUI::OnAddCurrentDirToFavorite );

    ui.menubar->setNativeMenuBar( true );

    StCommandMgr->SetMainUI( this );

    // Use the row layout for the status/settings/update controls. QMenuBar's
    // corner geometry can lag behind their changing size hints.
    auto* HeaderActions = ui.menubar->cornerWidget(Qt::TopRightCorner);
    ui.menubar->setCornerWidget(nullptr, Qt::TopRightCorner);
    HeaderActions->setParent(ui.menubar->parentWidget());
    ui.menubar->parentWidget()->layout()->addWidget(HeaderActions);
    HeaderActions->show();
    auto* UpdateButton = new QToolButton(HeaderActions);
    UpdateButton->setObjectName("hc.update.menuButton"); UpdateButton->setText(tr("업데이트"));
    UpdateButton->setPopupMode(QToolButton::InstantPopup);
    UpdateButton->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Preferred);
    auto* UpdateMenu = new QMenu(UpdateButton); UpdateButton->setMenu(UpdateMenu);
    HeaderActions->layout()->addWidget(UpdateButton);
    update_ = new hc::update::UpdateController(this, UpdateMenu, [this] { return canRestartForUpdate(); });

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
                hc::ui::MessageDialog::warning(this, tr("목록 모양 저장 실패"), tr("현재 화면에는 적용했지만 작업공간 파일을 저장하지 못했습니다.\n%1").arg(hc::settings::WorkspaceSettings().Error()));
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
    if (!State.geometry_.isEmpty() && !restoreGeometry(State.geometry_)) statusBar()->showMessage(tr("저장된 창 위치가 올바르지 않아 기본 위치를 사용합니다."));
    if (!State.layout_.isEmpty() && !ui.splitter->restoreState(State.layout_)) statusBar()->showMessage(tr("저장된 패널 배치가 올바르지 않아 기본 배치를 사용합니다."));
    setActivePanel(State.activePane_);
    // Dispatch the command source's listing first, with its final pane size.
    if (State.activePane_ == 1) {
        ui.cmpRightPanel->RestoreWorkspace(State.right_, State.defaults_);
        ui.cmpLeftPanel->RestoreWorkspace(State.left_, State.defaults_);
    } else {
        ui.cmpLeftPanel->RestoreWorkspace(State.left_, State.defaults_);
        ui.cmpRightPanel->RestoreWorkspace(State.right_, State.defaults_);
    }
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

void QMainUI::updateCommandLinePrompt(const QString& Path)
{
    commandLine_->setPrompt(Path);
    auto* Prompt = commandLine_->findChild<QLabel*>();
    if (!Prompt) return;
    constexpr int MaximumWidth = 240;
    auto Text = hc::ui::CommandLinePrompt(Path);
    for (int Limit = 35; Limit >= 8 && Prompt->fontMetrics().horizontalAdvance(Text) > MaximumWidth; --Limit)
        Text = hc::ui::CommandLinePrompt(Path, Limit);
    Prompt->setMaximumWidth(MaximumWidth);
    Prompt->setText(Text);
    Prompt->setAccessibleName(Path);
    // setPrompt() keeps the original full path in the label tooltip.
}

void QMainUI::SubmitCommandLine(bool KeepOpen)
{
    const auto Command = commandLine_->lineEdit()->text().trimmed();
    if (Command.isEmpty()) return;
    auto History = commandLine_->history(); History.removeAll(Command); History.prepend(Command);
    commandLine_->SetHistory(History);
    commandLine_->lineEdit()->clear();
    executeCommandLine(Command, KeepOpen);
}

void QMainUI::executeCommandLine(const QString& Command, bool KeepOpen)
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
    if (Command.startsWith("cd ", Qt::CaseInsensitive) && !hc::ui::commandLine::HasShellSyntax(Command)) {
        auto Path = Command.mid(3).trimmed();
        if (Path.startsWith("/d ", Qt::CaseInsensitive)) Path = Path.mid(3).trimmed();
        if (Path.startsWith('"') && Path.endsWith('"')) Path = Path.mid(1, Path.size() - 2);
        if (Path.isEmpty()) { statusBar()->showMessage(tr("이동할 경로를 입력하십시오.")); return; }
        if ((Location.kind_ == hc::storage::LocatorKind::Local || Location.kind_ == hc::storage::LocatorKind::Smb) && QDir::isRelativePath(Path)) Path = QDir(Panel->CurrentPath()).absoluteFilePath(Path);
        Panel->ChangeCurrentPath(Path, {}); return;
    }
    if (Location.kind_ != hc::storage::LocatorKind::Local && Location.kind_ != hc::storage::LocatorKind::Smb) {
        statusBar()->showMessage(tr("외부 명령은 로컬 폴더에서 실행할 수 있습니다. 현재 위치: %1").arg(Panel->CurrentPath())); return;
    }
    const auto Directory = Panel->CurrentPath();
    const auto Error = hc::ui::commandLine::Launch(hc::ui::commandLine::Plan(Command, Directory, KeepOpen), Directory);
    statusBar()->showMessage(Error.isEmpty() ? tr("외부 실행 · %1 · %2").arg(Directory, Command) : Error, 10000);
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
