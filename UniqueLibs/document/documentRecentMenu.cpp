#include "stdafx.h"
#include "documentRecentMenu.hpp"

/// 모듈

/// UIs

/// 기타 헤더파일
#include "recentDocuments.hpp"

/// 고유 라이브러리

/// 공통 라이브러리

/// 외부 라이브러리

/// 선언 헤더파일들

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QWidgetAction>

DECLARE_CMNLIBSV2_NAMESPACE

namespace hc::document
{
    namespace
    {
        // Two-line row: number · name (bold) on the first line, kind · place (muted) on the second.
        class RecentMenuRow final : public QWidget
        {
        public:
            RecentMenuRow( QMenu* Menu, QAction* Action, int Number, QString Name, QString Description )
                : QWidget( Menu ), menu_( Menu ), action_( Action ), number_( Number ), name_( std::move( Name ) ), description_( std::move( Description ) )
            {
                // The menu keeps keyboard focus so arrows, Enter and the 1–9 mnemonics stay with QMenu.
                setFocusPolicy( Qt::NoFocus ); setMouseTracking( true ); setAttribute( Qt::WA_Hover );
                connect( Menu, &QMenu::hovered, this, [ this ] { update(); } );
            }
            QSize sizeHint() const override
            {
                const auto Metrics = fontMetrics();
                QFont Bold = font(); Bold.setWeight( QFont::DemiBold );
                const int Width = qMax( QFontMetrics( Bold ).horizontalAdvance( name_ ), Metrics.horizontalAdvance( description_ ) );
                return { qMin( 520, Gutter + Width + 24 ), Metrics.height() * 2 + 10 };
            }
        protected:
            void enterEvent( QEnterEvent* Event ) override { QWidget::enterEvent( Event ); if ( menu_ ) menu_->setActiveAction( action_ ); update(); }
            void leaveEvent( QEvent* Event ) override { QWidget::leaveEvent( Event ); update(); }
            void mouseReleaseEvent( QMouseEvent* Event ) override
            {
                if ( Event->button() != Qt::LeftButton || !rect().contains( Event->position().toPoint() ) ) return;
                // Close the whole popup chain (up to the menu bar) as a normal menu item does, then run.
                QPointer< QAction > Action( action_ );
                while ( auto* Popup = QApplication::activePopupWidget() ) { if ( !Popup->close() ) break; }
                if ( Action ) Action->trigger();
            }
            void paintEvent( QPaintEvent* ) override
            {
                QPainter Painter( this );
                const bool Active = menu_ && menu_->activeAction() == action_;
                if ( Active ) Painter.fillRect( rect().adjusted( 4, 1, -4, -1 ), palette().color( QPalette::Highlight ) );
                const auto Text = Active ? palette().color( QPalette::HighlightedText ) : palette().color( QPalette::Text );
                auto Muted = Text; Muted.setAlphaF( 0.62 );
                const auto Metrics = fontMetrics(); const int Line = Metrics.height();
                Painter.setPen( Muted );
                Painter.drawText( QRect( 10, 5, Gutter - 14, Line ), Qt::AlignRight | Qt::AlignVCenter, QString::number( number_ ) );
                QFont Bold = font(); Bold.setWeight( QFont::DemiBold );
                Painter.setFont( Bold ); Painter.setPen( Text );
                Painter.drawText( QRect( Gutter, 5, width() - Gutter - 12, Line ), Qt::AlignLeft | Qt::AlignVCenter,
                    QFontMetrics( Bold ).elidedText( name_, Qt::ElideMiddle, width() - Gutter - 12 ) );
                Painter.setFont( font() ); Painter.setPen( Muted );
                Painter.drawText( QRect( Gutter, 5 + Line, width() - Gutter - 12, Line ), Qt::AlignLeft | Qt::AlignVCenter,
                    Metrics.elidedText( description_, Qt::ElideMiddle, width() - Gutter - 12 ) );
            }
        private:
            static constexpr int Gutter = 34;
            QPointer< QMenu >   menu_;
            QPointer< QAction > action_;
            int                 number_;
            QString             name_, description_;
        };
    }
    QList< QAction* > AppendRecentSection( QMenu* Menu, const QList< RecentDocument >& Entries, int Count, const RecentMenuHandlers& Handlers )
    {
        QList< QAction* > Added;
        if ( !Menu || Count <= 0 ) return Added;
        auto* Heading = Menu->addAction( QCoreApplication::translate( "hc::document::RecentMenu", "최근 항목" ) );
        Heading->setEnabled( false ); Heading->setObjectName( "recentHeading" ); Added << Heading;
        if ( Entries.isEmpty() ) {
            auto* Empty = Menu->addAction( QCoreApplication::translate( "hc::document::RecentMenu", "최근 항목 없음" ) );
            Empty->setEnabled( false ); Empty->setObjectName( "recentEmpty" ); Added << Empty;
        }
        for ( int I = 0; I < Entries.size() && I < Count; ++I ) {
            const auto Document = Entries[ I ];
            const auto Name = QFileInfo( Document.path_ ).fileName();
            auto* Row = new QWidgetAction( Menu );
            // Numbers past 9 have no mnemonic; the text still names the row for accessibility.
            Row->setText( I < 9 ? QStringLiteral( "&%1 %2" ).arg( I + 1 ).arg( Name ) : Name );
            Row->setToolTip( QDir::toNativeSeparators( Document.path_ ) ); Row->setObjectName( QStringLiteral( "recentItem%1" ).arg( I + 1 ) );
            Row->setDefaultWidget( new RecentMenuRow( Menu, Row, I + 1, Name, RecentDescription( Document ) ) );
            Row->defaultWidget()->setToolTip( Row->toolTip() );
            QObject::connect( Row, &QAction::triggered, Menu, [ Open = Handlers.open_, Document ] { if ( Open ) Open( Document ); } );
            Menu->addAction( Row ); Added << Row;
        }
        Added << Menu->addSeparator();
        auto* All = Menu->addAction( QCoreApplication::translate( "hc::document::RecentMenu", "최근 항목 모두 보기…" ) );
        All->setObjectName( "recentShowAll" ); Added << All;
        QObject::connect( All, &QAction::triggered, Menu, [ Show = Handlers.showAll_ ] { if ( Show ) Show(); } );
        auto* Clear = Menu->addAction( QCoreApplication::translate( "hc::document::RecentMenu", "최근 항목 지우기" ) );
        Clear->setObjectName( "recentClear" ); Clear->setEnabled( std::any_of( Entries.begin(), Entries.end(), []( const auto& Item ) { return !Item.pinned_; } ) ); Added << Clear;
        QObject::connect( Clear, &QAction::triggered, Menu, [ Run = Handlers.clear_ ] { if ( Run ) Run(); } );
        return Added;
    }
}
