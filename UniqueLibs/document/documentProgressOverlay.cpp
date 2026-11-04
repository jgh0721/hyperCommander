#include "stdafx.h"
#include "documentProgressOverlay.hpp"

/// 모듈

/// UIs

/// 기타 헤더파일

/// 고유 라이브러리

/// 공통 라이브러리

/// 외부 라이브러리
#include <fmstyle/Glyphs.h>
#include <fmstyle/ThemeManager.h>

/// 선언 헤더파일들

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QAbstractButton>
#include <QAccessible>
#include <QAccessibleWidget>
#include <QChildEvent>
#include <QEasingCurve>
#include <QEnterEvent>
#include <QFontMetricsF>
#include <QLoggingCategory>
#include <QPainter>
#include <QPainterPath>
#include <QPolygonF>
#include <QTimer>
#include <Windows.h>
#include <cmath>
#include <limits>

DECLARE_CMNLIBSV2_NAMESPACE

namespace hc::document
{
    namespace
    {
        Q_LOGGING_CATEGORY( progressLog, "hc.document.progress" )

        using Tk = fm::style::Token;

        // editor-chrome-prototype/style.css .progress, in DIP.
        constexpr qreal Padding = 6, Gap = 8, OverlayRadius = 4;
        constexpr qreal BarHeight = 18, BarRadius = 3, TextPadding = 7;
        constexpr qreal PercentWidth = 34, CloseSize = 18, CloseRadius = 3, GlyphSize = 12;
        constexpr qreal SweepWidth = 0.35, StripePeriod = 8, StripeWidth = 4;
        constexpr double FillAlpha = 0.55, DoneAlpha = 0.45, FailAlpha = 0.45;
        // Auto-raise tool button hover and press in fmstyle (FmStyle::drawToolPanel).
        constexpr double HoverAlpha = 0.07, PressAlpha = 0.12;
        constexpr int RecentFinished = 8;
        // DocumentStatusBar::arrange: one status row is max(30, font height + 12).
        constexpr int MinimumRow = 30, RowPadding = 12;

        bool readSystemReduceMotion()
        {
            BOOL Animate = TRUE;
            if( !::SystemParametersInfoW( SPI_GETCLIENTAREAANIMATION, 0, &Animate, 0 ) ) return false;
            return Animate == FALSE;
        }

        QColor withAlpha( QColor Color, double Alpha )
        {
            Color.setAlphaF( float( Color.alphaF() * Alpha ) );
            return Color;
        }

        // CSS ease-in-out = cubic-bezier(.42, 0, .58, 1).
        qreal easeInOut( qreal Progress )
        {
            static const QEasingCurve Curve = [] {
                QEasingCurve Result( QEasingCurve::BezierSpline );
                Result.addCubicBezierSegment( QPointF( 0.42, 0.0 ), QPointF( 0.58, 1.0 ), QPointF( 1.0, 1.0 ) );
                return Result;
            }();
            return Curve.valueForProgress( Progress );
        }

        // Reduced motion replaces the sweep with a still diagonal stripe over the whole bar.
        void paintStripes( QPainter& Painter, const QRectF& Area, const QColor& Color )
        {
            QPainterPath Stripes;
            const qreal Height = Area.height();
            for( qreal X = Area.left() - Height; X < Area.right(); X += StripePeriod ) {
                QPolygonF Band;
                Band << QPointF( X, Area.bottom() ) << QPointF( X + StripeWidth, Area.bottom() )
                     << QPointF( X + StripeWidth + Height, Area.top() ) << QPointF( X + Height, Area.top() );
                Stripes.addPolygon( Band );
                Stripes.closeSubpath();
            }
            Painter.fillPath( Stripes, Color );
        }

        // Transparent hit area over the bar or the ×; the overlay paints both.
        class HitButton final : public QAbstractButton
        {
        public:
            HitButton( QWidget* Parent, std::function<void()> Changed )
                : QAbstractButton( Parent ), changed_( std::move( Changed ) )
            {
                setFocusPolicy( Qt::NoFocus );
                setAttribute( Qt::WA_NoSystemBackground );
                hide();
                connect( this, &QAbstractButton::pressed, this, [this] { changed_(); } );
                connect( this, &QAbstractButton::released, this, [this] { changed_(); } );
            }
        protected:
            void paintEvent( QPaintEvent* ) override {}
            void enterEvent( QEnterEvent* Event ) override { QAbstractButton::enterEvent( Event ); changed_(); }
            void leaveEvent( QEvent* Event ) override { QAbstractButton::leaveEvent( Event ); changed_(); }
        private:
            std::function<void()> changed_;
        };

#if QT_CONFIG(accessibility)
        // Role progress bar, name = text in the bar, value = percent (plan §7.6).
        class OverlayAccessible final : public QAccessibleWidget, public QAccessibleValueInterface
        {
        public:
            explicit OverlayAccessible( DocumentProgressOverlay* Overlay )
                : QAccessibleWidget( Overlay, QAccessible::ProgressBar ) {}
            void* interface_cast( QAccessible::InterfaceType Type ) override
            {
                if( Type == QAccessible::ValueInterface ) return static_cast<QAccessibleValueInterface*>( this );
                return QAccessibleWidget::interface_cast( Type );
            }
            QString text( QAccessible::Text Type ) const override
            {
                if( Type == QAccessible::Name ) return overlay()->Text();
                if( Type == QAccessible::Value ) return overlay()->PercentText();
                return QAccessibleWidget::text( Type );
            }
            QVariant currentValue() const override
            {
                const auto Value = overlay()->Percent();
                return Value ? QVariant( *Value ) : QVariant();
            }
            void setCurrentValue( const QVariant& ) override {}
            QVariant maximumValue() const override { return QVariant( 100 ); }
            QVariant minimumValue() const override { return QVariant( 0 ); }
            QVariant minimumStepSize() const override { return QVariant( 1 ); }
        private:
            const DocumentProgressOverlay* overlay() const
            {
                return static_cast<const DocumentProgressOverlay*>( object() );
            }
        };

        QAccessibleInterface* overlayAccessible( const QString& Key, QObject* Object )
        {
            if( Key != QLatin1StringView( DocumentProgressOverlay::staticMetaObject.className() ) ) return nullptr;
            auto* Overlay = qobject_cast<DocumentProgressOverlay*>( Object );
            return Overlay ? new OverlayAccessible( Overlay ) : nullptr;
        }
#endif
    }

    DocumentProgressOverlay::DocumentProgressOverlay( QWidget* StatusBar ) : QWidget( StatusBar )
    {
#if QT_CONFIG(accessibility)
        static const bool Registered = [] { QAccessible::installFactory( &overlayAccessible ); return true; }();
        Q_UNUSED( Registered );
#endif
        setObjectName( "documentProgressOverlay" );
        // Paint only: the items below keep the mouse except where the hit buttons sit.
        setAttribute( Qt::WA_TransparentForMouseEvents, StatusBar != nullptr );
        setFocusPolicy( Qt::NoFocus );
        elapsed_.start();
        systemReduceMotion_ = readSystemReduceMotion();
        timer_ = new QTimer( this );
        timer_->setSingleShot( true );
        timer_->setTimerType( Qt::PreciseTimer );
        connect( timer_, &QTimer::timeout, this, &DocumentProgressOverlay::Advance );

        // Siblings stacked above the overlay: a transparent widget's children never get the mouse.
        auto* Host = StatusBar ? StatusBar : static_cast<QWidget*>( this );
        const auto Repaint = [this] { update(); };
        auto* Detail = new HitButton( Host, Repaint );
        Detail->setObjectName( "documentProgressDetail" );
        Detail->setEnabled( false );
        connect( Detail, &QAbstractButton::clicked, this, &DocumentProgressOverlay::detailClicked );
        detailButton_ = Detail;
        auto* Close = new HitButton( Host, Repaint );
        Close->setObjectName( "documentProgressClose" );
        connect( Close, &QAbstractButton::clicked, this, &DocumentProgressOverlay::closeClicked );
        closeButton_ = Close;

        connect( &fm::style::ThemeManager::instance(), &fm::style::ThemeManager::changed, this, Repaint );
        if( StatusBar ) StatusBar->installEventFilter( this );
        hide();
        place();
    }

    DocumentProgressOverlay::~DocumentProgressOverlay()
    {
        delete detailButton_.data();
        delete closeButton_.data();
    }

    void DocumentProgressOverlay::Update( const DocumentProgress& Progress )
    {
        if( Progress.token_.isEmpty() || finished_.contains( Progress.token_ ) ) return;
        const auto Now = now();
        const auto Before = state_;
        step( Now );
        if( Progress.token_ == current_.token_ && running() ) {
            // 50 ms ticks repeat the same values; repaint only what changed.
            if( !assign( Progress ) && state_ == Before ) return;
        } else {
            const bool Showing = state_ == State::Visible || ( state_ == State::Cancelling && cancelShown_ );
            start( Progress, Showing, Now );
        }
        sync();
    }

    void DocumentProgressOverlay::Finish( const QString& Token, const DocumentProgressResult& Result )
    {
        if( Token.isEmpty() || Token != current_.token_ || !running() ) return;
        const auto Now = now();
        step( Now );
        rememberFinished( Token );
        switch( state_ ) {
        case State::Pending:
            if( Result.success_ ) enterIdle();
            else enterFailed( Result );
            break;
        case State::Cancelling:
            enterIdle();
            break;
        case State::Visible:
            if( !Result.success_ ) enterFailed( Result );
            else if( Now - shownAt_ >= MinVisibleMs ) enterDone( Result, Now );
            else { result_ = Result; held_ = true; }
            break;
        default:
            break;
        }
        sync();
    }

    void DocumentProgressOverlay::SetCancelling( const QString& Token )
    {
        if( Token.isEmpty() || Token != current_.token_ || !running() || state_ == State::Cancelling ) return;
        step( now() );
        cancelShown_ = state_ == State::Visible;
        state_ = State::Cancelling;
        sync();
    }

    void DocumentProgressOverlay::ShowCurrent( const DocumentProgress& Progress )
    {
        if( Progress.token_.isEmpty() ) { Clear(); return; }
        if( finished_.contains( Progress.token_ ) ) {
            // Its result (Done · Fading · Failed) may still be on screen; another tab's is not.
            if( Progress.token_ != current_.token_ ) Clear();
            return;
        }
        const auto Now = now();
        step( Now );
        if( Progress.token_ == current_.token_ && running() ) {
            assign( Progress );
            if( state_ == State::Pending ) enterVisible( Now );
            else if( state_ == State::Cancelling && !cancelShown_ ) { cancelShown_ = true; shownAt_ = Now; }
        } else {
            start( Progress, true, Now );
        }
        sync();
    }

    void DocumentProgressOverlay::Clear()
    {
        if( state_ == State::Idle ) return;
        enterIdle();
        sync();
    }

    void DocumentProgressOverlay::Advance()
    {
        step( now() );
        sync();
    }

    QString DocumentProgressOverlay::Text() const
    {
        using Kind = DocumentProgress::Kind;
        switch( state_ ) {
        case State::Pending:
        case State::Visible:
            return current_.label_;
        case State::Cancelling:
            return tr( "취소 중…" );
        case State::Done:
        case State::Fading:
            if( !result_.doneLabel_.isEmpty() ) return result_.doneLabel_;
            switch( current_.kind_ ) {
            case Kind::Read: return tr( "열기 완료" );
            case Kind::Save:
            case Kind::EpubSave: return tr( "저장 완료" );
            case Kind::Reinterpret: return tr( "다시 읽기 완료" );
            default: return tr( "완료" );
            }
        case State::Failed: {
            auto Summary = result_.failureSummary_;
            if( Summary.isEmpty() )
                Summary = current_.kind_ == Kind::Read ? tr( "열기 실패" )
                        : current_.kind_ == Kind::Save || current_.kind_ == Kind::EpubSave ? tr( "저장 실패" ) : tr( "실패" );
            return tr( "%1 — 자세히" ).arg( Summary );
        }
        default:
            return {};
        }
    }

    QString DocumentProgressOverlay::PercentText() const
    {
        switch( state_ ) {
        case State::Failed:
            return QString( QChar( 0x26A0 ) );  // ⚠
        case State::Done:
        case State::Fading:
            return current_.fraction_ ? QStringLiteral( "%1 %" ).arg( 100 ) : QString();
        case State::Pending:
        case State::Visible:
        case State::Cancelling:
            if( const auto Value = Percent() ) return QStringLiteral( "%1 %" ).arg( *Value );
            if( current_.count_ ) return tr( "%1개" ).arg( *current_.count_ );
            return {};
        default:
            return {};
        }
    }

    std::optional<int> DocumentProgressOverlay::Percent() const
    {
        switch( state_ ) {
        case State::Done:
        case State::Fading:
            return current_.fraction_ ? std::optional<int>( 100 ) : std::nullopt;
        case State::Pending:
        case State::Visible:
        case State::Cancelling:
            // Floor: '100 %' only once the work is really over.
            if( const auto Fraction = shownFraction() )
                return qBound( 0, int( std::floor( *Fraction * 100.0 + 1e-9 ) ), 100 );
            return std::nullopt;
        default:
            return std::nullopt;
        }
    }

    bool DocumentProgressOverlay::TimerActive() const
    {
        return timer_->isActive();
    }

    void DocumentProgressOverlay::SetClock( Clock Now )
    {
        clock_ = std::move( Now );
    }

    void DocumentProgressOverlay::SetReduceMotion( std::optional<bool> Reduce )
    {
        reduceMotion_ = Reduce;
        if( !Reduce ) systemReduceMotion_ = readSystemReduceMotion();
        step( now() );
        sync();
    }

    bool DocumentProgressOverlay::ReduceMotion() const
    {
        return reduceMotion_.value_or( systemReduceMotion_ );
    }

    int DocumentProgressOverlay::OverlayWidth( int StatusBarWidth )
    {
        const int Capped = qMin( PreferredWidth, int( StatusBarWidth * MaximumWidthRatio ) );
        return qMin( qMax( MinimumWidth, Capped ), qMax( 0, StatusBarWidth - 2 * LeftInset ) );
    }

    QRect DocumentProgressOverlay::BarRect() const
    {
        return parts().bar_.toAlignedRect();
    }

    QRect DocumentProgressOverlay::CloseRect() const
    {
        return parts().close_.toAlignedRect();
    }

    QAbstractButton* DocumentProgressOverlay::DetailButton() const
    {
        return detailButton_.data();
    }

    QAbstractButton* DocumentProgressOverlay::CloseButton() const
    {
        return closeButton_.data();
    }

    bool DocumentProgressOverlay::eventFilter( QObject* Object, QEvent* Event )
    {
        if( Object == parentWidget() ) {
            switch( Event->type() ) {
            case QEvent::Resize:
            case QEvent::FontChange:
                place();
                if( displayed() ) syncButtons( true );
                break;
            case QEvent::ChildAdded:
            case QEvent::ChildPolished: {
                // Status items created later stack above us; take the top again.
                const auto* Child = static_cast<QChildEvent*>( Event )->child();
                if( !isHidden() && Child && Child->isWidgetType() && Child != this
                    && Child != detailButton_.data() && Child != closeButton_.data() )
                    restack();
                break;
            }
            case QEvent::PaletteChange:
                update();
                break;
            default:
                break;
            }
        }
        return QWidget::eventFilter( Object, Event );
    }

    void DocumentProgressOverlay::changeEvent( QEvent* Event )
    {
        QWidget::changeEvent( Event );
        const auto Type = Event->type();
        if( Type == QEvent::FontChange || Type == QEvent::PaletteChange || Type == QEvent::StyleChange ) update();
    }

    void DocumentProgressOverlay::paintEvent( QPaintEvent* )
    {
        const auto& Colors = fm::style::themeColorsFor( this );
        const auto Local = parts();
        const bool Failed = state_ == State::Failed;
        const bool Done = state_ == State::Done || state_ == State::Fading;
        QPainter Painter( this );
        Painter.setRenderHint( QPainter::Antialiasing );
        Painter.setOpacity( opacityAt( now() ) );

        // Status bar background, so the items below disappear under the overlay.
        const QWidget* Base = parentWidget() ? parentWidget() : this;
        Painter.setPen( Qt::NoPen );
        Painter.setBrush( Base->palette().color( Base->backgroundRole() ) );
        Painter.drawRoundedRect( QRectF( rect() ), OverlayRadius, OverlayRadius );

        // Bar: input background, fill clipped to the inside of the 1 DIP border.
        QPainterPath Bar;
        Bar.addRoundedRect( Local.bar_, BarRadius, BarRadius );
        Painter.fillPath( Bar, Colors[Tk::Field] );
        const QRectF Inner = Local.bar_.adjusted( 1, 1, -1, -1 );
        QPainterPath Clip;
        Clip.addRoundedRect( Inner, BarRadius - 1, BarRadius - 1 );
        Painter.save();
        Painter.setClipPath( Clip, Qt::IntersectClip );
        const auto Accent = withAlpha( Colors[Tk::Accent], FillAlpha );
        if( Failed ) Painter.fillRect( Inner, withAlpha( Colors[Tk::Danger], FailAlpha ) );
        else if( Done ) Painter.fillRect( Inner, withAlpha( Colors[Tk::Ok], DoneAlpha ) );
        else if( const auto Fraction = shownFraction() )
            Painter.fillRect( QRectF( Inner.left(), Inner.top(), Inner.width() * *Fraction, Inner.height() ), Accent );
        else if( ReduceMotion() ) paintStripes( Painter, Inner, Accent );
        else {
            // @keyframes indet: left -35 % → 100 %, 1.1 s ease-in-out, from the moment the bar appeared.
            const qint64 Elapsed = ( ( now() - shownAt_ ) % SweepPeriodMs + SweepPeriodMs ) % SweepPeriodMs;
            const qreal Phase = easeInOut( qreal( Elapsed ) / SweepPeriodMs );
            const qreal Left = Inner.left() + ( -SweepWidth + Phase * ( 1.0 + SweepWidth ) ) * Inner.width();
            Painter.fillRect( QRectF( Left, Inner.top(), Inner.width() * SweepWidth, Inner.height() ), Accent );
        }
        Painter.restore();
        Painter.setPen( QPen( Failed ? Colors[Tk::Danger] : Colors[Tk::Line], 1.0 ) );
        Painter.setBrush( Qt::NoBrush );
        Painter.drawRoundedRect( Local.bar_.adjusted( 0.5, 0.5, -0.5, -0.5 ), BarRadius - 0.5, BarRadius - 0.5 );

        const auto Font = labelFont();
        const QFontMetricsF Metrics( Font );
        Painter.setFont( Font );
        const QRectF TextArea = Local.bar_.adjusted( TextPadding, 0, -TextPadding, 0 );
        Painter.setPen( Colors[Tk::Fg] );
        Painter.drawText( TextArea, int( Qt::AlignVCenter | Qt::AlignLeft ) | Qt::TextSingleLine,
                          Metrics.elidedText( Text(), Qt::ElideRight, TextArea.width() ) );

        if( Failed ) {
            // A glyph, not U+26A0 text: font fallback may pick a colour emoji that ignores the pen.
            const QRectF Icon( Local.percent_.right() - GlyphSize, Local.percent_.center().y() - GlyphSize / 2,
                               GlyphSize, GlyphSize );
            fm::style::paintGlyph( &Painter, fm::style::Glyph::Warning, Icon, Colors[Tk::Danger] );
        } else {
            // Right-aligned in its 34 DIP slot; a longer count may borrow the gap on its left.
            Painter.setPen( Colors[Tk::Fg3] );
            Painter.drawText( Local.percent_.adjusted( -Gap, 0, 0, 0 ),
                              int( Qt::AlignVCenter | Qt::AlignRight ) | Qt::TextSingleLine, PercentText() );
        }

        if( closeShown() ) {
            const bool Hover = closeButton_ && closeButton_->underMouse();
            const bool Down = closeButton_ && closeButton_->isDown();
            if( Hover || Down ) {
                Painter.setPen( Qt::NoPen );
                Painter.setBrush( withAlpha( Colors[Tk::Fg], Down ? PressAlpha : HoverAlpha ) );
                Painter.drawRoundedRect( Local.close_, CloseRadius, CloseRadius );
            }
            const QRectF Icon( Local.close_.center() - QPointF( GlyphSize / 2, GlyphSize / 2 ),
                               QSizeF( GlyphSize, GlyphSize ) );
            const auto GlyphColor = Hover || Down ? Colors[Tk::Fg] : Colors[Tk::Fg3];
            fm::style::paintGlyph( &Painter, fm::style::Glyph::Close, Icon, GlyphColor );
        }
    }

    DocumentProgressOverlay::Parts DocumentProgressOverlay::parts() const
    {
        // [bar (flex)] gap [percent 34] gap [× 18], 6 DIP padding on both ends.
        const qreal Width = width(), Height = height();
        Parts Result;
        Result.close_ = QRectF( Width - Padding - CloseSize, std::floor( ( Height - CloseSize ) / 2 ),
                                CloseSize, CloseSize );
        Result.percent_ = QRectF( Result.close_.left() - Gap - PercentWidth, 0, PercentWidth, Height );
        Result.bar_ = QRectF( Padding, std::floor( ( Height - BarHeight ) / 2 ),
                              qMax<qreal>( 0, Result.percent_.left() - Gap - Padding ), BarHeight );
        return Result;
    }

    qint64 DocumentProgressOverlay::now() const
    {
        return clock_ ? clock_() : elapsed_.elapsed();
    }

    // Applies every deadline that has passed; a test clock may jump over several.
    void DocumentProgressOverlay::step( qint64 Now )
    {
        for( ;; ) {
            if( state_ == State::Pending && Now - startedAt_ >= ShowDelayMs ) {
                enterVisible( Now );
            } else if( state_ == State::Cancelling && !cancelShown_ && Now - startedAt_ >= ShowDelayMs ) {
                cancelShown_ = true;
                shownAt_ = Now;
            } else if( state_ == State::Visible && held_ && Now - shownAt_ >= MinVisibleMs ) {
                enterDone( result_, Now );
            } else if( state_ == State::Done && Now - doneAt_ >= DoneHoldMs ) {
                if( ReduceMotion() ) enterIdle();
                else { state_ = State::Fading; fadeAt_ = doneAt_ + DoneHoldMs; }
            } else if( state_ == State::Fading && ( ReduceMotion() || Now - fadeAt_ >= FadeMs ) ) {
                enterIdle();
            } else {
                break;
            }
        }
    }

    // Brings the widget, the hit buttons, the timer and the announcements in line with state_.
    void DocumentProgressOverlay::sync()
    {
        const bool Shown = displayed();
        if( Shown ) place();
        if( Shown == isHidden() ) {
            setVisible( Shown );
            if( Shown ) restack();
        }
        syncButtons( Shown );
        if( Shown ) update();
        schedule();
        if( announcement_ ) {
            // Only on show, completion and failure — never per percent (plan §7.6).
            const auto Message = announcement_->first;
            const bool Assertive = announcement_->second;
            announcement_.reset();
            emit Announced( Message );
#if QT_CONFIG(accessibility)
            QAccessibleAnnouncementEvent Event( this, Message );
            using Politeness = QAccessible::AnnouncementPoliteness;
            Event.setPoliteness( Assertive ? Politeness::Assertive : Politeness::Polite );
            QAccessible::updateAccessibility( &Event );
#endif
        }
        if( reported_ != state_ ) {
            qCDebug( progressLog ) << reported_ << "->" << state_ << current_.token_;
            reported_ = state_;
            emit StateChanged( state_ );
        }
    }

    // The timer runs only toward the next deadline, or per frame while something moves.
    void DocumentProgressOverlay::schedule()
    {
        std::optional<qint64> Deadline;
        switch( state_ ) {
        case State::Pending: Deadline = startedAt_ + ShowDelayMs; break;
        case State::Cancelling: if( !cancelShown_ ) Deadline = startedAt_ + ShowDelayMs; break;
        case State::Visible: if( held_ ) Deadline = shownAt_ + MinVisibleMs; break;
        case State::Done: Deadline = doneAt_ + DoneHoldMs; break;
        default: break;
        }
        int Interval = animating() ? FrameMs : -1;
        if( Deadline ) {
            const int Wait = int( qBound( qint64( 0 ), qint64( *Deadline - now() ), qint64( ( std::numeric_limits<int>::max )() ) ) );
            Interval = Interval < 0 ? Wait : qMin( Interval, Wait );
        }
        if( Interval < 0 ) timer_->stop();
        else timer_->start( Interval );
    }

    void DocumentProgressOverlay::place()
    {
        const auto* Bar = parentWidget();
        if( !Bar ) return;
        // The first status row only: a narrow status bar wraps its items to more rows.
        const int Row = qMin( Bar->height(), qMax( MinimumRow, Bar->fontMetrics().height() + RowPadding ) );
        const QRect Area( LeftInset, VerticalInset, OverlayWidth( Bar->width() ), qMax( 0, Row - 2 * VerticalInset ) );
        if( geometry() != Area ) setGeometry( Area );
    }

    void DocumentProgressOverlay::restack()
    {
        const auto* Bar = parentWidget();
        if( !Bar ) return;
        const auto& Children = Bar->children();
        const auto Count = Children.size();
        if( Count >= 3 && Children[Count - 3] == this && Children[Count - 2] == detailButton_.data()
            && Children[Count - 1] == closeButton_.data() )
            return;
        raise();
        if( detailButton_ ) detailButton_->raise();
        if( closeButton_ ) closeButton_->raise();
    }

    void DocumentProgressOverlay::syncButtons( bool Shown )
    {
        const auto Local = parts();
        const QPoint Origin = parentWidget() ? pos() : QPoint();
        if( detailButton_ ) {
            const bool Failed = state_ == State::Failed;
            detailButton_->setGeometry( Local.bar_.toAlignedRect().translated( Origin ) );
            // Disabled outside Failed: it still covers the bar, so the press reaches nothing below.
            detailButton_->setEnabled( Failed );
            if( Failed != detailButton_->testAttribute( Qt::WA_SetCursor ) ) {
                if( Failed ) detailButton_->setCursor( Qt::PointingHandCursor );
                else detailButton_->unsetCursor();
            }
            // Static names: a name change is an accessibility event, and the label changes per tick.
            const auto Name = Failed ? tr( "오류 자세히 보기" ) : tr( "진행 표시" );
            if( detailButton_->accessibleName() != Name ) detailButton_->setAccessibleName( Name );
            // The full label, which the bar may have elided.
            const auto Tip = Shown ? Text() : QString();
            if( detailButton_->toolTip() != Tip ) detailButton_->setToolTip( Tip );
            detailButton_->setVisible( Shown );
        }
        if( closeButton_ ) {
            const bool Cancels = state_ == State::Visible;
            closeButton_->setGeometry( Local.close_.toAlignedRect().translated( Origin ) );
            const auto Tip = Cancels ? tr( "작업 취소 (Ctrl+.)" ) : tr( "닫기" );
            if( closeButton_->toolTip() != Tip ) closeButton_->setToolTip( Tip );
            const auto Name = Cancels ? tr( "작업 취소" ) : tr( "닫기" );
            if( closeButton_->accessibleName() != Name ) closeButton_->setAccessibleName( Name );
            closeButton_->setVisible( Shown && closeShown() );
        }
    }

    bool DocumentProgressOverlay::displayed() const
    {
        switch( state_ ) {
        case State::Visible:
        case State::Done:
        case State::Fading:
        case State::Failed:
            return true;
        case State::Cancelling:
            return cancelShown_;
        default:
            return false;
        }
    }

    bool DocumentProgressOverlay::running() const
    {
        return ( state_ == State::Pending || state_ == State::Visible || state_ == State::Cancelling ) && !held_;
    }

    bool DocumentProgressOverlay::animating() const
    {
        if( state_ == State::Fading ) return true;
        const bool Running = state_ == State::Visible || ( state_ == State::Cancelling && cancelShown_ );
        return Running && !shownFraction() && !ReduceMotion();
    }

    bool DocumentProgressOverlay::closeShown() const
    {
        switch( state_ ) {
        case State::Visible:
            return current_.cancellable_ && !held_;
        case State::Done:
        case State::Fading:
        case State::Failed:
            return true;
        default:
            return false;
        }
    }

    bool DocumentProgressOverlay::assign( const DocumentProgress& Progress )
    {
        if( current_ == Progress ) return false;
        current_ = Progress;
        return true;
    }

    void DocumentProgressOverlay::start( const DocumentProgress& Progress, bool Immediately, qint64 Now )
    {
        enterIdle();
        if( !reduceMotion_ ) systemReduceMotion_ = readSystemReduceMotion();
        current_ = Progress;
        startedAt_ = Now;
        if( Immediately ) enterVisible( Now );
        else state_ = State::Pending;
    }

    void DocumentProgressOverlay::enterVisible( qint64 Now )
    {
        state_ = State::Visible;
        shownAt_ = Now;
        announcement_ = std::pair{ current_.label_, false };
    }

    void DocumentProgressOverlay::enterDone( const DocumentProgressResult& Result, qint64 Now )
    {
        result_ = Result;
        held_ = false;
        state_ = State::Done;
        doneAt_ = Now;
        announcement_ = std::pair{ Text(), false };
    }

    void DocumentProgressOverlay::enterFailed( const DocumentProgressResult& Result )
    {
        result_ = Result;
        held_ = false;
        cancelShown_ = false;
        state_ = State::Failed;
        announcement_ = std::pair{ Text(), true };
    }

    void DocumentProgressOverlay::enterIdle()
    {
        state_ = State::Idle;
        current_ = {};
        result_ = {};
        held_ = false;
        cancelShown_ = false;
    }

    void DocumentProgressOverlay::rememberFinished( const QString& Token )
    {
        finished_.removeAll( Token );
        finished_.append( Token );
        while( finished_.size() > RecentFinished ) finished_.removeFirst();
    }

    void DocumentProgressOverlay::closeClicked()
    {
        switch( state_ ) {
        case State::Visible: {
            if( !current_.cancellable_ || held_ ) return;
            const auto Token = current_.token_;
            state_ = State::Cancelling;
            cancelShown_ = true;
            sync();
            emit CancelRequested( Token );
            return;
        }
        case State::Done:
        case State::Fading:
        case State::Failed:
            enterIdle();
            sync();
            return;
        default:
            return;
        }
    }

    void DocumentProgressOverlay::detailClicked()
    {
        if( state_ != State::Failed ) return;
        const auto Detail = result_.failureDetail_.isEmpty() ? result_.failureSummary_ : result_.failureDetail_;
        enterIdle();
        sync();
        emit DetailRequested( Detail );
    }

    std::optional<double> DocumentProgressOverlay::shownFraction() const
    {
        if( !current_.fraction_ || !std::isfinite( *current_.fraction_ ) ) return std::nullopt;
        // Success arrived during the minimum visible time: show the bar full until Done.
        if( held_ ) return 1.0;
        return qBound( 0.0, *current_.fraction_, 1.0 );
    }

    qreal DocumentProgressOverlay::opacityAt( qint64 Now ) const
    {
        if( state_ != State::Fading ) return 1.0;
        return qBound( 0.0, 1.0 - double( Now - fadeAt_ ) / FadeMs, 1.0 );
    }

    QFont DocumentProgressOverlay::labelFont() const
    {
        // 0.5 pt below the status bar's font.
        auto Font = font();
        qreal Points = Font.pointSizeF();
        if( Points <= 0 && Font.pixelSize() > 0 ) Points = Font.pixelSize() * 72.0 / qMax( 1, logicalDpiY() );
        if( Points > 0 ) Font.setPointSizeF( qMax<qreal>( 1.0, Points - 0.5 ) );
        return Font;
    }
}
