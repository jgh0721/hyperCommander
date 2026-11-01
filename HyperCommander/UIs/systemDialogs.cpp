#include "stdafx.h"
#include "systemDialogs.hpp"

/// UIs
#include "UIs/Components/cmpSystemView.hpp"

/// 내부 라이브러리
#include "UniqueLibs/permissionService.hpp"
#include "UniqueLibs/qtMessageDialog.hpp"
#include "UniqueLibs/system/systemSupport.hpp"

/// 공통 라이브러리
#include "commonLibs/system/imageInfo.hpp"
#include "commonLibs/system/processInfo.hpp"

/// 외부 라이브러리
#include <fmwidgets/Button.h>

/// 프레임워크
#include <QCheckBox>
#include <QComboBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QSpinBox>
#include <QTabWidget>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace hc::ui {
namespace {

QString T( const char* Text ) { return QObject::tr( Text ); }

QLabel* ValueLabel( const QString& Text, bool Mono = false )
{
    auto* Label = new QLabel( Text.isEmpty() ? T( "—" ) : Text );
    Label->setWordWrap( true );
    Label->setTextFormat( Qt::PlainText );
    Label->setTextInteractionFlags( Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard );
    if( Mono ) Label->setFont( QFontDatabase::systemFont( QFontDatabase::FixedFont ) );
    return Label;
}

QWidget* FormPage( QTabWidget* Tabs, const QString& Title, QFormLayout*& Form )
{
    auto* Page = new QWidget( Tabs );
    Form = new QFormLayout( Page );
    Form->setLabelAlignment( Qt::AlignRight | Qt::AlignTop );
    Form->setFieldGrowthPolicy( QFormLayout::AllNonFixedFieldsGrow );
    Tabs->addTab( Page, Title );
    return Page;
}

QString StartTypeKey( hc::system::StartType Type )
{
    using hc::system::StartType;
    switch( Type )
    {
        case StartType::Boot:        return "boot";
        case StartType::System:      return "system";
        case StartType::Auto:        return "auto";
        case StartType::AutoDelayed: return "autoDelayed";
        case StartType::Manual:      return "manual";
        case StartType::Disabled:    return "disabled";
        default:                     return {};
    }
}

// 복구 작업 콤보의 순서(아무 작업 안 함, 서비스 다시 시작, 프로그램 실행, 컴퓨터 다시 시작)와 SC_ACTION 값
constexpr uint32_t FailureTypes[] = { 0, 1, 3, 2 };

int FailureIndex( uint32_t Type )
{
    for( int Index = 0; Index < 4; ++Index )
        if( FailureTypes[Index] == Type ) return Index;
    return 0;
}

} // namespace

//////////////////////////////////////////////////////////////////////////////
/// 끝내기 확인

TerminateChoice ConfirmTerminate( QWidget* Parent, const QVector<ProcessRef>& Targets, bool Tree, bool Restart )
{
    TerminateChoice Result;
    const auto Settings = hc::sys::Settings::Load();
    DialogShell Dialog( Restart ? T( "프로세스 다시 시작" ) : Tree ? T( "프로세스 트리 끝내기" ) : T( "프로세스 끝내기" ), Parent );
    Dialog.setObjectName( "hc.process.terminateDialog" );
    Dialog.resize( 640, 420 );
    Dialog.SetSummary( Restart ? T( "프로세스를 끝낸 뒤 같은 명령줄로 다시 실행합니다." )
                       : Tree  ? T( "선택한 프로세스와 하위 프로세스 %1개를 하위부터 끝냅니다." ).arg( Targets.size() )
                               : T( "프로세스 %1개를 끝냅니다. 저장하지 않은 데이터는 사라질 수 있습니다." ).arg( Targets.size() ) );
    auto* List = new QTreeWidget( &Dialog );
    List->setObjectName( "hc.process.terminateList" );
    List->setRootIsDecorated( false );
    List->setHeaderLabels( { T( "이름" ), T( "PID" ), T( "확인" ) } );
    List->header()->setSectionResizeMode( 0, QHeaderView::Stretch );
    bool Product = false, Critical = false;
    for( const auto& Target : Targets )
    {
        QString Note;
        if( Target.key.pid == GetCurrentProcessId() || Target.name.compare( "HyperCommanderAdmin.exe", Qt::CaseInsensitive ) == 0 )
            Note = T( "끝낼 수 없음(HyperCommander)" );
        else if( Target.critical || hc::system::IsSystemCriticalName( Target.name.toStdWString() ) )
        {
            Note = T( "중요 프로세스 — 시스템이 중단될 수 있음" );
            Critical = true;
        }
        else if( hc::sys::IsProductImage( Target.image.isEmpty() ? Target.name : Target.image ) )
        {
            Note = T( "HyperCommander 구성 요소" );
            Product = true;
        }
        else if( !Target.opened )
            Note = T( "관리자 도우미 필요" );
        auto* Item = new QTreeWidgetItem( List, { Target.name, QString::number( Target.key.pid ), Note } );
        Item->setToolTip( 0, Target.image );
    }
    Dialog.BodyLayout()->addWidget( List, 1 );
    auto* Graceful = new QCheckBox( T( "창이 있으면 먼저 닫기를 요청하고 %1초 기다립니다" ).arg( Settings.gracefulWaitSec ), &Dialog );
    Graceful->setObjectName( "hc.process.graceful" );
    Graceful->setChecked( Settings.gracefulClose );
    Dialog.BodyLayout()->addWidget( Graceful );
    QCheckBox* Warned = nullptr;
    if( Product )
    {
        Warned = new QCheckBox( T( "HyperCommander 구성 요소도 끝냅니다" ), &Dialog );
        Warned->setObjectName( "hc.process.allowWarned" );
        Dialog.BodyLayout()->addWidget( Warned );
    }
    if( Critical )
        Dialog.SetMessage( T( "중요 프로세스는 정책상 끝내지 않습니다. 목록의 다른 프로세스만 끝냅니다." ), true );
    Dialog.AcceptButton()->setText( Restart ? T( "다시 시작(&R)" ) : T( "끝내기(&E)" ) );
    Dialog.AcceptButton()->setRole( fm::ui::Button::Danger );
    if( Dialog.exec() != QDialog::Accepted )
        return Result;
    Result.accepted = true;
    Result.graceful = Graceful->isChecked();
    Result.allowWarned = Warned && Warned->isChecked();
    return Result;
}

//////////////////////////////////////////////////////////////////////////////
/// 새 작업 실행

bool PromptRun( QWidget* Parent, hc::system::RunRequest& Request, const QString& InitialDirectory )
{
    DialogShell Dialog( T( "새 작업 실행" ), Parent );
    Dialog.setObjectName( "hc.process.runDialog" );
    Dialog.SetSummary( T( "프로그램·문서·폴더를 엽니다. 인수는 명령줄에 함께 적습니다." ) );
    auto* Form = new QFormLayout;
    auto* CommandRow = new QHBoxLayout;
    auto* Command = new QLineEdit( &Dialog );
    Command->setObjectName( "hc.process.runCommand" );
    auto* Browse = new QPushButton( T( "찾아보기…" ), &Dialog );
    CommandRow->addWidget( Command, 1 );
    CommandRow->addWidget( Browse );
    Form->addRow( T( "명령줄" ), CommandRow );
    auto* Directory = new QLineEdit( QDir::toNativeSeparators( InitialDirectory ), &Dialog );
    Directory->setObjectName( "hc.process.runDirectory" );
    Form->addRow( T( "시작 폴더" ), Directory );
    auto* Priority = new QComboBox( &Dialog );
    Priority->setObjectName( "hc.process.runPriority" );
    Priority->addItem( T( "기본" ), 0 );
    for( const auto Class : { HIGH_PRIORITY_CLASS, ABOVE_NORMAL_PRIORITY_CLASS, NORMAL_PRIORITY_CLASS, BELOW_NORMAL_PRIORITY_CLASS, IDLE_PRIORITY_CLASS } )
        Priority->addItem( hc::sys::PriorityText( Class ), quint32( Class ) );
    Form->addRow( T( "우선순위" ), Priority );
    auto* Elevated = new QCheckBox( T( "관리자 권한으로 실행" ), &Dialog );
    Elevated->setObjectName( "hc.process.runElevated" );
    if( hc::permissions::Elevated() )
    {
        Elevated->setChecked( true );
        Elevated->setEnabled( false );
        Elevated->setToolTip( T( "HyperCommander가 관리자 권한으로 실행 중이므로 새 작업도 관리자 권한을 받습니다." ) );
    }
    Form->addRow( {}, Elevated );
    Dialog.BodyLayout()->addLayout( Form );
    Dialog.AcceptButton()->setText( T( "실행(&R)" ) );
    Dialog.SetAcceptEnabled( false );
    QObject::connect( Command, &QLineEdit::textChanged, &Dialog, [&Dialog]( const QString& Text ) { Dialog.SetAcceptEnabled( !Text.trimmed().isEmpty() ); } );
    QObject::connect( Browse, &QPushButton::clicked, &Dialog, [&] {
        const auto File = QFileDialog::getOpenFileName( &Dialog, T( "실행할 파일" ), Directory->text(), T( "프로그램 (*.exe *.com *.bat *.cmd *.ps1 *.msc);;모든 파일 (*)" ) );
        if( File.isEmpty() ) return;
        const auto Native = QDir::toNativeSeparators( File );
        Command->setText( Native.contains( ' ' ) ? '"' + Native + '"' : Native );
        Directory->setText( QDir::toNativeSeparators( QFileInfo( File ).absolutePath() ) );
    } );
    Dialog.SetInitialInput( Command );
    Dialog.resize( 620, 300 );
    if( Dialog.exec() != QDialog::Accepted )
        return false;
    Request.command = Command->text().trimmed().toStdWString();
    Request.directory = Directory->text().trimmed().toStdWString();
    Request.elevated = Elevated->isChecked() && Elevated->isEnabled();
    Request.priorityClass = Priority->currentData().toUInt();
    return true;
}

//////////////////////////////////////////////////////////////////////////////
/// 프로세스 속성

void ShowProcessProperties( QWidget* Parent, const ProcessRef& Process, const QStringList& Services )
{
    DialogShell Dialog( T( "프로세스 속성" ), Parent );
    Dialog.setObjectName( "hc.process.propertiesDialog" );
    Dialog.SetTitle( T( "%1 (PID %2)" ).arg( Process.name ).arg( Process.key.pid ) );
    hc::system::ProcessDetails Details;
    const bool Found = hc::system::QueryProcessDetails( Process.key.pid, Process.key.createTime, Details );
    const auto Image = Details.imagePath.empty() ? Process.image : QString::fromStdWString( Details.imagePath );
    hc::system::VersionInfo Version;
    if( !Image.isEmpty() ) hc::system::ReadVersionInfo( Image.toStdWString(), Version );
    Dialog.SetSummary( Found ? QString::fromStdWString( Version.description ) : T( "프로세스가 끝났거나 PID가 다른 프로세스에 재사용되었습니다." ) );
    auto* Tabs = new QTabWidget( &Dialog );
    Tabs->setObjectName( "hc.process.propertiesTabs" );
    QFormLayout* Form = nullptr;
    FormPage( Tabs, T( "이미지" ), Form );
    Form->addRow( T( "경로" ), ValueLabel( QDir::toNativeSeparators( Image ), true ) );
    Form->addRow( T( "명령줄" ), ValueLabel( Details.commandLine.empty() ? Process.commandLine : QString::fromStdWString( Details.commandLine ), true ) );
    Form->addRow( T( "설명" ), ValueLabel( QString::fromStdWString( Version.description ) ) );
    Form->addRow( T( "회사" ), ValueLabel( QString::fromStdWString( Version.company ) ) );
    Form->addRow( T( "제품·버전" ), ValueLabel( QStringList{ QString::fromStdWString( Version.product ), QString::fromStdWString( Version.version ) }.join( " · " ) ) );
    auto* Signature = ValueLabel( T( "확인 중…" ) );
    Signature->setObjectName( "hc.process.signature" );
    Form->addRow( T( "서명" ), Signature );
    FormPage( Tabs, T( "프로세스" ), Form );
    Form->addRow( T( "PID" ), ValueLabel( QString::number( Process.key.pid ) ) );
    Form->addRow( T( "부모 PID" ), ValueLabel( QString::number( Process.parentPid ) ) );
    Form->addRow( T( "시작 시각" ), ValueLabel( hc::sys::FileTimeText( Process.key.createTime ) ) );
    Form->addRow( T( "우선순위" ), ValueLabel( hc::sys::PriorityText( Details.priorityClass ? Details.priorityClass : Process.priorityClass ) ) );
    Form->addRow( T( "아키텍처" ), ValueLabel( Details.wow64 ? T( "32비트(WOW64)" ) : T( "64비트" ) ) );
    Form->addRow( T( "상태" ), ValueLabel( Process.suspended ? T( "일시 중단됨" ) : T( "실행 중" ) ) );
    if( Details.packaged ) Form->addRow( T( "패키지" ), ValueLabel( QString::fromStdWString( Details.packageName ) ) );
    FormPage( Tabs, T( "보안" ), Form );
    Form->addRow( T( "사용자" ), ValueLabel( QString::fromStdWString( Details.userName ) ) );
    Form->addRow( T( "SID" ), ValueLabel( QString::fromStdWString( Details.userSid ), true ) );
    Form->addRow( T( "무결성" ), ValueLabel( hc::sys::IntegrityText( Details.integrity ) ) );
    Form->addRow( T( "관리자 권한" ), ValueLabel( Details.tokenRead ? ( Details.elevated ? T( "예" ) : T( "아니요" ) ) : QString{} ) );
    Form->addRow( T( "보호" ), ValueLabel( Details.protectedProcess ? T( "보호된 프로세스" ) : Details.critical ? T( "중요 프로세스" ) : T( "없음" ) ) );
    if( !Details.tokenRead )
        Form->addRow( {}, ValueLabel( T( "표준 권한으로는 이 프로세스의 토큰을 읽을 수 없습니다." ) ) );
    if( !Services.isEmpty() )
    {
        FormPage( Tabs, T( "서비스" ), Form );
        for( const auto& Service : Services )
            Form->addRow( {}, ValueLabel( Service ) );
    }
    Dialog.BodyLayout()->addWidget( Tabs, 1 );
    Dialog.AcceptButton()->setText( T( "닫기" ) );
    Dialog.CancelButton()->hide();
    Dialog.resize( 680, 460 );
    if( !Image.isEmpty() )
    {
        const QPointer<QLabel> Guard( Signature );
        hc::sys::RunAsync( &Dialog, [Image] { return hc::system::VerifySignature( Image.toStdWString() ); },
                           [Guard]( const hc::system::SignatureInfo& Info ) {
                               if( !Guard ) return;
                               using hc::system::SignatureState;
                               Guard->setText( Info.state == SignatureState::Signed     ? T( "서명 확인됨 · %1%2" ).arg( QString::fromStdWString( Info.signer ), Info.catalog ? T( " (카탈로그)" ) : QString{} )
                                               : Info.state == SignatureState::Unsigned ? T( "서명 없음" )
                                               : Info.state == SignatureState::Invalid  ? T( "서명이 올바르지 않음 · %1" ).arg( hc::sys::Win32ErrorText( Info.code ) )
                                                                                        : T( "확인할 수 없음" ) );
                           } );
    }
    else
        Signature->setText( T( "확인할 수 없음" ) );
    Dialog.exec();
}

//////////////////////////////////////////////////////////////////////////////
/// 서비스 속성

namespace {
class ServiceDialog final : public DialogShell
{
public:
    ServiceDialog( const QString& Title, QWidget* Parent ) : DialogShell( Title, Parent ) {}
    std::function<bool()> apply_;

protected:
    void accept() override
    {
        if( apply_ && !apply_() )
            return;
        DialogShell::accept();
    }
};
} // namespace

void ShowServiceProperties( QWidget* Parent, const QString& Name, ServiceApply Apply, ServiceControlRequest Control )
{
    using namespace hc::system;
    ServiceConfig Config;
    uint32_t Error = 0;
    if( !QueryServiceConfiguration( Name.toStdWString(), Config, &Error ) )
    {
        hc::ui::MessageDialog::warning( Parent, T( "서비스 속성" ), T( "서비스 구성을 읽을 수 없습니다: %1" ).arg( hc::sys::Win32ErrorText( Error ) ) );
        return;
    }
    ServiceStatusInfo Status;
    QueryServiceStatusInfo( Name.toStdWString(), Status );
    ServiceDialog Dialog( T( "서비스 속성" ), Parent );
    Dialog.setObjectName( "hc.service.propertiesDialog" );
    Dialog.SetTitle( QString::fromStdWString( Config.displayName ) );
    Dialog.SetSummary( Name + " · " + hc::sys::ServiceTypeText( Config.type ) );
    const bool Driver = ( Config.type & ( SERVICE_KERNEL_DRIVER | SERVICE_FILE_SYSTEM_DRIVER ) ) != 0;
    auto* Tabs = new QTabWidget( &Dialog );
    Tabs->setObjectName( "hc.service.propertiesTabs" );
    QFormLayout* Form = nullptr;

    // 일반
    FormPage( Tabs, T( "일반" ), Form );
    auto* DisplayName = new QLineEdit( QString::fromStdWString( Config.displayName ) );
    DisplayName->setObjectName( "hc.service.displayName" );
    Form->addRow( T( "표시 이름" ), DisplayName );
    auto* Description = new QPlainTextEdit( QString::fromStdWString( Config.description ) );
    Description->setObjectName( "hc.service.description" );
    Description->setMaximumHeight( 80 );
    Form->addRow( T( "설명" ), Description );
    Form->addRow( T( "실행 파일 경로" ), ValueLabel( QString::fromStdWString( Config.binaryPath ), true ) );
    auto* Start = new QComboBox;
    Start->setObjectName( "hc.service.startType" );
    if( Config.start == StartType::Boot || Config.start == StartType::System )
        Start->addItem( hc::sys::StartTypeText( Config.start ), StartTypeKey( Config.start ) );
    for( const auto Type : { StartType::Auto, StartType::AutoDelayed, StartType::Manual, StartType::Disabled } )
        if( !Driver || Type != StartType::AutoDelayed )
            Start->addItem( hc::sys::StartTypeText( Type ), StartTypeKey( Type ) );
    Start->setCurrentIndex( std::max( 0, Start->findData( StartTypeKey( Config.start ) ) ) );
    Form->addRow( T( "시작 유형" ), Start );
    if( Config.triggerStart )
        Form->addRow( {}, ValueLabel( T( "트리거 시작 서비스입니다(조건 %1개). 트리거가 발생하면 시작 유형과 관계없이 시작될 수 있습니다." ).arg( Config.triggerCount ) ) );
    auto* State = ValueLabel( hc::sys::ServiceStateText( Status.state ) );
    State->setObjectName( "hc.service.state" );
    Form->addRow( T( "서비스 상태" ), State );
    auto* Buttons = new QHBoxLayout;
    for( const auto& [Key, Label] : { std::pair{ "start", T( "시작" ) }, std::pair{ "stop", T( "중지" ) }, std::pair{ "pause", T( "일시 중지" ) }, std::pair{ "continue", T( "계속" ) } } )
    {
        auto* Button = new QPushButton( Label );
        Button->setObjectName( QString( "hc.service.control." ) + Key );
        const QString Command = Key;
        QObject::connect( Button, &QPushButton::clicked, &Dialog, [Control, Command] { if( Control ) Control( Command ); } );
        const bool Running = Status.state == ServiceState::Running, Stopped = Status.state == ServiceState::Stopped, Paused = Status.state == ServiceState::Paused;
        Button->setEnabled( Command == "start" ? Stopped : Command == "stop" ? ( Running || Paused ) && ( Status.controlsAccepted & SERVICE_ACCEPT_STOP )
                            : Command == "pause"  ? Running && ( Status.controlsAccepted & SERVICE_ACCEPT_PAUSE_CONTINUE )
                                                  : Paused );
        Buttons->addWidget( Button );
    }
    Buttons->addStretch( 1 );
    Form->addRow( {}, Buttons );

    // 로그온
    auto* AccountPage = FormPage( Tabs, T( "로그온" ), Form );
    AccountPage->setEnabled( !Driver );
    const auto Account = QString::fromStdWString( Config.account );
    auto* System = new QRadioButton( T( "로컬 시스템 계정" ) );
    System->setObjectName( "hc.service.accountSystem" );
    auto* Other = new QRadioButton( T( "다음 계정" ) );
    Other->setObjectName( "hc.service.accountOther" );
    const bool LocalSystem = Account.isEmpty() || Account.compare( "LocalSystem", Qt::CaseInsensitive ) == 0;
    ( LocalSystem ? System : Other )->setChecked( true );
    auto* AccountName = new QLineEdit( LocalSystem ? QString{} : Account );
    AccountName->setObjectName( "hc.service.accountName" );
    auto* Password = new QLineEdit;
    Password->setObjectName( "hc.service.password" );
    Password->setEchoMode( QLineEdit::Password );
    auto* Confirm = new QLineEdit;
    Confirm->setObjectName( "hc.service.passwordConfirm" );
    Confirm->setEchoMode( QLineEdit::Password );
    Form->addRow( {}, System );
    Form->addRow( {}, Other );
    Form->addRow( T( "계정" ), AccountName );
    Form->addRow( T( "암호" ), Password );
    Form->addRow( T( "암호 확인" ), Confirm );
    Form->addRow( {}, ValueLabel( T( "NT AUTHORITY\\LocalService · NT AUTHORITY\\NetworkService · NT SERVICE\\이름(가상 계정)은 암호 없이 지정합니다." ) ) );
    const auto SyncAccount = [=] {
        AccountName->setEnabled( Other->isChecked() );
        Password->setEnabled( Other->isChecked() );
        Confirm->setEnabled( Other->isChecked() );
    };
    QObject::connect( Other, &QRadioButton::toggled, &Dialog, SyncAccount );
    SyncAccount();

    // 복구
    auto* RecoveryPage = FormPage( Tabs, T( "복구" ), Form );
    RecoveryPage->setEnabled( !Driver );
    QComboBox* Failure[3] = {};
    const QStringList FailureLabels{ T( "아무 작업 안 함" ), T( "서비스 다시 시작" ), T( "프로그램 실행" ), T( "컴퓨터 다시 시작" ) };
    const QStringList Rows{ T( "첫 번째 실패" ), T( "두 번째 실패" ), T( "후속 실패" ) };
    for( int Index = 0; Index < 3; ++Index )
    {
        Failure[Index] = new QComboBox;
        Failure[Index]->setObjectName( QString( "hc.service.failure%1" ).arg( Index ) );
        Failure[Index]->addItems( FailureLabels );
        const auto& Actions = Config.failure.actions;
        if( !Actions.empty() )
            Failure[Index]->setCurrentIndex( FailureIndex( Actions[std::min<size_t>( Index, Actions.size() - 1 )].type ) );
        Form->addRow( Rows[Index], Failure[Index] );
    }
    auto* Reset = new QSpinBox;
    Reset->setObjectName( "hc.service.failureReset" );
    Reset->setRange( 0, 9999 );
    Reset->setSuffix( T( " 일" ) );
    Reset->setValue( int( Config.failure.resetPeriodSec / 86400 ) );
    Form->addRow( T( "실패 횟수 초기화" ), Reset );
    auto* Delay = new QSpinBox;
    Delay->setObjectName( "hc.service.failureDelay" );
    Delay->setRange( 0, 9999 );
    Delay->setSuffix( T( " 분" ) );
    uint32_t DelayMs = 60000;
    for( const auto& Action : Config.failure.actions )
        if( Action.type == SC_ACTION_RESTART ) { DelayMs = Action.delayMs; break; }
    Delay->setValue( int( DelayMs / 60000 ) );
    Form->addRow( T( "서비스 다시 시작 대기" ), Delay );
    auto* Command = new QLineEdit( QString::fromStdWString( Config.failure.command ) );
    Command->setObjectName( "hc.service.failureCommand" );
    Form->addRow( T( "실행할 프로그램" ), Command );
    auto* NonCrash = new QCheckBox( T( "오류와 함께 중지된 경우에도 복구 작업 사용" ) );
    NonCrash->setObjectName( "hc.service.failureNonCrash" );
    NonCrash->setChecked( Config.failure.onNonCrashFailures );
    Form->addRow( {}, NonCrash );

    // 종속성
    auto* Dependencies = new QTreeWidget( Tabs );
    Dependencies->setObjectName( "hc.service.dependencies" );
    Dependencies->setHeaderLabels( { T( "구성 요소" ), T( "상태" ) } );
    auto* Needs = new QTreeWidgetItem( Dependencies, { T( "이 서비스가 필요로 하는 구성 요소" ) } );
    for( const auto& Dependency : Config.dependencies )
    {
        const auto Text = QString::fromStdWString( Dependency );
        ServiceStatusInfo Other;
        const bool Group = Text.startsWith( '+' );
        const bool Known = !Group && QueryServiceStatusInfo( Dependency, Other );
        new QTreeWidgetItem( Needs, { Group ? T( "%1 (그룹)" ).arg( Text.mid( 1 ) ) : Text, Known ? hc::sys::ServiceStateText( Other.state ) : QString{} } );
    }
    auto* Users = new QTreeWidgetItem( Dependencies, { T( "이 서비스를 필요로 하는 구성 요소" ) } );
    std::vector<ServiceStatusInfo> Dependents;
    QueryDependents( Name.toStdWString(), Dependents );
    for( const auto& Dependent : Dependents )
        new QTreeWidgetItem( Users, { QString::fromStdWString( Dependent.name ) + " — " + QString::fromStdWString( Dependent.displayName ), hc::sys::ServiceStateText( Dependent.state ) } );
    Dependencies->expandAll();
    Dependencies->header()->setSectionResizeMode( 0, QHeaderView::Stretch );
    Tabs->addTab( Dependencies, T( "종속성" ) );

    // 고급
    FormPage( Tabs, T( "고급" ), Form );
    Form->addRow( T( "오류 제어" ), ValueLabel( Config.errorControl == 0 ? T( "무시" ) : Config.errorControl == 1 ? T( "보통" ) : Config.errorControl == 2 ? T( "심각" ) : T( "매우 심각" ) ) );
    Form->addRow( T( "로드 순서 그룹" ), ValueLabel( QString::fromStdWString( Config.loadOrderGroup ) ) );
    Form->addRow( T( "서비스 SID" ), ValueLabel( Config.sidType == SERVICE_SID_TYPE_UNRESTRICTED ? T( "제한 없음" ) : Config.sidType == SERVICE_SID_TYPE_RESTRICTED ? T( "제한됨" ) : T( "없음" ) ) );
    QStringList Privileges;
    for( const auto& Privilege : Config.privileges ) Privileges.append( QString::fromStdWString( Privilege ) );
    Form->addRow( T( "필요한 권한" ), ValueLabel( Privileges.join( '\n' ) ) );
    Form->addRow( T( "실행 보호" ), ValueLabel( Config.launchProtected ? T( "보호됨(수준 %1)" ).arg( Config.launchProtected ) : T( "없음" ) ) );
    if( !Config.serviceDll.empty() )
        Form->addRow( T( "ServiceDll" ), ValueLabel( QString::fromStdWString( Config.serviceDll ), true ) );
    Form->addRow( T( "실행 파일" ), ValueLabel( QString::fromStdWString( Config.resolvedImage ), true ) );

    Dialog.BodyLayout()->addWidget( Tabs, 1 );
    Dialog.AcceptButton()->setText( T( "적용(&A)" ) );
    Dialog.resize( 720, 560 );
    Dialog.apply_ = [&]() -> bool {
        ServiceConfigChange Change;
        QJsonObject Json;
        if( DisplayName->text() != QString::fromStdWString( Config.displayName ) && !DisplayName->text().trimmed().isEmpty() )
        {
            Change.displayName = DisplayName->text().toStdWString();
            Json["displayName"] = DisplayName->text();
        }
        if( Description->toPlainText() != QString::fromStdWString( Config.description ) )
        {
            Change.description = Description->toPlainText().toStdWString();
            Json["description"] = Description->toPlainText();
        }
        const auto StartKey = Start->currentData().toString();
        if( StartKey != StartTypeKey( Config.start ) )
        {
            using hc::system::StartType;
            Change.start = StartKey == "auto" ? StartType::Auto : StartKey == "autoDelayed" ? StartType::AutoDelayed : StartKey == "manual" ? StartType::Manual : StartType::Disabled;
            Json["startType"] = StartKey;
        }
        if( !Driver )
        {
            const auto NewAccount = System->isChecked() ? QStringLiteral( "LocalSystem" ) : AccountName->text().trimmed();
            const bool AccountChanged = NewAccount.compare( LocalSystem ? QStringLiteral( "LocalSystem" ) : Account, Qt::CaseInsensitive ) != 0;
            if( Other->isChecked() && Password->text() != Confirm->text() )
            {
                Dialog.SetMessage( T( "암호와 암호 확인이 다릅니다." ), true );
                Tabs->setCurrentIndex( 1 );
                return false;
            }
            if( Other->isChecked() && NewAccount.isEmpty() )
            {
                Dialog.SetMessage( T( "계정을 입력하십시오." ), true );
                Tabs->setCurrentIndex( 1 );
                return false;
            }
            if( AccountChanged || !Password->text().isEmpty() )
            {
                Change.account = NewAccount.toStdWString();
                Json["account"] = NewAccount;
                if( Other->isChecked() && !Password->text().isEmpty() )
                {
                    Change.password = Password->text().toStdWString();
                    Json["password"] = Password->text();
                }
            }
            FailureActions Actions;
            Actions.resetPeriodSec = uint32_t( Reset->value() ) * 86400u;
            Actions.command = Command->text().toStdWString();
            Actions.rebootMessage = Config.failure.rebootMessage;
            Actions.onNonCrashFailures = NonCrash->isChecked();
            bool Any = false;
            for( auto* Box : Failure )
            {
                const uint32_t Type = FailureTypes[std::clamp( Box->currentIndex(), 0, 3 )];
                Any |= Type != 0;
                Actions.actions.push_back( { Type, Type == SC_ACTION_RESTART ? uint32_t( Delay->value() ) * 60000u : Type == SC_ACTION_REBOOT ? 60000u : 0u } );
            }
            if( !Any ) Actions.actions.clear();
            const auto& Old = Config.failure;
            bool FailureChanged = Old.resetPeriodSec / 86400 != Actions.resetPeriodSec / 86400 || Old.command != Actions.command
                                  || Old.onNonCrashFailures != Actions.onNonCrashFailures || Old.actions.size() != Actions.actions.size();
            for( size_t Index = 0; !FailureChanged && Index < Actions.actions.size(); ++Index )
                FailureChanged = Old.actions[Index].type != Actions.actions[Index].type
                                 || ( Actions.actions[Index].type == SC_ACTION_RESTART && Old.actions[Index].delayMs / 60000 != Actions.actions[Index].delayMs / 60000 );
            if( FailureChanged )
            {
                Change.failure = Actions;
                QJsonArray Items;
                for( const auto& Item : Actions.actions ) Items.append( QJsonObject{ { "type", qint64( Item.type ) }, { "delayMs", qint64( Item.delayMs ) } } );
                Json["failure"] = QJsonObject{ { "resetPeriodSec", qint64( Actions.resetPeriodSec ) }, { "command", Command->text() },
                                               { "rebootMessage", QString::fromStdWString( Actions.rebootMessage ) }, { "onNonCrashFailures", Actions.onNonCrashFailures },
                                               { "actions", Items } };
            }
        }
        if( Json.isEmpty() )
            return true;
        const auto Failure = Apply ? Apply( Change, Json ) : QString{};
        // 암호는 이 함수가 끝나면 사라진다. 입력란도 비운다.
        Password->clear();
        Confirm->clear();
        if( Failure.isEmpty() )
            return true;
        Dialog.SetMessage( Failure, true );
        return false;
    };
    Dialog.exec();
}

} // namespace hc::ui
