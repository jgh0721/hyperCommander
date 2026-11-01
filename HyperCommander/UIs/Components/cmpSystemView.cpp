#include "stdafx.h"
#include "cmpSystemView.hpp"

/// UIs
#include "UIs/cmpPanel.hpp"

/// 내부 라이브러리
#include "UniqueLibs/commandMgr.hpp"
#include "UniqueLibs/commandUiBridge.hpp"
#include "UniqueLibs/permissionService.hpp"
#include "UniqueLibs/settings/appSettings.hpp"
#include "UniqueLibs/system/systemCommands.hpp"

/// 공통 라이브러리
#include "commonLibs/system/imageInfo.hpp"
#include "commonLibs/system/processActions.hpp"
#include "commonLibs/system/serviceControl.hpp"

/// 외부 라이브러리
#include <QtitanGrid.h>
#include <fmstyle/ThemeManager.h>
#include <fmwidgets/SegmentedControl.h>

/// 프레임워크
#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QLabel>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QStyleOptionViewItem>
#include <QPushButton>
#include <QScrollArea>
#include <QTabWidget>
#include <QToolButton>
#include <QTreeWidget>

using hc::sys::ProcessTreeModel;
using hc::sys::ServiceListModel;

namespace {
CommandUiBridge* Bridge() { return TyStCommandMgr::GetInstance()->Bridge(); }

class Sparkline final : public QWidget
{
public:
    Sparkline( const QString& Title, QWidget* Parent ) : QWidget( Parent ), title_( Title ) { setMinimumHeight( 64 ); }
    void SetValues( const QVector<float>& Values, const QString& Current, const QColor& Color )
    {
        values_ = Values;
        current_ = Current;
        color_ = Color;
        update();
    }

protected:
    void paintEvent( QPaintEvent* ) override
    {
        QPainter Painter( this );
        const auto& Colors = fm::style::ThemeManager::instance().colors();
        const QRect Header( 0, 0, width(), fontMetrics().height() + 2 );
        Painter.setPen( Colors[fm::style::Token::Fg2] );
        Painter.drawText( Header, Qt::AlignLeft | Qt::AlignVCenter, title_ );
        Painter.drawText( Header, Qt::AlignRight | Qt::AlignVCenter, current_ );
        const QRect Plot = rect().adjusted( 0, Header.height() + 2, -1, -1 );
        Painter.fillRect( Plot, Colors[fm::style::Token::Alt] );
        Painter.setPen( Colors[fm::style::Token::Line] );
        Painter.drawRect( Plot );
        if( values_.size() < 2 )
            return;
        float Maximum = 0;
        for( const auto Value : values_ )
            Maximum = std::max( Maximum, Value );
        if( Maximum <= 0 )
            Maximum = 1;
        QPainterPath Line;
        for( int Index = 0; Index < values_.size(); ++Index )
        {
            const double X = Plot.left() + 1 + ( Plot.width() - 2 ) * double( Index ) / double( std::max<qsizetype>( 59, values_.size() - 1 ) );
            const double Y = Plot.bottom() - 1 - ( Plot.height() - 4 ) * double( values_[Index] / Maximum );
            Index == 0 ? Line.moveTo( X, Y ) : Line.lineTo( X, Y );
        }
        auto Area = Line;
        Area.lineTo( Line.currentPosition().x(), Plot.bottom() );
        Area.lineTo( Plot.left() + 1, Plot.bottom() );
        Area.closeSubpath();
        Painter.setRenderHint( QPainter::Antialiasing );
        auto Fill = color_;
        Fill.setAlphaF( 0.18 );
        Painter.fillPath( Area, Fill );
        Painter.setPen( QPen( color_, 1.4 ) );
        Painter.drawPath( Line );
    }

private:
    QString        title_, current_;
    QVector<float> values_;
    QColor         color_;
};
} // namespace

// 프로세스 트리의 연결선. Qtitan은 조상 칸마다 '다음 형제 있음'을 늘 켠 채 PE_IndicatorBranch를 부르므로,
// 행의 실제 조상(셀 옵션의 모델 인덱스)으로 칸마다 다시 그린다. 나머지 요소는 모두 지금의 앱 스타일에 넘긴다
// (테마를 바꾸면 앱 스타일이 바뀌므로 소유하지 않고 호출할 때마다 찾는다).
class TreeBranchStyle final : public QStyle
{
public:
    explicit TreeBranchStyle( QObject* Parent ) { setParent( Parent ); }
    bool lines_ = true;

    void drawPrimitive( PrimitiveElement Element, const QStyleOption* Option, QPainter* Painter, const QWidget* Widget ) const override
    {
        if( Element == PE_IndicatorBranch ) { drawBranch( Option, Painter, Widget ); return; }
        app()->drawPrimitive( Element, Option, Painter, Widget );
    }
    void drawControl( ControlElement Element, const QStyleOption* Option, QPainter* Painter, const QWidget* Widget ) const override { app()->drawControl( Element, Option, Painter, Widget ); }
    QRect subElementRect( SubElement Element, const QStyleOption* Option, const QWidget* Widget ) const override { return app()->subElementRect( Element, Option, Widget ); }
    void drawComplexControl( ComplexControl Control, const QStyleOptionComplex* Option, QPainter* Painter, const QWidget* Widget ) const override { app()->drawComplexControl( Control, Option, Painter, Widget ); }
    SubControl hitTestComplexControl( ComplexControl Control, const QStyleOptionComplex* Option, const QPoint& Point, const QWidget* Widget ) const override { return app()->hitTestComplexControl( Control, Option, Point, Widget ); }
    QRect subControlRect( ComplexControl Control, const QStyleOptionComplex* Option, SubControl Sub, const QWidget* Widget ) const override { return app()->subControlRect( Control, Option, Sub, Widget ); }
    int pixelMetric( PixelMetric Metric, const QStyleOption* Option, const QWidget* Widget ) const override { return app()->pixelMetric( Metric, Option, Widget ); }
    QSize sizeFromContents( ContentsType Type, const QStyleOption* Option, const QSize& Size, const QWidget* Widget ) const override { return app()->sizeFromContents( Type, Option, Size, Widget ); }
    int styleHint( StyleHint Hint, const QStyleOption* Option, const QWidget* Widget, QStyleHintReturn* Return ) const override { return app()->styleHint( Hint, Option, Widget, Return ); }
    QPixmap standardPixmap( StandardPixmap Pixmap, const QStyleOption* Option, const QWidget* Widget ) const override { return app()->standardPixmap( Pixmap, Option, Widget ); }
    QIcon standardIcon( StandardPixmap Icon, const QStyleOption* Option, const QWidget* Widget ) const override { return app()->standardIcon( Icon, Option, Widget ); }
    QPixmap generatedIconPixmap( QIcon::Mode Mode, const QPixmap& Pixmap, const QStyleOption* Option ) const override { return app()->generatedIconPixmap( Mode, Pixmap, Option ); }
    int layoutSpacing( QSizePolicy::ControlType First, QSizePolicy::ControlType Second, Qt::Orientation Orientation, const QStyleOption* Option, const QWidget* Widget ) const override
    { return app()->layoutSpacing( First, Second, Orientation, Option, Widget ); }
    void polish( QWidget* Widget ) override { app()->polish( Widget ); }
    void unpolish( QWidget* Widget ) override { app()->unpolish( Widget ); }
    void polish( QPalette& Palette ) override { app()->polish( Palette ); }
    QPalette standardPalette() const override { return app()->standardPalette(); }
    QRect itemTextRect( const QFontMetrics& Metrics, const QRect& Rect, int Flags, bool Enabled, const QString& Text ) const override { return app()->itemTextRect( Metrics, Rect, Flags, Enabled, Text ); }
    QRect itemPixmapRect( const QRect& Rect, int Flags, const QPixmap& Pixmap ) const override { return app()->itemPixmapRect( Rect, Flags, Pixmap ); }
    void drawItemText( QPainter* Painter, const QRect& Rect, int Flags, const QPalette& Palette, bool Enabled, const QString& Text, QPalette::ColorRole Role ) const override
    { app()->drawItemText( Painter, Rect, Flags, Palette, Enabled, Text, Role ); }
    void drawItemPixmap( QPainter* Painter, const QRect& Rect, int Alignment, const QPixmap& Pixmap ) const override { app()->drawItemPixmap( Painter, Rect, Alignment, Pixmap ); }

private:
    static QStyle* app() { return QApplication::style(); }
    static bool hasNext( const QModelIndex& Index ) { return Index.row() + 1 < Index.model()->rowCount( Index.parent() ); }

    // 디자인마다 펼침 표시가 다르다(수채화 = 상자 + 점선, 기본 = 꺾쇠만). 연결선은 디자인과 관계없이 여기서
    // 그리고, 펼침 표시만 앱 스타일에 맡긴다. 상자 크기의 사각형을 넘겨 스타일이 자기 선을 긋지 않게 한다.
    void drawBranch( const QStyleOption* Option, QPainter* Painter, const QWidget* Widget ) const
    {
        const auto* Item = qstyleoption_cast<const QStyleOptionViewItem*>( Option );
        // 조상 칸은 마지막 칸(State_Item)을 그릴 때 함께 그린다.
        if( !Item || !( Option->state & State_Item ) || !Item->index.isValid() )
            return;
        const auto Index = Item->index.siblingAtColumn( 0 );
        QVector<QModelIndex> Chain;
        for( auto Up = Index; Up.isValid(); Up = Up.parent() )
            Chain.prepend( Up );
        const int Depth = int( Chain.size() );
        const QRect Own = Option->rect;
        const int Indent = Own.width();
        const int MidX = Own.left() + Indent / 2;
        const int MidY = Own.top() + Own.height() / 2;
        const bool Children = Option->state & State_Children;
        // 최상위 프로세스끼리는 잇지 않는다(목록 왼쪽 끝을 가로지르는 긴 선이 생긴다).
        if( lines_ && Depth > 1 )
        {
            const auto& Colors = fm::style::ThemeManager::instance().colors();
            const QColor Line = Colors[fm::style::Token::Fg3];
            const int Gap = Children ? 6 : 0;   // 펼침 표시 둘레는 비운다
            const auto Vertical = [&]( int X, int Top, int Bottom ) { if( Bottom > Top ) Painter->fillRect( QRect( X, Top, 1, Bottom - Top ), Line ); };
            for( int Level = 1; Level + 1 < Depth; ++Level )
                if( hasNext( Chain[Level] ) )
                    Vertical( MidX - ( Depth - 1 - Level ) * Indent, Own.top(), Own.bottom() + 1 );
            Vertical( MidX, Own.top(), MidY - Gap );
            if( hasNext( Index ) )
                Vertical( MidX, MidY + Gap + 1, Own.bottom() + 1 );
            Painter->fillRect( QRect( MidX + Gap + 1, MidY, std::max( 0, Own.right() - 1 - MidX - Gap ), 1 ), Line );
        }
        if( !Children )
            return;
        QStyleOptionViewItem Copy = *Item;
        Copy.state = Option->state & ~( State_Item | State_Sibling );
        Copy.rect = QRect( MidX - 4, MidY - 4, 9, 9 );
        app()->drawPrimitive( PE_IndicatorBranch, &Copy, Painter, Widget );
    }
};

//////////////////////////////////////////////////////////////////////////////
/// CSystemView

CSystemView::CSystemView( Kind Value, QWidget* Parent )
    : CViewT( Parent ), kind_( Value ), settings_( hc::sys::Settings::Load() ), alive_( std::make_shared<std::atomic_bool>( true ) )
{
    viewMode_ = VM_SYSTEM;
    setObjectName( kind_ == Kind::Processes ? "hc.system.processView" : "hc.system.serviceView" );
    auto* Layout = new QVBoxLayout( this );
    Layout->setContentsMargins( 0, 6, 0, 0 );
    Layout->setSpacing( 4 );
    auto* Bar = new QWidget( this );
    Bar->setObjectName( "hc.system.bar" );
    if( kind_ == Kind::Processes ) buildProcessBar( Bar );
    else buildServiceBar( Bar );
    Layout->addWidget( Bar );

    noticeRow_ = new QWidget( this );
    noticeRow_->setObjectName( "hc.system.notice" );
    auto* NoticeLayout = new QHBoxLayout( noticeRow_ );
    NoticeLayout->setContentsMargins( 6, 2, 6, 2 );
    notice_ = new QLabel( noticeRow_ );
    notice_->setWordWrap( true );
    notice_->setTextFormat( Qt::PlainText );
    auto* Helper = new QPushButton( tr( "관리자 도우미 미리 시작" ), noticeRow_ );
    Helper->setObjectName( "hc.system.startHelper" );
    connect( Helper, &QPushButton::clicked, this, [] { if( auto* B = Bridge() ) B->ExecuteCommand( "hc.system.startAdminHelper" ); } );
    NoticeLayout->addWidget( notice_, 1 );
    NoticeLayout->addWidget( Helper );
    Layout->addWidget( noticeRow_ );

    if( kind_ == Kind::Processes )
    {
        searchRow_ = new QWidget( this );
        searchRow_->setObjectName( "hc.system.searchRow" );
        auto* Search = new QHBoxLayout( searchRow_ );
        Search->setContentsMargins( 6, 0, 6, 0 );
        searchStatus_ = new QLabel( searchRow_ );
        searchScope_ = new QComboBox( searchRow_ );
        searchScope_->setObjectName( "hc.system.searchScope" );
        searchScope_->addItem( tr( "이름·설명·명령줄" ), int( hc::sys::ProcessFilterProxy::ScopeName | hc::sys::ProcessFilterProxy::ScopeDescription | hc::sys::ProcessFilterProxy::ScopeCommandLine ) );
        searchScope_->addItem( tr( "이름" ), int( hc::sys::ProcessFilterProxy::ScopeName ) );
        searchScope_->addItem( tr( "명령줄" ), int( hc::sys::ProcessFilterProxy::ScopeCommandLine ) );
        searchScope_->addItem( tr( "사용자" ), int( hc::sys::ProcessFilterProxy::ScopeUser ) );
        searchScope_->addItem( tr( "PID" ), int( hc::sys::ProcessFilterProxy::ScopePid ) );
        searchScope_->addItem( tr( "전체" ), int( hc::sys::ProcessFilterProxy::ScopeAll ) );
        keepAncestors_ = new QCheckBox( tr( "상위 유지" ), searchRow_ );
        keepAncestors_->setObjectName( "hc.system.keepAncestors" );
        keepAncestors_->setChecked( true );
        Search->addWidget( searchStatus_, 1 );
        Search->addWidget( searchScope_ );
        Search->addWidget( keepAncestors_ );
        searchRow_->hide();
        Layout->addWidget( searchRow_ );
    }

    grid_ = new Qtitan::TreeGrid( this );
    grid_->setObjectName( "hc.system.grid" );
    grid_->setViewType( Qtitan::TreeGrid::TreeView );
    grid_->setSizePolicy( QSizePolicy::Expanding, QSizePolicy::Expanding );
    grid_->viewport()->setFocusPolicy( Qt::StrongFocus );
    view_ = grid_->view<Qtitan::GridTreeView>();
    // 모델을 붙이기 전에 끈다. 켜진 채 붙이면 모델 열마다 열이 자동으로 생기고, configureGrid가 같은 데이터 바인딩을
    // 쓰는 열을 한 벌 더 만들어 해제할 때 바인딩을 두 번 지운다.
    view_->options().setAutoCreateColumns( false );
    if( kind_ == Kind::Processes )
    {
        processes_ = new hc::sys::ProcessTreeModel( this );
        processProxy_ = new hc::sys::ProcessFilterProxy( this );
        processProxy_->setSourceModel( processes_ );
        processProxy_->SetOwnOnly( !settings_.allUsers );
        view_->setModel( processProxy_ );
        connect( searchScope_, &QComboBox::currentIndexChanged, this, [this] {
            processProxy_->SetScopes( searchScope_->currentData().toInt() );
            updateBar();
        } );
        connect( keepAncestors_, &QCheckBox::toggled, this, [this]( bool Keep ) {
            processProxy_->SetKeepAncestors( Keep );
            view_->expandAll();
            updateBar();
        } );
        connect( processes_, &ProcessTreeModel::sigNodesInserted, this, [this]( const QVector<hc::system::ProcessKey>& Keys ) {
            if( !expandedOnce_ )
                return;
            // 새 프로세스가 보이도록 부모를 펼친다.
            for( const auto& Key : Keys )
                if( const auto* Node = processes_->Find( Key ); Node && Node->parent )
                {
                    const auto Index = processProxy_->mapFromSource( processes_->IndexOf( Node->parent->key ) );
                    if( Index.isValid() )
                        view_->expand( Index );
                }
        } );
        connect( processes_, &ProcessTreeModel::sigDetailsLoaded, this, [this] {
            // 사용자·설명·명령줄이 들어오면 찾기·사용자 거르기만 다시 한다.
            if( processProxy_->Filtering() )
            {
                processProxy_->Refilter();
                if( !processProxy_->Text().isEmpty() ) view_->expandAll();
                updateBar();
            }
            emit sigCursorChanged();
        } );
        // 한 표본의 행 추가·삭제·값 변경을 Qtitan이 한 번에 처리하게 한다.
        connect( processes_, &ProcessTreeModel::sigBatchStarted, this, [this] { view_->beginUpdate(); } );
        connect( processes_, &ProcessTreeModel::sigBatchFinished, this, [this] { view_->endUpdate(); } );
        branchStyle_ = new TreeBranchStyle( this );
        branchStyle_->lines_ = settings_.treeLines;
        grid_->setStyle( branchStyle_ );
        // 앱 스타일이 바뀌면(테마 전환) 그리드를 새 스타일로 다시 다듬는다.
        connect( &fm::style::ThemeManager::instance(), &fm::style::ThemeManager::changed, this, [this] {
            if( grid_ && branchStyle_ ) grid_->setStyle( branchStyle_ );
        } );
    }
    else
    {
        services_ = new hc::sys::ServiceListModel( this );
        serviceProxy_ = new hc::sys::ServiceFilterProxy( this );
        serviceProxy_->setSourceModel( services_ );
        view_->setModel( serviceProxy_ );
        connect( services_, &ServiceListModel::sigCountsChanged, this, [this] { updateBar(); emit sigSummaryChanged(); } );
        connect( services_, &ServiceListModel::sigConfigLoaded, this, [this] { emit sigCursorChanged(); } );
        connect( services_, &ServiceListModel::sigBatchStarted, this, [this] { view_->beginUpdate(); } );
        connect( services_, &ServiceListModel::sigBatchFinished, this, [this] { view_->endUpdate(); } );
    }
    configureGrid();
    Layout->addWidget( grid_, 1 );
    grid_->installEventFilter( this );
    grid_->viewport()->installEventFilter( this );
    connect( view_, &Qtitan::GridViewBase::focusRowChanged, this, [this] { emit sigCursorChanged(); } );
    connect( view_, &Qtitan::GridViewBase::selectionChanged, this, [this] { emit sigSummaryChanged(); } );
    connect( view_, &Qtitan::GridViewBase::contextMenu, this, [this]( Qtitan::ContextMenuEventArgs* Args ) {
        const auto Type = Args->hitInfo().info();
        if( Type != Qtitan::GridHitInfo::Cell && Type != Qtitan::GridHitInfo::Rows )
            return;
        Args->setHandled( true );
        QTimer::singleShot( 0, this, [this] { ShowContextMenu( QCursor::pos() ); } );
    } );
    connect( view_, &Qtitan::GridViewBase::rowDblClicked, this, [this] {
        if( auto* B = Bridge() ) B->ExecuteCommand( kind_ == Kind::Processes ? "hc.process.properties" : "hc.service.properties" );
    } );

    if( kind_ == Kind::Processes )
    {
        collector_ = std::make_shared<hc::system::ProcessCollector>();
        timer_.setInterval( settings_.refreshMs );
        connect( &timer_, &QTimer::timeout, this, &CSystemView::sample );
        timer_.start();
        QTimer::singleShot( 0, this, &CSystemView::sample );
        servicesTimer_.setInterval( 5000 );
        const auto LoadServices = [this] {
            hc::sys::RunAsync( this,
                               [] {
                                   std::vector<hc::system::ServiceStatusInfo> List;
                                   hc::system::EnumerateServices( false, List );
                                   QHash<quint32, QStringList> Result;
                                   for( const auto& Item : List )
                                       if( Item.pid ) Result[Item.pid].append( QString::fromStdWString( Item.name ) );
                                   return Result;
                               },
                               [this]( const QHash<quint32, QStringList>& Map ) { processes_->SetServices( Map ); } );
        };
        connect( &servicesTimer_, &QTimer::timeout, this, LoadServices );
        servicesTimer_.start();
        LoadServices();
    }
    else
    {
        monitor_ = std::make_unique<hc::system::ServiceMonitor>();
        startMonitor();
        timer_.setInterval( 1000 );
        connect( &timer_, &QTimer::timeout, this, &CSystemView::updateNotice );
        timer_.start();
    }
    // 관리자 도우미가 연결되면 표준 권한으로 읽지 못한 정보를 다시 읽는다(자동·수동 시작 모두).
    connect( &hc::sys::AdminEvents(), &hc::sys::AdminSessionEvents::sigConnected, this, [this] {
        if( processes_ ) processes_->RequeryRestricted();
        if( services_ ) services_->ReloadMissingConfig();
        updateNotice();
    } );
    if( settings_.autoStartHelper )
        QTimer::singleShot( 0, this, [] { hc::sys::EnsureAdminHelper(); } );
    updateNotice();
    updateBar();
}

CSystemView::~CSystemView()
{
    *alive_ = false;
    if( monitor_ ) monitor_->Stop();
}

void CSystemView::CloseView()
{
    timer_.stop();
    servicesTimer_.stop();
    if( monitor_ ) monitor_->Stop();
}

void CSystemView::buildProcessBar( QWidget* Bar )
{
    auto* Layout = new QHBoxLayout( Bar );
    Layout->setContentsMargins( 6, 0, 6, 0 );
    Layout->setSpacing( 6 );
    summary_ = new QLabel( Bar );
    summary_->setObjectName( "hc.system.summary" );
    summary_->setTextFormat( Qt::PlainText );
    mode_ = new fm::ui::SegmentedControl( { tr( "트리" ), tr( "목록" ) }, Bar );
    mode_->setObjectName( "hc.system.mode" );
    mode_->setSegmentSize( fm::ui::SegmentedControl::Compact );
    mode_->setCurrentIndex( 0 );
    mode_->setItemToolTips( { tr( "부모-자식 트리" ), tr( "평면 목록" ) } );
    connect( mode_, &fm::ui::SegmentedControl::currentIndexChanged, this, [this]( int Index ) { SetFlat( Index == 1 ); } );
    interval_ = new QComboBox( Bar );
    interval_->setObjectName( "hc.system.interval" );
    for( const int Ms : { 500, 1000, 2000, 5000 } )
        interval_->addItem( Ms < 1000 ? tr( "%1초" ).arg( Ms / 1000.0 ) : tr( "%1초" ).arg( Ms / 1000 ), Ms );
    interval_->setCurrentIndex( std::max( 0, interval_->findData( settings_.refreshMs ) ) );
    interval_->setToolTip( tr( "갱신 주기 · 설정 → 프로세스 · 서비스에서 기본값을 바꿉니다." ) );
    connect( interval_, &QComboBox::currentIndexChanged, this, [this] { timer_.setInterval( interval_->currentData().toInt() ); } );
    pause_ = new QToolButton( Bar );
    pause_->setObjectName( "hc.system.pause" );
    pause_->setText( tr( "일시 정지" ) );
    pause_->setCheckable( true );
    pause_->setToolTip( tr( "갱신 일시 정지·재개 (Pause)" ) );
    connect( pause_, &QToolButton::toggled, this, [this]( bool Checked ) { if( Checked != paused_ ) TogglePause(); } );
    Layout->addWidget( summary_, 1 );
    Layout->addWidget( mode_ );
    Layout->addWidget( interval_ );
    Layout->addWidget( pause_ );
}

void CSystemView::buildServiceBar( QWidget* Bar )
{
    auto* Layout = new QHBoxLayout( Bar );
    Layout->setContentsMargins( 6, 0, 6, 0 );
    Layout->setSpacing( 6 );
    stateFilter_ = new fm::ui::SegmentedControl( { tr( "전체" ), tr( "실행 중" ), tr( "중지됨" ), tr( "오류 중지" ), tr( "사용 안 함" ) }, Bar );
    stateFilter_->setObjectName( "hc.system.stateFilter" );
    stateFilter_->setSegmentSize( fm::ui::SegmentedControl::Compact );
    stateFilter_->setCurrentIndex( 0 );
    stateFilter_->setItemToolTips( { tr( "모든 서비스" ), tr( "실행 중인 서비스" ), tr( "중지된 서비스" ),
                                      tr( "시작 유형이 자동인데 오류 코드와 함께 중지된 서비스(트리거·지연 시작 제외 설정 반영)" ), tr( "사용 안 함으로 설정된 서비스" ) } );
    connect( stateFilter_, &fm::ui::SegmentedControl::currentIndexChanged, this, [this]( int Index ) {
        serviceProxy_->SetFilter( ServiceListModel::Filter( std::clamp( Index, 0, 4 ) ) );
        updateBar();
    } );
    drivers_ = new QCheckBox( tr( "드라이버" ), Bar );
    drivers_->setObjectName( "hc.system.drivers" );
    drivers_->setToolTip( tr( "커널·파일 시스템 드라이버를 함께 표시합니다." ) );
    connect( drivers_, &QCheckBox::toggled, this, [this]( bool Checked ) { SetIncludeDrivers( Checked ); } );
    summary_ = new QLabel( Bar );
    summary_->setObjectName( "hc.system.summary" );
    Layout->addWidget( stateFilter_ );
    Layout->addWidget( drivers_ );
    Layout->addWidget( summary_, 1 );
    for( const auto& [Id, Label] : { std::pair{ "hc.service.start", tr( "시작" ) }, std::pair{ "hc.service.stop", tr( "중지" ) }, std::pair{ "hc.service.restart", tr( "다시 시작" ) } } )
    {
        auto* Button = new QToolButton( Bar );
        Button->setObjectName( QString( "hc.system.button." ) + Id );
        Button->setText( Label );
        connect( Button, &QToolButton::clicked, this, [Id = QString( Id )] { if( auto* B = Bridge() ) B->ExecuteCommand( Id ); } );
        Layout->addWidget( Button );
    }
}

void CSystemView::configureGrid()
{
    view_->beginUpdate();
    auto& Options = view_->options();
    Options.setGroupsHeader( false );
    Options.setScrollBarsTransparent( true );
    Options.setAutoCreateColumns( false );
    Options.setColumnMovingEnabled( true );
    Options.setAlternatingRowColors( false );
    Options.setGridLines( Qtitan::LinesNone );
    Options.setModelDecoration( true );
    Options.setRowAutoHeight( false );
    Options.setFocusFrameEnabled( false );
    Options.setShowFocusDecoration( true );
    Options.setKeepFocusedRow( true );
    Options.setFocusFollowsMouse( false );
    Options.setDragEnabled( false );
    Options.setDropEnabled( false );
    Options.setRubberBandSelection( false );
    Options.setShowWaitCursor( false );
    const auto& Colors = fm::style::ThemeManager::instance().colors();
    Options.setBackgroundColor( Colors[fm::style::Token::Surface] );
    Options.setSelectedBackgroundColor( Colors[fm::style::Token::Sel] );
    Options.setRowFocusColor( Colors[fm::style::Token::InvCur] );
    // 파일 패널처럼 왼쪽 세로 머리글(빠른 열 구성·행 선택 칸)을 두지 않는다.
    view_->tableOptions().setColumnsQuickCustomization( false );
    view_->tableOptions().setColumnsQuickMenuVisible( false );
    view_->tableOptions().setFrozenPlaceQuickSelection( false );
    view_->tableOptions().setRowFrozenButtonVisible( false );
    view_->tableOptions().setRowsQuickSelection( false );
    // 파일 목록과 같은 커서·표시 방식. 커서가 움직여도(거르기·정렬) 찾기 상자의 키보드 포커스를 빼앗지 않는다
    // (Qtitan HC-QTN-011은 파일 관리자 방식에서만 외부 포커스를 지킨다). 표시는 Space·Insert·Ctrl+클릭.
    view_->setFileManagerMode( true );
    applyCursorColors();
    for( const auto& Action : view_->actions() )
    {
        ( void )Action->disconnect();
        Action->setShortcut( {} );
    }
    struct Spec { int column; int width; bool visible; };
    QVector<Spec> Specs;
    const int Role = kind_ == Kind::Processes ? int( ProcessTreeModel::SortRole ) : int( ServiceListModel::SortRole );
    const auto* Model = kind_ == Kind::Processes ? static_cast<QAbstractItemModel*>( processes_ ) : static_cast<QAbstractItemModel*>( services_ );
    if( kind_ == Kind::Processes )
        Specs = { { ProcessTreeModel::Name, 260, true },       { ProcessTreeModel::Pid, 64, true },          { ProcessTreeModel::Cpu, 60, true },
                  { ProcessTreeModel::PrivateBytes, 92, true }, { ProcessTreeModel::WorkingSet, 92, true },   { ProcessTreeModel::User, 140, true },
                  { ProcessTreeModel::Description, 260, true }, { ProcessTreeModel::Company, 170, false },    { ProcessTreeModel::Threads, 64, false },
                  { ProcessTreeModel::Handles, 72, false },     { ProcessTreeModel::Session, 56, false },     { ProcessTreeModel::Priority, 84, false },
                  { ProcessTreeModel::Integrity, 72, false },   { ProcessTreeModel::CommandLine, 320, false }, { ProcessTreeModel::ImagePath, 300, false },
                  { ProcessTreeModel::Io, 90, false } };
    else
        Specs = { { ServiceListModel::Name, 170, true },     { ServiceListModel::DisplayName, 250, true }, { ServiceListModel::State, 140, true },
                  { ServiceListModel::StartType, 150, true }, { ServiceListModel::Account, 130, true },     { ServiceListModel::Pid, 64, true },
                  { ServiceListModel::Description, 320, true }, { ServiceListModel::Type, 200, false },    { ServiceListModel::ImagePath, 320, false } };
    for( const auto& Item : Specs )
    {
        auto* Column = static_cast<Qtitan::GridTableColumn*>( view_->addColumn( Item.column, Model->headerData( Item.column, Qt::Horizontal, Qt::DisplayRole ).toString() ) );
        Column->setWidth( Item.width );
        Column->setVisible( Item.visible );
        const auto Alignment = Qt::Alignment( Model->index( 0, Item.column ).data( Qt::TextAlignmentRole ).toInt() );
        const bool Right = ( kind_ == Kind::Processes && ( Item.column == ProcessTreeModel::Pid || Item.column == ProcessTreeModel::Cpu || Item.column == ProcessTreeModel::PrivateBytes
                                                           || Item.column == ProcessTreeModel::WorkingSet || Item.column == ProcessTreeModel::Threads
                                                           || Item.column == ProcessTreeModel::Handles || Item.column == ProcessTreeModel::Session || Item.column == ProcessTreeModel::Io ) )
                           || ( kind_ == Kind::Services && Item.column == ServiceListModel::Pid );
        Q_UNUSED( Alignment );
        Column->setTextAlignment( ( Right ? Qt::AlignRight : Qt::AlignLeft ) | Qt::AlignVCenter );
        Column->dataBinding()->setSortRole( Qt::ItemDataRole( Role ) );
    }
    if( kind_ == Kind::Services )
        if( auto* Name = view_->getColumnByModelColumn( ServiceListModel::Name ) )
            static_cast<Qtitan::GridTableColumn*>( Name )->setSortOrder( Qtitan::SortAscending );
    view_->endUpdate();
}

void CSystemView::applyCursorColors()
{
    using fm::style::Token;
    const auto& Colors = fm::style::ThemeManager::instance().colors();
    auto& Manager = view_->fileManagerOptions();
    Manager.cursorColor = Colors[Token::InvCur];
    Manager.cursorTextColor = Colors[Token::OnInvCur];
    Manager.selectedColor = Colors[Token::InvSel];
    Manager.selectedTextColor = Colors[Token::OnInvSel];
    Manager.backgroundColor = QColor();          // 분류 바탕색(모델 BackgroundRole)을 그대로 쓴다
    Manager.alternateBackgroundColor = QColor();
}

void CSystemView::ApplyColorScheme( const TyColorScheme& )
{
    applyCursorColors();
    const auto& Colors = fm::style::ThemeManager::instance().colors();
    view_->options().setBackgroundColor( Colors[fm::style::Token::Surface] );
    view_->options().setSelectedBackgroundColor( Colors[fm::style::Token::Sel] );
    view_->options().setRowFocusColor( Colors[fm::style::Token::InvCur] );
    if( processes_ ) processes_->RefreshColors();
    grid_->viewport()->update();
}

void CSystemView::EnsureKeyboardFocusOnView() const
{
    grid_->viewport()->setFocus( Qt::OtherFocusReason );
    if( view_->focusedRowIndex() < 0 && view_->getRowCount() > 0 )
        view_->setFocusedRowIndex( 0 );
}

void CSystemView::SetPanelActive( bool Active )
{
    panelActive_ = Active;
    // 파일 목록처럼 활성 패널에서만 커서를 칠한다.
    if( view_->options().showFocusDecoration() != Active )
        view_->options().setShowFocusDecoration( Active );
    grid_->viewport()->update();
}

void CSystemView::SetShown( bool Shown )
{
    if( shown_ == Shown )
        return;
    shown_ = Shown;
    if( kind_ == Kind::Services && monitor_ )
    {
        if( !Shown && settings_.pauseWhenHidden ) monitor_->Stop();
        else if( Shown && !paused_ ) startMonitor();
    }
    if( Shown && kind_ == Kind::Processes )
    {
        // 숨겨져 있던 동안의 평균이 아니라 새 기준점에서 다시 잰다.
        resetPending_ = true;
        QTimer::singleShot( 0, this, &CSystemView::sample );
    }
}

void CSystemView::ApplySettings()
{
    settings_ = hc::sys::Settings::Load();
    if( processes_ )
    {
        processes_->SetSettings( settings_ );
        processProxy_->SetOwnOnly( !settings_.allUsers );
        if( branchStyle_ && branchStyle_->lines_ != settings_.treeLines )
        {
            branchStyle_->lines_ = settings_.treeLines;
            grid_->viewport()->update();
        }
    }
    if( services_ ) services_->SetSettings( settings_ );
    if( interval_ ) interval_->setCurrentIndex( std::max( 0, interval_->findData( settings_.refreshMs ) ) );
    updateBar();
}

void CSystemView::sample()
{
    if( kind_ != Kind::Processes || sampling_ || paused_ || ( !shown_ && settings_.pauseWhenHidden ) || !collector_ )
        return;
    sampling_ = true;
    if( std::exchange( resetPending_, false ) ) collector_->Reset();
    collector_->SetMethod( settings_.cycleCpu ? hc::system::CpuMethod::Auto : hc::system::CpuMethod::Time );
    // 작업자는 수집기를 공유해 붙잡는다. 보기가 먼저 사라져도 작업자가 끝날 때까지 수집기는 살아 있다.
    hc::sys::RunAsync( this,
                       [Collector = collector_] {
                           hc::system::Snapshot Value;
                           Collector->Sample( Value );
                           return Value;
                       },
                       [this]( const hc::system::Snapshot& Value ) {
                           sampling_ = false;
                           if( Value.processes.empty() )
                               return;
                           processes_->Apply( Value );
                           if( !expandedOnce_ && processes_->ProcessCount() > 0 )
                           {
                               view_->expandAll();
                               expandedOnce_ = true;
                               if( view_->focusedRowIndex() < 0 && view_->getRowCount() > 0 )
                                   view_->setFocusedRowIndex( 0 );
                           }
                           updateBar();
                           updateNotice();
                           emit sigSummaryChanged();
                       } );
}

void CSystemView::updateNotice()
{
    const bool Show = !hc::permissions::Elevated() && !hc::sys::AdminSessionConnected();
    if( Show )
        notice_->setText( kind_ == Kind::Processes ? tr( "표준 권한으로 실행 중입니다. 관리자 프로세스의 상세 정보와 제어는 관리자 도우미가 권한을 요청합니다." )
                                                   : tr( "표준 권한으로 실행 중입니다. 시작·중지·설정 변경은 실행할 때 관리자 도우미가 권한을 요청합니다." ) );
    noticeRow_->setVisible( Show );
}

void CSystemView::updateBar()
{
    if( kind_ == Kind::Processes )
    {
        const auto& Totals = processes_->Totals();
        auto Text = tr( "프로세스 %1 · 스레드 %2 · 핸들 %3" ).arg( processes_->ProcessCount() ).arg( QLocale().toString( Totals.threadCount ) ).arg( QLocale().toString( Totals.handleCount ) );
        if( Totals.cpu >= 0 ) Text += tr( " · CPU %1 %" ).arg( Totals.cpu, 0, 'f', 0 );
        if( Totals.physicalTotal ) Text += tr( " · 메모리 %1 / %2" ).arg( hc::sys::FormatBytes( Totals.physicalUsed ), hc::sys::FormatBytes( Totals.physicalTotal ) );
        summary_->setText( Text );
        if( searchRow_ )
        {
            searchRow_->setVisible( !processProxy_->Text().isEmpty() );
            searchStatus_->setText( tr( "일치 %1 / %2" ).arg( processProxy_->MatchCount() ).arg( processes_->ProcessCount() ) );
        }
        if( pause_ ) { QSignalBlocker Block( pause_ ); pause_->setChecked( paused_ ); }
        return;
    }
    using F = ServiceListModel::Filter;
    const QStringList Labels{ tr( "전체 %1" ).arg( services_->Count( F::All ) ), tr( "실행 중 %1" ).arg( services_->Count( F::Running ) ),
                              tr( "중지됨 %1" ).arg( services_->Count( F::Stopped ) ), tr( "오류 중지 %1" ).arg( services_->Count( F::Problem ) ),
                              tr( "사용 안 함 %1" ).arg( services_->Count( F::Disabled ) ) };
    if( Labels != stateLabels_ )
    {
        stateLabels_ = Labels;
        stateFilter_->setItems( Labels );
    }
    summary_->setText( serviceProxy_->Text().isEmpty() ? QString{} : tr( "일치 %1" ).arg( serviceProxy_->rowCount() ) );
}

void CSystemView::SetSearch( const QString& Text )
{
    if( processProxy_ )
    {
        processProxy_->SetText( Text );
        view_->expandAll();
    }
    if( serviceProxy_ ) serviceProxy_->SetText( Text );
    updateBar();
    emit sigSummaryChanged();
}

QString CSystemView::Search() const
{
    return processProxy_ ? processProxy_->Text() : serviceProxy_ ? serviceProxy_->Text() : QString{};
}

void CSystemView::SetFlat( bool Flat )
{
    if( !processes_ || processes_->Flat() == Flat )
        return;
    const auto Current = CurrentProcess();
    processes_->SetFlat( Flat );
    if( mode_ ) { QSignalBlocker Block( mode_ ); mode_->setCurrentIndex( Flat ? 1 : 0 ); }
    view_->expandAll();
    if( Current ) SelectProcess( Current->key.pid );
    emit sigSummaryChanged();
}

bool CSystemView::Flat() const { return processes_ && processes_->Flat(); }

void CSystemView::SetIncludeDrivers( bool Include )
{
    includeDrivers_ = Include;
    if( drivers_ ) { QSignalBlocker Block( drivers_ ); drivers_->setChecked( Include ); }
    if( monitor_ ) monitor_->SetIncludeDrivers( Include );
    emit sigSummaryChanged();
}

void CSystemView::startMonitor()
{
    // 감시 스레드의 알림은 GUI 스레드로 옮겨 적용한다. 보기가 사라졌으면 버린다.
    const QPointer<CSystemView> Self( this );
    monitor_->Start( includeDrivers_, [Self]( std::vector<hc::system::ServiceStatusInfo> List ) {
        QMetaObject::invokeMethod( qApp, [Self, List = std::move( List )]() mutable {
            if( Self && Self->services_ ) Self->services_->Apply( std::move( List ) );
        }, Qt::QueuedConnection );
    } );
}

void CSystemView::TogglePause()
{
    paused_ = !paused_;
    if( !paused_ && collector_ )
    {
        resetPending_ = true;
        QTimer::singleShot( 0, this, &CSystemView::sample );
    }
    if( monitor_ )
    {
        if( paused_ ) monitor_->Stop();
        else startMonitor();
    }
    updateBar();
    emit sigSummaryChanged();
}

void CSystemView::Refresh()
{
    if( kind_ == Kind::Processes )
    {
        if( paused_ ) TogglePause();
        else sample();
    }
    else
    {
        if( monitor_ ) monitor_->RequestRefresh();
        services_->ReloadConfig();
    }
}

QModelIndex CSystemView::sourceIndex( const QModelIndex& ViewIndex ) const
{
    if( !ViewIndex.isValid() )
        return {};
    if( processProxy_ ) return processProxy_->mapToSource( ViewIndex );
    if( serviceProxy_ ) return serviceProxy_->mapToSource( ViewIndex );
    return ViewIndex;
}

QModelIndex CSystemView::focusedSource() const
{
    const auto Row = view_->focusedRow();
    return Row.isValid() ? sourceIndex( Row.modelIndex( 0 ) ) : QModelIndex{};
}

ProcessRef CSystemView::refOf( const hc::sys::ProcessNode& Node ) const
{
    ProcessRef Ref;
    Ref.key = Node.key;
    Ref.name = QString::fromStdWString( hc::system::ProcessDisplayName( Node.sample ) );
    Ref.image = Node.image;
    Ref.commandLine = Node.commandLine;
    Ref.parentPid = Node.sample.parentPid;
    Ref.suspended = Node.sample.suspended;
    Ref.critical = Node.critical;
    Ref.opened = Node.opened;
    Ref.priorityClass = Node.priorityClass;
    return Ref;
}

std::optional<ProcessRef> CSystemView::CurrentProcess() const
{
    if( !processes_ )
        return {};
    const auto* Node = processes_->Node( focusedSource() );
    if( !Node || Node->pseudo )
        return {};
    return refOf( *Node );
}

QVector<ProcessRef> CSystemView::SelectedProcesses() const
{
    QVector<ProcessRef> Result;
    if( !processes_ )
        return Result;
    auto* Selection = view_->selection();
    for( const auto& Index : Selection ? Selection->selectedRowIndexes() : QModelIndexList{} )
        if( const auto* Node = processes_->Node( sourceIndex( Index ) ); Node && !Node->pseudo )
            Result.append( refOf( *Node ) );
    if( Result.isEmpty() )
        if( const auto Current = CurrentProcess() ) Result.append( *Current );
    return Result;
}

QVector<ProcessRef> CSystemView::TreeOf( const hc::system::ProcessKey& Root ) const
{
    QVector<ProcessRef> Result;
    if( processes_ )
        for( const auto* Node : processes_->Descendants( Root ) )
            Result.append( refOf( *Node ) );
    return Result;
}

QString CSystemView::CurrentService() const
{
    if( !services_ )
        return {};
    return focusedSource().data( ServiceListModel::NameRole ).toString();
}

QStringList CSystemView::SelectedServices() const
{
    QStringList Result;
    if( !services_ )
        return Result;
    auto* Selection = view_->selection();
    for( const auto& Index : Selection ? Selection->selectedRowIndexes() : QModelIndexList{} )
        Result.append( sourceIndex( Index ).data( ServiceListModel::NameRole ).toString() );
    if( Result.isEmpty() && !CurrentService().isEmpty() )
        Result.append( CurrentService() );
    return Result;
}

const hc::sys::ServiceRow* CSystemView::ServiceRow( const QString& Name ) const
{
    return services_ ? services_->Row( services_->IndexOfName( Name ) ) : nullptr;
}

void CSystemView::SelectProcess( quint32 Pid )
{
    if( !processes_ )
        return;
    std::function<QModelIndex( const QModelIndex& )> Find = [&]( const QModelIndex& Parent ) -> QModelIndex {
        for( int Row = 0; Row < processes_->rowCount( Parent ); ++Row )
        {
            const auto Index = processes_->index( Row, 0, Parent );
            if( Index.data( ProcessTreeModel::PidRole ).toULongLong() == Pid ) return Index;
            if( const auto Child = Find( Index ); Child.isValid() ) return Child;
        }
        return {};
    };
    const auto Proxy = processProxy_->mapFromSource( Find( {} ) );
    if( !Proxy.isValid() )
        return;
    for( auto Parent = Proxy.parent(); Parent.isValid(); Parent = Parent.parent() )
        view_->expand( Parent );
    const auto Row = view_->getRow( Proxy );
    if( Row.isValid() )
    {
        view_->setFocusedRowIndex( Row.rowIndex() );
        view_->scrollToIndex( Proxy, false );
    }
}

void CSystemView::SelectService( const QString& Name )
{
    if( !services_ )
        return;
    const auto Proxy = serviceProxy_->mapFromSource( services_->index( services_->IndexOfName( Name ), 0 ) );
    const auto Row = view_->getRow( Proxy );
    if( Row.isValid() )
    {
        view_->setFocusedRowIndex( Row.rowIndex() );
        view_->scrollToIndex( Proxy, false );
    }
}

void CSystemView::SelectServiceByPid( quint32 Pid )
{
    if( !services_ )
        return;
    for( int Row = 0; Row < services_->rowCount(); ++Row )
        if( services_->Row( Row )->status.pid == Pid )
        {
            SelectService( QString::fromStdWString( services_->Row( Row )->status.name ) );
            return;
        }
}

QPoint CSystemView::CursorPoint() const
{
    return grid_->viewport()->mapToGlobal( QPoint( 48, std::min( grid_->viewport()->height() / 2, 80 ) ) );
}

bool CSystemView::HasDenied() const { return false; }

int CSystemView::VisibleRowCount() const { return view_->getRowCount(); }

bool CSystemView::CursorExpanded() const
{
    const auto Row = view_->focusedRow();
    return Row.isValid() && view_->modelController()->isRowExpanded( Row );
}

void CSystemView::ShowContextMenu( QPoint Global )
{
    auto* B = Bridge();
    if( !B )
        return;
    QMenu Menu( this );
    Menu.setObjectName( "hc.system.contextMenu" );
    const auto Add = [&]( QMenu* Target, const char* Id ) { if( auto* Action = B->Action( Id ) ) Target->addAction( Action ); };
    if( kind_ == Kind::Processes )
    {
        const auto Current = CurrentProcess();
        Add( &Menu, "hc.process.properties" );
        Add( &Menu, "hc.process.openLocation" );
        Add( &Menu, "hc.process.openLocationNewTab" );
        Add( &Menu, "hc.system.followLocation" );
        Add( &Menu, "hc.process.switchToWindow" );
        Menu.addSeparator();
        Add( &Menu, "hc.process.suspendToggle" );
        auto* Priority = Menu.addMenu( tr( "우선순위(&P)" ) );
        Priority->setObjectName( "hc.system.priorityMenu" );
        for( const auto Class : { REALTIME_PRIORITY_CLASS, HIGH_PRIORITY_CLASS, ABOVE_NORMAL_PRIORITY_CLASS, NORMAL_PRIORITY_CLASS, BELOW_NORMAL_PRIORITY_CLASS, IDLE_PRIORITY_CLASS } )
        {
            auto* Item = Priority->addAction( hc::sys::PriorityText( Class ) );
            Item->setCheckable( true );
            Item->setChecked( Current && Current->priorityClass == Class );
            connect( Item, &QAction::triggered, this, [B, Class] { B->ExecuteSystemCommand( "hc.process.priority", int( Class ) ); } );
        }
        Add( &Menu, "hc.process.efficiencyMode" );
        Menu.addSeparator();
        Add( &Menu, "hc.process.terminate" );
        Add( &Menu, "hc.process.terminateTree" );
        Add( &Menu, "hc.process.restart" );
        Menu.addSeparator();
        auto* Copy = Menu.addMenu( tr( "복사(&C)" ) );
        for( const auto& [Field, Label] : { std::pair{ "name", tr( "이름" ) }, std::pair{ "pid", tr( "PID" ) }, std::pair{ "commandLine", tr( "명령줄" ) },
                                            std::pair{ "image", tr( "이미지 경로" ) }, std::pair{ "row", tr( "행 전체" ) } } )
            connect( Copy->addAction( Label ), &QAction::triggered, this, [B, Field = QString( Field )] { B->ExecuteSystemCommand( "hc.process.copy", Field ); } );
        Add( &Menu, "hc.process.showServices" );
        Add( &Menu, "hc.process.searchOnline" );
    }
    else
    {
        Add( &Menu, "hc.service.properties" );
        Add( &Menu, "hc.service.openLocation" );
        Add( &Menu, "hc.service.openLocationNewTab" );
        Add( &Menu, "hc.system.followLocation" );
        Menu.addSeparator();
        Add( &Menu, "hc.service.start" );
        Add( &Menu, "hc.service.stop" );
        Add( &Menu, "hc.service.restart" );
        Add( &Menu, "hc.service.pauseToggle" );
        auto* Start = Menu.addMenu( tr( "시작 유형(&T)" ) );
        for( const auto& [Type, Label] : { std::pair{ "auto", tr( "자동" ) }, std::pair{ "autoDelayed", tr( "자동(지연된 시작)" ) }, std::pair{ "manual", tr( "수동" ) },
                                           std::pair{ "disabled", tr( "사용 안 함" ) } } )
            connect( Start->addAction( Label ), &QAction::triggered, this, [B, Type = QString( Type )] { B->ExecuteSystemCommand( "hc.service.startType", Type ); } );
        Menu.addSeparator();
        Add( &Menu, "hc.service.showProcess" );
        auto* Copy = Menu.addMenu( tr( "복사(&C)" ) );
        for( const auto& [Field, Label] : { std::pair{ "name", tr( "서비스 이름" ) }, std::pair{ "displayName", tr( "표시 이름" ) }, std::pair{ "image", tr( "실행 파일" ) },
                                            std::pair{ "row", tr( "행 전체" ) } } )
            connect( Copy->addAction( Label ), &QAction::triggered, this, [B, Field = QString( Field )] { B->ExecuteSystemCommand( "hc.service.copy", Field ); } );
        Add( &Menu, "hc.service.toggleDrivers" );
    }
    Menu.exec( Global.isNull() ? CursorPoint() : Global );
}

QJsonObject CSystemView::CaptureState() const
{
    QJsonObject State{ { "kind", KindKey( kind_ ) } };
    if( processes_ )
    {
        State["flat"] = processes_->Flat();
        State["searchScopes"] = processProxy_->Scopes();
        State["keepAncestors"] = processProxy_->KeepAncestors();
    }
    else
    {
        State["drivers"] = includeDrivers_;
        State["stateFilter"] = stateFilter_ ? stateFilter_->currentIndex() : 0;
    }
    QJsonArray Columns;
    for( int Index = 0; Index < view_->getColumnCount(); ++Index )
    {
        auto* Column = static_cast<Qtitan::GridTableColumn*>( view_->getColumn( Index ) );
        Columns.append( QJsonObject{ { "column", Column->dataBinding()->column() }, { "width", Column->width() }, { "visible", Column->isVisible() },
                                     { "order", Column->visualIndex() }, { "sort", int( Column->sortOrder() ) } } );
    }
    State["columns"] = Columns;
    return State;
}

void CSystemView::RestoreState( const QJsonObject& State )
{
    if( processes_ )
    {
        if( State.contains( "flat" ) ) SetFlat( State["flat"].toBool() );
        if( State.contains( "searchScopes" ) ) searchScope_->setCurrentIndex( std::max( 0, searchScope_->findData( State["searchScopes"].toInt() ) ) );
        if( State.contains( "keepAncestors" ) ) keepAncestors_->setChecked( State["keepAncestors"].toBool( true ) );
    }
    else
    {
        if( State.contains( "drivers" ) ) SetIncludeDrivers( State["drivers"].toBool() );
        if( State.contains( "stateFilter" ) && stateFilter_ ) stateFilter_->setCurrentIndex( std::clamp( State["stateFilter"].toInt(), 0, 4 ) );
    }
    view_->beginUpdate();
    for( const auto& Value : State["columns"].toArray() )
    {
        const auto Item = Value.toObject();
        auto* Column = static_cast<Qtitan::GridTableColumn*>( view_->getColumnByModelColumn( Item["column"].toInt( -1 ) ) );
        if( !Column ) continue;
        if( Item["width"].toInt() >= 24 ) Column->setWidth( std::min( Item["width"].toInt(), 2000 ) );
        Column->setVisible( Item["visible"].toBool( true ) );
        const int Sort = Item["sort"].toInt();
        if( Sort >= 0 && Sort <= 2 ) Column->setSortOrder( Qtitan::SortOrder( Sort ) );
    }
    view_->endUpdate();
}

QString CSystemView::PathText() const
{
    if( kind_ == Kind::Processes )
    {
        auto Text = tr( "프로세스 — %1 · %2" ).arg( settings_.allUsers ? tr( "모든 사용자" ) : tr( "현재 사용자" ), Flat() ? tr( "목록" ) : tr( "트리" ) );
        if( !Search().isEmpty() ) Text += tr( " · 찾기 “%1”" ).arg( Search() );
        return Text;
    }
    auto Text = includeDrivers_ ? tr( "서비스 — Win32 서비스·드라이버" ) : tr( "서비스 — Win32 서비스" );
    if( !Search().isEmpty() ) Text += tr( " · 찾기 “%1”" ).arg( Search() );
    return Text;
}

QString CSystemView::SummaryText() const
{
    if( kind_ == Kind::Processes )
    {
        const auto& Totals = processes_->Totals();
        return Totals.cpu >= 0 ? tr( "프로세스 %1 · CPU %2 %" ).arg( processes_->ProcessCount() ).arg( Totals.cpu, 0, 'f', 0 ) : tr( "프로세스 %1" ).arg( processes_->ProcessCount() );
    }
    return tr( "서비스 %1 · 실행 중 %2" ).arg( services_->Count( ServiceListModel::Filter::All ) ).arg( services_->Count( ServiceListModel::Filter::Running ) );
}

QString CSystemView::StatusLeft() const
{
    if( kind_ == Kind::Processes )
    {
        const auto Current = CurrentProcess();
        const auto Selected = view_->selection() ? view_->selection()->selectedRowIndexes().size() : 0;
        return tr( "선택 %1 / %2" ).arg( Selected ).arg( processes_->ProcessCount() )
               + ( Current ? tr( " · %1 (%2)" ).arg( Current->name ).arg( Current->key.pid ) : QString{} );
    }
    const auto Selected = view_->selection() ? view_->selection()->selectedRowIndexes().size() : 0;
    const auto Current = CurrentService();
    return tr( "선택 %1 / %2" ).arg( Selected ).arg( services_->rowCount() ) + ( Current.isEmpty() ? QString{} : " · " + Current );
}

QString CSystemView::StatusRight() const
{
    if( kind_ == Kind::Processes )
    {
        if( paused_ ) return tr( "갱신 일시 정지" );
        auto Text = tr( "갱신 %1" ).arg( interval_ ? interval_->currentText() : QString{} );
        if( !processes_->Totals().cycleBased && processes_->Totals().cpu >= 0 ) Text += tr( " · 시간 기반 CPU" );
        return Text;
    }
    return monitor_ && monitor_->NotificationsActive() ? tr( "상태 알림 구독 중" ) : tr( "주기 확인" );
}

bool CSystemView::handleKey( QKeyEvent* Event )
{
    const auto Modifiers = Event->modifiers() & ~Qt::KeypadModifier;
    const int Key = Event->key();
    if( ( Key == Qt::Key_Space || Key == Qt::Key_Insert ) && Modifiers == Qt::NoModifier )
    {
        const int Row = view_->focusedRowIndex();
        if( Row >= 0 )
        {
            view_->toggleRowSelection( Row );
            if( Key == Qt::Key_Insert && Row + 1 < view_->getRowCount() ) view_->setFocusedRowIndex( Row + 1 );
        }
        emit sigSummaryChanged();
        return true;
    }
    if( processes_ && Key == Qt::Key_Asterisk )
    {
        view_->expandAll();
        return true;
    }
    // 트리 펼치기·접기(+ - → ←). 표의 열 이동 대신 트리를 다룬다. 목록 보기에서는 자식이 없어 커서만 움직인다.
    const bool Plain = Modifiers == Qt::NoModifier || ( Key == Qt::Key_Plus && Modifiers == Qt::ShiftModifier );
    if( processes_ && Plain && ( Key == Qt::Key_Plus || Key == Qt::Key_Minus || Key == Qt::Key_Left || Key == Qt::Key_Right ) )
    {
        const auto Row = view_->focusedRow();
        if( !Row.isValid() )
            return true;
        auto* Controller = view_->modelController();
        const bool Children = Controller->hasChildren( Row );
        const bool Open = Children && Controller->isRowExpanded( Row );
        const auto Index = Row.modelIndex( 0 );
        const auto Focus = [this]( const QModelIndex& Target ) {
            const auto Next = view_->getRow( Target );
            if( !Next.isValid() )
                return;
            view_->setFocusedRowIndex( Next.rowIndex() );
            view_->scrollToIndex( Target, false );
        };
        if( Key == Qt::Key_Plus || ( Key == Qt::Key_Right && !Open ) )
        {
            if( Children && !Open ) view_->expandRow( Row );
        }
        else if( Key == Qt::Key_Minus || ( Key == Qt::Key_Left && Open ) )
        {
            if( Open ) view_->collapseRow( Row );
        }
        else if( Key == Qt::Key_Right )
            Focus( Index.model()->index( 0, 0, Index ) );
        else if( Index.parent().isValid() )
            Focus( Index.parent() );
        emit sigCursorChanged();
        return true;
    }
    // Esc는 미리 듣기·상세 보기를 하나씩 닫는다.
    if( Key == Qt::Key_Escape && Modifiers == Qt::NoModifier )
        if( auto* B = Bridge(); B && ( B->CancelAudioPreview() || ( B->System() && B->System()->CloseDetail() ) ) )
            return true;
    if( TyStCommandMgr::GetInstance()->ProcessScopedKeyPress( Event, Scope() ) )
        return true;
    if( ( Key == Qt::Key_Return || Key == Qt::Key_Enter ) && Modifiers == Qt::NoModifier )
    {
        if( auto* B = Bridge() ) B->ExecuteCommand( kind_ == Kind::Processes ? "hc.process.properties" : "hc.service.properties" );
        return true;
    }
    // 목록 이동 키는 표가 처리한다.
    switch( Key )
    {
        case Qt::Key_Up: case Qt::Key_Down: case Qt::Key_Left: case Qt::Key_Right:
        case Qt::Key_PageUp: case Qt::Key_PageDown: case Qt::Key_Home: case Qt::Key_End:
            if( !( Modifiers & ( Qt::AltModifier | Qt::MetaModifier ) ) ) return false;
            break;
        default: break;
    }
    const auto Id = hc::ui::ShortcutCommand( *Event, TyStCommandMgr::GetInstance()->Retrieve() );
    if( Id.isEmpty() )
        return false;
    if( hc::ui::IsSystemViewGlobalCommand( Id ) )
        TyStCommandMgr::GetInstance()->ProcessKeyPressEvent( Event, {} );
    // 파일 작업 키는 숨은 파일 목록에 닿지 않도록 여기서 끝낸다.
    return true;
}

bool CSystemView::eventFilter( QObject* Watched, QEvent* Event )
{
    // Ctrl+클릭은 커서를 옮기며 그 행의 표시를 바꾼다(파일 목록과 같다).
    if( Watched == grid_->viewport() && Event->type() == QEvent::MouseButtonPress )
    {
        const auto* Mouse = static_cast<QMouseEvent*>( Event );
        if( Mouse->button() == Qt::LeftButton && ( Mouse->modifiers() & ~Qt::KeypadModifier ) == Qt::ControlModifier )
        {
            const auto Hit = grid_->hitInfo( Mouse->position().toPoint() );
            if( Hit.info() == Qtitan::GridHitInfo::Cell )
                if( const auto Row = view_->getRow( Hit.modelIndex() ); Row.isValid() )
                {
                    view_->toggleRowSelection( Row.rowIndex() );
                    view_->setFocusedRowIndex( Row.rowIndex(), Qt::ControlModifier );
                    emit sigSummaryChanged();
                    Event->accept();
                    return true;
                }
        }
    }
    if( Event->type() == QEvent::ShortcutOverride )
    {
        auto* Key = static_cast<QKeyEvent*>( Event );
        const auto Scoped = hc::ui::ScopeShortcut( *Key, hc::settings::AppSettings().Read( hc::settings::Section::Shortcuts ).toObject(), Scope() );
        if( !Scoped.isEmpty() || Key->key() == Qt::Key_Space || Key->key() == Qt::Key_Insert )
        {
            Event->accept();
            return true;
        }
    }
    if( Event->type() == QEvent::KeyPress && handleKey( static_cast<QKeyEvent*>( Event ) ) )
    {
        Event->accept();
        return true;
    }
    return CViewT::eventFilter( Watched, Event );
}

//////////////////////////////////////////////////////////////////////////////
/// CSystemDetailView

CSystemDetailView::CSystemDetailView( QWidget* Parent ) : CViewT( Parent )
{
    viewMode_ = VM_DETAIL;
    setObjectName( "hc.system.detail" );
    auto* Layout = new QVBoxLayout( this );
    Layout->setContentsMargins( 8, 8, 8, 4 );
    auto* Header = new QHBoxLayout;
    icon_ = new QLabel( this );
    icon_->setFixedSize( 36, 36 );
    auto* Texts = new QVBoxLayout;
    title_ = new QLabel( this );
    title_->setObjectName( "hc.system.detail.title" );
    auto Font = title_->font();
    Font.setBold( true );
    Font.setPointSizeF( Font.pointSizeF() * 1.2 );
    title_->setFont( Font );
    subtitle_ = new QLabel( this );
    subtitle_->setWordWrap( true );
    badges_ = new QLabel( this );
    badges_->setObjectName( "hc.system.detail.badges" );
    badges_->setTextFormat( Qt::PlainText );
    Texts->addWidget( title_ );
    Texts->addWidget( subtitle_ );
    Texts->addWidget( badges_ );
    Header->addWidget( icon_, 0, Qt::AlignTop );
    Header->addLayout( Texts, 1 );
    Layout->addLayout( Header );
    tabs_ = new QTabWidget( this );
    tabs_->setObjectName( "hc.system.detail.tabs" );
    auto* Scroll = new QScrollArea( tabs_ );
    Scroll->setWidgetResizable( true );
    Scroll->setFrameShape( QFrame::NoFrame );
    auto* Page = new QWidget( Scroll );
    form_ = new QFormLayout( Page );
    form_->setLabelAlignment( Qt::AlignRight | Qt::AlignTop );
    form_->setFieldGrowthPolicy( QFormLayout::AllNonFixedFieldsGrow );
    Scroll->setWidget( Page );
    tabs_->addTab( Scroll, tr( "개요" ) );
    graphs_ = new QWidget( tabs_ );
    auto* GraphLayout = new QVBoxLayout( graphs_ );
    for( const auto& Title : { tr( "CPU" ), tr( "개인 바이트" ), tr( "I/O" ) } )
    {
        auto* Spark = new Sparkline( Title, graphs_ );
        Spark->setObjectName( QString( "hc.system.detail.spark%1" ).arg( GraphLayout->count() ) );
        GraphLayout->addWidget( Spark );
    }
    GraphLayout->addStretch( 1 );
    tabs_->addTab( graphs_, tr( "성능" ) );
    dependencies_ = new QTreeWidget( tabs_ );
    dependencies_->setObjectName( "hc.system.detail.dependencies" );
    dependencies_->setHeaderLabels( { tr( "구성 요소" ), tr( "상태" ) } );
    dependencies_->setColumnWidth( 0, 300 );
    tabs_->addTab( dependencies_, tr( "종속성" ) );
    Layout->addWidget( tabs_, 1 );
    actions_ = new QWidget( this );
    new QHBoxLayout( actions_ );
    actions_->layout()->setContentsMargins( 0, 0, 0, 0 );
    Layout->addWidget( actions_ );
    clear( tr( "왼쪽 패널의 프로세스·서비스 보기에서 항목을 고르세요." ) );
}

void CSystemDetailView::CloseView() { source_ = nullptr; }

void CSystemDetailView::SetSource( CSystemView* Source )
{
    if( source_ == Source )
        return;
    if( source_ ) disconnect( source_, nullptr, this, nullptr );
    source_ = Source;
    shownKey_.clear();
    if( Source )
    {
        connect( Source, &CSystemView::sigCursorChanged, this, &CSystemDetailView::Update );
        connect( Source, &CSystemView::sigSummaryChanged, this, &CSystemDetailView::Update );
    }
    // 각 보기 종류에 맞는 탭만 보인다.
    const bool Processes = Source && Source->ViewKind() == CSystemView::Kind::Processes;
    tabs_->setTabVisible( 1, Processes );
    tabs_->setTabVisible( 2, !Processes );
    Update();
}

void CSystemDetailView::clear( const QString& Message )
{
    icon_->clear();
    title_->setText( Message );
    subtitle_->clear();
    badges_->clear();
    while( form_->rowCount() ) form_->removeRow( 0 );
    dependencies_->clear();
    shownKey_.clear();
}

void CSystemDetailView::Update()
{
    if( !source_ ) { clear( tr( "시스템 보기가 닫혔습니다." ) ); return; }
    if( source_->ViewKind() == CSystemView::Kind::Processes )
    {
        const auto Current = source_->CurrentProcess();
        const auto* Node = Current ? source_->Processes()->Find( Current->key ) : nullptr;
        if( !Node ) { clear( tr( "프로세스를 고르세요." ) ); return; }
        showProcess( *Node );
    }
    else
    {
        const auto Name = source_->CurrentService();
        if( Name.isEmpty() ) { clear( tr( "서비스를 고르세요." ) ); return; }
        showService( Name );
    }
}

void CSystemDetailView::showProcess( const hc::sys::ProcessNode& Node )
{
    const auto Key = QString( "p:%1:%2" ).arg( Node.key.pid ).arg( Node.key.createTime );
    const auto& Colors = fm::style::ThemeManager::instance().colors();
    if( shownKey_ != Key )
    {
        shownKey_ = Key;
        while( form_->rowCount() ) form_->removeRow( 0 );
        const auto Add = [this]( const QString& Label, const QString& Value, bool Mono = false ) {
            auto* Text = new QLabel( Value.isEmpty() ? tr( "—" ) : Value );
            Text->setWordWrap( true );
            Text->setTextInteractionFlags( Qt::TextSelectableByMouse );
            if( Mono ) Text->setFont( QFontDatabase::systemFont( QFontDatabase::FixedFont ) );
            form_->addRow( Label, Text );
        };
        const auto* Parent = source_->Processes()->Find( { Node.sample.parentPid, 0 } );
        Q_UNUSED( Parent );
        Add( tr( "PID / 부모" ), tr( "%1 · 부모 %2" ).arg( Node.key.pid ).arg( Node.sample.parentPid ) );
        Add( tr( "사용자" ), Node.user.isEmpty() ? QString{} : tr( "%1 · 세션 %2 · 무결성 %3" ).arg( Node.user ).arg( Node.sample.sessionId ).arg( hc::sys::IntegrityText( Node.integrity ) ) );
        Add( tr( "시작 시각" ), hc::sys::FileTimeText( Node.key.createTime ) );
        Add( tr( "이미지 경로" ), Node.image, true );
        Add( tr( "명령줄" ), Node.commandLine, true );
        Add( tr( "회사·버전" ), Node.company + ( Node.version.isEmpty() ? QString{} : " · " + Node.version ) );
        Add( tr( "보호" ), Node.protectedProcess ? tr( "보호된 프로세스" ) : Node.critical ? tr( "중요 프로세스" ) : tr( "없음" ) );
        const auto Hosted = source_->Processes()->Services( Node.key.pid );
        if( !Hosted.isEmpty() ) Add( tr( "서비스" ), Hosted.join( ", " ) );
        if( Node.detailsLoaded && !Node.opened )
            Add( tr( "알림" ), tr( "표준 권한으로 이 프로세스의 상세 정보를 읽을 수 없습니다." ) );
        icon_->setPixmap( Node.icon.isNull() ? QPixmap() : Node.icon.pixmap( 32, 32 ) );
        // 서명 확인은 처음 볼 때 백그라운드에서 한 번 한다.
        if( !Node.image.isEmpty() && signatureFor_ != Node.image )
        {
            signatureFor_ = Node.image;
            signature_.clear();
            const QString Path = Node.image;
            hc::sys::RunAsync( this, [Path] { return hc::system::VerifySignature( Path.toStdWString() ); },
                               [this, Path]( const hc::system::SignatureInfo& Info ) {
                                   if( signatureFor_ != Path ) return;
                                   using hc::system::SignatureState;
                                   signature_ = Info.state == SignatureState::Signed ? tr( "✓ 서명 확인됨%1" ).arg( Info.signer.empty() ? QString{} : " · " + QString::fromStdWString( Info.signer ) )
                                              : Info.state == SignatureState::Unsigned ? tr( "서명 없음" )
                                              : Info.state == SignatureState::Invalid  ? tr( "⚠ 서명이 올바르지 않음" ) : QString{};
                                   shownKey_.clear();
                                   Update();
                               } );
        }
        // 동작 단추
        auto* Layout = actions_->layout();
        while( auto* Item = Layout->takeAt( 0 ) ) { delete Item->widget(); delete Item; }
        for( const auto& [Id, Label] : { std::pair{ "hc.process.openLocation", tr( "파일 위치 열기(F4)" ) }, std::pair{ "hc.process.suspendToggle", tr( "일시 중단·재개(F9)" ) },
                                         std::pair{ "hc.process.terminateTree", tr( "트리 끝내기" ) }, std::pair{ "hc.process.terminate", tr( "끝내기(F8)" ) } } )
        {
            auto* Button = new QPushButton( Label, actions_ );
            Button->setObjectName( QString( "hc.system.detail." ) + Id );
            connect( Button, &QPushButton::clicked, this, [this, Id = QString( Id )] {
                if( auto* B = Bridge(); B && source_ ) B->ExecuteSystemCommand( Id, {}, source_ );
            } );
            Layout->addWidget( Button );
        }
    }
    title_->setText( QString::fromStdWString( hc::system::ProcessDisplayName( Node.sample ) ) );
    subtitle_->setText( QStringList{ Node.description, Node.company }.filter( QRegularExpression( "\\S" ) ).join( " · " ) );
    QStringList Badges;
    if( !signature_.isEmpty() ) Badges << signature_;
    Badges << ( Node.wow64 ? tr( "32비트" ) : tr( "64비트" ) );
    if( Node.priorityClass ) Badges << tr( "%1 우선순위" ).arg( hc::sys::PriorityText( Node.priorityClass ) );
    if( Node.elevated ) Badges << tr( "관리자 권한" );
    if( Node.sample.suspended ) Badges << tr( "일시 중단됨" );
    uint32_t Windows = 0;
    if( hc::system::HasTopLevelWindows( Node.key.pid, &Windows ) ) Badges << tr( "창 %1개" ).arg( Windows );
    Badges << tr( "CPU %1 %" ).arg( Node.cpu >= 0 ? hc::sys::FormatCpu( Node.cpu ) : QStringLiteral( "—" ) );
    badges_->setText( Badges.join( "   " ) );
    QVector<Sparkline*> Sparks;
    for( int Index = 0; Index < 3; ++Index )
        if( auto* Spark = graphs_->findChild<QWidget*>( QString( "hc.system.detail.spark%1" ).arg( Index ) ) ) Sparks.append( static_cast<Sparkline*>( Spark ) );
    if( Sparks.size() == 3 )
    {
        Sparks[0]->SetValues( Node.cpuHistory, Node.cpu >= 0 ? hc::sys::FormatCpu( Node.cpu ) + " %" : QString{}, Colors[fm::style::Token::Accent] );
        Sparks[1]->SetValues( Node.privateHistory, hc::sys::FormatBytes( Node.sample.privateBytes ), Colors[fm::style::Token::KCode] );
        Sparks[2]->SetValues( Node.ioHistory, hc::sys::FormatRate( Node.io ), Colors[fm::style::Token::Warn] );
    }
}

void CSystemDetailView::showService( const QString& Name )
{
    const auto* Row = source_->ServiceRow( Name );
    if( !Row ) { clear( tr( "서비스를 찾을 수 없습니다." ) ); return; }
    const auto Key = "s:" + Name.toLower();
    const bool Changed = shownKey_ != Key;
    if( Changed )
    {
        shownKey_ = Key;
        configFor_.clear();
        const QString Service = Name;
        hc::sys::RunAsync( this,
                           [Service] {
                               std::pair<hc::system::ServiceConfig, std::vector<hc::system::ServiceStatusInfo>> Result;
                               hc::system::QueryServiceConfiguration( Service.toStdWString(), Result.first );
                               hc::system::QueryDependents( Service.toStdWString(), Result.second );
                               return Result;
                           },
                           [this, Service]( const std::pair<hc::system::ServiceConfig, std::vector<hc::system::ServiceStatusInfo>>& Result ) {
                               if( shownKey_ != "s:" + Service.toLower() ) return;
                               config_ = Result.first;
                               dependents_ = Result.second;
                               configFor_ = Service;
                               shownKey_.clear();
                               Update();
                           } );
        auto* Layout = actions_->layout();
        while( auto* Item = Layout->takeAt( 0 ) ) { delete Item->widget(); delete Item; }
        for( const auto& [Id, Label] : { std::pair{ "hc.service.properties", tr( "속성(F3)" ) }, std::pair{ "hc.service.openLocation", tr( "파일 위치(F4)" ) },
                                         std::pair{ "hc.service.restart", tr( "다시 시작(F6)" ) }, std::pair{ "hc.service.stop", tr( "중지(F8)" ) } } )
        {
            auto* Button = new QPushButton( Label, actions_ );
            Button->setObjectName( QString( "hc.system.detail." ) + Id );
            connect( Button, &QPushButton::clicked, this, [this, Id = QString( Id )] {
                if( auto* B = Bridge(); B && source_ ) B->ExecuteSystemCommand( Id, {}, source_ );
            } );
            Layout->addWidget( Button );
        }
    }
    title_->setText( QString::fromStdWString( Row->status.displayName ) );
    subtitle_->setText( Name + " · " + hc::sys::ServiceTypeText( Row->status.type ) );
    QStringList Badges{ hc::sys::ServiceStateText( Row->status.state ) };
    if( Row->configLoaded ) Badges << hc::sys::StartTypeText( Row->start, Row->trigger ) << hc::sys::AccountText( Row->account );
    badges_->setText( Badges.join( "   " ) );
    icon_->setPixmap( QApplication::style()->standardIcon( QStyle::SP_ComputerIcon ).pixmap( 32, 32 ) );
    if( !Changed && configFor_ != Name )
        return;
    while( form_->rowCount() ) form_->removeRow( 0 );
    const auto Add = [this]( const QString& Label, const QString& Value, bool Mono = false ) {
        auto* Text = new QLabel( Value.isEmpty() ? tr( "—" ) : Value );
        Text->setWordWrap( true );
        Text->setTextInteractionFlags( Qt::TextSelectableByMouse );
        if( Mono ) Text->setFont( QFontDatabase::systemFont( QFontDatabase::FixedFont ) );
        form_->addRow( Label, Text );
    };
    Add( tr( "프로세스" ), Row->status.pid ? tr( "PID %1 · 바로 가기 메뉴의 ‘프로세스 보기로 이동’" ).arg( Row->status.pid ) : QString{} );
    Add( tr( "실행 파일" ), QString::fromStdWString( config_.resolvedImage ), true );
    Add( tr( "명령줄" ), QString::fromStdWString( config_.binaryPath ), true );
    Add( tr( "설명" ), QString::fromStdWString( config_.description ) );
    Add( tr( "오류 제어" ), config_.errorControl == 0 ? tr( "무시" ) : config_.errorControl == 1 ? tr( "보통" ) : config_.errorControl == 2 ? tr( "심각" ) : tr( "매우 심각" ) );
    Add( tr( "마지막 종료 코드" ), QString::number( Row->status.win32ExitCode ) + ( Row->status.serviceExitCode ? tr( " · 서비스 코드 %1" ).arg( Row->status.serviceExitCode ) : QString{} ) );
    dependencies_->clear();
    auto* Needs = new QTreeWidgetItem( dependencies_, { tr( "이 서비스가 필요로 하는 구성 요소" ) } );
    for( const auto& Dependency : config_.dependencies )
    {
        const auto Text = QString::fromStdWString( Dependency );
        const auto* Other = source_->ServiceRow( Text.startsWith( '+' ) ? Text.mid( 1 ) : Text );
        new QTreeWidgetItem( Needs, { Text.startsWith( '+' ) ? tr( "%1 (그룹)" ).arg( Text.mid( 1 ) ) : Text, Other ? hc::sys::ServiceStateText( Other->status.state ) : QString{} } );
    }
    auto* Users = new QTreeWidgetItem( dependencies_, { tr( "이 서비스를 필요로 하는 구성 요소" ) } );
    for( const auto& Dependent : dependents_ )
        new QTreeWidgetItem( Users, { QString::fromStdWString( Dependent.name ) + " — " + QString::fromStdWString( Dependent.displayName ), hc::sys::ServiceStateText( Dependent.state ) } );
    dependencies_->expandAll();
}
