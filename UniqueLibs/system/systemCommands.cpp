#include "stdafx.h"
#include "systemCommands.hpp"

/// UIs
#include "HyperCommander/UIs/Components/cmpSystemView.hpp"
#include "HyperCommander/UIs/systemDialogs.hpp"
#include "UIs/cmpPanel.hpp"

/// 내부 라이브러리
#include "UniqueLibs/commandUiBridge.hpp"
#include "UniqueLibs/permissionService.hpp"
#include "UniqueLibs/qtMessageDialog.hpp"
#include "UniqueLibs/systemSessionService.hpp"
#include "systemSupport.hpp"

/// 공통 라이브러리
#include "commonLibs/system/processActions.hpp"
#include "commonLibs/system/serviceControl.hpp"

/// 프레임워크
#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QJsonArray>
#include <QMainWindow>
#include <QMenu>
#include <QStatusBar>
#include <QUrl>

namespace hc::sys {
namespace {

QString T( const char* Text ) { return QObject::tr( Text ); }

hc::system::ProcessTarget TargetOf( const ProcessRef& Ref )
{
    return { Ref.key.pid, Ref.key.createTime, Ref.image.toStdWString() };
}

QJsonObject TargetJson( const ProcessRef& Ref )
{
    return { { "pid", qint64( Ref.key.pid ) }, { "created", QString::number( Ref.key.createTime ) }, { "image", Ref.image } };
}

struct Attempt
{
    QString name;
    quint32 pid = 0;
    QString outcome, reason;
    quint32 code = 0;
    bool    ok() const { return outcome == "Completed" || outcome == "AlreadyGone"; }
};

Attempt FromResult( const ProcessRef& Ref, const hc::system::ActionResult& Result )
{
    return { Ref.name, Ref.key.pid, QString::fromLatin1( hc::system::OutcomeName( Result.outcome ) ), QString::fromStdString( Result.reason ), Result.nativeCode };
}

Attempt FromReply( const QString& Name, quint32 Pid, const QJsonObject& Reply )
{
    return { Name, Pid, Reply["outcome"].toString(), Reply["reason"].toString(), Reply["nativeCode"].toString().toUInt() };
}

// 표준 권한에서 거부되었으면 관리자 도우미로 보낼 수 있다.
bool NeedsHelper( const hc::system::ActionResult& Result )
{
    return Result.outcome == hc::system::Outcome::AccessDenied && !hc::permissions::Elevated();
}

QStringList Failures( const QVector<Attempt>& Attempts )
{
    QStringList Result;
    for( const auto& Item : Attempts )
        if( !Item.ok() )
            Result.append( T( "%1 (%2): %3" ).arg( Item.name ).arg( Item.pid ).arg( OutcomeText( Item.outcome, Item.reason, Item.code ) ) );
    return Result;
}

QString ServiceError( quint32 Code )
{
    return Code == 0 ? QString{} : Win32ErrorText( Code );
}

// 로컬로 시도하고, 접근 거부면 관리자 도우미에 같은 동작을 요청한다. 반환은 Win32 오류 코드.
quint32 ServiceRequest( const QString& Name, const QString& Control )
{
    using hc::system::ServiceControl;
    const auto Wide = Name.toStdWString();
    const quint32 Code = Control == "restart"  ? hc::system::RestartServiceByName( Wide )
                         : Control == "start"  ? hc::system::ControlServiceByName( Wide, ServiceControl::Start )
                         : Control == "stop"   ? hc::system::ControlServiceByName( Wide, ServiceControl::Stop )
                         : Control == "pause"  ? hc::system::ControlServiceByName( Wide, ServiceControl::Pause )
                                               : hc::system::ControlServiceByName( Wide, ServiceControl::Continue );
    if( Code != ERROR_ACCESS_DENIED || hc::permissions::Elevated() )
        return Code;
    const auto Reply = AdminSession().Request( "service.control", { { "name", Name }, { "control", Control } } );
    return Reply["outcome"].toString() == "Completed" ? 0u : Reply["nativeCode"].toString().toUInt();
}

} // namespace

SystemCommands::SystemCommands( QMainWindow* Main, CommandUiBridge* Bridge, PanelAccessor Active )
    : QObject( Main ), main_( Main ), bridge_( Bridge ), active_( std::move( Active ) )
{
    followTimer_.setSingleShot( true );
    connect( &followTimer_, &QTimer::timeout, this, &SystemCommands::followNow );
    AdminSession().SetIdleTimeout( Settings::Load().helperIdleMinutes * 60000 );
}

bool SystemCommands::Handles( const QString& Id )
{
    return Id.startsWith( "hc.system." ) || Id.startsWith( "hc.process." ) || Id.startsWith( "hc.service." ) || Id.startsWith( "hc.panel.locationMenu" );
}

CmpPanel* SystemCommands::panelOf( const QObject* Object ) const
{
    for( auto* Item = Object; Item; Item = Item->parent() )
        if( auto* Panel = qobject_cast<CmpPanel*>( const_cast<QObject*>( Item ) ) )
            return Panel;
    return nullptr;
}

CmpPanel* SystemCommands::namedPanel( bool Left ) const
{
    return main_ ? main_->findChild<CmpPanel*>( Left ? "cmpLeftPanel" : "cmpRightPanel" ) : nullptr;
}

CmpPanel* SystemCommands::opposite( CmpPanel* Panel ) const
{
    auto* Left = namedPanel( true );
    auto* Right = namedPanel( false );
    return Panel == Left ? Right : Panel == Right ? Left : nullptr;
}

CSystemView* SystemCommands::target( QObject* Source ) const
{
    if( auto* View = qobject_cast<CSystemView*>( Source ) )
        return View;
    if( auto* Detail = qobject_cast<CSystemDetailView*>( Source ) )
        return Detail->Source();
    auto* Panel = active_ ? active_() : nullptr;
    // 상세 보기 패널에서 명령을 부르면 그 상세 보기의 원본을 대상으로 한다.
    if( Panel && detail_ && detailPanel_ == Panel && Panel->GetFocusView( Panel->CurrentTabIndex() ) == detail_ )
        return detail_->Source();
    return Panel ? Panel->SystemView() : nullptr;
}

int SystemCommands::ActiveKind() const
{
    auto* Panel = active_ ? active_() : nullptr;
    if( !Panel )
        return -1;
    // 상세 보기가 덮은 패널은 시스템 보기가 아니다. 원본 보기 쪽에서 명령을 쓴다.
    auto* View = qobject_cast<CSystemView*>( Panel->GetFocusView( Panel->CurrentTabIndex() ) );
    return View ? int( View->ViewKind() ) : -1;
}

void SystemCommands::status( const QString& Text ) const
{
    if( main_ ) main_->statusBar()->showMessage( Text, 8000 );
}

void SystemCommands::report( const QString& Title, const QStringList& Problems, const QString& Success ) const
{
    if( Problems.isEmpty() )
    {
        status( Success );
        return;
    }
    hc::ui::MessageDialog::warning( main_, Title, Problems.join( '\n' ) );
}

bool SystemCommands::Execute( const QString& Id, const QVariant& Argument, QObject* Source )
{
    if( Id == "hc.system.processes.open" ) { openView( 0 ); return true; }
    if( Id == "hc.system.services.open" ) { openView( 1 ); return true; }
    if( Id == "hc.system.startAdminHelper" ) { startHelper(); return true; }
    if( Id == "hc.panel.locationMenuLeft" || Id == "hc.panel.locationMenuRight" )
    {
        if( auto* Panel = namedPanel( Id.endsWith( "Left" ) ) ) Panel->ShowLocationMenu();
        return true;
    }
    if( Id == "hc.process.run" ) { run( target( Source ) ); return true; }
    if( Id == "hc.system.followLocation" ) { setFollow( !follow_ ); return true; }
    auto* View = target( Source );
    if( !View )
    {
        status( T( "프로세스·서비스 보기에서 사용할 수 있는 명령입니다." ) );
        return false;
    }
    const bool Processes = View->ViewKind() == CSystemView::Kind::Processes;
    if( Id == "hc.system.refreshPause" ) { View->TogglePause(); RefreshState(); return true; }
    if( Id.startsWith( "hc.process." ) && !Processes ) return false;
    if( Id.startsWith( "hc.service." ) && Processes ) return false;

    if( Id == "hc.process.properties" ) processProperties( View );
    else if( Id == "hc.process.openLocation" ) openLocation( View, false );
    else if( Id == "hc.process.openLocationNewTab" ) openLocation( View, true );
    else if( Id == "hc.process.switchToWindow" )
    {
        if( const auto Current = View->CurrentProcess(); Current && !hc::system::SwitchToProcessWindow( Current->key.pid ) )
            status( T( "%1에는 전환할 창이 없습니다." ).arg( Current->name ) );
    }
    else if( Id == "hc.process.suspendToggle" ) suspend( View );
    else if( Id == "hc.process.priority" ) priority( View, Argument );
    else if( Id == "hc.process.efficiencyMode" ) efficiency( View );
    else if( Id == "hc.process.terminate" ) terminate( View, false, false );
    else if( Id == "hc.process.terminateTree" ) terminate( View, true, false );
    else if( Id == "hc.process.restart" ) terminate( View, false, true );
    else if( Id == "hc.process.copy" ) copyProcess( View, Argument );
    else if( Id == "hc.process.searchOnline" )
    {
        if( const auto Current = View->CurrentProcess() )
            QDesktopServices::openUrl( QUrl( "https://www.bing.com/search?q=" + QString::fromLatin1( QUrl::toPercentEncoding( Current->name ) ) ) );
    }
    else if( Id == "hc.process.contextMenu" || Id == "hc.service.contextMenu" ) View->ShowContextMenu();
    else if( Id == "hc.process.viewMode" ) { View->SetFlat( !View->Flat() ); RefreshState(); }
    else if( Id == "hc.process.showServices" )
    {
        const auto Current = View->CurrentProcess();
        auto* Other = opposite( panelOf( View ) );
        if( !Current || !Other ) return true;
        if( View->Processes()->Services( Current->key.pid ).isEmpty() )
        {
            status( T( "%1은(는) 서비스를 호스팅하지 않습니다." ).arg( Current->name ) );
            return true;
        }
        const quint32 Pid = Current->key.pid;
        if( auto* Services = Other->ShowSystemView( 1 ) )
            selectLater( Services, [Pid]( CSystemView* Target ) {
                if( !Target->Services() || Target->Services()->rowCount() == 0 ) return false;
                Target->SelectServiceByPid( Pid );
                return true;
            } );
    }
    else if( Id == "hc.service.properties" ) serviceProperties( View );
    else if( Id == "hc.service.openLocation" ) openLocation( View, false );
    else if( Id == "hc.service.openLocationNewTab" ) openLocation( View, true );
    else if( Id == "hc.service.start" ) serviceControl( View, "start" );
    else if( Id == "hc.service.stop" ) serviceControl( View, "stop" );
    else if( Id == "hc.service.restart" ) serviceControl( View, "restart" );
    else if( Id == "hc.service.pauseToggle" ) serviceControl( View, "pauseToggle" );
    else if( Id == "hc.service.startType" ) serviceStartType( View, Argument );
    else if( Id == "hc.service.copy" ) copyService( View, Argument );
    else if( Id == "hc.service.toggleDrivers" ) { View->SetIncludeDrivers( !View->IncludeDrivers() ); RefreshState(); }
    else if( Id == "hc.service.showProcess" )
    {
        const auto* Row = View->ServiceRow( View->CurrentService() );
        auto* Other = opposite( panelOf( View ) );
        if( !Row || !Other ) return true;
        if( !Row->status.pid )
        {
            status( T( "%1 서비스는 실행 중이 아닙니다." ).arg( View->CurrentService() ) );
            return true;
        }
        const quint32 Pid = Row->status.pid;
        if( auto* Processes = Other->ShowSystemView( 0 ) )
            selectLater( Processes, [Pid]( CSystemView* Target ) {
                if( !Target->Processes() || !Target->Processes()->HasPid( Pid ) ) return false;
                Target->SelectProcess( Pid );
                return true;
            } );
    }
    else
        return false;
    return true;
}

void SystemCommands::RefreshState()
{
    if( !bridge_ )
        return;
    const int Kind = ActiveKind();
    // 상세 보기에서 부른 명령도 원본 보기에 적용된다.
    const bool DetailFocused = [&] {
        auto* Panel = active_ ? active_() : nullptr;
        return Panel && detail_ && Panel->GetFocusView( Panel->CurrentTabIndex() ) == detail_ && detail_->Source();
    }();
    const int Effective = Kind >= 0 ? Kind : DetailFocused ? int( detail_->Source()->ViewKind() ) : -1;
    const auto Set = [this]( const char* Id, bool Enabled ) { if( auto* Action = bridge_->Action( Id ) ) Action->setEnabled( Enabled ); };
    const auto Check = [this]( const char* Id, bool Checked ) {
        if( auto* Action = bridge_->Action( Id ) )
        {
            Action->setCheckable( true );
            if( Action->isChecked() != Checked ) Action->setChecked( Checked );
        }
    };
    for( const auto* Id : { "hc.process.properties", "hc.process.openLocation", "hc.process.openLocationNewTab", "hc.process.switchToWindow",
                            "hc.process.suspendToggle", "hc.process.priority", "hc.process.efficiencyMode", "hc.process.terminate",
                            "hc.process.terminateTree", "hc.process.restart", "hc.process.copy", "hc.process.searchOnline", "hc.process.contextMenu",
                            "hc.process.viewMode", "hc.process.showServices" } )
        Set( Id, Effective == 0 );
    for( const auto* Id : { "hc.service.properties", "hc.service.openLocation", "hc.service.openLocationNewTab", "hc.service.start", "hc.service.stop",
                            "hc.service.restart", "hc.service.pauseToggle", "hc.service.startType", "hc.service.showProcess", "hc.service.toggleDrivers",
                            "hc.service.copy", "hc.service.contextMenu" } )
        Set( Id, Effective == 1 );
    Set( "hc.system.refreshPause", Effective >= 0 );
    Set( "hc.system.followLocation", Effective >= 0 || follow_ );
    Set( "hc.system.startAdminHelper", !hc::permissions::Elevated() );
    auto* View = target( nullptr );
    Check( "hc.system.followLocation", follow_ );
    Check( "hc.system.refreshPause", View && View->Paused() );
    Check( "hc.process.viewMode", View && View->Flat() );
    Check( "hc.service.toggleDrivers", View && View->IncludeDrivers() );
    // 서비스 단추는 현재 행의 상태를 따른다.
    if( View && Effective == 1 )
        if( const auto* Row = View->ServiceRow( View->CurrentService() ) )
        {
            using hc::system::ServiceState;
            const auto State = Row->status.state;
            Set( "hc.service.start", State == ServiceState::Stopped );
            Set( "hc.service.stop", ( State == ServiceState::Running || State == ServiceState::Paused ) && ( Row->status.controlsAccepted & SERVICE_ACCEPT_STOP ) );
            Set( "hc.service.restart", State == ServiceState::Running && ( Row->status.controlsAccepted & SERVICE_ACCEPT_STOP ) );
            Set( "hc.service.pauseToggle", State == ServiceState::Paused || ( State == ServiceState::Running && ( Row->status.controlsAccepted & SERVICE_ACCEPT_PAUSE_CONTINUE ) ) );
            Set( "hc.service.showProcess", Row->status.pid != 0 );
        }
    // 따라가기 원본이 바뀌면 새 원본을 따른다.
    if( follow_ && View && View != followSource_ )
        setFollow( true, false );
}

//////////////////////////////////////////////////////////////////////////////
/// 보기 열기·위치

void SystemCommands::openView( int Kind )
{
    auto* Panel = active_ ? active_() : nullptr;
    if( !Panel )
        return;
    // 이미 같은 보기를 가진 탭이 있으면 그 탭으로 간다.
    if( const int Tab = Panel->FindSystemTab( Kind ); Tab >= 0 && Tab != Panel->CurrentTabIndex() )
    {
        if( auto* Tabs = Panel->findChild<QTabWidget*>( "tabWidget" ) ) Tabs->setCurrentIndex( Tab );
        emit Panel->sig_NotifyPanelActivated();
        Panel->SetFocusView( Tab );
        return;
    }
    Panel->ShowSystemView( Kind );
}

void SystemCommands::revealIn( CmpPanel* Panel, const QString& Path, bool NewTab, bool Focus )
{
    if( !Panel || Path.isEmpty() )
        return;
    if( NewTab ) Panel->AddTab();
    Panel->RevealFile( Path );
    if( Focus )
    {
        emit Panel->sig_NotifyPanelActivated();
        Panel->SetFocusView( Panel->CurrentTabIndex() );
    }
}

void SystemCommands::openLocation( CSystemView* View, bool NewTab )
{
    QString Path;
    if( View->ViewKind() == CSystemView::Kind::Processes )
    {
        const auto Current = View->CurrentProcess();
        if( !Current ) return;
        Path = Current->image;
        if( Path.isEmpty() )
        {
            hc::system::ProcessDetails Details;
            if( hc::system::QueryProcessDetails( Current->key.pid, Current->key.createTime, Details ) )
                Path = QString::fromStdWString( Details.imagePath );
        }
        if( Path.isEmpty() )
        {
            status( T( "%1의 실행 파일 경로를 읽을 수 없습니다. 관리자 권한이 필요할 수 있습니다." ).arg( Current->name ) );
            return;
        }
    }
    else
    {
        const auto Name = View->CurrentService();
        if( Name.isEmpty() ) return;
        hc::system::ServiceConfig Config;
        if( hc::system::QueryServiceConfiguration( Name.toStdWString(), Config ) )
            // svchost에 들어가는 서비스는 실제 코드가 있는 ServiceDll 위치를 연다.
            Path = QString::fromStdWString( Config.serviceDll.empty() ? Config.resolvedImage : Config.serviceDll );
        if( Path.isEmpty() )
        {
            status( T( "%1 서비스의 실행 파일을 확인할 수 없습니다." ).arg( Name ) );
            return;
        }
    }
    if( !QFileInfo::exists( Path ) )
    {
        status( T( "파일이 없습니다: %1" ).arg( QDir::toNativeSeparators( Path ) ) );
        return;
    }
    revealIn( opposite( panelOf( View ) ), Path, NewTab, true );
}

void SystemCommands::selectLater( CSystemView* View, std::function<bool( CSystemView* )> Select, int Tries )
{
    const QPointer<CSystemView> Guard( View );
    auto Attempt = std::make_shared<std::function<void( int )>>();
    *Attempt = [this, Guard, Select, Attempt]( int Left ) {
        if( !Guard ) return;
        if( Select( Guard ) || Left <= 0 ) return;
        QTimer::singleShot( 100, this, [Attempt, Left] { ( *Attempt )( Left - 1 ); } );
    };
    ( *Attempt )( Tries );
}

void SystemCommands::setFollow( bool Follow, bool Announce )
{
    follow_ = Follow;
    disconnect( followConnection_ );
    followSource_ = Follow ? target( nullptr ) : nullptr;
    followShown_.clear();
    if( followSource_ )
    {
        followConnection_ = connect( followSource_, &CSystemView::sigCursorChanged, this, [this] {
            followTimer_.start( std::max( 50, Settings::Load().followDebounceMs ) );
        } );
        followTimer_.start( 0 );
    }
    if( !Announce )
        return;
    if( Follow && !followSource_ )
        status( T( "따라가기는 프로세스·서비스 보기에서 켭니다." ) );
    else
        status( Follow ? T( "커서를 옮기면 반대 패널이 실행 파일 위치를 따라갑니다." ) : T( "위치 따라가기를 껐습니다." ) );
}

void SystemCommands::followNow()
{
    if( !follow_ || !followSource_ )
        return;
    QString Path;
    if( const auto Current = followSource_->CurrentProcess() )
        Path = Current->image;
    else if( const auto* Row = followSource_->ServiceRow( followSource_->CurrentService() ) )
        Path = Row->image;
    if( Path.isEmpty() || Path == followShown_ || !QFileInfo::exists( Path ) )
        return;
    followShown_ = Path;
    // 따라가기는 포커스를 옮기지 않는다. 반대 패널이 시스템 보기면 걷지 않고 건너뛴다.
    auto* Other = opposite( panelOf( followSource_ ) );
    if( !Other || Other->SystemView() || Other->GetFocusView( Other->CurrentTabIndex() ) == detail_ )
        return;
    Other->RevealFile( Path );
}

void SystemCommands::ToggleDetail()
{
    if( CloseDetail() )
        return;
    auto* View = target( nullptr );
    auto* Other = View ? opposite( panelOf( View ) ) : nullptr;
    if( !View || !Other )
        return;
    Other->OpenDetailView( Other->CurrentTabIndex(), View );
    detail_ = qobject_cast<CSystemDetailView*>( Other->GetFocusView( Other->CurrentTabIndex() ) );
    detailPanel_ = Other;
    View->EnsureKeyboardFocusOnView();
}

bool SystemCommands::CloseDetail()
{
    if( !detail_ )
        return false;
    auto* Detail = detail_.data();
    if( detailPanel_ )
        if( auto* Tabs = detailPanel_->findChild<QTabWidget*>( "tabWidget" ) )
            for( int Index = 0; Index < Tabs->count(); ++Index )
                if( detailPanel_->GetFocusView( Index ) == Detail )
                {
                    detailPanel_->CloseQuickView( Index );
                    break;
                }
    detail_.clear();
    detailPanel_.clear();
    return true;
}

//////////////////////////////////////////////////////////////////////////////
/// 프로세스 동작

void SystemCommands::processProperties( CSystemView* View )
{
    const auto Current = View->CurrentProcess();
    if( !Current )
        return;
    hc::ui::ShowProcessProperties( main_, *Current, View->Processes()->Services( Current->key.pid ) );
}

void SystemCommands::terminate( CSystemView* View, bool Tree, bool Restart )
{
    auto Targets = Tree ? ( View->CurrentProcess() ? View->TreeOf( View->CurrentProcess()->key ) : QVector<ProcessRef>{} ) : View->SelectedProcesses();
    if( Restart && !Targets.isEmpty() ) Targets = { Targets.front() };
    if( Targets.isEmpty() )
        return;
    const auto Config = Settings::Load();
    hc::ui::TerminateChoice Choice;
    // 한 개를 끝낼 때의 확인은 설정으로 끌 수 있다. 여러 개·트리·다시 시작은 항상 묻는다.
    if( Targets.size() == 1 && !Tree && !Restart && !Config.confirmSingleTerminate
        && !IsProductImage( Targets.front().image ) && !Targets.front().critical )
    {
        Choice.accepted = true;
        Choice.graceful = Config.gracefulClose;
    }
    else
        Choice = hc::ui::ConfirmTerminate( main_, Targets, Tree, Restart );
    if( !Choice.accepted )
        return;
    const int GracefulMs = Choice.graceful ? Config.gracefulWaitSec * 1000 : 0;
    const auto Limits = ProcessLimits( Choice.allowWarned );
    const QPointer<CSystemView> Guard( View );
    RunAsync( this,
              [Targets, GracefulMs, Limits, AllowWarned = Choice.allowWarned] {
                  QVector<Attempt> Attempts;
                  QVector<ProcessRef> Denied;
                  for( const auto& Ref : Targets )
                  {
                      const auto Target = TargetOf( Ref );
                      if( GracefulMs > 0 && hc::system::HasTopLevelWindows( Target.pid )
                          && hc::system::RequestCloseWindows( Target ).outcome == hc::system::Outcome::Completed && hc::system::WaitForExit( Target, GracefulMs ) )
                      {
                          Attempts.append( { Ref.name, Ref.key.pid, "Completed", {}, 0 } );
                          continue;
                      }
                      const auto Result = hc::system::TerminateProcessTarget( Target, Limits );
                      if( NeedsHelper( Result ) ) Denied.append( Ref );
                      else Attempts.append( FromResult( Ref, Result ) );
                  }
                  if( !Denied.isEmpty() )
                  {
                      QJsonArray List;
                      for( const auto& Ref : Denied ) List.append( TargetJson( Ref ) );
                      const auto Reply = AdminSession().Request( "process.terminate", { { "targets", List }, { "gracefulMs", 0 }, { "allowWarned", AllowWarned } } );
                      const auto Results = Reply["results"].toArray();
                      for( const auto& Ref : Denied )
                      {
                          QJsonObject Found{ { "outcome", Reply["outcome"].toString() == "Completed" ? QStringLiteral( "Unknown" ) : Reply["outcome"].toString() },
                                             { "nativeCode", Reply["nativeCode"] } };
                          for( const auto& Item : Results )
                              if( quint32( Item.toObject()["pid"].toInteger() ) == Ref.key.pid ) Found = Item.toObject();
                          Attempts.append( FromReply( Ref.name, Ref.key.pid, Found ) );
                      }
                  }
                  return Attempts;
              },
              [this, Guard, Targets, Restart]( const QVector<Attempt>& Attempts ) {
                  const auto Problems = Failures( Attempts );
                  if( Guard ) Guard->Refresh();
                  if( !Restart || !Problems.isEmpty() )
                  {
                      report( Restart ? T( "프로세스 다시 시작" ) : T( "프로세스 끝내기" ), Problems, T( "프로세스 %1개를 끝냈습니다." ).arg( Attempts.size() ) );
                      return;
                  }
                  // 같은 명령줄로 다시 실행한다. 작업 폴더는 실행 파일 폴더를 쓴다.
                  const auto& Ref = Targets.front();
                  hc::system::RunRequest Request;
                  Request.command = ( Ref.commandLine.isEmpty() ? '"' + QDir::toNativeSeparators( Ref.image ) + '"' : Ref.commandLine ).toStdWString();
                  Request.directory = QDir::toNativeSeparators( QFileInfo( Ref.image ).absolutePath() ).toStdWString();
                  Request.priorityClass = Ref.priorityClass == NORMAL_PRIORITY_CLASS ? 0 : Ref.priorityClass;
                  const auto Started = hc::system::RunProcess( Request );
                  if( Started.started ) status( T( "%1을(를) 다시 시작했습니다(PID %2)." ).arg( Ref.name ).arg( Started.pid ) );
                  else report( T( "프로세스 다시 시작" ), { T( "%1을(를) 다시 실행하지 못했습니다: %2" ).arg( Ref.name, Win32ErrorText( Started.nativeCode ) ) }, {} );
              } );
}

void SystemCommands::suspend( CSystemView* View )
{
    const auto Targets = View->SelectedProcesses();
    if( Targets.isEmpty() )
        return;
    // 첫 대상이 일시 중단 상태면 모두 재개하고, 아니면 모두 일시 중단한다.
    const bool Resume = Targets.front().suspended;
    const auto Limits = ProcessLimits( false );
    const QPointer<CSystemView> Guard( View );
    RunAsync( this,
              [Targets, Resume, Limits] {
                  QVector<Attempt> Attempts;
                  for( const auto& Ref : Targets )
                  {
                      const auto Result = hc::system::SuspendProcessTarget( TargetOf( Ref ), !Resume, Limits );
                      if( NeedsHelper( Result ) )
                          Attempts.append( FromReply( Ref.name, Ref.key.pid, AdminSession().Request( "process.suspend", { { "target", TargetJson( Ref ) }, { "resume", Resume } } ) ) );
                      else
                          Attempts.append( FromResult( Ref, Result ) );
                  }
                  return Attempts;
              },
              [this, Guard, Resume]( const QVector<Attempt>& Attempts ) {
                  if( Guard ) Guard->Refresh();
                  report( Resume ? T( "프로세스 재개" ) : T( "프로세스 일시 중단" ), Failures( Attempts ),
                          Resume ? T( "프로세스 %1개를 재개했습니다." ).arg( Attempts.size() ) : T( "프로세스 %1개를 일시 중단했습니다." ).arg( Attempts.size() ) );
              } );
}

void SystemCommands::priority( CSystemView* View, const QVariant& Argument )
{
    const auto Targets = View->SelectedProcesses();
    if( Targets.isEmpty() )
        return;
    quint32 Class = Argument.toUInt();
    if( !Class )
    {
        QMenu Menu( View );
        Menu.setObjectName( "hc.process.priorityMenu" );
        for( const auto Value : { REALTIME_PRIORITY_CLASS, HIGH_PRIORITY_CLASS, ABOVE_NORMAL_PRIORITY_CLASS, NORMAL_PRIORITY_CLASS, BELOW_NORMAL_PRIORITY_CLASS, IDLE_PRIORITY_CLASS } )
        {
            auto* Item = Menu.addAction( PriorityText( Value ) );
            Item->setData( quint32( Value ) );
            Item->setCheckable( true );
            Item->setChecked( Targets.front().priorityClass == Value );
        }
        auto* Chosen = Menu.exec( View->CursorPoint() );
        if( !Chosen ) return;
        Class = Chosen->data().toUInt();
    }
    if( Class == REALTIME_PRIORITY_CLASS
        && hc::ui::MessageDialog::warning( main_, T( "우선순위 변경" ), T( "실시간 우선순위는 입력 장치와 시스템 응답을 막을 수 있습니다. 계속하시겠습니까?" ),
                                           QMessageBox::Yes | QMessageBox::No, QMessageBox::No ) != QMessageBox::Yes )
        return;
    const auto Limits = ProcessLimits( false );
    const QPointer<CSystemView> Guard( View );
    RunAsync( this,
              [Targets, Class, Limits] {
                  QVector<Attempt> Attempts;
                  for( const auto& Ref : Targets )
                  {
                      const auto Result = hc::system::SetPriorityTarget( TargetOf( Ref ), Class, Limits );
                      if( NeedsHelper( Result ) )
                          Attempts.append( FromReply( Ref.name, Ref.key.pid, AdminSession().Request( "process.priority", { { "target", TargetJson( Ref ) }, { "priorityClass", qint64( Class ) } } ) ) );
                      else
                          Attempts.append( FromResult( Ref, Result ) );
                  }
                  return Attempts;
              },
              [this, Guard, Class]( const QVector<Attempt>& Attempts ) {
                  if( Guard ) Guard->Refresh();
                  report( T( "우선순위 변경" ), Failures( Attempts ), T( "우선순위를 %1(으)로 바꿨습니다." ).arg( PriorityText( Class ) ) );
              } );
}

void SystemCommands::efficiency( CSystemView* View )
{
    const auto Targets = View->SelectedProcesses();
    if( Targets.isEmpty() )
        return;
    const bool Enable = Targets.front().priorityClass != IDLE_PRIORITY_CLASS;
    const auto Limits = ProcessLimits( false );
    const QPointer<CSystemView> Guard( View );
    RunAsync( this,
              [Targets, Enable, Limits] {
                  QVector<Attempt> Attempts;
                  for( const auto& Ref : Targets )
                  {
                      const auto Result = hc::system::SetEfficiencyTarget( TargetOf( Ref ), Enable, Limits );
                      if( NeedsHelper( Result ) )
                          Attempts.append( FromReply( Ref.name, Ref.key.pid, AdminSession().Request( "process.efficiency", { { "target", TargetJson( Ref ) }, { "enable", Enable } } ) ) );
                      else
                          Attempts.append( FromResult( Ref, Result ) );
                  }
                  return Attempts;
              },
              [this, Guard, Enable]( const QVector<Attempt>& Attempts ) {
                  if( Guard ) Guard->Refresh();
                  report( T( "효율 모드" ), Failures( Attempts ), Enable ? T( "효율 모드를 켰습니다." ) : T( "효율 모드를 껐습니다." ) );
              } );
}

void SystemCommands::run( CSystemView* View )
{
    auto* Panel = active_ ? active_() : nullptr;
    const auto Directory = Panel && !Panel->SystemView() ? Panel->CurrentPath() : QDir::homePath();
    hc::system::RunRequest Request;
    if( !hc::ui::PromptRun( main_, Request, Directory ) )
        return;
    const QPointer<CSystemView> Guard( View );
    RunAsync( this, [Request] { return hc::system::RunProcess( Request ); },
              [this, Guard, Command = QString::fromStdWString( Request.command )]( const hc::system::RunResult& Result ) {
                  if( !Result.started )
                  {
                      if( Result.nativeCode != ERROR_CANCELLED )
                          report( T( "새 작업 실행" ), { T( "%1\n%2" ).arg( Command, Win32ErrorText( Result.nativeCode ) ) }, {} );
                      return;
                  }
                  status( Result.pid ? T( "실행했습니다(PID %1)." ).arg( Result.pid ) : T( "실행했습니다." ) );
                  if( Guard && Result.pid && Guard->ViewKind() == CSystemView::Kind::Processes )
                  {
                      const quint32 Pid = Result.pid;
                      Guard->Refresh();
                      selectLater( Guard, [Pid]( CSystemView* Target ) {
                          if( !Target->Processes()->HasPid( Pid ) ) return false;
                          Target->SelectProcess( Pid );
                          return true;
                      }, 30 );
                  }
              } );
}

void SystemCommands::copyProcess( CSystemView* View, const QVariant& Argument )
{
    auto Field = Argument.toString();
    if( Field.isEmpty() )
    {
        QMenu Menu( View );
        for( const auto& [Key, Label] : { std::pair{ "name", T( "이름" ) }, std::pair{ "pid", T( "PID" ) }, std::pair{ "commandLine", T( "명령줄" ) },
                                          std::pair{ "image", T( "이미지 경로" ) }, std::pair{ "row", T( "행 전체" ) } } )
            Menu.addAction( Label )->setData( QString( Key ) );
        auto* Chosen = Menu.exec( View->CursorPoint() );
        if( !Chosen ) return;
        Field = Chosen->data().toString();
    }
    QStringList Lines;
    for( const auto& Ref : View->SelectedProcesses() )
    {
        if( Field == "name" ) Lines << Ref.name;
        else if( Field == "pid" ) Lines << QString::number( Ref.key.pid );
        else if( Field == "commandLine" ) Lines << Ref.commandLine;
        else if( Field == "image" ) Lines << QDir::toNativeSeparators( Ref.image );
        else Lines << QStringList{ Ref.name, QString::number( Ref.key.pid ), QString::number( Ref.parentPid ), QDir::toNativeSeparators( Ref.image ), Ref.commandLine }.join( '\t' );
    }
    QApplication::clipboard()->setText( Lines.join( "\r\n" ) );
    status( T( "%1줄을 복사했습니다." ).arg( Lines.size() ) );
}

//////////////////////////////////////////////////////////////////////////////
/// 서비스 동작

void SystemCommands::serviceControl( CSystemView* View, const QString& Requested )
{
    const auto Names = View->SelectedServices();
    if( Names.isEmpty() )
        return;
    QString Control = Requested;
    if( Control == "pauseToggle" )
    {
        const auto* Row = View->ServiceRow( Names.front() );
        Control = Row && Row->status.state == hc::system::ServiceState::Paused ? "continue" : "pause";
    }
    // 실행 중인 종속 서비스가 있으면 함께 중지됨을 알리고 확인을 받는다.
    QStringList Dependents;
    if( Control == "stop" || Control == "restart" )
    {
        for( const auto& Name : Names )
        {
            std::vector<hc::system::ServiceStatusInfo> List;
            hc::system::QueryDependents( Name.toStdWString(), List );
            for( const auto& Item : List )
                if( Item.state != hc::system::ServiceState::Stopped ) Dependents << QString::fromStdWString( Item.name );
        }
        Dependents.removeDuplicates();
        QString Text = Names.size() == 1 ? T( "%1 서비스를 %2하시겠습니까?" ).arg( Names.front(), Control == "stop" ? T( "중지" ) : T( "다시 시작" ) )
                                         : T( "서비스 %1개를 %2하시겠습니까?" ).arg( Names.size() ).arg( Control == "stop" ? T( "중지" ) : T( "다시 시작" ) );
        if( !Dependents.isEmpty() ) Text += T( "\n\n다음 종속 서비스도 중지됩니다:\n%1" ).arg( Dependents.join( '\n' ) );
        if( hc::ui::MessageDialog::warning( main_, T( "서비스 제어" ), Text, QMessageBox::Yes | QMessageBox::No, QMessageBox::No ) != QMessageBox::Yes )
            return;
    }
    status( T( "서비스 %1 요청을 보냈습니다…" ).arg( Control ) );
    const QPointer<CSystemView> Guard( View );
    RunAsync( this,
              [Names, Control, Dependents] {
                  QStringList Problems;
                  // 종속 서비스를 먼저 중지한다(EnumDependentServices 순서).
                  if( Control == "stop" || Control == "restart" )
                      for( const auto& Name : Dependents )
                          if( const auto Code = ServiceRequest( Name, "stop" ); Code && Code != ERROR_SERVICE_NOT_ACTIVE )
                              Problems << QObject::tr( "%1: %2" ).arg( Name, ServiceError( Code ) );
                  for( const auto& Name : Names )
                      if( const auto Code = ServiceRequest( Name, Control ); Code && Code != ERROR_SERVICE_ALREADY_RUNNING && Code != ERROR_SERVICE_NOT_ACTIVE )
                          Problems << QObject::tr( "%1: %2" ).arg( Name, ServiceError( Code ) );
                  return Problems;
              },
              [this, Guard, Names]( const QStringList& Problems ) {
                  if( Guard ) Guard->Refresh();
                  report( T( "서비스 제어" ), Problems, T( "서비스 %1개를 처리했습니다." ).arg( Names.size() ) );
              } );
}

void SystemCommands::serviceStartType( CSystemView* View, const QVariant& Argument )
{
    const auto Names = View->SelectedServices();
    if( Names.isEmpty() )
        return;
    auto Type = Argument.toString();
    if( Type.isEmpty() )
    {
        QMenu Menu( View );
        Menu.setObjectName( "hc.service.startTypeMenu" );
        for( const auto& [Key, Label] : { std::pair{ "auto", T( "자동" ) }, std::pair{ "autoDelayed", T( "자동(지연된 시작)" ) }, std::pair{ "manual", T( "수동" ) },
                                          std::pair{ "disabled", T( "사용 안 함" ) } } )
            Menu.addAction( Label )->setData( QString( Key ) );
        auto* Chosen = Menu.exec( View->CursorPoint() );
        if( !Chosen ) return;
        Type = Chosen->data().toString();
    }
    using hc::system::StartType;
    const auto Start = Type == "auto" ? StartType::Auto : Type == "autoDelayed" ? StartType::AutoDelayed : Type == "manual" ? StartType::Manual : StartType::Disabled;
    const QPointer<CSystemView> Guard( View );
    RunAsync( this,
              [Names, Start, Type] {
                  QStringList Problems;
                  for( const auto& Name : Names )
                  {
                      hc::system::ServiceConfigChange Change;
                      Change.start = Start;
                      quint32 Code = hc::system::ApplyServiceConfig( Name.toStdWString(), Change );
                      if( Code == ERROR_ACCESS_DENIED && !hc::permissions::Elevated() )
                      {
                          const auto Reply = AdminSession().Request( "service.config", { { "name", Name }, { "changes", QJsonObject{ { "startType", Type } } } } );
                          Code = Reply["outcome"].toString() == "Completed" ? 0u : Reply["nativeCode"].toString().toUInt();
                      }
                      if( Code ) Problems << QObject::tr( "%1: %2" ).arg( Name, ServiceError( Code ) );
                  }
                  return Problems;
              },
              [this, Guard, Names]( const QStringList& Problems ) {
                  if( Guard )
                  {
                      Guard->Refresh();
                  }
                  report( T( "시작 유형 변경" ), Problems, T( "서비스 %1개의 시작 유형을 바꿨습니다." ).arg( Names.size() ) );
              } );
}

void SystemCommands::serviceProperties( CSystemView* View )
{
    const auto Name = View->CurrentService();
    if( Name.isEmpty() )
        return;
    const QPointer<CSystemView> Guard( View );
    hc::ui::ShowServiceProperties(
        main_, Name,
        [Name]( const hc::system::ServiceConfigChange& Change, const QJsonObject& Json ) -> QString {
            // 설정 변경은 짧으므로 대화 상자 안에서 바로 처리한다(도우미 UAC 확인 포함).
            quint32 Code = hc::system::ApplyServiceConfig( Name.toStdWString(), Change );
            if( Code == ERROR_ACCESS_DENIED && !hc::permissions::Elevated() )
            {
                QApplication::setOverrideCursor( Qt::WaitCursor );
                const auto Reply = AdminSession().Request( "service.config", { { "name", Name }, { "changes", Json } } );
                QApplication::restoreOverrideCursor();
                Code = Reply["outcome"].toString() == "Completed" ? 0u : Reply["nativeCode"].toString().toUInt();
            }
            return Code ? QObject::tr( "설정을 바꾸지 못했습니다: %1" ).arg( ServiceError( Code ) ) : QString{};
        },
        [this, Guard]( const QString& Control ) {
            if( Guard ) serviceControl( Guard, Control );
        } );
    if( Guard ) Guard->Refresh();
}

void SystemCommands::copyService( CSystemView* View, const QVariant& Argument )
{
    auto Field = Argument.toString();
    if( Field.isEmpty() )
    {
        QMenu Menu( View );
        for( const auto& [Key, Label] : { std::pair{ "name", T( "서비스 이름" ) }, std::pair{ "displayName", T( "표시 이름" ) }, std::pair{ "image", T( "실행 파일" ) },
                                          std::pair{ "row", T( "행 전체" ) } } )
            Menu.addAction( Label )->setData( QString( Key ) );
        auto* Chosen = Menu.exec( View->CursorPoint() );
        if( !Chosen ) return;
        Field = Chosen->data().toString();
    }
    QStringList Lines;
    for( const auto& Name : View->SelectedServices() )
    {
        const auto* Row = View->ServiceRow( Name );
        if( !Row ) continue;
        const auto Display = QString::fromStdWString( Row->status.displayName );
        if( Field == "name" ) Lines << Name;
        else if( Field == "displayName" ) Lines << Display;
        else if( Field == "image" ) Lines << QDir::toNativeSeparators( Row->image );
        else Lines << QStringList{ Name, Display, ServiceStateText( Row->status.state ), Row->configLoaded ? StartTypeText( Row->start, Row->trigger ) : QString{},
                                   Row->status.pid ? QString::number( Row->status.pid ) : QString{}, QDir::toNativeSeparators( Row->image ) }.join( '\t' );
    }
    QApplication::clipboard()->setText( Lines.join( "\r\n" ) );
    status( T( "%1줄을 복사했습니다." ).arg( Lines.size() ) );
}

void SystemCommands::startHelper()
{
    if( hc::permissions::Elevated() )
    {
        status( T( "HyperCommander가 이미 관리자 권한으로 실행 중입니다." ) );
        return;
    }
    if( AdminSessionConnected() )
    {
        status( T( "관리자 도우미가 이미 연결되어 있습니다." ) );
        return;
    }
    status( T( "관리자 도우미를 시작합니다…" ) );
    RunAsync( this, [] { return AdminSession().Request( "ping", {}, 120000 ); },
              [this]( const QJsonObject& Reply ) {
                  if( Reply["outcome"].toString() == "Completed" )
                  {
                      status( T( "관리자 도우미가 연결되었습니다. 설정한 시간 동안 쓰지 않으면 스스로 끝납니다." ) );
                      NotifyAdminConnected();
                  }
                  else
                      report( T( "관리자 도우미" ), { OutcomeText( Reply["outcome"].toString(), Reply["reason"].toString(), Reply["nativeCode"].toString().toUInt() ) }, {} );
              } );
}

} // namespace hc::sys
