#include "stdafx.h"
#include "rstDocumentView.hpp"

/// 모듈
#include "documentShortcutRegistry.hpp"
#include "documentStatusPopup.hpp"
#include "rstPreviewProcess.hpp"
#include "rstProjectDialogs.hpp"
#include "rst/pythonEnvManager.hpp"
#include "rst/rstEditor.hpp"
#include "rst/rstSettings.hpp"
#include "rst/rstWorkspaceController.hpp"
#include "rst/sphinxDiagnosticsStore.hpp"
#include "rst/sphinxScanner.hpp"
#include "rst/uvDownloader.hpp"

/// UIs
#include "text/QTextView.h"
#include "text/ScintillaQtDirectBackend.h"

/// 기타 헤더파일
#include "core/ThemeManager.h"
#include "core/ToolbarIcons.h"

/// 고유 라이브러리
#include "UniqueLibs/qtMessageDialog.hpp"

/// 공통 라이브러리

/// 외부 라이브러리
#include <fmstyle/StyleProps.h>
#include <fmwidgets/Banner.h>
#include <fmwidgets/Card.h>
#include <fmwidgets/ProgressBar.h>

/// 선언 헤더파일들

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QActionGroup>
#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPlainTextEdit>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStackedWidget>
#include <QTabWidget>
#include <QToolBar>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <qt_windows.h>

DECLARE_CMNLIBSV2_NAMESPACE

namespace hc::document
{
    namespace
    {
        using hc::rst::DiagnosticEntry;
        using hc::rst::EnvState;
        using hc::rst::OutlineSymbol;

        constexpr int LineRole = Qt::UserRole, PathRole = Qt::UserRole + 1, IndexRole = Qt::UserRole + 2;

        QString SeverityText( const int Severity )
        {
            switch( Severity ) {
                case 1:  return QObject::tr( "오류" );
                case 2:  return QObject::tr( "경고" );
                case 3:  return QObject::tr( "정보" );
                default: return QObject::tr( "힌트" );
            }
        }
        bool SamePath( const QString& A, const QString& B )
        {
            return !A.isEmpty() && QFileInfo( A ).absoluteFilePath().compare( QFileInfo( B ).absoluteFilePath(), Qt::CaseInsensitive ) == 0;
        }
        /// conf.py 의 `project = '…'`. 못 읽으면 폴더 이름.
        QString ProjectName( const hc::rst::SphinxProject& Project )
        {
            QFile File( hc::rst::toQString( Project.confPath ) );
            if( File.open( QIODevice::ReadOnly ) ) {
                static const QRegularExpression Assignment( QStringLiteral( R"((?:^|\n)\s*project\s*=\s*[uUrR]?(['"])(.+?)\1)" ) );
                const auto Match = Assignment.match( QString::fromUtf8( File.read( 256 * 1024 ) ) );
                if( Match.hasMatch() && !Match.captured( 2 ).trimmed().isEmpty() ) return Match.captured( 2 ).trimmed();
            }
            return hc::rst::toQString( Project.rootPath.filename() );
        }
        void OpenFolder( const QString& Path )
        {
            // HyperCommander 는 시작 인자의 경로를 활성 패널에서 연다( 이미 떠 있으면 그 창이 받는다 ).
            // 옆에 HyperCommander 가 없으면 탐색기가 대신한다.
            const QFileInfo Info( Path );
            if( !Info.exists() ) return;
            const QString Folder = Info.isDir() ? Info.absoluteFilePath() : Info.absolutePath();
            const QString Commander = QCoreApplication::applicationDirPath() + QStringLiteral( "/HyperCommander.exe" );
            if( QFileInfo( Commander ).isFile() && QProcess::startDetached( Commander, { QDir::toNativeSeparators( Folder ) } ) ) return;
            if( Info.isDir() ) QDesktopServices::openUrl( QUrl::fromLocalFile( Path ) );
            else QProcess::startDetached( QStringLiteral( "explorer.exe" ), { QStringLiteral( "/select," ) + QDir::toNativeSeparators( Path ) } );
        }
        void FilterTree( QTreeWidget* Tree, const QString& Text )
        {
            const auto Visit = [ & ]( auto&& Self, QTreeWidgetItem* Item ) -> bool {
                bool Match = Item->text( 0 ).contains( Text, Qt::CaseInsensitive );
                for( int I = 0; I < Item->childCount(); ++I ) Match = Self( Self, Item->child( I ) ) || Match;
                Item->setHidden( !Match );
                if( !Text.isEmpty() && Match ) Item->setExpanded( true );
                return Match;
            };
            for( int I = 0; I < Tree->topLevelItemCount(); ++I ) Visit( Visit, Tree->topLevelItem( I ) );
        }
        QWidget* OutlinePage( QTabWidget* Tabs, const QString& Title, const char* Name, QTreeWidget*& Tree )
        {
            auto* Page = new QWidget( Tabs );
            auto* Layout = new QVBoxLayout( Page );
            Layout->setContentsMargins( 0, 4, 0, 0 ); Layout->setSpacing( 4 );
            auto* Filter = new QLineEdit( Page );
            Filter->setObjectName( QLatin1String( Name ) + QStringLiteral( "Filter" ) );
            Filter->setPlaceholderText( QObject::tr( "개요 검색" ) ); Filter->setClearButtonEnabled( true );
            Tree = new QTreeWidget( Page );
            Tree->setObjectName( QLatin1String( Name ) );
            Tree->setHeaderHidden( true ); Tree->setContextMenuPolicy( Qt::CustomContextMenu );
            Layout->addWidget( Filter ); Layout->addWidget( Tree, 1 );
            QObject::connect( Filter, &QLineEdit::textChanged, Tree, [ Tree ]( const QString& Text ) { FilterTree( Tree, Text ); } );
            Tabs->addTab( Page, Title );
            return Page;
        }
    }

    RstDocumentView::RstDocumentView( QTextView* Source, const QString& Path, const bool Editor, QWidget* Parent )
        : QWidget( Parent ), source_( Source ), path_( Path ), editorHost_( Editor ), workspace_( hc::rst::RstWorkspace() )
    {
        setObjectName( "rstDocumentView" );
        auto* Layout = new QVBoxLayout( this );
        Layout->setContentsMargins( 0, 0, 0, 0 ); Layout->setSpacing( 0 );

        // 한 줄: [미리보기 | 소스 | 나란히] | 개요 · 진단 · 스크롤 동기화 | 축소 · 확대 · 다시 빌드 | 찾기.
        toolbar_ = new QToolBar( tr( "reStructuredText" ), this );
        toolbar_->setObjectName( "rstToolbar" ); toolbar_->setMovable( false );
        toolbar_->setIconSize( QSize( 20, 20 ) ); toolbar_->setToolButtonStyle( Qt::ToolButtonIconOnly );
        const auto Make = [ this ]( const QString& Id, const QString& Label, const bool Check = false, const char* Icon = nullptr ) {
            auto* A = toolbar_->addAction( Label );
            A->setObjectName( Id ); A->setCheckable( Check );
            if( Icon ) { A->setIcon( ToolbarIcons::iconByName( QString::fromLatin1( Icon ) ) ); A->setProperty( "documentIconName", QString::fromLatin1( Icon ) ); }
            actions_[ Id ] = A;
            connect( A, &QAction::triggered, this, [ this, Id ] { Run( Id ); } );
            return A;
        };
        const auto Named = [ this ]( QAction* A ) {
            if( auto* Button = qobject_cast< QToolButton* >( toolbar_->widgetForAction( A ) ) ) Button->setToolButtonStyle( Qt::ToolButtonTextBesideIcon );
            return A;
        };
        auto* Modes = new QActionGroup( this );
        QList< QAbstractButton* > Segment;
        for( const auto& [ Id, Label, Tip, Icon ] : { std::tuple{ "rst.preview", tr( "미리보기" ), tr( "미리보기" ), "view-preview" },
                                                      std::tuple{ "rst.source", tr( "소스" ), tr( "소스 편집" ), "view-source" },
                                                      std::tuple{ "rst.split", tr( "나란히" ), tr( "나란히 보기" ), "view-split" } } ) {
            auto* A = Named( Make( QString::fromLatin1( Id ), Label, true, Icon ) );
            A->setToolTip( Tip ); Modes->addAction( A );
            if( auto* Button = qobject_cast< QAbstractButton* >( toolbar_->widgetForAction( A ) ) ) Segment << Button;
        }
        fm::style::setSegments( Segment );
        toolbar_->addSeparator();
        Named( Make( "rst.toggleToc", tr( "개요" ), true, "toc-toggle" ) )->setToolTip( tr( "개요 표시 전환" ) );
        Named( Make( "rst.toggleProblems", tr( "진단" ), true, "alert" ) )->setToolTip( tr( "진단 · 빌드 로그 패널 표시 전환" ) );
        Named( Make( "rst.syncScroll", tr( "스크롤 동기화" ), true, "sync" ) )->setToolTip( tr( "소스와 미리보기의 화면 스크롤을 서로 맞춥니다." ) );
        toolbar_->addSeparator();
        Make( "rst.zoomOut", tr( "미리보기 축소" ), false, "zoom-out" );
        Make( "rst.zoomIn", tr( "미리보기 확대" ), false, "zoom-in" );
        Make( "rst.rebuild", tr( "미리보기 다시 빌드" ), false, "refresh" );
        toolbar_->addSeparator();
        Make( "rst.find", tr( "찾기" ), false, "search" );
        // 단추가 없는 명령: 메뉴 · 상태 표시줄 · 단축키가 부른다.
        for( const auto& [ Id, Label ] : { std::pair{ "rst.focusOtherPane", tr( "포커스 이동" ) }, std::pair{ "rst.toggleSource", tr( "소스 전환" ) },
                 std::pair{ "rst.currentHeading", tr( "현재 개요 항목" ) }, std::pair{ "rst.problems", tr( "진단 목록" ) },
                 std::pair{ "rst.zoomReset", tr( "미리보기 원래 크기" ) }, std::pair{ "rst.zoomSet", tr( "미리보기 배율" ) },
                 std::pair{ "rst.fullScreen", tr( "미리보기 전체 화면" ) }, std::pair{ "rst.complete", tr( "자동 완성" ) },
                 std::pair{ "rst.goToReference", tr( "참조로 이동" ) }, std::pair{ "rst.buildProject", tr( "프로젝트 빌드" ) },
                 std::pair{ "rst.cancelBuild", tr( "프로젝트 빌드 취소" ) }, std::pair{ "rst.openConf", tr( "conf.py 열기" ) },
                 std::pair{ "rst.openProjectFolder", tr( "프로젝트 폴더 열기" ) }, std::pair{ "rst.openBuildFolder", tr( "빌드 결과 폴더 열기" ) },
                 std::pair{ "rst.rescanProjects", tr( "프로젝트 다시 찾기" ) }, std::pair{ "rst.configureEnvironment", tr( "환경 구성" ) },
                 std::pair{ "rst.restartServer", tr( "언어 서버 다시 시작" ) }, std::pair{ "rst.showLog", tr( "구성 · 빌드 로그 보기" ) },
                 std::pair{ "rst.openEnvironmentFolder", tr( "환경 폴더 열기" ) }, std::pair{ "rst.openInBrowser", tr( "빌드한 페이지를 브라우저에서 열기" ) },
                 std::pair{ "rst.showInPreview", tr( "미리보기에서 이 위치 보기" ) } } )
            toolbar_->removeAction( Make( QString::fromLatin1( Id ), Label ) );

        // 위: 개요 | 소스 | 미리보기.  아래: 진단 · 빌드 로그.
        vertical_ = new QSplitter( Qt::Vertical, this );
        vertical_->setChildrenCollapsible( false );
        splitter_ = new QSplitter( Qt::Horizontal, vertical_ );
        splitter_->setChildrenCollapsible( false );

        outlinePanel_ = new QWidget( splitter_ );
        outlinePanel_->setMinimumWidth( 140 ); outlinePanel_->setMaximumWidth( 460 );
        auto* OutlineLayout = new QVBoxLayout( outlinePanel_ );
        OutlineLayout->setContentsMargins( 0, 0, 0, 0 );
        outlineTabs_ = new QTabWidget( outlinePanel_ );
        outlineTabs_->setObjectName( "rstOutlineTabs" ); outlineTabs_->setDocumentMode( true );
        OutlinePage( outlineTabs_, tr( "활성 문서" ), "rstOutlineDocument", outlineDocument_ );
        OutlinePage( outlineTabs_, tr( "프로젝트" ), "rstOutlineProject", outlineProject_ );
        OutlineLayout->addWidget( outlineTabs_ );
        // 사용자가 직접 고른 탭은 문서가 바뀌어도 그대로 둔다.
        connect( outlineTabs_->tabBar(), &QTabBar::tabBarClicked, this, [ this ] { outlineTabChosen_ = true; } );
        for( auto* Tree : { outlineDocument_, outlineProject_ } ) {
            connect( Tree, &QTreeWidget::itemClicked, this, [ this ]( QTreeWidgetItem* Item ) { activateOutlineItem( Item ); } );
            connect( Tree, &QTreeWidget::itemActivated, this, [ this ]( QTreeWidgetItem* Item ) { activateOutlineItem( Item ); } );
            connect( Tree, &QWidget::customContextMenuRequested, this, [ this, Tree ]( const QPoint& Position ) { showOutlineMenu( Tree, Position ); } );
        }

        splitter_->addWidget( source_ );

        previewPanel_ = new QWidget( splitter_ );
        auto* PreviewLayout = new QVBoxLayout( previewPanel_ );
        PreviewLayout->setContentsMargins( 0, 0, 0, 0 ); PreviewLayout->setSpacing( 0 );
        // 미리보기 위의 띠: 없는 확장·테마( 경고 ), 빌드 실패( 위험 ), 잠깐 뜨는 알림( 정보 ). 단추는 글 오른쪽에 붙는다.
        banner_ = new fm::ui::Banner( previewPanel_ );
        banner_->setObjectName( "rstBanner" );
        auto* BannerLayout = static_cast< QHBoxLayout* >( banner_->layout() );
        BannerLayout->setSpacing( 8 );
        for( int Index = 0; Index < 2; ++Index ) {
            auto* Button = new QPushButton( banner_ );
            Button->setObjectName( QStringLiteral( "rstBannerButton%1" ).arg( Index ) );
            fm::style::setSmall( Button );
            connect( Button, &QPushButton::clicked, this, [ this, Index ] { bannerButton( Index ); } );
            BannerLayout->addWidget( Button ); bannerButtons_ << Button;
        }
        banner_->hide();
        PreviewLayout->addWidget( banner_ );
        previewStack_ = new QStackedWidget( previewPanel_ );
        preview_ = new RstPreviewProcess( previewStack_ );
        previewStack_->addWidget( preview_->Widget() );
        environmentCard_ = new QWidget( previewStack_ );
        environmentCard_->setObjectName( "rstEnvironmentCard" );
        {
            auto* Outer = new QVBoxLayout( environmentCard_ );
            Outer->addStretch( 2 );
            // 미리보기 자리 한가운데의 안내 카드( 시안 ② ): 제목 띠, 설명, 진행 막대, 단추.
            auto* Box = new fm::ui::Card( environmentCard_ ); Box->setObjectName( "rstEnvironmentBox" ); Box->setMaximumWidth( 460 );
            environmentBox_ = Box;
            auto* Card = new QVBoxLayout( Box ); Card->setSpacing( 9 );
            environmentText_ = new QLabel( Box ); environmentText_->setWordWrap( true ); environmentText_->setTextFormat( Qt::PlainText );
            auto* Progress = new fm::ui::ProgressBar( Box ); Progress->setThickness( fm::ui::ProgressBar::ThinBar ); Progress->setTextVisible( false );
            environmentProgress_ = Progress;
            auto* Buttons = new QHBoxLayout; Buttons->addStretch();
            environmentSecondary_ = new QPushButton( Box ); environmentSecondary_->setObjectName( "rstEnvironmentSecondary" );
            environmentPrimary_ = new QPushButton( Box ); environmentPrimary_->setObjectName( "rstEnvironmentPrimary" );
            fm::style::setButtonRole( environmentPrimary_, fm::style::ButtonRole::Primary );
            Buttons->addWidget( environmentSecondary_ ); Buttons->addWidget( environmentPrimary_ );
            Card->addWidget( environmentText_ ); Card->addWidget( environmentProgress_ ); Card->addLayout( Buttons );
            auto* Row = new QHBoxLayout; Row->addStretch(); Row->addWidget( Box, 4 ); Row->addStretch();
            Outer->addLayout( Row ); Outer->addStretch( 3 );
        }
        previewStack_->addWidget( environmentCard_ );
        PreviewLayout->addWidget( previewStack_, 1 );
        splitter_->addWidget( previewPanel_ );
        splitter_->setStretchFactor( 0, 0 ); splitter_->setStretchFactor( 1, 1 ); splitter_->setStretchFactor( 2, 1 );
        splitter_->setSizes( { 210, 520, 520 } );

        problems_ = new QTabWidget( vertical_ );
        problems_->setObjectName( "rstProblems" ); problems_->setDocumentMode( true ); problems_->setMinimumHeight( 90 );
        diagnostics_ = new QTreeWidget( problems_ );
        diagnostics_->setObjectName( "rstDiagnostics" ); diagnostics_->setRootIsDecorated( false ); diagnostics_->setUniformRowHeights( true );
        diagnostics_->setHeaderLabels( { tr( "심각도" ), tr( "파일" ), tr( "줄" ), tr( "메시지" ), tr( "출처" ) } );
        diagnostics_->header()->setStretchLastSection( false );
        diagnostics_->header()->setSectionResizeMode( 3, QHeaderView::Stretch );
        diagnostics_->setColumnWidth( 0, 70 ); diagnostics_->setColumnWidth( 1, 170 ); diagnostics_->setColumnWidth( 2, 52 ); diagnostics_->setColumnWidth( 4, 100 );
        diagnostics_->setContextMenuPolicy( Qt::CustomContextMenu );
        connect( diagnostics_, &QWidget::customContextMenuRequested, this, &RstDocumentView::showDiagnosticsMenu );
        connect( diagnostics_, &QTreeWidget::itemActivated, this, [ this ]( QTreeWidgetItem* Item ) {
            if( Item ) goTo( Item->data( 0, PathRole ).toString(), Item->data( 0, LineRole ).toInt() );
        } );
        connect( diagnostics_, &QTreeWidget::itemDoubleClicked, this, [ this ]( QTreeWidgetItem* Item ) {
            if( Item ) goTo( Item->data( 0, PathRole ).toString(), Item->data( 0, LineRole ).toInt() );
        } );
        problems_->addTab( diagnostics_, tr( "진단" ) );
        log_ = new QPlainTextEdit( problems_ );
        log_->setObjectName( "rstLog" ); log_->setReadOnly( true ); log_->setMaximumBlockCount( 5000 ); log_->setLineWrapMode( QPlainTextEdit::NoWrap );
        log_->setContextMenuPolicy( Qt::CustomContextMenu );
        connect( log_, &QWidget::customContextMenuRequested, this, &RstDocumentView::showLogMenu );
        problems_->addTab( log_, tr( "빌드 로그" ) );

        vertical_->addWidget( splitter_ ); vertical_->addWidget( problems_ );
        vertical_->setStretchFactor( 0, 1 ); vertical_->setStretchFactor( 1, 0 );
        vertical_->setSizes( { 600, 150 } );
        Layout->addWidget( vertical_ );

        connect( splitter_, &QSplitter::splitterMoved, this, [ this ] {
            saveToggle( QStringLiteral( "layout/" ) + ( editorHost_ ? "editor" : "viewer" ), QString::fromLatin1( splitter_->saveState().toBase64() ) );
        } );
        connect( vertical_, &QSplitter::splitterMoved, this, [ this ] {
            saveToggle( QStringLiteral( "layout/problems" ), QString::fromLatin1( vertical_->saveState().toBase64() ) );
        } );

        // ── 편집기와 미리보기 화면 ──
        editor_ = new hc::rst::RstEditor( source_, this );
        editor_->setFilePath( path_ );
        connect( preview_, &RstPreviewProcess::ContextRequested, this, &RstDocumentView::showPreviewMenu );
        connect( preview_, &RstPreviewProcess::LinkActivated, this, &RstDocumentView::openLink );
        connect( preview_, &RstPreviewProcess::PageChanged, this, &RstDocumentView::pageChanged );
        connect( preview_, &RstPreviewProcess::EscapePressed, this, [ this ] {
            if( previewOnly_ ) emit FullScreenToggleRequested(); else emit EscapeRequested();
        } );
        connect( preview_, &RstPreviewProcess::CommandRequested, this, [ this ]( const QString& Id ) {
            // 미리보기 프로세스에 포커스가 있을 때 눌린 단축키.
            if( Id.startsWith( QLatin1String( "rst." ) ) ) {
                if( auto* Own = actions_.value( Id ); Own && Own->isCheckable() ) Own->trigger(); else Run( Id );
            } else emit WorkspaceCommandRequested( Id );
        } );
        connect( preview_, &RstPreviewProcess::Failed, this, [ this ]( const QString& Message ) {
            showBanner( Banner::PreviewStopped, Message, { tr( "다시 시작" ) } );
        } );
        connect( preview_->Widget(), &QWidget::customContextMenuRequested, this, [ this ]( const QPoint& Position ) {
            showPreviewMenu( { { "x", Position.x() / qMax( .1, zoom_ ) }, { "y", Position.y() / qMax( .1, zoom_ ) }, { "src", -1 }, { "line", 0 } } );
        } );
        source_->Backend()->widget()->installEventFilter( this );
        connect( source_->Backend(), &ITextEditorBackend::cursorPositionChanged, this, &RstDocumentView::markCurrentOutlineItem );

        // ── 조율자가 내는 결과: 활성 문서가 이 보기일 때만 받는다 ──
        using hc::rst::WorkspaceController;
        connect( workspace_, &WorkspaceController::logMessage, this, &RstDocumentView::appendLog );
        connect( workspace_, &WorkspaceController::documentOutlineReady, this, [ this ]( const QString& Path, const QVector< OutlineSymbol >& Symbols ) {
            if( !SamePath( Path, path_ ) ) return;
            documentOutline_ = Symbols; fillDocumentOutline();
        } );
        connect( workspace_, &WorkspaceController::projectOutlineReady, this,
            [ this ]( const QString& ProjectId, const QVector< hc::rst::OutlineDocumentEntry >& Documents, const int Truncated ) {
                if( !isActive() || ProjectId != workspace_->projectIdOf( editor_ ) ) return;
                projectOutline_ = Documents; projectOutlineTruncated_ = Truncated; fillProjectOutline();
            } );
        connect( workspace_, &WorkspaceController::activeDocumentResolved, this, [ this ]( const bool Standalone ) {
            if( !isActive() ) return;
            // 프로젝트에 속한 문서는 프로젝트 탭, 단독 문서는 활성 문서 탭이 먼저다.
            const int Wanted = outlineDefaultTab_ == QLatin1String( "document" ) ? 0 : outlineDefaultTab_ == QLatin1String( "project" ) ? 1 : Standalone ? 0 : 1;
            if( !outlineTabChosen_ ) outlineTabs_->setCurrentIndex( Wanted );
            if( Standalone ) { projectOutline_.clear(); fillProjectOutline(); }
            emit Changed();
        } );
        connect( workspace_, &WorkspaceController::activeProjectChanged, this, [ this ] { if( isActive() ) emit Changed(); } );
        connect( workspace_, &WorkspaceController::previewStatusChanged, this, [ this ]( const QString& Text, const bool Busy, int ) {
            if( !isActive() ) return;
            previewStatus_ = Text; previewBusy_ = Busy;
            emit Changed();
        } );
        connect( workspace_, &WorkspaceController::previewBuildFailed, this, [ this ]( const QString& File ) {
            if( !SamePath( File, path_ ) ) return;
            previewFailed_ = true;
            showBanner( Banner::BuildFailed, previewShown_ ? tr( "미리보기 빌드에 실패했습니다. 마지막으로 성공한 결과를 보여 줍니다." )
                                                           : tr( "미리보기 빌드에 실패했습니다. 빌드 로그에서 원인을 확인하세요." ),
                        { tr( "빌드 로그" ), tr( "다시 빌드" ) } );
            emit Changed();
        } );
        connect( workspace_, &WorkspaceController::previewBuildSucceeded, this, [ this ]( const QString& File ) {
            if( !SamePath( File, path_ ) ) return;
            previewFailed_ = false; previewShown_ = true;
            hideBanner( Banner::BuildFailed );
            emit Changed();
        } );
        connect( workspace_, &WorkspaceController::missingDependenciesDetected, this,
            [ this ]( const QString& ProjectId, const QStringList& Distributions, const QStringList& ) {
                if( !isActive() || ProjectId != workspace_->projectIdOf( editor_ ) ) return;
                QStringList Wanted;
                for( const auto& Name : Distributions ) if( !ignoredPackages_.contains( Name ) && !Wanted.contains( Name ) ) Wanted << Name;
                if( Wanted.isEmpty() ) return;
                missingPackages_ = Wanted;
                showBanner( Banner::MissingPackages, tr( "이 프로젝트가 쓰는 Sphinx 확장/테마가 HyperEditor 환경에 없습니다: %1. 설치하면 미리보기를 다시 빌드합니다." )
                                                         .arg( Wanted.join( QStringLiteral( ", " ) ) ), { tr( "설치" ), tr( "무시" ) } );
            } );
        connect( workspace_, &WorkspaceController::navigateRequested, this, [ this ]( const QString& Path, const int Line, int ) {
            if( isActive() ) goTo( Path, Line );
        } );
        connect( workspace_, &WorkspaceController::referenceNavigateRequested, this, [ this ]( const QString& Path, const int Line ) {
            if( isActive() ) goTo( Path, Line );
        } );
        connect( workspace_, &WorkspaceController::referenceNavigationStatus, this, [ this ]( const QString& Text ) {
            if( !isActive() ) return;
            if( Text.isEmpty() ) hideBanner( Banner::Note ); else showBanner( Banner::Note, Text );
        } );
        connect( workspace_, &WorkspaceController::projectBuildStarted, this, [ this ] {
            projectBuilding_ = true;
            if( isActive() ) { setProblemsVisible( true, false ); problems_->setCurrentWidget( log_ ); }
            emit Changed();
        } );
        connect( workspace_, &WorkspaceController::projectBuildFinished, this,
            [ this ]( const QString&, const QString&, const QString& Output, const bool Ok, const bool Cancelled ) {
                projectBuilding_ = false;
                emit Changed();
                if( !isActive() || Cancelled ) return;
                if( !Ok ) { hc::ui::MessageDialog::warning( this, tr( "빌드" ), tr( "빌드에 실패했습니다. 빌드 로그를 확인하세요." ) ); return; }
                if( hc::rst::Settings().value( QStringLiteral( "build/openWhenDone" ), true ).toBool() ) OpenFolder( Output );
            } );
        connect( workspace_, &WorkspaceController::pythonEnvironmentDamaged, this, [ this ] { emit Changed(); } );
        connect( workspace_, &WorkspaceController::pythonEnvironmentDamageCleared, this, [ this ] { emit Changed(); } );
        connect( workspace_->diagnostics(), &hc::rst::DiagnosticsStore::changed, this, [ this ] {
            // 한 빌드가 여러 번 알린다. 이벤트 루프 한 바퀴에 한 번만 다시 그린다.
            if( diagnosticsQueued_ ) return;
            diagnosticsQueued_ = true;
            QTimer::singleShot( 0, this, [ this ] { diagnosticsQueued_ = false; refreshDiagnostics(); } );
        } );

        auto* Environment = workspace_->pythonEnvironment();
        connect( Environment, &hc::rst::PythonEnvManager::stateChanged, this, [ this ] { refreshEnvironmentCard(); emit Changed(); } );
        connect( Environment, &hc::rst::PythonEnvManager::progressChanged, this, [ this ]( const int Percent, const QString& Phase ) {
            environmentPercent_ = Percent; environmentPhase_ = Phase;
            refreshEnvironmentCard(); emit Changed();
        } );
        connect( Environment, &hc::rst::PythonEnvManager::bootstrapLog, this, &RstDocumentView::appendLog );
        connect( Environment, &hc::rst::PythonEnvManager::packageInstallFinished, this, [ this ]( const bool Ok, const QStringList& Packages ) {
            appendLog( Ok ? tr( "패키지 설치 완료: %1" ).arg( Packages.join( QStringLiteral( ", " ) ) ) : tr( "패키지 설치 실패: %1" ).arg( Packages.join( QStringLiteral( ", " ) ) ) );
            if( Ok && isActive() ) workspace_->requestPreviewBuild( true, true );
        } );
        connect( environmentPrimary_, &QPushButton::clicked, this, [ this ] { Run( "rst.configureEnvironment" ); } );
        connect( environmentSecondary_, &QPushButton::clicked, this, [ this ] {
            auto* Env = workspace_->pythonEnvironment();
            if( Env->isBusy() ) { Env->cancel(); return; }
            // 나중에: 소스만 본다. 미리보기를 다시 열면 안내가 다시 보인다.
            declinedEnvironment_ = true;
            if( SourceVisible() ) Run( "rst.source" );
        } );

        bannerTimer_.setSingleShot( true ); bannerTimer_.setInterval( 6000 );
        connect( &bannerTimer_, &QTimer::timeout, this, [ this ] { hideBanner( Banner::Note ); } );
        // 미리보기 창의 포커스는 이 프로세스에 Qt 신호로 오지 않는다.
        focusPoll_.setInterval( 250 );
        connect( &focusPoll_, &QTimer::timeout, this, &RstDocumentView::pollFocus );
        focusPoll_.start();
        connect( &ThemeManager::instance(), &ThemeManager::themeChanged, this, [ this ] {
            preview_->SetBackground( palette().color( QPalette::Base ).name() );
        } );
        preview_->SetBackground( palette().color( QPalette::Base ).name() );

        workspace_->attachDocument( editor_ );
        ReloadPreferences();
        hc::rst::Settings Stored;
        splitter_->restoreState( QByteArray::fromBase64( Stored.value( QStringLiteral( "layout/" ) + ( editorHost_ ? "editor" : "viewer" ) ).toString().toLatin1() ) );
        vertical_->restoreState( QByteArray::fromBase64( Stored.value( QStringLiteral( "layout/problems" ) ).toString().toLatin1() ) );
        // 미리보기는 다른 프로세스의 창이다. 끄는 동안 다시 그리면 빈 바탕이 드러난다.
        splitter_->setOpaqueResize( false );
        mode( Stored.value( editorHost_ ? QStringLiteral( "editorMode" ) : QStringLiteral( "viewerMode" ),
                            editorHost_ ? QStringLiteral( "split" ) : QStringLiteral( "preview" ) ).toString() );
    }

    RstDocumentView::~RstDocumentView()
    {
        source_->Backend()->widget()->removeEventFilter( this );
        if( workspace_ ) {
            workspace_->forgetPreviewSurface( preview_ );
            workspace_->detachDocument( editor_ );
        }
    }

    bool RstDocumentView::SourceVisible() const { return mode_ != QLatin1String( "preview" ); }
    bool RstDocumentView::PreviewVisible() const { return mode_ != QLatin1String( "source" ); }
    bool RstDocumentView::isActive() const { return workspace_ && workspace_->activeDocument() == editor_; }

    QWidget* RstDocumentView::InputWidget() const
    {
        return SourceVisible() && !preview_->HasFocus() ? source_->Backend()->widget() : preview_->Widget();
    }

    void RstDocumentView::SetReadOnly( const bool Value )
    {
        readOnly_ = Value;
        source_->setReadOnly( Value );
        actions_[ "rst.source" ]->setToolTip( Value ? tr( "소스 보기" ) : tr( "소스 편집" ) );
    }

    void RstDocumentView::Focus()
    {
        if( InputWidget() == preview_->Widget() ) preview_->FocusInput(); else focusSource();
    }

    void RstDocumentView::focusSource()
    {
        auto* Input = source_->Backend()->widget();
        Input->setFocus( Qt::OtherFocusReason );
        // 미리보기는 다른 프로세스의 창이라 네이티브 포커스가 Qt 의 포커스를 따라오지 않는다.
        const auto Root = GetAncestor( reinterpret_cast< HWND >( window()->effectiveWinId() ), GA_ROOT );
        if( Root && GetAncestor( GetForegroundWindow(), GA_ROOT ) == Root ) SetFocus( reinterpret_cast< HWND >( Input->effectiveWinId() ) );
    }

    void RstDocumentView::pollFocus()
    {
        const bool Active = isVisible() && PreviewVisible() && preview_->HasFocus();
        if( Active == previewActive_ ) return;
        previewActive_ = Active;
        emit Changed();
    }

    void RstDocumentView::SetSourceTools( QToolBar* Tools )
    {
        if( !Tools ) return;
        // 텍스트 도구모음은 둘째 줄로 보이지 않는다. 필요한 단추만 이 줄로 가져온다.
        Tools->hide();
        const auto Take = [ Tools ]( const char* Name, const char* Key ) -> QAction* {
            for( auto* A : Tools->actions() )
                if( A->objectName() == QLatin1String( Name ) || A->objectName() == QLatin1String( Key ) ) { Tools->removeAction( A ); return A; }
            return nullptr;
        };
        auto* Replace = Take( "textToolbar.replace", "text.replaceBar" );
        auto* Wrap = Take( "textToolbar.wordWrap", "text.wordWrap" );
        auto* Whitespace = Take( "textToolbar.whitespace", "text.whitespace" );
        auto* Minimap = Take( "textToolbar.minimap", "text.minimap" );
        if( Replace ) sourceActions_ << Replace;
        if( Wrap || Whitespace || Minimap ) { auto* Gap = new QAction( toolbar_ ); Gap->setSeparator( true ); sourceActions_ << Gap; }
        for( auto* A : { Wrap, Whitespace, Minimap } ) if( A ) sourceActions_ << A;
        for( auto* A : sourceActions_ ) { toolbar_->addAction( A ); A->setVisible( SourceVisible() ); }
    }

    void RstDocumentView::SetShortcutBindings( const QJsonObject& Bindings )
    {
        bindings_ = DocumentShortcutRegistry::Bindings( Bindings );
        QJsonObject Keys;
        for( const auto& Item : DocumentShortcutRegistry::Commands() )
            if( Item.scope_ == "rst" || Item.scope_ == "common" ) Keys[ Item.id_ ] = bindings_.value( Item.id_ );
        // 자동 완성과 참조로 이동은 소스에서만 뜻이 있다.
        Keys.remove( "rst.complete" ); Keys.remove( "rst.goToReference" );
        Keys[ "rst.find" ] = bindings_.value( "text.find" );
        preview_->SetShortcuts( Keys );
    }

    void RstDocumentView::SetPath( const QString& Path )
    {
        if( Path.isEmpty() || path_ == Path ) return;
        path_ = Path;
        editor_->setFilePath( Path );
        // 조율자가 경로가 바뀐 것을 보고 프로젝트를 다시 찾는다.
        if( isActive() ) workspace_->setActiveDocument( editor_ ); else activate();
        emit Changed();
    }

    void RstDocumentView::NotifySaved()
    {
        if( !isVisible() ) return;
        if( !isActive() ) activate();
        workspace_->notifyDocumentSaved( editor_ );
    }

    void RstDocumentView::activate()
    {
        if( !isVisible() || !workspace_ ) return;
        workspace_->setPreviewSurface( PreviewVisible() ? preview_ : nullptr );
        workspace_->setActiveDocument( editor_ );
        ensureEnvironment();
        refreshEnvironmentCard();
        refreshDiagnostics();
    }

    void RstDocumentView::showEvent( QShowEvent* Event )
    {
        QWidget::showEvent( Event );
        // 탭이 앞에 나오는 중이다. 배치가 끝난 뒤에 활성으로 만든다.
        QTimer::singleShot( 0, this, [ this ] { activate(); } );
    }

    void RstDocumentView::hideEvent( QHideEvent* Event )
    {
        QWidget::hideEvent( Event );
        // 가려진 문서의 미리보기는 만들지 않는다. 다른 reST 탭이 앞에 나오면 그 탭이 화면을 가져간다.
        if( isActive() && workspace_->previewSurface() == preview_ ) workspace_->setPreviewSurface( nullptr );
    }

    bool RstDocumentView::eventFilter( QObject* Watched, QEvent* Event )
    {
        // 창이 여럿일 때: 입력을 받는 보기가 활성 문서다.
        if( Event->type() == QEvent::FocusIn && Watched == source_->Backend()->widget() && !isActive() ) activate();
        return QWidget::eventFilter( Watched, Event );
    }

    void RstDocumentView::mode( const QString& Value )
    {
        const QString Previous = mode_;
        mode_ = QStringList{ "source", "preview", "split" }.contains( Value ) ? Value : QStringLiteral( "split" );
        source_->setVisible( SourceVisible() );
        for( auto* A : sourceActions_ ) A->setVisible( SourceVisible() );
        previewPanel_->setVisible( PreviewVisible() );
        for( const auto& Id : { "source", "preview", "split" } ) actions_[ "rst." + QString( Id ) ]->setChecked( mode_ == QLatin1String( Id ) );
        syncPanels();
        if( isVisible() && Previous != mode_ ) {
            const bool Opened = PreviewVisible() && ( Previous.isEmpty() || Previous == QLatin1String( "source" ) );
            activate();
            // 미리보기를 다시 열었다: 그동안의 편집을 반영해 만든다.
            if( Opened && isActive() ) workspace_->requestPreviewBuild( true );
        }
        emit Changed();
    }

    void RstDocumentView::syncPanels()
    {
        outlinePanel_->setVisible( !previewOnly_ && actions_[ "rst.toggleToc" ]->isChecked() );
        problems_->setVisible( !previewOnly_ && actions_[ "rst.toggleProblems" ]->isChecked() );
        for( const auto& Id : { "rst.zoomIn", "rst.zoomOut", "rst.zoomReset", "rst.zoomSet", "rst.openInBrowser", "rst.fullScreen" } )
            actions_[ QString::fromLatin1( Id ) ]->setEnabled( PreviewVisible() || QLatin1String( Id ) == QLatin1String( "rst.fullScreen" ) );
        for( const auto& Id : { "rst.complete", "rst.goToReference", "rst.showInPreview" } )
            actions_[ QString::fromLatin1( Id ) ]->setEnabled( SourceVisible() );
    }

    void RstDocumentView::saveToggle( const QString& Key, const QVariant& Value )
    {
        hc::rst::Settings Stored;
        Stored.setValue( Key, Value );
    }

    void RstDocumentView::setProblemsVisible( const bool Visible, const bool Remember )
    {
        auto* Toggle = actions_[ "rst.toggleProblems" ];
        if( Toggle->isChecked() != Visible ) { const QSignalBlocker Block( Toggle ); Toggle->setChecked( Visible ); }
        if( Remember ) problemsClosedByUser_ = !Visible;
        syncPanels();
    }

    void RstDocumentView::SetPreviewOnly( const bool On )
    {
        if( previewOnly_ == On ) return;
        if( On ) {
            modeBeforePreviewOnly_ = mode_;
            previewOnly_ = true;
            toolbar_->hide();
            mode( "preview" );
            preview_->FocusInput();
        } else {
            previewOnly_ = false;
            toolbar_->show();
            mode( modeBeforePreviewOnly_ );
            Focus();
        }
        syncPanels();
    }

    bool RstDocumentView::CanRun( const QString& Id ) const
    {
        if( !actions_.contains( Id ) || !actions_[ Id ]->isEnabled() ) return false;
        const auto* Project = workspace_->projectOf( editor_ );
        if( Id == "rst.buildProject" ) return Project != nullptr && !projectBuilding_ && workspace_->pythonEnvironment()->isReady();
        if( Id == "rst.cancelBuild" ) return projectBuilding_;
        if( Id == "rst.openConf" || Id == "rst.openProjectFolder" || Id == "rst.openBuildFolder" ) return Project != nullptr;
        if( Id == "rst.restartServer" ) return !workspace_->projectIdOf( editor_ ).isEmpty() && workspace_->pythonEnvironment()->isReady();
        if( Id == "rst.configureEnvironment" ) return !workspace_->pythonEnvironment()->isBusy();
        if( Id == "rst.openInBrowser" ) return PreviewVisible() && preview_->url().isLocalFile();
        return true;
    }

    bool RstDocumentView::RunPreviewCommand( const QString& Id )
    {
        if( Id == QLatin1String( "text.minimap" ) ) return false;
        if( SourceVisible() && !preview_->HasFocus() ) return false;
        if( Id == "edit.copy" ) preview_->Copy();
        else if( Id == "edit.selectAll" ) preview_->SelectAll();
        else if( Id == "text.find" ) Run( "rst.find" );
        return Id.startsWith( "edit." ) || Id.startsWith( "text." );
    }

    bool RstDocumentView::Run( const QString& Id, const QJsonValue& Value )
    {
        if( !CanRun( Id ) ) return false;
        auto* Environment = workspace_->pythonEnvironment();
        const auto* Project = workspace_->projectOf( editor_ );
        if( Id == "rst.source" || Id == "rst.preview" || Id == "rst.split" ) {
            if( previewOnly_ ) return false;
            mode( Id.section( '.', 1 ) );
            saveToggle( editorHost_ ? QStringLiteral( "editorMode" ) : QStringLiteral( "viewerMode" ), mode_ );
            Focus();
        }
        else if( Id == "rst.toggleSource" ) { if( previewOnly_ ) return false; mode( SourceVisible() ? "preview" : "source" ); Focus(); }
        else if( Id == "rst.focusOtherPane" ) {
            if( mode_ == QLatin1String( "split" ) ) { if( preview_->HasFocus() ) focusSource(); else preview_->FocusInput(); }
        }
        else if( Id == "rst.toggleToc" ) { syncPanels(); saveToggle( editorHost_ ? QStringLiteral( "editorOutline" ) : QStringLiteral( "viewerOutline" ), actions_[ Id ]->isChecked() ); }
        else if( Id == "rst.toggleProblems" ) { problemsClosedByUser_ = !actions_[ Id ]->isChecked(); syncPanels(); }
        else if( Id == "rst.syncScroll" ) { saveToggle( QStringLiteral( "scrollSync" ), actions_[ Id ]->isChecked() ); workspace_->reloadSettings(); }
        else if( Id == "rst.rebuild" ) {
            if( !isActive() ) activate();
            if( bannerKind_ == Banner::PreviewStopped ) { hideBanner( Banner::PreviewStopped ); preview_->Restart(); }
            if( !Environment->isReady() ) { declinedEnvironment_ = false; ensureEnvironment(); refreshEnvironmentCard(); }
            workspace_->requestPreviewBuild( true, true );
        }
        else if( Id == "rst.currentHeading" ) {
            actions_[ "rst.toggleToc" ]->setChecked( true ); syncPanels();
            outlineTabs_->setCurrentIndex( 0 ); markCurrentOutlineItem();
            if( outlineDocument_->currentItem() ) outlineDocument_->scrollToItem( outlineDocument_->currentItem() );
        }
        else if( Id == "rst.problems" ) { setProblemsVisible( true, true ); problems_->setCurrentWidget( diagnostics_ ); diagnostics_->setFocus(); }
        else if( Id == "rst.showLog" ) { setProblemsVisible( true, true ); problems_->setCurrentWidget( log_ ); }
        else if( Id == "rst.find" ) {
            if( mode_ == QLatin1String( "source" ) || ( SourceVisible() && !preview_->HasFocus() && source_->Backend()->widget()->hasFocus() ) ) source_->showFindBar();
            else {
                bool Ok = false;
                const auto Text = QInputDialog::getText( this, tr( "미리보기 찾기" ), tr( "찾을 내용" ), QLineEdit::Normal, {}, &Ok );
                if( Ok ) preview_->Find( Text );
            }
        }
        else if( Id == "rst.zoomIn" || Id == "rst.zoomOut" ) SetPreviewZoom( PreviewZoomPercent() + ( Id.endsWith( "In" ) ? 10 : -10 ) );
        else if( Id == "rst.zoomReset" ) SetPreviewZoom( 100 );
        else if( Id == "rst.zoomSet" ) {
            bool Ok = false;
            int Percent = qRound( Value.toVariant().toDouble( &Ok ) );
            if( !Ok ) Percent = QInputDialog::getInt( this, tr( "미리보기 배율" ), tr( "배율 (%)" ), PreviewZoomPercent(), MinZoomPercent, MaxZoomPercent, 10, &Ok );
            if( Ok ) SetPreviewZoom( Percent );
        }
        else if( Id == "rst.fullScreen" ) emit FullScreenToggleRequested();
        else if( Id == "rst.complete" ) { if( !isActive() ) activate(); focusSource(); workspace_->requestCompletion(); }
        else if( Id == "rst.goToReference" ) { if( !isActive() ) activate(); workspace_->requestReferenceNavigation(); }
        else if( Id == "rst.showInPreview" ) showLineInPreview( editor_->caretLine() );
        else if( Id == "rst.buildProject" ) buildProject();
        else if( Id == "rst.cancelBuild" ) workspace_->cancelProjectBuild();
        else if( Id == "rst.openConf" ) openProjectFile( hc::rst::toQString( Project->confPath ) );
        else if( Id == "rst.openProjectFolder" ) OpenFolder( hc::rst::toQString( Project->rootPath ) );
        else if( Id == "rst.openBuildFolder" ) {
            const QString Build = hc::rst::toQString( Project->buildPath );
            OpenFolder( QFileInfo( Build ).isDir() ? Build : hc::rst::toQString( Project->rootPath ) );
        }
        else if( Id == "rst.rescanProjects" ) workspace_->rescanProjects();
        else if( Id == "rst.configureEnvironment" ) {
            // 처음이면 구성, 이미 있으면 다시 구성한다. 다시 구성은 되묻는다.
            if( Environment->isReady()
                && hc::ui::MessageDialog::question( this, tr( "Python 환경 재구성" ),
                       tr( "Python · Sphinx · Esbonio 환경을 다시 구성합니다. 패키지를 다시 확인하고 필요한 것을 내려받습니다. 계속할까요?" ) ) != QMessageBox::Yes )
                return false;
            declinedEnvironment_ = false;
            Environment->configureEnvironmentAsync( Environment->isReady() );
            refreshEnvironmentCard();
        }
        else if( Id == "rst.restartServer" ) workspace_->restartLanguageServer( workspace_->projectIdOf( editor_ ) );
        else if( Id == "rst.openEnvironmentFolder" ) { QDir().mkpath( Environment->runtimeRoot() ); OpenFolder( Environment->runtimeRoot() ); }
        else if( Id == "rst.openInBrowser" ) QDesktopServices::openUrl( preview_->url() );
        emit Changed();
        return true;
    }

    int RstDocumentView::PreviewZoomPercent() const { return qRound( zoom_ * 100 ); }

    void RstDocumentView::SetPreviewZoom( const int Percent )
    {
        const int Value = qBound( MinZoomPercent, Percent, MaxZoomPercent );
        if( Value == PreviewZoomPercent() ) return;
        zoom_ = Value / 100.0;
        preview_->SetZoom( zoom_ );
        emit Changed();
    }

    void RstDocumentView::ReloadPreferences()
    {
        const hc::rst::Settings Stored;
        {
            const QSignalBlocker Block( actions_[ "rst.syncScroll" ] );
            actions_[ "rst.syncScroll" ]->setChecked( Stored.value( QStringLiteral( "scrollSync" ), true ).toBool() );
        }
        {
            const QSignalBlocker Block( actions_[ "rst.toggleToc" ] );
            actions_[ "rst.toggleToc" ]->setChecked( Stored.value( editorHost_ ? QStringLiteral( "editorOutline" ) : QStringLiteral( "viewerOutline" ), editorHost_ ).toBool() );
        }
        outlineDefaultTab_ = Stored.value( QStringLiteral( "outlineTab" ), QStringLiteral( "auto" ) ).toString();
        problemsPolicy_ = editorHost_ ? Stored.value( QStringLiteral( "problemsPanel" ), QStringLiteral( "auto" ) ).toString() : QStringLiteral( "manual" );
        if( const int Depth = qBound( 0, Stored.value( QStringLiteral( "outlineDepth" ), 3 ).toInt(), 9 ); Depth != outlineDepth_ ) {
            outlineDepth_ = Depth;
            fillDocumentOutline(); fillProjectOutline();
        }
        syncPanels();
        // 미리보기 · 언어 서버 설정은 조율자가 읽는다. 열린 보기마다 부르지만 같은 값이면 하는 일이 없다.
        if( isActive() ) { workspace_->pythonEnvironment()->reloadSettings(); workspace_->reloadSettings(); }
    }

    // ── 환경 ─────────────────────────────────────────────────────────────

    void RstDocumentView::ensureEnvironment()
    {
        auto* Environment = workspace_->pythonEnvironment();
        // 준비된 환경은 프로세스에서 한 번, 실제로 도는지 확인한다( 기반 Python 이 지워졌을 수 있다 ).
        static bool Verified = false;
        if( Environment->isReady() ) {
            if( !Verified ) { Verified = true; Environment->ensureEnvironmentAsync(); }
            return;
        }
        if( Environment->isBusy() || declinedEnvironment_ ) return;
        // 확인 없이 구성하도록 정했거나, 직접 지정한 uv 가 있을 때만 스스로 시작한다. 그 밖에는 안내 카드가 묻는다.
        if( Environment->state() == EnvState::NotConfigured && Environment->autoBootstrap() ) {
            Verified = true;
            Environment->ensureEnvironmentAsync();
        }
    }

    QString RstDocumentView::environmentText() const
    {
        const auto* Environment = workspace_->pythonEnvironment();
        if( Environment->isBusy() )
            return environmentPercent_ >= 0 && !environmentPhase_.isEmpty() ? tr( "환경 %1 (%2%)" ).arg( environmentPhase_ ).arg( environmentPercent_ )
                                                                             : tr( "환경 %1" ).arg( environmentPhase_.isEmpty() ? Environment->stateText() : environmentPhase_ );
        return tr( "환경 %1" ).arg( Environment->stateText() );
    }

    void RstDocumentView::refreshEnvironmentCard()
    {
        auto* Environment = workspace_->pythonEnvironment();
        const bool Ready = Environment->isReady();
        previewStack_->setCurrentWidget( Ready ? preview_->Widget() : environmentCard_ );
        if( Ready ) return;
        const bool Busy = Environment->isBusy();
        environmentProgress_->setVisible( Busy );
        if( Busy ) {
            environmentBox_->setTitle( tr( "환경 구성 중" ) );
            environmentText_->setText( environmentPhase_.isEmpty() ? Environment->stateText() : environmentPhase_ );
            if( environmentPercent_ < 0 ) environmentProgress_->setRange( 0, 0 );
            else { environmentProgress_->setRange( 0, 100 ); environmentProgress_->setValue( environmentPercent_ ); }
            environmentPrimary_->hide();
            environmentSecondary_->setText( tr( "취소" ) ); environmentSecondary_->show();
            return;
        }
        environmentPrimary_->show(); environmentSecondary_->setVisible( SourceVisible() );
        environmentSecondary_->setText( tr( "나중에" ) );
        if( Environment->state() == EnvState::Failed ) {
            environmentBox_->setTitle( tr( "환경을 구성하지 못했습니다" ) );
            environmentText_->setText( Environment->lastError().isEmpty() ? tr( "빌드 로그에서 원인을 확인하세요." ) : Environment->lastError() );
            environmentPrimary_->setText( tr( "다시 시도" ) );
            return;
        }
        environmentBox_->setTitle( tr( "미리보기 환경이 아직 없습니다" ) );
        environmentPrimary_->setText( tr( "환경 구성" ) );
        if( Environment->useExternalUv() )
            environmentText_->setText( tr( "reStructuredText 미리보기와 진단에는 Python · Sphinx · Esbonio가 필요합니다. 지정한 uv로 이 PC의 사용자 폴더에 구성합니다. "
                                           "제품 폴더와 시스템 Python은 건드리지 않습니다. 편집은 그동안에도 할 수 있습니다." ) );
        else if( Environment->uvAcquire() == QLatin1String( "never" ) )
            environmentText_->setText( tr( "reStructuredText 미리보기와 진단에는 Python · Sphinx · Esbonio가 필요합니다. uv를 내려받지 않도록 설정되어 있습니다. "
                                           "설정 › reStructuredText › Python 환경에서 uv 경로를 지정하세요." ) );
        else
            environmentText_->setText( tr( "reStructuredText 미리보기와 진단에는 Python · Sphinx · Esbonio가 필요합니다. uv %1을(를) astral.sh의 공식 배포처에서 내려받아 "
                                           "이 PC의 사용자 폴더에 구성합니다. 제품 폴더와 시스템 Python은 건드리지 않습니다. 처음 한 번 수백 MB를 내려받으며, 편집은 그동안에도 할 수 있습니다." )
                                           .arg( QLatin1String( hc::rst::UvRelease::Version ) ) );
    }

    // ── 띠 ───────────────────────────────────────────────────────────────

    void RstDocumentView::showBanner( const Banner Kind, const QString& Text, const QStringList& Buttons )
    {
        // 설치 안내와 실패 안내는 잠깐 뜨는 알림보다 앞선다.
        if( Kind == Banner::Note && bannerKind_ != Banner::None && bannerKind_ != Banner::Note ) return;
        // 다른 페이지 안내는 실패 · 설치 안내를 가리지 않는다. 돌아가기는 오른쪽 버튼 메뉴에도 있다.
        if( Kind == Banner::OtherPage && bannerKind_ != Banner::None && bannerKind_ != Banner::Note && bannerKind_ != Banner::OtherPage ) return;
        // 빌드는 없는 확장을 알린 직후 실패로 끝난다. 그 실패의 원인과 고칠 방법( 설치 )이 앞의 안내다 —
        // 일반적인 실패 안내로 덮으면 설치할 길이 화면에서 사라진다.
        if( Kind == Banner::BuildFailed && bannerKind_ == Banner::MissingPackages ) return;
        bannerKind_ = Kind;
        banner_->setText( Text.toHtmlEscaped() );
        for( int Index = 0; Index < bannerButtons_.size(); ++Index ) {
            bannerButtons_[ Index ]->setVisible( Index < Buttons.size() );
            if( Index < Buttons.size() ) bannerButtons_[ Index ]->setText( Buttons[ Index ] );
        }
        const bool Danger = Kind == Banner::BuildFailed || Kind == Banner::PreviewStopped;
        banner_->setTone( Danger ? fm::ui::Banner::Danger : Kind == Banner::MissingPackages ? fm::ui::Banner::Warn : fm::ui::Banner::Info );
        banner_->setGlyph( Kind == Banner::Note || Kind == Banner::OtherPage ? fm::ui::glyph::Info : fm::ui::glyph::Warning );
        banner_->show();
        if( Kind == Banner::Note ) bannerTimer_.start(); else bannerTimer_.stop();
    }

    void RstDocumentView::hideBanner( const Banner Kind )
    {
        if( bannerKind_ != Kind ) return;
        bannerKind_ = Banner::None;
        banner_->hide();
    }

    void RstDocumentView::bannerButton( const int Index )
    {
        switch( bannerKind_ ) {
            case Banner::MissingPackages:
                if( Index == 0 ) {
                    appendLog( tr( "패키지 설치: %1" ).arg( missingPackages_.join( QStringLiteral( ", " ) ) ) );
                    workspace_->pythonEnvironment()->installPackagesAsync( missingPackages_ );
                } else ignoredPackages_ += missingPackages_;   // 이번 실행 동안 다시 묻지 않는다.
                hideBanner( Banner::MissingPackages );
                break;
            case Banner::BuildFailed:
                if( Index == 0 ) Run( "rst.showLog" ); else Run( "rst.rebuild" );
                break;
            case Banner::PreviewStopped:
                Run( "rst.rebuild" );
                break;
            case Banner::OtherPage: {
                // [소스 열기] [돌아가기] 또는 [돌아가기]. 소스를 열면 이 탭의 미리보기는 제 문서로 돌아간다.
                const QString Source = otherPageSource_;
                const bool Open = Index == 0 && !Source.isEmpty();
                preview_->ReturnToRequested();
                if( Open ) emit OpenRequested( Source );
                break;
            }
            default: break;
        }
    }

    // ── 개요 ─────────────────────────────────────────────────────────────

    void RstDocumentView::addOutlineSymbols( QTreeWidgetItem* Parent, QTreeWidget* Tree, const QVector< OutlineSymbol >& Symbols, const int Depth )
    {
        for( const OutlineSymbol& Symbol : Symbols ) {
            auto* Item = Parent ? new QTreeWidgetItem( Parent ) : new QTreeWidgetItem( Tree );
            const bool Cut = outlineDepth_ > 0 && Depth >= outlineDepth_ && !Symbol.children.isEmpty();
            Item->setText( 0, Symbol.detail.isEmpty() ? Symbol.name : QStringLiteral( "%1 — %2" ).arg( Symbol.name, Symbol.detail ) );
            Item->setData( 0, LineRole, Symbol.line ); Item->setData( 0, PathRole, Symbol.path );
            Item->setToolTip( 0, tr( "줄 %1" ).arg( Symbol.line ) );
            if( Cut ) {
                // 깊이 설정에 걸려 빠진 항목이 있다.
                Item->setText( 0, Item->text( 0 ) + QStringLiteral( " …" ) );
                Item->setToolTip( 0, tr( "줄 %1 · 하위 %2개 항목이 개요 깊이 설정에 걸려 빠졌습니다." ).arg( Symbol.line ).arg( Symbol.children.size() ) );
            } else addOutlineSymbols( Item, Tree, Symbol.children, Depth + 1 );
        }
    }

    void RstDocumentView::fillDocumentOutline()
    {
        outlineDocument_->clear();
        if( documentOutline_.isEmpty() ) {
            auto* Note = new QTreeWidgetItem( outlineDocument_, { tr( "문서 제목이 없습니다." ) } );
            Note->setFlags( Qt::NoItemFlags );
            return;
        }
        addOutlineSymbols( nullptr, outlineDocument_, documentOutline_, 1 );
        outlineDocument_->expandToDepth( 1 );
        markCurrentOutlineItem();
    }

    void RstDocumentView::fillProjectOutline()
    {
        outlineProject_->clear();
        if( projectOutline_.isEmpty() ) {
            auto* Note = new QTreeWidgetItem( outlineProject_, { workspace_->projectOf( editor_ ) ? tr( "프로젝트에 문서가 없습니다." )
                                                                                               : tr( "이 문서는 Sphinx 프로젝트에 속하지 않습니다." ) } );
            Note->setFlags( Qt::NoItemFlags );
            return;
        }
        for( const auto& Document : projectOutline_ ) {
            auto* Item = new QTreeWidgetItem( outlineProject_, { Document.label } );
            Item->setData( 0, PathRole, Document.path ); Item->setData( 0, LineRole, 1 ); Item->setToolTip( 0, QDir::toNativeSeparators( Document.path ) );
            auto Font = Item->font( 0 ); Font.setWeight( QFont::DemiBold ); Item->setFont( 0, Font );
            // 문서 줄은 깊이로 세지 않는다.
            addOutlineSymbols( Item, outlineProject_, Document.symbols, 1 );
            Item->setExpanded( SamePath( Document.path, path_ ) );
        }
        if( projectOutlineTruncated_ > 0 ) {
            auto* More = new QTreeWidgetItem( outlineProject_, { tr( "… %1개 문서 생략" ).arg( projectOutlineTruncated_ ) } );
            More->setFlags( Qt::NoItemFlags );
        }
    }

    void RstDocumentView::markCurrentOutlineItem()
    {
        if( !outlinePanel_->isVisible() || outlineDocument_->topLevelItemCount() == 0 ) return;
        // 캐럿이 놓인 줄 위에서 가장 가까운 제목.
        const int Line = editor_->caretLine();
        QTreeWidgetItem* Best = nullptr;
        for( QTreeWidgetItemIterator It( outlineDocument_ ); *It; ++It ) {
            const int At = ( *It )->data( 0, LineRole ).toInt();
            if( At > 0 && At <= Line && ( !Best || At >= Best->data( 0, LineRole ).toInt() ) ) Best = *It;
        }
        if( Best && Best != outlineDocument_->currentItem() ) { const QSignalBlocker Block( outlineDocument_ ); outlineDocument_->setCurrentItem( Best ); }
    }

    void RstDocumentView::activateOutlineItem( QTreeWidgetItem* Item )
    {
        if( !Item || !( Item->flags() & Qt::ItemIsEnabled ) ) return;
        goTo( Item->data( 0, PathRole ).toString(), Item->data( 0, LineRole ).toInt() );
    }

    void RstDocumentView::showOutlineMenu( QTreeWidget* Tree, const QPoint& Position )
    {
        auto* Item = Tree->itemAt( Position );
        const bool Usable = Item && ( Item->flags() & Qt::ItemIsEnabled );
        const QString Path = Usable ? Item->data( 0, PathRole ).toString() : QString();
        const int Line = Usable ? Item->data( 0, LineRole ).toInt() : 0;
        const QString Text = Usable ? Item->text( 0 ) : QString();
        const bool DocumentRow = Tree == outlineProject_ && Usable && !Item->parent();
        const bool Own = Path.isEmpty() || SamePath( Path, path_ );
        QMenu Menu( this );
        Menu.setObjectName( Tree == outlineProject_ ? "rstProjectOutlineMenu" : "rstOutlineMenu" );
        if( DocumentRow ) {
            Menu.addAction( tr( "문서 열기" ), this, [ this, Path ] { goTo( Path, 1 ); } );
            Menu.addAction( tr( "경로 복사" ), this, [ Path ] { QGuiApplication::clipboard()->setText( QDir::toNativeSeparators( Path ) ); } );
            Menu.addAction( tr( "폴더에서 보기" ), this, [ Path ] { OpenFolder( Path ); } );
            Menu.addSeparator();
            Menu.addAction( tr( "프로젝트 빌드…" ), this, [ this ] { Run( "rst.buildProject" ); } )->setEnabled( CanRun( "rst.buildProject" ) );
            Menu.addAction( tr( "conf.py 열기" ), this, [ this ] { Run( "rst.openConf" ); } )->setEnabled( CanRun( "rst.openConf" ) );
        } else {
            Menu.addAction( Own ? tr( "이 제목으로 이동" ) : tr( "문서를 열고 이동" ), this, [ this, Path, Line ] { goTo( Path, Line ); } )->setEnabled( Usable );
            Menu.addAction( tr( "미리보기에서 보기" ), this, [ this, Line ] { showLineInPreview( Line ); } )->setEnabled( Usable && Own );
        }
        Menu.addSeparator();
        Menu.addAction( tr( "모두 펼치기" ), Tree, &QTreeWidget::expandAll );
        Menu.addAction( tr( "모두 접기" ), Tree, &QTreeWidget::collapseAll );
        auto* Depth = Menu.addMenu( tr( "표시 깊이" ) );
        auto* Group = new QActionGroup( Depth );
        for( const int Value : { 1, 2, 3, 4, 6, 0 } ) {
            auto* Choice = Depth->addAction( Value == 0 ? tr( "제한 없음" ) : tr( "%1 단계" ).arg( Value ) );
            Choice->setCheckable( true ); Choice->setChecked( Value == outlineDepth_ ); Group->addAction( Choice );
            connect( Choice, &QAction::triggered, this, [ this, Value ] {
                saveToggle( QStringLiteral( "outlineDepth" ), Value );
                outlineDepth_ = Value; fillDocumentOutline(); fillProjectOutline();
            } );
        }
        if( Tree == outlineProject_ ) Menu.addAction( tr( "개요 새로 고침" ), this, [ this ] { workspace_->rescanProjects(); } );
        Menu.addSeparator();
        Menu.addAction( tr( "제목 텍스트 복사" ), this, [ Text ] { QGuiApplication::clipboard()->setText( Text ); } )->setEnabled( Usable && !DocumentRow );
        Menu.exec( Tree->viewport()->mapToGlobal( Position ) );
    }

    // ── 진단 · 로그 ──────────────────────────────────────────────────────

    void RstDocumentView::refreshDiagnostics()
    {
        if( !isActive() && diagnostics_->topLevelItemCount() > 0 ) return;
        const auto All = hc::rst::deduplicateDiagnostics( workspace_->diagnostics()->all() );
        const auto* Project = workspace_->projectOf( editor_ );
        shownDiagnostics_.clear();
        errorCount_ = warningCount_ = 0;
        for( const DiagnosticEntry& Entry : All ) {
            const bool Own = SamePath( Entry.path, path_ );
            if( Own ) { if( Entry.severity == 1 ) ++errorCount_; else if( Entry.severity == 2 ) ++warningCount_; }
            // 이 문서의 진단만 보거나, 같은 프로젝트의 것까지 본다. 다른 프로젝트의 진단은 그 문서의 보기가 보인다.
            if( Own || ( !diagnosticsThisDocument_ && Project != nullptr && Project->contains( hc::rst::toPath( Entry.path ) ) ) ) shownDiagnostics_.append( Entry );
        }
        std::stable_sort( shownDiagnostics_.begin(), shownDiagnostics_.end(), [ this ]( const DiagnosticEntry& A, const DiagnosticEntry& B ) {
            const bool OwnA = SamePath( A.path, path_ ), OwnB = SamePath( B.path, path_ );
            if( OwnA != OwnB ) return OwnA;
            if( A.path != B.path ) return A.path < B.path;
            return A.line < B.line;
        } );
        diagnostics_->clear();
        const auto Danger = ThemeManager::instance().color( QStringLiteral( "text.diagnostic.error" ) );
        const auto Warn = ThemeManager::instance().color( QStringLiteral( "text.diagnostic.warning" ) );
        for( int Index = 0; Index < shownDiagnostics_.size(); ++Index ) {
            const auto& Entry = shownDiagnostics_[ Index ];
            auto* Item = new QTreeWidgetItem( diagnostics_, { SeverityText( Entry.severity ), QFileInfo( Entry.path ).fileName(), QString::number( Entry.line ),
                                                              Entry.message.simplified(), Entry.source } );
            Item->setData( 0, PathRole, Entry.path ); Item->setData( 0, LineRole, Entry.line ); Item->setData( 0, IndexRole, Index );
            Item->setToolTip( 1, QDir::toNativeSeparators( Entry.path ) ); Item->setToolTip( 3, Entry.message );
            Item->setTextAlignment( 2, Qt::AlignRight | Qt::AlignVCenter );
            if( Entry.severity == 1 && Danger.isValid() ) Item->setForeground( 0, Danger );
            else if( Entry.severity == 2 && Warn.isValid() ) Item->setForeground( 0, Warn );
        }
        problems_->setTabText( 0, shownDiagnostics_.isEmpty() ? tr( "진단" ) : tr( "진단 %1" ).arg( shownDiagnostics_.size() ) );
        // 문제가 생기면 패널을 연다. 사용자가 닫았으면 다시 열지 않는다.
        if( problemsPolicy_ == QLatin1String( "auto" ) && !problemsClosedByUser_ && !previewOnly_ && errorCount_ + warningCount_ > 0
            && !actions_[ "rst.toggleProblems" ]->isChecked() )
            setProblemsVisible( true, false );
        emit Changed();
    }

    void RstDocumentView::showDiagnosticsMenu( const QPoint& Position )
    {
        auto* Item = diagnostics_->itemAt( Position );
        const QString Path = Item ? Item->data( 0, PathRole ).toString() : QString();
        const int Line = Item ? Item->data( 0, LineRole ).toInt() : 0;
        const QString Message = Item ? Item->toolTip( 3 ) : QString();
        QMenu Menu( this );
        Menu.setObjectName( "rstDiagnosticsMenu" );
        Menu.addAction( tr( "이 위치로 이동" ), this, [ this, Path, Line ] { goTo( Path, Line ); } )->setEnabled( Item != nullptr );
        Menu.addAction( tr( "미리보기에서 보기" ), this, [ this, Line ] { showLineInPreview( Line ); } )->setEnabled( Item && SamePath( Path, path_ ) );
        Menu.addSeparator();
        Menu.addAction( tr( "메시지 복사" ), this, [ Message ] { QGuiApplication::clipboard()->setText( Message ); } )->setEnabled( Item != nullptr );
        Menu.addAction( tr( "모두 복사" ), this, [ this ] {
            QStringList Lines;
            for( const auto& Entry : shownDiagnostics_ )
                Lines << QStringLiteral( "%1:%2: %3: %4 [%5]" ).arg( QDir::toNativeSeparators( Entry.path ) ).arg( Entry.line ).arg( SeverityText( Entry.severity ), Entry.message.simplified(), Entry.source );
            QGuiApplication::clipboard()->setText( Lines.join( QLatin1Char( '\n' ) ) );
        } )->setEnabled( !shownDiagnostics_.isEmpty() );
        Menu.addSeparator();
        auto* Own = Menu.addAction( tr( "이 문서의 진단만" ) );
        Own->setCheckable( true ); Own->setChecked( diagnosticsThisDocument_ );
        connect( Own, &QAction::triggered, this, [ this ]( const bool On ) { diagnosticsThisDocument_ = On; refreshDiagnostics(); } );
        Menu.addSeparator();
        Menu.addAction( tr( "미리보기 다시 빌드" ), this, [ this ] { Run( "rst.rebuild" ); } );
        Menu.exec( diagnostics_->viewport()->mapToGlobal( Position ) );
    }

    void RstDocumentView::showLogMenu( const QPoint& Position )
    {
        QMenu Menu( this );
        Menu.setObjectName( "rstLogMenu" );
        // `경로:줄: …` 로 시작하는 줄이면 그 위치로 갈 수 있다.
        const QString LineText = log_->cursorForPosition( Position ).block().text();
        static const QRegularExpression Location( QStringLiteral( R"(([A-Za-z]:[\\/][^:*?"<>|]+|[^\s:]+\.(?:rst|rest|txt|py)):(\d+):)" ) );
        const auto Match = Location.match( LineText );
        Menu.addAction( tr( "이 위치로 이동" ), this, [ this, Match ] {
            QString Path = Match.captured( 1 );
            if( QDir::isRelativePath( Path ) ) Path = QFileInfo( path_ ).absoluteDir().filePath( Path );
            goTo( Path, Match.captured( 2 ).toInt() );
        } )->setEnabled( Match.hasMatch() );
        Menu.addSeparator();
        Menu.addAction( tr( "복사" ), log_, &QPlainTextEdit::copy )->setEnabled( log_->textCursor().hasSelection() );
        Menu.addAction( tr( "전체 선택" ), log_, &QPlainTextEdit::selectAll );
        Menu.addSeparator();
        Menu.addAction( tr( "로그 지우기" ), log_, &QPlainTextEdit::clear );
        Menu.exec( log_->viewport()->mapToGlobal( Position ) );
    }

    void RstDocumentView::appendLog( const QString& Text )
    {
        if( Text.isEmpty() ) return;
        // 여러 줄이면 이어지는 줄을 같은 칸만큼 밀어 한 덩어리로 보이게 한다.
        const QString Stamp = QDateTime::currentDateTime().toString( QStringLiteral( "[ MM-dd HH:mm:ss.zzz ] " ) );
        QString Body = Text;
        Body.replace( QLatin1Char( '\n' ), QLatin1Char( '\n' ) + QString( Stamp.size(), QLatin1Char( ' ' ) ) );
        log_->appendPlainText( Stamp + Body );
    }

    void RstDocumentView::goTo( const QString& Path, const int Line )
    {
        if( Path.isEmpty() || SamePath( Path, path_ ) ) {
            if( !SourceVisible() && !previewOnly_ ) mode( "split" );
            if( previewOnly_ ) return;
            editor_->goToLine( qMax( 1, Line ) );
            focusSource();
            return;
        }
        if( Line > 1 ) emit OpenAtRequested( Path, Line ); else emit OpenRequested( Path );
    }

    void RstDocumentView::GoToLine( const int Line ) { goTo( path_, Line ); }

    void RstDocumentView::Trigger( const QString& Id )
    {
        if( auto* Own = actions_.value( Id ); Own && Own->isCheckable() ) Own->trigger(); else Run( Id );
    }

    void RstDocumentView::openProjectFile( const QString& Path )
    {
        if( QFileInfo::exists( Path ) ) emit OpenRequested( Path );
    }

    // ── 미리보기 ─────────────────────────────────────────────────────────

    void RstDocumentView::showLineInPreview( const int Line )
    {
        if( previewOnly_ ) return;
        if( !PreviewVisible() ) mode( "split" );
        if( !isActive() ) activate();
        // 편집기를 그 줄에 두면 조율자가 미리보기를 같은 자리로 맞춘다.
        editor_->goToLine( qMax( 1, Line ) );
        editor_->scrollFractionalLineToViewportRatio( qMax( 1, Line ), 0.5 );
    }

    QString RstDocumentView::sourceForPage( const QString& HtmlPath ) const
    {
        // <출력>/<docname>.html ↔ <소스 뿌리>/<docname>.<확장자>. 출력 뿌리는 이 문서의 페이지에서 거꾸로 얻는다.
        const auto* Project = workspace_->projectOf( editor_ );
        if( !Project || path_.isEmpty() ) return {};
        const QString SourceRoot = QDir::cleanPath( hc::rst::toQString( Project->sourcePath ) );
        QString OwnDocument = QDir( SourceRoot ).relativeFilePath( path_ );
        if( OwnDocument.startsWith( QLatin1String( ".." ) ) || QDir::isAbsolutePath( OwnDocument ) ) return {};
        if( const auto Dot = OwnDocument.lastIndexOf( QLatin1Char( '.' ) ); Dot > OwnDocument.lastIndexOf( QLatin1Char( '/' ) ) ) OwnDocument.truncate( Dot );
        const QString OwnPage = QDir::cleanPath( preview_->RequestedUrl().toLocalFile() );
        const QString Tail = QLatin1Char( '/' ) + OwnDocument + QStringLiteral( ".html" );
        if( !OwnPage.endsWith( Tail, Qt::CaseInsensitive ) ) return {};
        const QString OutputRoot = OwnPage.left( OwnPage.size() - Tail.size() + 1 );   // '/' 로 끝난다.
        const QString Page = QDir::cleanPath( HtmlPath );
        if( !Page.startsWith( OutputRoot, Qt::CaseInsensitive ) || !Page.endsWith( QLatin1String( ".html" ), Qt::CaseInsensitive ) ) return {};
        const QString Document = Page.mid( OutputRoot.size() ).chopped( 5 );
        for( const auto& Suffix : { ".rst", ".rest", ".md", ".txt" } ) {
            const QString Candidate = SourceRoot + QLatin1Char( '/' ) + Document + QLatin1String( Suffix );
            if( QFileInfo( Candidate ).isFile() ) return Candidate;
        }
        return {};
    }

    void RstDocumentView::pageChanged()
    {
        if( !preview_->IsOtherPage() || !preview_->url().isLocalFile() ) {
            otherPageSource_.clear();
            hideBanner( Banner::OtherPage );
            emit Changed();
            return;
        }
        // 미리보기 안의 링크로 다른 페이지에 와 있다( 원본과 같은 동작 ). 내용을 누르면 그 원본의 줄로 가고,
        // 띠에서는 원본을 열거나 이 문서의 미리보기로 돌아간다.
        const QString Page = preview_->url().toLocalFile();
        otherPageSource_ = sourceForPage( Page );
        const auto* Project = workspace_->projectOf( editor_ );
        const QString Shown = otherPageSource_.isEmpty() ? QFileInfo( Page ).fileName()
                              : Project ? QDir( hc::rst::toQString( Project->sourcePath ) ).relativeFilePath( otherPageSource_ ) : QFileInfo( otherPageSource_ ).fileName();
        showBanner( Banner::OtherPage, otherPageSource_.isEmpty() ? tr( "미리보기가 다른 페이지를 보여 주고 있습니다: %1" ).arg( Shown )
                                                                  : tr( "미리보기가 다른 문서를 보여 주고 있습니다: %1" ).arg( Shown ),
                    otherPageSource_.isEmpty() ? QStringList{ tr( "돌아가기" ) } : QStringList{ tr( "소스 열기" ), tr( "돌아가기" ) } );
        emit Changed();
    }

    void RstDocumentView::openLink( const QUrl& Url )
    {
        if( Url.scheme() == QLatin1String( "https" ) || Url.scheme() == QLatin1String( "http" ) || Url.scheme() == QLatin1String( "mailto" ) )
            QDesktopServices::openUrl( Url );
        else if( Url.isLocalFile() && QFileInfo( Url.toLocalFile() ).isFile() ) emit OpenRequested( Url.toLocalFile() );
    }

    void RstDocumentView::showPreviewMenu( const QJsonObject& Report )
    {
        if( !PreviewVisible() ) return;
        const auto Global = preview_->Widget()->mapToGlobal( QPoint( qRound( Report[ "x" ].toDouble() * zoom_ ), qRound( Report[ "y" ].toDouble() * zoom_ ) ) );
        const QString Link = Report[ "link" ].toString();
        const int Line = qRound( Report[ "line" ].toDouble() );
        const QUrl Url( Link );
        const auto Key = [ this ]( const char* Id ) { return QKeySequence::fromString( bindings_.value( QString::fromLatin1( Id ) ).toString(), QKeySequence::PortableText ); };
        const bool HadFocus = preview_->HasFocus();
        QMenu Menu( this );
        Menu.setObjectName( "rstPreviewMenu" );
        Menu.addAction( tr( "복사" ), Key( "edit.copy" ), this, [ this ] { preview_->Copy(); } )->setEnabled( !Report[ "selection" ].toString().isEmpty() );
        Menu.addAction( tr( "전체 선택" ), Key( "edit.selectAll" ), this, [ this ] { preview_->SelectAll(); } );
        Menu.addSeparator();
        // 누른 자리의 원본: include 한 조각이나 링크로 넘어간 다른 문서일 수 있다.
        const QString HitPath = workspace_->previewSourcePath( Report[ "src" ].toInt( -1 ) );
        const QString Target = HitPath.isEmpty() ? ( preview_->IsOtherPage() ? otherPageSource_ : path_ ) : HitPath;
        auto* Source = Menu.addAction( SamePath( Target, path_ ) ? tr( "소스에서 이 위치 보기" ) : tr( "이 위치의 소스 열기" ), this, [ this, Target, Line ] { goTo( Target, Line ); } );
        Source->setEnabled( !Target.isEmpty() && ( Line >= 1 || !SamePath( Target, path_ ) ) && ( !previewOnly_ || !SamePath( Target, path_ ) ) );
        if( preview_->IsOtherPage() ) Menu.addAction( tr( "이 문서의 미리보기로 돌아가기" ), this, [ this ] { preview_->ReturnToRequested(); } );
        auto* Open = Menu.addAction( tr( "링크 열기" ), this, [ this, Url ] { openLink( Url ); } );
        Open->setEnabled( Url.scheme() == QLatin1String( "https" ) || Url.scheme() == QLatin1String( "http" ) || Url.isLocalFile() );
        Menu.addAction( tr( "링크 주소 복사" ), this, [ Link ] { QGuiApplication::clipboard()->setText( Link ); } )->setEnabled( !Link.isEmpty() );
        Menu.addSeparator();
        for( const auto& [ Id, Label ] : { std::pair{ "rst.zoomIn", tr( "미리보기 확대" ) }, std::pair{ "rst.zoomOut", tr( "미리보기 축소" ) },
                 std::pair{ "rst.zoomReset", tr( "미리보기 원래 크기" ) }, std::pair{ "rst.rebuild", tr( "미리보기 다시 빌드" ) },
                 std::pair{ "rst.openInBrowser", tr( "빌드한 페이지를 브라우저에서 열기" ) }, std::pair{ "rst.fullScreen", tr( "미리보기 전체 화면" ) } } )
            Menu.addAction( Label, Key( Id ), this, [ this, Command = QString::fromLatin1( Id ) ] { Run( Command ); } )->setEnabled( CanRun( QString::fromLatin1( Id ) ) );
        if( !previewOnly_ ) {
            Menu.addSeparator();
            for( const auto& [ Id, Label ] : { std::pair{ "rst.toggleToc", tr( "개요" ) }, std::pair{ "rst.syncScroll", tr( "스크롤 동기화" ) } } ) {
                auto* Own = actions_[ QString::fromLatin1( Id ) ];
                auto* Item = Menu.addAction( Label, Key( Id ), Own, [ Own ] { Own->trigger(); } );
                Item->setCheckable( true ); Item->setChecked( Own->isChecked() );
            }
        }
        const QPointer< RstDocumentView > Self( this );
        auto* Chosen = Menu.exec( Global );
        // 메뉴가 키보드를 가져갔다. 고른 항목이 다른 곳으로 옮기지 않았으면 미리보기에 돌려준다.
        if( Self && HadFocus && Chosen != Source && Chosen != Open && PreviewVisible() ) preview_->FocusInput();
    }

    void RstDocumentView::PrependSourceMenu( QMenu& Menu )
    {
        const auto Key = [ this ]( const char* Id ) { return QKeySequence::fromString( bindings_.value( QString::fromLatin1( Id ) ).toString(), QKeySequence::PortableText ); };
        // 참조( :doc: · :ref: · :term: · 경로 ) 위에서는 참조로 이동이 맨 위다.
        const auto Role = editor_->roleAtPosition( editor_->currentPosition() );
        if( !Role.role.isEmpty() ) {
            Menu.addAction( tr( "참조로 이동: %1" ).arg( Role.target.left( 48 ) ), Key( "rst.goToReference" ), this, [ this ] { Run( "rst.goToReference" ); } );
            Menu.addSeparator();
        }
        // 밑줄 친 진단 위.
        const auto Here = editor_->diagnosticsAtLine( editor_->caretLine() );
        if( !Here.isEmpty() ) {
            const auto& First = Here.first();
            auto* Title = Menu.addAction( QStringLiteral( "%1 · %2" ).arg( SeverityText( First.severity ), First.message.simplified().left( 80 ) ) );
            Title->setEnabled( false );
            Menu.addAction( tr( "진단 패널에서 보기" ), this, [ this ] { Run( "rst.problems" ); } );
            Menu.addAction( tr( "진단 메시지 복사" ), this, [ Here ] {
                QStringList Lines; for( const auto& Entry : Here ) Lines << Entry.message.simplified();
                QGuiApplication::clipboard()->setText( Lines.join( QLatin1Char( '\n' ) ) );
            } );
            Menu.addSeparator();
        }
        Menu.addAction( tr( "미리보기에서 이 위치 보기" ), this, [ this ] { Run( "rst.showInPreview" ); } );
        Menu.addAction( tr( "자동 완성" ), Key( "rst.complete" ), this, [ this ] { Run( "rst.complete" ); } )->setEnabled( !source_->isReadOnly() );
        if( Role.role.isEmpty() ) Menu.addAction( tr( "참조로 이동" ), Key( "rst.goToReference" ), this, [ this ] { Run( "rst.goToReference" ); } );
        Menu.addSeparator();
    }

    // ── 프로젝트 ─────────────────────────────────────────────────────────

    QString RstDocumentView::projectLabel() const
    {
        if( const auto* Project = workspace_->projectOf( editor_ ) ) return ProjectName( *Project );
        return workspace_->isVirtualDocument( editor_ ) ? tr( "가상 프로젝트" ) : QString();
    }

    void RstDocumentView::buildProject()
    {
        const auto* Project = workspace_->projectOf( editor_ );
        if( !Project ) return;
        if( workspace_->isProjectBuildRunning() ) {
            hc::ui::MessageDialog::information( this, tr( "빌드" ), tr( "빌드가 이미 돌고 있습니다. 끝난 뒤에 다시 요청하세요." ) );
            return;
        }
        SphinxBuildDialog Dialog( projectLabel(), hc::rst::toQString( Project->rootPath ), this );
        if( Dialog.exec() != QDialog::Accepted ) return;
        const QString ProjectId = QString::fromStdWString( Project->projectId );
        if( !workspace_->buildProject( ProjectId, Dialog.Builder(), Dialog.OutputDirectory() ) )
            hc::ui::MessageDialog::warning( this, tr( "빌드" ), tr( "빌드를 시작하지 못했습니다. 빌드 로그를 확인하세요." ) );
    }

    // ── 상태 표시줄 ──────────────────────────────────────────────────────

    QList< DocumentStatusField > RstDocumentView::StatusFields() const
    {
        // 칸은 {id, 글자, 짧은 글자, 설명, 명령, 꼭 보임, 오른쪽 묶음 시작, 피커}.
        QList< DocumentStatusField > Fields;
        const auto Percent = []( const int Value ) { return QStringLiteral( "%1 %" ).arg( Value ); };
        Fields.append( { "rst.viewMode", mode_ == QLatin1String( "split" ) ? ( previewActive_ ? tr( "나란히 · 미리보기" ) : tr( "나란히 · 소스" ) )
                                         : mode_ == QLatin1String( "source" ) ? tr( "소스" ) : tr( "미리보기" ) } );
        const QString Project = projectLabel();
        if( !Project.isEmpty() )
            Fields.append( { "rstProject", workspace_->isVirtualDocument( editor_ ) ? Project : tr( "프로젝트 %1" ).arg( Project ), Project,
                             tr( "이 문서가 속한 Sphinx 프로젝트" ), "rst.openProjectFolder", false, !SourceVisible(), true } );
        if( PreviewVisible() ) {
            Fields.append( { "rstzoom", tr( "미리보기 %1" ).arg( Percent( PreviewZoomPercent() ) ), Percent( PreviewZoomPercent() ), tr( "미리보기 배율" ), "rst.zoomSet", false, false, true } );
            const auto* Environment = workspace_->pythonEnvironment();
            const QString Text = projectBuilding_ ? tr( "프로젝트 빌드 중" ) : !Environment->isReady() ? tr( "미리보기 없음" )
                                 : !previewStatus_.isEmpty() ? previewStatus_ : previewFailed_ ? tr( "미리보기 실패" ) : previewShown_ ? tr( "미리보기 최신" ) : tr( "미리보기 대기" );
            Fields.append( { "rstPreview", Text, {}, tr( "미리보기 빌드 상태" ), "rst.rebuild", previewFailed_ || projectBuilding_, false, true } );
        }
        if( errorCount_ + warningCount_ > 0 )
            Fields.append( { "rstDiag", tr( "오류 %1 · 경고 %2" ).arg( errorCount_ ).arg( warningCount_ ), QStringLiteral( "%1 · %2" ).arg( errorCount_ ).arg( warningCount_ ),
                             tr( "이 문서의 진단" ), "rst.problems", true, false, true } );
        Fields.append( { "rstEnv", environmentText(), {}, tr( "Python · Sphinx · Esbonio 환경" ), "rst.configureEnvironment",
                         !workspace_->pythonEnvironment()->isReady(), false, true } );
        return Fields;
    }

    void RstDocumentView::OpenStatusPicker( const QString& Id, QWidget* Anchor, std::function< void() > Done )
    {
        if( !Anchor ) return;
        const QPointer< RstDocumentView > Self( this );
        const auto Command = [ Self, Done ]( const char* Name ) {
            return [ Self, Done, Command = QString::fromLatin1( Name ) ] { if( Self ) Self->Run( Command ); if( Done ) Done(); };
        };
        const auto Key = [ this ]( const char* Name ) {
            return QKeySequence::fromString( bindings_.value( QString::fromLatin1( Name ) ).toString(), QKeySequence::PortableText ).toString( QKeySequence::NativeText );
        };
        auto* Environment = workspace_->pythonEnvironment();
        if( Id == "rstProject" ) {
            const auto* Project = workspace_->projectOf( editor_ );
            auto* Popup = DocumentStatusPopup::Open( Anchor, Project ? projectLabel() : tr( "가상 프로젝트" ) );
            if( Project ) {
                Popup->AddNote( Popup->Root(), tr( "conf.py: %1" ).arg( QDir::toNativeSeparators( hc::rst::toQString( Project->confPath ) ) ) );
                const QString Theme = QString::fromStdString( hc::rst::readHtmlTheme( Project->confPath ) );
                Popup->AddNote( Popup->Root(), tr( "테마: %1 · 언어 서버: %2" ).arg( Theme.isEmpty() ? QStringLiteral( "alabaster" ) : Theme,
                    workspace_->isLanguageServerRunning( QString::fromStdWString( Project->projectId ) ) ? tr( "실행 중" ) : tr( "꺼짐" ) ) );
                Popup->AddAction( Popup->Root(), tr( "프로젝트 빌드…" ), {}, Command( "rst.buildProject" ) )->setEnabled( CanRun( "rst.buildProject" ) );
                Popup->AddAction( Popup->Root(), tr( "conf.py 열기" ), {}, Command( "rst.openConf" ) );
                Popup->AddAction( Popup->Root(), tr( "프로젝트 폴더 열기" ), {}, Command( "rst.openProjectFolder" ) );
                Popup->AddAction( Popup->Root(), tr( "빌드 결과 폴더 열기" ), {}, Command( "rst.openBuildFolder" ) );
            } else {
                Popup->AddNote( Popup->Root(), tr( "conf.py가 없는 단독 문서입니다. 임시 폴더에 최소 설정을 만들어 미리보기합니다. 문서가 있는 폴더에는 아무것도 쓰지 않습니다." ) );
                Popup->AddAction( Popup->Root(), tr( "새 Sphinx 프로젝트…" ), {}, [ Self, Done ] { if( Self ) emit Self->WorkspaceCommandRequested( QStringLiteral( "file.newSphinxProject" ) ); if( Done ) Done(); } );
            }
            Popup->AddAction( Popup->Root(), tr( "프로젝트 다시 찾기" ), {}, Command( "rst.rescanProjects" ) );
            Popup->ShowAbove(); return;
        }
        if( Id == "rstDiag" ) {
            auto* Popup = DocumentStatusPopup::Open( Anchor, tr( "진단 — 오류 %1 · 경고 %2" ).arg( errorCount_ ).arg( warningCount_ ) );
            QList< QPair< QString, QString > > Items; QList< int > Lines;
            for( const auto& Entry : shownDiagnostics_ ) {
                if( !SamePath( Entry.path, path_ ) ) continue;
                Items.append( { Entry.message.simplified().left( 120 ), tr( "%1 · 줄 %2" ).arg( SeverityText( Entry.severity ) ).arg( Entry.line ) } );
                Lines << Entry.line;
            }
            if( Items.isEmpty() ) Popup->AddNote( Popup->Root(), tr( "이 문서에는 진단이 없습니다." ) );
            else Popup->AddList( Popup->Root(), Items, -1, [ Self, Popup, Lines, Done ]( const int Index ) {
                Popup->close();
                if( Self ) Self->goTo( Self->path_, Lines.value( Index, 1 ) );
                if( Done ) Done();
            }, nullptr );
            Popup->AddAction( Popup->Root(), tr( "진단 패널 열기" ), Key( "rst.toggleProblems" ), Command( "rst.problems" ) );
            Popup->AddAction( Popup->Root(), tr( "미리보기 다시 빌드" ), Key( "rst.rebuild" ), Command( "rst.rebuild" ) );
            Popup->ShowAbove(); return;
        }
        if( Id == "rstzoom" ) {
            const int Zoom = PreviewZoomPercent();
            auto* Popup = DocumentStatusPopup::Open( Anchor, tr( "미리보기 배율 — %1 %" ).arg( Zoom ) );
            const QList< int > Steps{ 75, 90, 100, 110, 125, 150 };
            QStringList Labels; for( const int Step : Steps ) Labels << tr( "%1 %" ).arg( Step );
            Popup->AddChips( Popup->Root(), Labels, Steps.indexOf( Zoom ), [ Self, Steps, Done ]( const int Index ) { if( Self ) Self->SetPreviewZoom( Steps.value( Index, 100 ) ); if( Done ) Done(); } );
            Popup->AddAction( Popup->Root(), tr( "원래 크기" ), QStringLiteral( "100 %" ), Command( "rst.zoomReset" ) );
            Popup->AddAction( Popup->Root(), tr( "배율 입력…" ), tr( "%1–%2 %" ).arg( MinZoomPercent ).arg( MaxZoomPercent ), [ Self ] { if( Self ) Self->Run( "rst.zoomSet" ); } );
            Popup->ShowAbove(); return;
        }
        if( Id == "rstPreview" ) {
            auto* Popup = DocumentStatusPopup::Open( Anchor, tr( "미리보기" ) );
            if( previewFailed_ ) Popup->AddWarning( Popup->Root(), tr( "마지막 빌드가 실패했습니다. 빌드 로그에서 원인을 확인하세요." ) );
            Popup->AddAction( Popup->Root(), tr( "미리보기 다시 빌드" ), Key( "rst.rebuild" ), Command( "rst.rebuild" ) );
            Popup->AddAction( Popup->Root(), tr( "빌드 로그 보기" ), {}, Command( "rst.showLog" ) );
            if( projectBuilding_ ) Popup->AddAction( Popup->Root(), tr( "프로젝트 빌드 취소" ), {}, Command( "rst.cancelBuild" ) );
            Popup->AddAction( Popup->Root(), tr( "빌드한 페이지를 브라우저에서 열기" ), {}, Command( "rst.openInBrowser" ) )->setEnabled( CanRun( "rst.openInBrowser" ) );
            Popup->ShowAbove(); return;
        }
        if( Id == "rstEnv" ) {
            auto* Popup = DocumentStatusPopup::Open( Anchor, tr( "Python 환경 — %1" ).arg( Environment->stateText() ) );
            if( Environment->isReady() ) Popup->AddNote( Popup->Root(), tr( "구성일: %1" ).arg( Environment->configuredDateText() ) );
            else if( !Environment->lastError().isEmpty() ) Popup->AddWarning( Popup->Root(), Environment->lastError() );
            Popup->AddNote( Popup->Root(), Environment->uvDescription() );
            if( Environment->isBusy() ) Popup->AddAction( Popup->Root(), tr( "구성 취소" ), {}, [ Environment, Done ] { Environment->cancel(); if( Done ) Done(); } );
            else Popup->AddAction( Popup->Root(), Environment->isReady() ? tr( "재구성…" ) : tr( "환경 구성" ), {}, Command( "rst.configureEnvironment" ) );
            Popup->AddAction( Popup->Root(), tr( "언어 서버 다시 시작" ), {}, Command( "rst.restartServer" ) )->setEnabled( CanRun( "rst.restartServer" ) );
            Popup->AddAction( Popup->Root(), tr( "구성 · 빌드 로그 보기" ), {}, Command( "rst.showLog" ) );
            Popup->AddAction( Popup->Root(), tr( "환경 폴더 열기" ), {}, Command( "rst.openEnvironmentFolder" ) );
            Popup->AddAction( Popup->Root(), tr( "설정 열기…" ), {}, [ Self, Done ] { if( Self ) emit Self->WorkspaceCommandRequested( QStringLiteral( "app.settings.rst" ) ); if( Done ) Done(); } );
            Popup->ShowAbove(); return;
        }
    }

    QJsonObject RstDocumentView::State() const
    {
        const auto* Environment = workspace_->pythonEnvironment();
        return { { "mode", mode_ },
                 { "active", isActive() },
                 { "project", projectLabel() },
                 { "projectId", workspace_->projectIdOf( editor_ ) },
                 { "virtual", workspace_->isVirtualDocument( editor_ ) },
                 { "environment", Environment->stateText() },
                 { "environmentReady", Environment->isReady() },
                 { "previewStatus", previewStatus_ },
                 { "previewFailed", previewFailed_ },
                 { "previewShown", previewShown_ },
                 { "previewUrl", preview_->url().toString() },
                 { "previewOnly", previewOnly_ },
                 { "zoom", zoom_ },
                 { "errors", errorCount_ },
                 { "warnings", warningCount_ },
                 { "diagnostics", int( shownDiagnostics_.size() ) },
                 { "outlineVisible", outlinePanel_->isVisible() },
                 { "outlineHeadings", outlineDocument_->topLevelItemCount() },
                 { "problemsVisible", problems_->isVisible() },
                 { "banner", bannerKind_ == Banner::None ? QString() : banner_->text() } };
    }
}
