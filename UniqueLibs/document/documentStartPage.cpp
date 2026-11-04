#include "stdafx.h"
#include "documentStartPage.hpp"

/// 모듈

/// UIs

/// 기타 헤더파일

/// 고유 라이브러리
#include "UniqueLibs/qtMessageDialog.hpp"
#include "core/ThemeManager.h"
#include "core/ToolbarIcons.h"

/// 공통 라이브러리

/// 외부 라이브러리
#include <fmstyle/ThemeManager.h>

/// 선언 헤더파일들

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QAbstractButton>
#include <QAction>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QDir>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QMenu>
#include <QMimeData>
#include <QPainter>
#include <QResizeEvent>
#include <QShowEvent>
#include <QUrl>
#include <QVBoxLayout>
#include <algorithm>

DECLARE_CMNLIBSV2_NAMESPACE

namespace hc::document
{
    namespace
    {
        using fm::style::Token;

        constexpr int    Gutter   = 16;      // 좁은 창에서 상자 양옆에 남길 여백
        constexpr int    IconSize = 16;
        constexpr qreal  Radius   = 5.0;

        QFont SmallFont( QFont Font )
        {
            // 시안의 <small> · 12 px 머리(본문 13 px 기준)에 맞춘 비율.
            if( Font.pointSizeF() > 0 ) Font.setPointSizeF( Font.pointSizeF() * 0.92 );
            else if( Font.pixelSize() > 0 ) Font.setPixelSize( qMax( 1, qRound( Font.pixelSize() * 0.92 ) ) );
            return Font;
        }

        // FmStyle 의 항목 마우스 올림과 같은 규칙: 바탕에 글자색을 조금 섞는다.
        QColor Blend( const QColor& Base, const QColor& Tint, qreal Amount )
        {
            const auto Mix = [ Amount ]( float From, float To ) { return float( From + ( To - From ) * Amount ); };
            return QColor::fromRgbF( Mix( Base.redF(), Tint.redF() ), Mix( Base.greenF(), Tint.greenF() ), Mix( Base.blueF(), Tint.blueF() ) );
        }

        // 위 · 아래 · Enter · 숫자는 시작 화면이 처리하도록 부모로 넘긴다.
        bool Forwarded( QKeyEvent* Event )
        {
            if( ( Event->modifiers() & ~Qt::KeypadModifier ) != Qt::NoModifier ) return false;
            const int Key = Event->key();
            if( Key != Qt::Key_Up && Key != Qt::Key_Down && Key != Qt::Key_Return && Key != Qt::Key_Enter && ( Key < Qt::Key_1 || Key > Qt::Key_9 ) )
                return false;
            Event->ignore();
            return true;
        }

        /// [열기… Ctrl+O] 처럼 아이콘 · 글자 · 흐린 단축키를 한 단추에 그린다(시안 .start .acts). Link 는 '모두 보기'.
        class StartButton final : public QAbstractButton
        {
        public:
            enum class Kind { Action, Link };

            StartButton( Kind Type, const QString& Text, QString Keys, QWidget* Parent )
                : QAbstractButton( Parent ), kind_( Type ), keys_( std::move( Keys ) )
            {
                setText( Text );
                setAccessibleName( Text );
                setFocusPolicy( Qt::StrongFocus );
                setAttribute( Qt::WA_Hover );
                if( Type == Kind::Link ) setCursor( Qt::PointingHandCursor );
                if( !keys_.isEmpty() ) setToolTip( QStringLiteral( "%1 (%2)" ).arg( Text, keys_ ) );
            }

            QSize sizeHint() const override
            {
                const QFontMetrics Metrics( font() );
                if( kind_ == Kind::Link ) return { Metrics.horizontalAdvance( text() ) + 2, Metrics.height() };
                int Width = 12 + IconSize + 6 + Metrics.horizontalAdvance( text() ) + 12;
                if( !keys_.isEmpty() ) Width += 6 + QFontMetrics( SmallFont( font() ) ).horizontalAdvance( keys_ );
                return { Width, qMax( IconSize, Metrics.height() ) + 12 + 2 };
            }
            QSize minimumSizeHint() const override { return sizeHint(); }

        protected:
            void keyPressEvent( QKeyEvent* Event ) override
            {
                if( !Forwarded( Event ) ) QAbstractButton::keyPressEvent( Event );
            }

            void paintEvent( QPaintEvent* ) override
            {
                QPainter Painter( this );
                Painter.setRenderHint( QPainter::Antialiasing );
                const auto& Colors = fm::style::themeColorsFor( this );
                if( !isEnabled() ) Painter.setOpacity( 0.45 );
                if( kind_ == Kind::Link ) {
                    auto Font = font();
                    Font.setUnderline( underMouse() || hasFocus() );
                    Painter.setFont( Font );
                    Painter.setPen( Colors[ Token::AccentFg ] );
                    Painter.drawText( rect(), Qt::AlignRight | Qt::AlignVCenter, text() );
                    return;
                }
                const qreal Amount = isDown() ? 0.09 : underMouse() ? 0.045 : 0.0;
                Painter.setPen( QPen( hasFocus() ? Colors[ Token::Focus ] : Colors[ Token::Line ], 1 ) );
                Painter.setBrush( Blend( Colors[ Token::Win ], Colors[ Token::Fg ], Amount ) );
                Painter.drawRoundedRect( QRectF( rect() ).adjusted( 0.5, 0.5, -0.5, -0.5 ), Radius, Radius );

                int X = 12;
                icon().paint( &Painter, QRect( X, ( height() - IconSize ) / 2, IconSize, IconSize ), Qt::AlignCenter, isEnabled() ? QIcon::Normal : QIcon::Disabled );
                X += IconSize + 6;
                const QFontMetrics Metrics( font() );
                const int          Baseline = ( height() + Metrics.ascent() - Metrics.descent() ) / 2;
                Painter.setFont( font() );
                Painter.setPen( Colors[ Token::Fg ] );
                Painter.drawText( X, Baseline, text() );
                if( keys_.isEmpty() ) return;
                X += Metrics.horizontalAdvance( text() ) + 6;
                Painter.setFont( SmallFont( font() ) );
                Painter.setPen( Colors[ Token::Fg3 ] );
                Painter.drawText( X, Baseline, keys_ );
            }

        private:
            Kind    kind_;
            QString keys_;
        };

        /// 최근 항목 한 행: 번호(흐림) · 굵은 이름 · 흐린 설명 · 오른쪽 날짜(시안 .start .rl).
        class StartRow final : public QAbstractButton
        {
        public:
            explicit StartRow( QWidget* Parent )
                : QAbstractButton( Parent )
            {
                setObjectName( QStringLiteral( "startRecentRow" ) );
                setFocusPolicy( Qt::StrongFocus );
                setAttribute( Qt::WA_Hover );
            }

            void SetDocument( int Number, const RecentDocument& Document, bool Missing )
            {
                number_   = Number;
                document_ = Document;
                missing_  = Missing;
                const auto Name = QFileInfo( Document.path_ ).fileName();
                setText( Name );
                setToolTip( QDir::toNativeSeparators( Document.path_ ) );
                setAccessibleName( Name );
                setAccessibleDescription( RecentDescription( Document ) );
                update();
            }

            void SetMissing( bool Missing )
            {
                if( missing_ == Missing ) return;
                missing_ = Missing;
                update();
            }

            QSize sizeHint() const override
            {
                auto Bold = font();
                Bold.setWeight( QFont::DemiBold );
                return { 320, qMax( QFontMetrics( Bold ).height(), QFontMetrics( font() ).height() ) + 14 };
            }

        protected:
            void keyPressEvent( QKeyEvent* Event ) override
            {
                if( !Forwarded( Event ) ) QAbstractButton::keyPressEvent( Event );
            }

            void paintEvent( QPaintEvent* ) override
            {
                QPainter Painter( this );
                Painter.setRenderHint( QPainter::Antialiasing );
                const auto& Colors = fm::style::themeColorsFor( this );
                if( underMouse() || isDown() || hasFocus() ) {
                    Painter.setPen( hasFocus() ? QPen( Colors[ Token::Focus ], 1 ) : QPen( Qt::NoPen ) );
                    Painter.setBrush( Blend( Colors[ Token::Surface ], Colors[ Token::Fg ], isDown() ? 0.09 : 0.045 ) );
                    Painter.drawRoundedRect( QRectF( rect() ).adjusted( 0.5, 0.5, -0.5, -0.5 ), Radius, Radius );
                }
                if( missing_ ) Painter.setOpacity( 0.5 );

                auto Bold = font();
                Bold.setWeight( QFont::DemiBold );
                const auto         Small = SmallFont( font() );
                const QFontMetrics BaseMetrics( font() ), BoldMetrics( Bold ), SmallMetrics( Small );
                const QRect        Inner    = rect().adjusted( 8, 0, -8, 0 );
                const int          Baseline = Inner.top() + ( Inner.height() + BoldMetrics.ascent() - BoldMetrics.descent() ) / 2;

                Painter.setFont( font() );
                Painter.setPen( Colors[ Token::Fg3 ] );
                Painter.drawText( Inner.left(), Baseline, QString::number( number_ ) );

                const auto Date      = RecentDateText( document_.openedAt_, QDateTime::currentDateTime() );
                const int  DateWidth = SmallMetrics.horizontalAdvance( Date );
                Painter.setFont( Small );
                Painter.drawText( Inner.right() + 1 - DateWidth, Baseline, Date );

                int        X     = Inner.left() + 22 + 8;
                const int  Right = Inner.right() + 1 - DateWidth - 8;
                const auto Name  = BoldMetrics.elidedText( QFileInfo( document_.path_ ).fileName(), Qt::ElideMiddle, qMax( 0, Right - X ) );
                Painter.setFont( Bold );
                Painter.setPen( Colors[ Token::Fg ] );
                Painter.drawText( X, Baseline, Name );
                X += BoldMetrics.horizontalAdvance( Name ) + BaseMetrics.horizontalAdvance( QLatin1Char( ' ' ) );

                auto Description = RecentDescription( document_ );
                if( missing_ ) Description += QLatin1Char( ' ' ) + DocumentStartPage::tr( "(찾을 수 없음)" );
                if( X >= Right ) return;
                Painter.setFont( Small );
                Painter.setPen( Colors[ Token::Fg3 ] );
                Painter.drawText( X, Baseline, SmallMetrics.elidedText( Description, Qt::ElideRight, Right - X ) );
            }

        private:
            int            number_  = 0;
            RecentDocument document_;
            bool           missing_ = false;
        };
    }

    DocumentStartPage::DocumentStartPage( RecentDocumentStore* Store, QWidget* Parent )
        : QWidget( Parent ), store_( Store ), probe_( new RecentPathProbe( this ) )
    {
        setObjectName( QStringLiteral( "documentStartPage" ) );
        setAcceptDrops( true );
        // 빈 곳을 누르면 숫자 키를 받을 수 있게 하되, Tab 순서에는 넣지 않는다.
        setFocusPolicy( Qt::ClickFocus );

        box_ = new QWidget( this );
        box_->setObjectName( QStringLiteral( "startBox" ) );
        box_->installEventFilter( this );
        auto* Layout = new QVBoxLayout( box_ );
        Layout->setContentsMargins( 0, 0, 0, 0 );
        Layout->setSpacing( 0 );

        title_ = new QLabel( QStringLiteral( "HyperEditor" ), box_ );
        title_->setObjectName( QStringLiteral( "startTitle" ) );
        hint_ = new QLabel( tr( "문서를 끌어다 놓거나 열기 · 새 문서를 선택하십시오." ), box_ );
        hint_->setObjectName( QStringLiteral( "startHint" ) );
        hint_->setWordWrap( true );
        Layout->addWidget( title_ );
        Layout->addSpacing( 4 );
        Layout->addWidget( hint_ );
        Layout->addSpacing( 16 );

        auto* Actions = new QHBoxLayout();
        Actions->setSpacing( 8 );
        open_ = new StartButton( StartButton::Kind::Action, tr( "열기…" ), QKeySequence( Qt::CTRL | Qt::Key_O ).toString( QKeySequence::NativeText ), box_ );
        open_->setObjectName( QStringLiteral( "startOpen" ) );
        new_ = new StartButton( StartButton::Kind::Action, tr( "새 문서" ), QKeySequence( Qt::CTRL | Qt::Key_N ).toString( QKeySequence::NativeText ), box_ );
        new_->setObjectName( QStringLiteral( "startNew" ) );
        Actions->addWidget( open_ );
        Actions->addWidget( new_ );
        Actions->addStretch( 1 );
        Layout->addLayout( Actions );
        Layout->addSpacing( 18 );

        auto* Header = new QHBoxLayout();
        Header->setSpacing( 8 );
        header_ = new QLabel( tr( "최근 항목" ), box_ );
        header_->setObjectName( QStringLiteral( "startRecentHeader" ) );
        showAll_ = new StartButton( StartButton::Kind::Link, tr( "모두 보기" ), {}, box_ );
        showAll_->setObjectName( QStringLiteral( "startShowAll" ) );
        Header->addWidget( header_ );
        Header->addStretch( 1 );
        Header->addWidget( showAll_ );
        Layout->addLayout( Header );
        Layout->addSpacing( 4 );

        for( int Index = 0; Index < MaxRows; ++Index ) {
            auto* Row = new StartRow( box_ );
            Row->hide();
            Layout->addWidget( Row );
            rows_.append( Row );
            connect( Row, &QAbstractButton::clicked, this, [ this, Index ] { openRow( Index ); } );
        }
        empty_ = new QLabel( tr( "최근 항목 없음" ), box_ );
        empty_->setObjectName( QStringLiteral( "startRecentEmpty" ) );
        empty_->setContentsMargins( 8, 7, 8, 7 );
        Layout->addWidget( empty_ );

        connect( open_, &QAbstractButton::clicked, this, &DocumentStartPage::OpenFileRequested );
        connect( new_, &QAbstractButton::clicked, this, &DocumentStartPage::NewDocumentRequested );
        connect( showAll_, &QAbstractButton::clicked, this, &DocumentStartPage::ShowAllRequested );
        connect( probe_, &RecentPathProbe::Checked, this, &DocumentStartPage::refreshMissing );
        connect( &fm::style::ThemeManager::instance(), &fm::style::ThemeManager::changed, this, &DocumentStartPage::applyTheme );
        // 도구 모음 아이콘 색은 HyperEditor 테마를 따른다.
        connect( &::ThemeManager::instance(), &::ThemeManager::themeChanged, this, &DocumentStartPage::applyTheme );
        if( Store ) connect( Store, &RecentDocumentStore::Changed, this, &DocumentStartPage::rebuild );

        applyFonts();
        applyTheme();
        rebuild();
    }

    DocumentStartPage::~DocumentStartPage() = default;

    int DocumentStartPage::RowCount() const
    {
        return int( documents_.size() );
    }

    QSize DocumentStartPage::sizeHint() const
    {
        return { BoxWidth + 4 * Gutter, box_->sizeHint().height() + 4 * Gutter };
    }

    QSize DocumentStartPage::minimumSizeHint() const
    {
        return { 240, 160 };
    }

    QMenu* DocumentStartPage::CreateContextMenu( const QPoint& Pos, QWidget* Parent )
    {
        auto* Menu = new QMenu( Parent ? Parent : this );
        Menu->setObjectName( QStringLiteral( "startContextMenu" ) );
        const QPointer< RecentDocumentStore > Store = store_;

        if( const int Row = rowAt( Pos ); Row >= 0 ) {
            const auto Document = documents_[ Row ];
            auto*      Open     = Menu->addAction( tr( "열기" ) );
            connect( Open, &QAction::triggered, this, [ this, Document ] { emit OpenRequested( Document ); } );
            Menu->addSeparator();
            auto* Pin = Menu->addAction( Document.pinned_ ? tr( "고정 해제" ) : tr( "고정" ) );
            Pin->setEnabled( !Store.isNull() );
            connect( Pin, &QAction::triggered, this, [ Store, Document ] {
                if( Store ) Store->Pin( Document.Key(), !Document.pinned_ );
            } );
            auto* Remove = Menu->addAction( tr( "목록에서 제거" ) );
            Remove->setEnabled( !Store.isNull() );
            connect( Remove, &QAction::triggered, this, [ Store, Document ] {
                if( Store ) Store->Remove( Document.Key() );
            } );
            Menu->addSeparator();
            auto* Copy = Menu->addAction( tr( "경로 복사" ) );
            connect( Copy, &QAction::triggered, this, [ Document ] { QGuiApplication::clipboard()->setText( QDir::toNativeSeparators( Document.path_ ) ); } );
            return Menu;
        }

        // 메뉴 안에서만 쓰는 단축키 표시. 실제 명령은 창의 동작이 가진다.
        auto* Open = Menu->addAction( ToolbarIcons::iconByName( QStringLiteral( "open" ) ), tr( "열기…" ) );
        Open->setShortcut( QKeySequence( Qt::CTRL | Qt::Key_O ) );
        Open->setShortcutContext( Qt::WidgetShortcut );
        connect( Open, &QAction::triggered, this, &DocumentStartPage::OpenFileRequested );
        auto* New = Menu->addAction( ToolbarIcons::iconByName( QStringLiteral( "new-file" ) ), tr( "새 문서" ) );
        New->setShortcut( QKeySequence( Qt::CTRL | Qt::Key_N ) );
        New->setShortcutContext( Qt::WidgetShortcut );
        connect( New, &QAction::triggered, this, &DocumentStartPage::NewDocumentRequested );
        Menu->addSeparator();
        auto*      Clear    = Menu->addAction( tr( "최근 항목 지우기" ) );
        const auto Entries  = Store ? Store->Entries() : QList< RecentDocument >();
        Clear->setEnabled( std::any_of( Entries.cbegin(), Entries.cend(), []( const RecentDocument& Entry ) { return !Entry.pinned_; } ) );
        connect( Clear, &QAction::triggered, this, &DocumentStartPage::confirmClear );
        return Menu;
    }

    bool DocumentStartPage::eventFilter( QObject* Watched, QEvent* Event )
    {
        // 행을 보이거나 숨기면 상자 높이가 바뀌므로 가운데 배치를 다시 계산한다.
        if( Watched == box_ && Event->type() == QEvent::LayoutRequest )
            QMetaObject::invokeMethod( this, &DocumentStartPage::relayout, Qt::QueuedConnection );
        return QWidget::eventFilter( Watched, Event );
    }

    void DocumentStartPage::changeEvent( QEvent* Event )
    {
        QWidget::changeEvent( Event );
        if( !empty_ ) return;      // 생성 도중(자식을 다 만들기 전)에 온 변경은 생성자 끝에서 한꺼번에 반영한다
        if( Event->type() == QEvent::FontChange ) applyFonts();
        else if( Event->type() == QEvent::PaletteChange || Event->type() == QEvent::StyleChange ) applyTheme();
    }

    void DocumentStartPage::contextMenuEvent( QContextMenuEvent* Event )
    {
        auto* Menu = CreateContextMenu( Event->pos(), this );
        Menu->setAttribute( Qt::WA_DeleteOnClose );
        Menu->popup( Event->globalPos() );
        Event->accept();
    }

    void DocumentStartPage::dragEnterEvent( QDragEnterEvent* Event )
    {
        if( localFiles( Event->mimeData() ).isEmpty() ) { Event->ignore(); return; }
        Event->acceptProposedAction();
    }

    void DocumentStartPage::dragMoveEvent( QDragMoveEvent* Event )
    {
        if( localFiles( Event->mimeData() ).isEmpty() ) { Event->ignore(); return; }
        Event->acceptProposedAction();
    }

    void DocumentStartPage::dropEvent( QDropEvent* Event )
    {
        const auto Paths = localFiles( Event->mimeData() );
        if( Paths.isEmpty() ) { Event->ignore(); return; }
        Event->acceptProposedAction();
        emit FilesDropped( Paths );
    }

    void DocumentStartPage::hideEvent( QHideEvent* Event )
    {
        probe_->Cancel();
        QWidget::hideEvent( Event );
    }

    void DocumentStartPage::keyPressEvent( QKeyEvent* Event )
    {
        if( ( Event->modifiers() & ~Qt::KeypadModifier ) == Qt::NoModifier ) {
            const int Key  = Event->key();
            const int Last = int( documents_.size() ) - 1;
            if( Key >= Qt::Key_1 && Key < Qt::Key_1 + MaxRows && Key - Qt::Key_1 <= Last ) {
                openRow( Key - Qt::Key_1 );
                return;
            }
            if( ( Key == Qt::Key_Down || Key == Qt::Key_Up ) && Last >= 0 ) {
                const int Current = focusedRow();
                const int Step    = Key == Qt::Key_Down ? 1 : -1;
                focusRow( Current < 0 ? ( Step > 0 ? 0 : Last ) : qBound( 0, Current + Step, Last ) );
                return;
            }
            if( Key == Qt::Key_Return || Key == Qt::Key_Enter ) {
                // 행 · [열기…] · [새 문서] · '모두 보기' 모두 단추이므로 포커스를 가진 단추를 누른다.
                auto* Button = qobject_cast< QAbstractButton* >( focusWidget() );
                if( Button && isAncestorOf( Button ) && Button->isVisible() && Button->isEnabled() ) {
                    Button->click();
                    return;
                }
            }
        }
        QWidget::keyPressEvent( Event );
    }

    void DocumentStartPage::paintEvent( QPaintEvent* )
    {
        QPainter Painter( this );
        Painter.fillRect( rect(), fm::style::themeColorsFor( this )[ Token::Surface ] );
    }

    void DocumentStartPage::resizeEvent( QResizeEvent* Event )
    {
        QWidget::resizeEvent( Event );
        relayout();
    }

    void DocumentStartPage::showEvent( QShowEvent* Event )
    {
        QWidget::showEvent( Event );
        // 다시 보일 때마다 새로 확인한다(그 사이에 파일이 지워지거나 돌아올 수 있다).
        probe_->Reset();
        rebuild();
    }

    void DocumentStartPage::rebuild()
    {
        const int Focused = focusedRow();
        documents_.clear();
        if( store_ ) documents_ = store_->Entries().mid( 0, MaxRows );

        QStringList Paths;
        for( int Index = 0; Index < rows_.size(); ++Index ) {
            auto* Row = static_cast< StartRow* >( rows_[ Index ] );
            if( Index >= documents_.size() ) {
                Row->hide();
                continue;
            }
            const auto& Document = documents_[ Index ];
            Row->SetDocument( Index + 1, Document, probe_->StateOf( Document.path_ ) == RecentPathProbe::State::Missing );
            Row->show();
            Paths.append( Document.path_ );
        }
        empty_->setVisible( documents_.isEmpty() );
        showAll_->setVisible( !documents_.isEmpty() );
        if( Focused >= 0 && !documents_.isEmpty() ) focusRow( qMin( Focused, int( documents_.size() ) - 1 ) );
        if( isVisible() ) probe_->Check( Paths );
        relayout();
    }

    void DocumentStartPage::relayout()
    {
        const int Width  = qMax( 0, qMin( int( BoxWidth ), width() - 2 * Gutter ) );
        const int Fitted = box_->heightForWidth( Width );
        const int Height = Fitted > 0 ? Fitted : box_->sizeHint().height();
        box_->setGeometry( ( width() - Width ) / 2, qMax( Gutter, ( height() - Height ) / 2 ), Width, Height );
    }

    void DocumentStartPage::applyTheme()
    {
        const auto& Colors = fm::style::themeColorsFor( this );
        const auto  Tint   = [ &Colors ]( QWidget* Widget, Token Which ) {
            auto Palette = Widget->palette();
            Palette.setColor( QPalette::WindowText, Colors[ Which ] );
            Widget->setPalette( Palette );
        };
        Tint( title_, Token::Fg );
        Tint( hint_, Token::Fg3 );
        Tint( header_, Token::Fg3 );
        Tint( empty_, Token::Fg3 );
        open_->setIcon( ToolbarIcons::iconByName( QStringLiteral( "open" ) ) );
        new_->setIcon( ToolbarIcons::iconByName( QStringLiteral( "new-file" ) ) );
        update();
        for( auto* Child : box_->findChildren< QWidget* >() ) Child->update();
    }

    void DocumentStartPage::applyFonts()
    {
        auto Title = font();
        Title.setPixelSize( 20 );
        Title.setWeight( QFont::DemiBold );
        title_->setFont( Title );
        const auto Small = SmallFont( font() );
        header_->setFont( Small );
        showAll_->setFont( Small );
        relayout();
    }

    void DocumentStartPage::openRow( int Index )
    {
        if( Index < 0 || Index >= documents_.size() ) return;
        // 받는 쪽이 기록을 갱신하면 행이 다시 채워지므로 값을 먼저 꺼낸다.
        const auto Document = documents_[ Index ];
        emit OpenRequested( Document );
    }

    void DocumentStartPage::focusRow( int Index )
    {
        if( Index >= 0 && Index < rows_.size() ) rows_[ Index ]->setFocus( Qt::TabFocusReason );
    }

    int DocumentStartPage::focusedRow() const
    {
        for( int Index = 0; Index < rows_.size(); ++Index )
            if( rows_[ Index ]->hasFocus() ) return Index;
        return -1;
    }

    int DocumentStartPage::rowAt( const QPoint& Pos ) const
    {
        const auto Local = box_->mapFromParent( Pos );
        for( int Index = 0; Index < documents_.size() && Index < rows_.size(); ++Index )
            if( !rows_[ Index ]->isHidden() && rows_[ Index ]->geometry().contains( Local ) ) return Index;
        return -1;
    }

    void DocumentStartPage::refreshMissing( const QString& Path )
    {
        const auto Key     = RecentKey( Path );
        const bool Missing = probe_->StateOf( Path ) == RecentPathProbe::State::Missing;
        for( int Index = 0; Index < documents_.size() && Index < rows_.size(); ++Index )
            if( documents_[ Index ].Key() == Key ) static_cast< StartRow* >( rows_[ Index ] )->SetMissing( Missing );
    }

    void DocumentStartPage::confirmClear()
    {
        if( !store_ ) return;
        if( confirm_ ) {
            confirm_->raise();
            confirm_->activateWindow();
            return;
        }
        auto* Box = new hc::ui::MessageDialog( QMessageBox::Question, tr( "최근 항목 지우기" ),
                                               tr( "고정한 항목을 제외하고 최근 항목을 지웁니다. 계속할까요?" ),
                                               QMessageBox::Yes | QMessageBox::No, this );
        Box->setObjectName( QStringLiteral( "startClearConfirm" ) );
        Box->setAttribute( Qt::WA_DeleteOnClose );
        Box->button( QMessageBox::Yes )->setText( tr( "지우기" ) );
        Box->button( QMessageBox::No )->setText( tr( "취소" ) );
        Box->setDefaultButton( QMessageBox::No );
        connect( Box, &QMessageBox::finished, this, [ this ]( int Result ) {
            if( Result == QMessageBox::Yes && store_ ) store_->Clear( false );
        } );
        confirm_ = Box;
        Box->open();
    }

    QStringList DocumentStartPage::localFiles( const QMimeData* Data )
    {
        QStringList Paths;
        if( !Data || !Data->hasUrls() ) return Paths;
        for( const auto& Url : Data->urls() ) {
            if( !Url.isLocalFile() ) continue;
            const auto Path = Url.toLocalFile();
            if( !Path.isEmpty() ) Paths.append( QDir::toNativeSeparators( Path ) );
        }
        return Paths;
    }
}
