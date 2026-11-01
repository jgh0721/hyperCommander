#include "stdafx.h"
#include "audioTitleWidget.hpp"

/// 모듈

/// UIs

/// 기타 헤더파일

/// 고유 라이브러리
#include "UniqueLibs/audio/audioSession.hpp"

/// 공통 라이브러리

/// 외부 라이브러리
#include <fmstyle/PaperFluentChrome.h>
#include <fmstyle/StylePaint.h>
#include <fmstyle/ThemeManager.h>
#include <fmstyle/WatercolorChrome.h>

/// 선언 헤더파일들

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QDesktopServices>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QStyle>
#include <QToolButton>
#include <QUrl>

DECLARE_CMNLIBSV2_NAMESPACE

namespace hc::ui
{
    namespace
    {
        QString Rgba( const QColor& Color, int Alpha ) { return QString( "rgba(%1,%2,%3,%4)" ).arg( Color.red() ).arg( Color.green() ).arg( Color.blue() ).arg( Alpha ); }

        void DrawGlyph( QPainter& Painter, MediaGlyph Glyph, const QColor& Color )
        {
            // 12 × 12 상자 기준. 채움 기호와 선 기호를 섞어 쓴다.
            Painter.setRenderHint( QPainter::Antialiasing );
            QPen Pen( Color, 1.2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin ); Painter.setPen( Pen ); Painter.setBrush( Qt::NoBrush );
            const auto Fill = [ & ]( const QPolygonF& Shape ) { Painter.save(); Painter.setPen( Qt::NoPen ); Painter.setBrush( Color ); Painter.drawPolygon( Shape ); Painter.restore(); };
            const auto Bar = [ & ]( qreal X, qreal Y, qreal W, qreal H ) { Painter.save(); Painter.setPen( Qt::NoPen ); Painter.setBrush( Color ); Painter.drawRoundedRect( QRectF( X, Y, W, H ), 0.5, 0.5 ); Painter.restore(); };
            const QPolygonF Speaker{ { 1.5, 4.6 }, { 3.8, 4.6 }, { 6.6, 2.2 }, { 6.6, 9.8 }, { 3.8, 7.4 }, { 1.5, 7.4 } };
            switch( Glyph )
            {
                case MediaGlyph::Previous: Bar( 1.5, 2.0, 1.6, 8.0 ); Fill( { { 10.5, 2.4 }, { 4.2, 6.0 }, { 10.5, 9.6 } } ); break;
                case MediaGlyph::Next: Bar( 8.9, 2.0, 1.6, 8.0 ); Fill( { { 1.5, 2.4 }, { 7.8, 6.0 }, { 1.5, 9.6 } } ); break;
                case MediaGlyph::Play: Fill( { { 3.0, 1.8 }, { 10.4, 6.0 }, { 3.0, 10.2 } } ); break;
                case MediaGlyph::Pause: Bar( 2.6, 2.0, 2.4, 8.0 ); Bar( 7.0, 2.0, 2.4, 8.0 ); break;
                case MediaGlyph::Volume:
                    Painter.drawPolygon( Speaker );
                    Painter.drawArc( QRectF( 5.6, 3.6, 4.8, 4.8 ), -60 * 16, 120 * 16 );
                    Painter.drawArc( QRectF( 4.6, 1.6, 8.4, 8.8 ), -60 * 16, 120 * 16 );
                    break;
                case MediaGlyph::Mute:
                    Painter.drawPolygon( Speaker ); Painter.drawLine( QPointF( 8.4, 4.4 ), QPointF( 11.2, 7.6 ) ); Painter.drawLine( QPointF( 11.2, 4.4 ), QPointF( 8.4, 7.6 ) );
                    break;
                case MediaGlyph::Close: Painter.drawLine( QPointF( 2.5, 2.5 ), QPointF( 9.5, 9.5 ) ); Painter.drawLine( QPointF( 9.5, 2.5 ), QPointF( 2.5, 9.5 ) ); break;
                case MediaGlyph::Note:
                    Painter.drawPolyline( QPolygonF{ { 4.5, 9.2 }, { 4.5, 2.6 }, { 10.0, 1.5 }, { 10.0, 7.9 } } );
                    Painter.drawEllipse( QPointF( 3.2, 9.3 ), 1.4, 1.4 ); Painter.drawEllipse( QPointF( 8.7, 8.0 ), 1.4, 1.4 );
                    break;
                case MediaGlyph::Repeat: case MediaGlyph::RepeatOne:
                    Painter.drawPolyline( QPolygonF{ { 1.8, 5.6 }, { 1.8, 4.6 }, { 3.6, 3.0 }, { 10.2, 3.0 } } ); Painter.drawPolyline( QPolygonF{ { 8.6, 1.4 }, { 10.2, 3.0 }, { 8.6, 4.6 } } );
                    Painter.drawPolyline( QPolygonF{ { 10.2, 6.4 }, { 10.2, 7.4 }, { 8.4, 9.0 }, { 1.8, 9.0 } } ); Painter.drawPolyline( QPolygonF{ { 3.4, 7.4 }, { 1.8, 9.0 }, { 3.4, 10.6 } } );
                    if( Glyph == MediaGlyph::RepeatOne ) { Painter.setFont( fm::style::pixelFont( QFont(), 6, QFont::Bold ) ); Painter.drawText( QRectF( 3.0, 3.6, 6.0, 5.0 ), Qt::AlignCenter, "1" ); }
                    break;
                case MediaGlyph::Shuffle:
                    Painter.drawPolyline( QPolygonF{ { 1.2, 3.4 }, { 3.4, 3.4 }, { 7.6, 8.6 }, { 10.6, 8.6 } } ); Painter.drawPolyline( QPolygonF{ { 9.0, 7.0 }, { 10.6, 8.6 }, { 9.0, 10.2 } } );
                    Painter.drawPolyline( QPolygonF{ { 1.2, 8.6 }, { 3.4, 8.6 }, { 4.6, 7.1 } } ); Painter.drawPolyline( QPolygonF{ { 6.4, 4.9 }, { 7.6, 3.4 }, { 10.6, 3.4 } } );
                    Painter.drawPolyline( QPolygonF{ { 9.0, 1.8 }, { 10.6, 3.4 }, { 9.0, 5.0 } } );
                    break;
            }
        }

        QString FolderName( const QString& Folder )
        {
            const auto Clean = QString( Folder ).replace( '\\', '/' );
            const auto Name = Clean.section( '/', -1, -1, QString::SectionSkipEmpty );
            return Name.isEmpty() ? Folder : Name;
        }
    }

    QIcon MediaIcon( MediaGlyph Glyph, const QColor& Color, int Px )
    {
        QIcon Icon;
        for( const qreal Dpr : { 1.0, 1.25, 1.5, 2.0 } )
        {
            QPixmap Pixmap( qCeil( Px * Dpr ), qCeil( Px * Dpr ) ); Pixmap.setDevicePixelRatio( Dpr ); Pixmap.fill( Qt::transparent );
            QPainter Painter( &Pixmap ); Painter.scale( Px / 12.0, Px / 12.0 );
            DrawGlyph( Painter, Glyph, Color );
            Painter.end(); Icon.addPixmap( Pixmap );
        }
        return Icon;
    }

    QIcon MediaDiscIcon( bool Pause, const QColor& Disc, const QColor& Glyph, int Px )
    {
        QIcon Icon;
        for( const qreal Dpr : { 1.0, 1.25, 1.5, 2.0 } )
        {
            QPixmap Pixmap( qCeil( Px * Dpr ), qCeil( Px * Dpr ) ); Pixmap.setDevicePixelRatio( Dpr ); Pixmap.fill( Qt::transparent );
            QPainter Painter( &Pixmap ); Painter.setRenderHint( QPainter::Antialiasing );
            Painter.setPen( Qt::NoPen ); Painter.setBrush( Disc ); Painter.drawEllipse( QRectF( 0, 0, Px, Px ) );
            const auto Inner = Px * 0.56; Painter.translate( ( Px - Inner ) / 2.0 + ( Pause ? 0 : Px * 0.04 ), ( Px - Inner ) / 2.0 ); Painter.scale( Inner / 12.0, Inner / 12.0 );
            DrawGlyph( Painter, Pause ? MediaGlyph::Pause : MediaGlyph::Play, Glyph );
            Painter.end(); Icon.addPixmap( Pixmap );
        }
        return Icon;
    }

    ////////////////////////////////////////////////////////////////////////////
    /// 진행막대

    AudioSeekBar::AudioSeekBar( QWidget* Parent ) : QSlider( Qt::Horizontal, Parent )
    {
        setObjectName( "hc.audio.seek" ); setAccessibleName( tr( "재생 위치" ) );
        setFocusPolicy( Qt::TabFocus ); setRange( 0, 0 ); setSingleStep( 50 ); setPageStep( 300 );
        setMouseTracking( true ); setAttribute( Qt::WA_Hover );
        // 화살표·Page 키는 QSlider가 값만 바꾸므로 그 결과를 탐색으로 넘긴다(단위 100 ms).
        connect( this, &QSlider::actionTriggered, this, [ this ]( int Action ) {
            if( Action == SliderMove || dragging_ ) return;
            QMetaObject::invokeMethod( this, [ this ] { emit sigSeekRequested( qint64( value() ) * 100 ); }, Qt::QueuedConnection );
        } );
    }

    void AudioSeekBar::SetColors( const QColor& Track, const QColor& Fill, const QColor& Knob, bool KnobAlways )
    {
        track_ = Track; fill_ = Fill; knob_ = Knob; knobAlways_ = KnobAlways; update();
    }

    void AudioSeekBar::SetPosition( qint64 PositionMs, qint64 DurationMs )
    {
        if( dragging_ ) return;
        const auto Max = DurationMs > 0 ? int( DurationMs / 100 ) : 0;
        if( maximum() != Max ) setRange( 0, Max );
        setValue( int( std::clamp<qint64>( PositionMs / 100, 0, Max ) ) );
        setAccessibleDescription( QString( "%1 / %2" ).arg( hc::audio::FormatDuration( PositionMs ), hc::audio::FormatDuration( DurationMs ) ) );
    }

    int AudioSeekBar::valueAt( int X ) const
    {
        const int Inset = 5, Span = std::max( 1, width() - Inset * 2 );
        return QStyle::sliderValueFromPosition( minimum(), maximum(), std::clamp( X - Inset, 0, Span ), Span );
    }

    void AudioSeekBar::paintEvent( QPaintEvent* )
    {
        QPainter Painter( this ); Painter.setRenderHint( QPainter::Antialiasing );
        const qreal Inset = 5, Y = height() / 2.0, Span = std::max( 1.0, width() - Inset * 2 );
        const auto Ratio = maximum() > 0 ? qreal( value() ) / maximum() : 0.0;
        Painter.setPen( Qt::NoPen );
        Painter.setBrush( track_.isValid() ? track_ : palette().color( QPalette::Mid ) ); Painter.drawRoundedRect( QRectF( Inset, Y - 2, Span, 4 ), 2, 2 );
        Painter.setBrush( fill_.isValid() ? fill_ : palette().color( QPalette::Highlight ) ); Painter.drawRoundedRect( QRectF( Inset, Y - 2, Span * Ratio, 4 ), 2, 2 );
        if( knobAlways_ || hovered_ || dragging_ || hasFocus() )
        {
            Painter.setBrush( knob_.isValid() ? knob_ : palette().color( QPalette::Base ) );
            Painter.setPen( QPen( QColor( 0, 0, 0, 70 ), 1 ) );
            Painter.drawEllipse( QPointF( Inset + Span * Ratio, Y ), 5, 5 );
        }
        if( hasFocus() ) { Painter.setPen( QPen( fill_.isValid() ? fill_ : palette().color( QPalette::Highlight ), 1, Qt::DotLine ) ); Painter.setBrush( Qt::NoBrush ); Painter.drawRoundedRect( QRectF( rect() ).adjusted( 0.5, 0.5, -0.5, -0.5 ), 3, 3 ); }
    }

    void AudioSeekBar::mousePressEvent( QMouseEvent* Event )
    {
        if( Event->button() != Qt::LeftButton || maximum() <= 0 ) { Event->accept(); return; }
        dragging_ = true; setValue( valueAt( qRound( Event->position().x() ) ) ); update(); Event->accept();
    }

    void AudioSeekBar::mouseMoveEvent( QMouseEvent* Event )
    {
        if( dragging_ ) { setValue( valueAt( qRound( Event->position().x() ) ) ); update(); }
        Event->accept();
    }

    void AudioSeekBar::mouseReleaseEvent( QMouseEvent* Event )
    {
        if( dragging_ ) { dragging_ = false; emit sigSeekRequested( qint64( value() ) * 100 ); update(); }
        Event->accept();
    }

    void AudioSeekBar::wheelEvent( QWheelEvent* Event )
    {
        if( maximum() <= 0 ) { Event->ignore(); return; }
        const auto Steps = Event->angleDelta().y() / 120;
        if( Steps ) emit sigSeekRequested( qint64( value() ) * 100 + Steps * 5000 );
        Event->accept();
    }

    void AudioSeekBar::enterEvent( QEnterEvent* Event ) { hovered_ = true; update(); QSlider::enterEvent( Event ); }
    void AudioSeekBar::leaveEvent( QEvent* Event ) { hovered_ = false; update(); QSlider::leaveEvent( Event ); }

    ////////////////////////////////////////////////////////////////////////////
    /// 제목표시줄 위젯

    AudioTitleWidget::AudioTitleWidget( hc::audio::AudioSession* Session, QWidget* Chrome )
        : QWidget( Chrome ), session_( Session )
    {
        setObjectName( "hc.audio.titleWidget" ); setAccessibleName( tr( "오디오 재생" ) );
        setAttribute( Qt::WA_StyledBackground, false );
        auto* Layout = new QHBoxLayout( this );
        Layout->setContentsMargins( 16 + 4, 0, 8 + 4, 0 ); Layout->setSpacing( 2 );
        prev_ = button( "hc.audio.prev", tr( "이전 곡" ) );
        play_ = button( "hc.audio.playPause", tr( "재생" ) );
        next_ = button( "hc.audio.next", tr( "다음 곡" ) );
        auto* AfterTransport = separator();
        track_ = button( "hc.audio.track", tr( "재생 정보" ) );
        track_->setToolButtonStyle( Qt::ToolButtonTextBesideIcon ); track_->setMinimumWidth( 0 ); track_->setMaximumWidth( 230 );
        track_->setFixedWidth( 230 ); track_->setSizePolicy( QSizePolicy::Fixed, QSizePolicy::Fixed );
        auto* AfterTrack = separator();
        position_ = new QLabel( this ); position_->setObjectName( "hc.audio.position" ); position_->setFixedWidth( 36 ); position_->setAlignment( Qt::AlignRight | Qt::AlignVCenter );
        seek_ = new AudioSeekBar( this ); seek_->setFixedWidth( 150 + 16 );
        duration_ = new QLabel( this ); duration_->setObjectName( "hc.audio.duration" ); duration_->setFixedWidth( 36 );
        ring_ = new QWidget( this ); ring_->setObjectName( "hc.audio.ring" ); ring_->setFixedWidth( 22 ); ring_->setAttribute( Qt::WA_TransparentForMouseEvents );
        remaining_ = new QLabel( this ); remaining_->setObjectName( "hc.audio.remaining" ); remaining_->setFixedWidth( 44 );
        error_ = new QLabel( this ); error_->setObjectName( "hc.audio.error" ); error_->setMaximumWidth( 320 );
        external_ = button( "hc.audio.openExternal", tr( "연결 프로그램으로 열기" ) );
        external_->setText( tr( "연결 프로그램으로 열기" ) ); external_->setToolButtonStyle( Qt::ToolButtonTextOnly );
        external_->setMinimumWidth( 0 ); external_->setMaximumWidth( 220 );
        seek_->setFixedHeight( 16 );
        auto* BeforeVolume = separator();
        volume_ = button( "hc.audio.volume", tr( "음소거" ) );
        close_ = button( "hc.audio.close", tr( "정지하고 닫기" ) );
        for( auto* Item : std::initializer_list<QWidget*>{ prev_, play_, next_, AfterTransport, track_, AfterTrack, position_, seek_, duration_, ring_, remaining_, error_, external_, BeforeVolume, volume_, close_ } )
            Layout->addWidget( Item, 0, Qt::AlignVCenter );
        external_->setMinimumWidth( external_->sizeHint().width() );

        connect( prev_, &QToolButton::clicked, this, [ this ] { if( session_ ) session_->Previous(); } );
        connect( play_, &QToolButton::clicked, this, [ this ] { if( session_ ) session_->PlayPause(); } );
        connect( next_, &QToolButton::clicked, this, [ this ] { if( session_ ) session_->Next(); } );
        connect( track_, &QToolButton::clicked, this, &AudioTitleWidget::sigPopoverRequested );
        connect( volume_, &QToolButton::clicked, this, [ this ] { if( session_ ) session_->SetMuted( !session_->Muted() ); } );
        connect( close_, &QToolButton::clicked, this, [ this ] { if( session_ ) session_->Stop(); } );
        connect( external_, &QToolButton::clicked, this, [ this ] {
            if( const auto* Track = session_ ? session_->Current() : nullptr; Track && !Track->localPath_.isEmpty() )
                QDesktopServices::openUrl( QUrl::fromLocalFile( Track->localPath_ ) );
        } );
        connect( seek_, &AudioSeekBar::sigSeekRequested, this, [ this ]( qint64 Position ) { if( session_ ) session_->Seek( Position ); } );
        volume_->installEventFilter( this );
        Chrome->installEventFilter( this );
        if( auto* Top = Chrome->window() ) Top->installEventFilter( this );
        connect( &fm::style::ThemeManager::instance(), &fm::style::ThemeManager::changed, this, &AudioTitleWidget::refreshTheme );
        if( Session )
        {
            connect( Session, &hc::audio::AudioSession::sigStateChanged, this, &AudioTitleWidget::refreshState );
            connect( Session, &hc::audio::AudioSession::sigTrackChanged, this, &AudioTitleWidget::refreshTrack );
            connect( Session, &hc::audio::AudioSession::sigPositionChanged, this, &AudioTitleWidget::refreshPosition );
            connect( Session, &hc::audio::AudioSession::sigModeChanged, this, &AudioTitleWidget::refreshVolume );
        }
        refreshTheme();
        refreshTrack();
        refreshState();
    }

    QToolButton* AudioTitleWidget::button( const char* Name, const QString& Text )
    {
        auto* Button = new QToolButton( this );
        Button->setObjectName( Name ); Button->setAccessibleName( Text ); Button->setToolTip( Text );
        // 캡션 단추와 같다: 클릭은 패널 입력을 그대로 두고 Tab으로만 닿는다.
        Button->setFocusPolicy( Qt::TabFocus ); Button->setAutoRaise( true );
        Button->setFixedSize( 26, 20 ); Button->setIconSize( QSize( 12, 12 ) );
        return Button;
    }

    QWidget* AudioTitleWidget::separator()
    {
        auto* Item = new QWidget( this ); Item->setFixedWidth( 9 ); Item->setAttribute( Qt::WA_TransparentForMouseEvents );
        separators_.append( Item ); return Item;
    }

    void AudioTitleWidget::refreshTheme()
    {
        const auto& Colors = fm::style::themeColorsFor( this );
        const bool Active = !window() || window()->isActiveWindow();
        if( Colors.isPaperFluent() )
        {
            const auto Paper = fm::style::paperFluentChrome( Colors );
            foreground_ = Paper.titleFg; glyph_ = Paper.titleBottom; wellHeight_ = 24;
            // 워시 위 흰 글자는 4.5:1에 못 미치므로 어두운 홈에 둔다. 비활성 창은 워시가 옅어져 더 진하게 한다.
            well_ = !Active && !Colors.isDark() ? QColor( 20, 40, 80, 122 ) : QColor( 0, 0, 0, Colors.isDark() ? 56 : 41 );
        }
        else
        {
            const auto& Chrome = fm::style::watercolorChrome( Colors.variant() );
            foreground_ = Qt::white; glyph_ = Active ? Chrome.titleCap : Chrome.titleInactive; wellHeight_ = 20;
            well_ = QColor( 0, 0, 0, Active ? 46 : 92 );
        }
        wellLine_ = foreground_; wellLine_.setAlpha( 41 );
        trackColor_ = foreground_; trackColor_.setAlpha( 82 );
        const auto Fg = foreground_;
        setFixedHeight( wellHeight_ );
        setStyleSheet( QString( "QToolButton{background:transparent;border:0;border-radius:4px;padding:0 4px;color:%1;}"
                                "QToolButton:hover{background:%2;}QToolButton:pressed{background:%3;}QToolButton:focus{border:1px solid %4;}"
                                "QLabel{color:%1;background:transparent;}" )
                           .arg( Fg.name(), Rgba( Fg, 40 ), Rgba( Fg, 66 ), Rgba( Fg, 170 ) ) );
        for( auto* Button : { prev_, play_, next_, volume_, close_, external_ } ) Button->setFixedHeight( wellHeight_ - 4 );
        track_->setFixedHeight( wellHeight_ - 4 );
        prev_->setIcon( MediaIcon( MediaGlyph::Previous, Fg ) ); next_->setIcon( MediaIcon( MediaGlyph::Next, Fg ) );
        close_->setIcon( MediaIcon( MediaGlyph::Close, Fg, 10 ) ); track_->setIcon( MediaIcon( MediaGlyph::Note, Fg ) );
        refreshVolume();
        auto Small = fm::style::pixelFont( font(), 11 ); Small.setFeature( QFont::Tag( "tnum" ), 1 );
        for( auto* Label : { position_, duration_, remaining_ } ) Label->setFont( Small );
        track_->setFont( fm::style::pixelFont( font(), 12 ) ); error_->setFont( fm::style::pixelFont( font(), 12 ) );
        seek_->SetColors( trackColor_, Fg, Fg, false );
        refreshState();
        update();
    }

    void AudioTitleWidget::refreshVolume()
    {
        const bool Muted = session_ && session_->Muted();
        volume_->setIcon( MediaIcon( Muted ? MediaGlyph::Mute : MediaGlyph::Volume, foreground_, 14 ) ); volume_->setIconSize( QSize( 14, 14 ) );
        volume_->setAccessibleName( Muted ? tr( "음소거 해제" ) : tr( "음소거" ) );
        volume_->setToolTip( tr( "%1 · 볼륨 %2% · 휠로 조절" ).arg( volume_->accessibleName() ).arg( session_ ? session_->Volume() : 0 ) );
    }

    void AudioTitleWidget::refreshState()
    {
        using hc::audio::SessionState;
        const auto State = session_ ? session_->State() : SessionState::Idle;
        setVisible( State != SessionState::Idle );
        const bool Pause = State == SessionState::Playing || State == SessionState::Loading || State == SessionState::Resolving;
        const int Disc = wellHeight_ == 24 ? 18 : 16;
        play_->setIcon( MediaDiscIcon( Pause, foreground_, glyph_, Disc ) ); play_->setIconSize( QSize( Disc, Disc ) );
        play_->setAccessibleName( State == SessionState::Failed ? tr( "다시 시도" ) : Pause ? tr( "일시정지" ) : tr( "재생" ) );
        play_->setToolTip( play_->accessibleName() );
        if( State == SessionState::Failed ) error_->setText( session_->ErrorText() );
        if( State == SessionState::Loading || State == SessionState::Resolving ) position_->setText( QStringLiteral( "…" ) );
        updateTier();
    }

    void AudioTitleWidget::refreshTrack()
    {
        const auto* Track = session_ ? session_->Current() : nullptr;
        if( !Track ) { trackText_.clear(); track_->setText( {} ); return; }
        const auto Base = QFileInfo( Track->name_ ).completeBaseName();
        trackText_ = Base + QStringLiteral( " · " ) + FolderName( Track->folder_ );
        const auto& Meta = session_->Metadata();
        auto Tip = Track->path_;
        if( !Meta.title_.isEmpty() ) Tip = Meta.title_ + ( Meta.artist_.isEmpty() ? QString() : QStringLiteral( " — " ) + Meta.artist_ ) + '\n' + Tip;
        track_->setToolTip( Tip + '\n' + tr( "눌러서 재생 정보 · 대기열 열기" ) );
        track_->setAccessibleName( tr( "%1 · 재생 정보 열기" ).arg( Base ) );
        track_->setText( track_->fontMetrics().elidedText( trackText_, Qt::ElideRight, 230 - 34 ) );
        refreshPosition();
    }

    void AudioTitleWidget::refreshPosition()
    {
        if( !session_ ) return;
        const auto Position = session_->Position(), Duration = session_->Duration();
        seek_->SetPosition( Position, Duration );
        if( session_->State() != hc::audio::SessionState::Loading && session_->State() != hc::audio::SessionState::Resolving )
            position_->setText( hc::audio::FormatDuration( Position ) );
        duration_->setText( hc::audio::FormatDuration( Duration ) );
        remaining_->setText( Duration > 0 ? QStringLiteral( "-" ) + hc::audio::FormatDuration( std::max<qint64>( 0, Duration - Position ) ) : hc::audio::FormatDuration( Position ) );
        if( ring_->isVisible() ) update( ring_->geometry() );
    }

    int AudioTitleWidget::tierWidth( Tier Value ) const
    {
        const int Button = 26 + 2, Separator = 9 + 2, Outer = 16 + 8 + 8;
        const int Track = std::min( 230, track_->sizeHint().width() ) + 2;
        const int Middle = Outer + 3 * Button + Separator + ( 36 + 2 ) + ( 166 + 2 ) + ( 36 + 2 ) + Separator + 2 * Button;
        if( Value == Tier::Full ) return Middle + Track + Separator;
        if( Value == Tier::Middle ) return Middle;
        return Outer + Button + ( 22 + 2 ) + ( 44 + 2 ) + Button;
    }

    void AudioTitleWidget::updateTier()
    {
        auto* Chrome = parentWidget();
        if( !Chrome ) { applyTier( Tier::Full ); return; }
        int Captions = 0;
        for( const auto* Button : Chrome->findChildren<QToolButton*>( Qt::FindDirectChildrenOnly ) )
            if( Button->objectName().startsWith( "fm.window." ) && Button->isVisible() ) Captions += Button->width();
        // 앱 기호와 최소 제목 폭(160 px)은 남긴다.
        const auto Available = Chrome->width() - Captions - 38 - 160;
        applyTier( tierWidth( Tier::Full ) <= Available ? Tier::Full : tierWidth( Tier::Middle ) <= Available ? Tier::Middle : Tier::Compact );
    }

    void AudioTitleWidget::applyTier( Tier Value )
    {
        tier_ = Value;
        const bool Failed = session_ && session_->State() == hc::audio::SessionState::Failed;
        const bool Normal = !Failed, Wide = Value != Tier::Compact;
        prev_->setVisible( Normal && Wide ); next_->setVisible( Normal && Wide );
        separators_.at( 0 )->setVisible( Normal && Wide );
        track_->setVisible( Normal && Value == Tier::Full ); separators_.at( 1 )->setVisible( Normal && Value == Tier::Full );
        for( auto* Item : std::initializer_list<QWidget*>{ position_, seek_, duration_ } ) Item->setVisible( Normal && Wide );
        ring_->setVisible( Normal && !Wide ); remaining_->setVisible( Normal && !Wide );
        error_->setVisible( Failed && Wide );
        const auto* Track = session_ ? session_->Current() : nullptr;
        external_->setVisible( Failed && Track && !Track->localPath_.isEmpty() );
        separators_.at( 2 )->setVisible( Normal && Wide ); volume_->setVisible( Normal && Wide );
        play_->setToolTip( Value == Tier::Full ? play_->accessibleName() : play_->accessibleName() + '\n' + trackText_ );
        updateGeometry();
        update();
    }

    void AudioTitleWidget::paintEvent( QPaintEvent* )
    {
        QPainter Painter( this ); Painter.setRenderHint( QPainter::Antialiasing );
        const QRectF Well = QRectF( rect() ).adjusted( 16.5, 0.5, -8.5, -0.5 );
        Painter.setPen( QPen( wellLine_, 1 ) ); Painter.setBrush( well_ );
        Painter.drawRoundedRect( Well, wellHeight_ == 24 ? 6 : 4, wellHeight_ == 24 ? 6 : 4 );
        auto Line = foreground_; Line.setAlpha( 62 ); Painter.setPen( QPen( Line, 1 ) );
        for( const auto* Item : separators_ )
            if( Item->isVisible() ) { const auto X = Item->geometry().center().x() + 0.5; Painter.drawLine( QPointF( X, height() / 2.0 - 7 ), QPointF( X, height() / 2.0 + 7 ) ); }
        if( ring_->isVisible() && session_ )
        {
            // 좁은 창: 원형 진행
            const auto Center = QRectF( ring_->geometry() ).center();
            const QRectF Circle( Center.x() - 7, Center.y() - 7, 14, 14 );
            Painter.setBrush( Qt::NoBrush );
            Painter.setPen( QPen( trackColor_, 2 ) ); Painter.drawEllipse( Circle );
            const auto Duration = session_->Duration();
            if( Duration > 0 )
            {
                Painter.setPen( QPen( foreground_, 2, Qt::SolidLine, Qt::RoundCap ) );
                Painter.drawArc( Circle, 90 * 16, -int( 360 * 16 * std::clamp( double( session_->Position() ) / Duration, 0.0, 1.0 ) ) );
            }
        }
    }

    void AudioTitleWidget::mousePressEvent( QMouseEvent* Event ) { Event->accept(); }   // 위젯 위에서는 창을 끌지 않는다.
    void AudioTitleWidget::mouseDoubleClickEvent( QMouseEvent* Event ) { Event->accept(); }

    bool AudioTitleWidget::eventFilter( QObject* Object, QEvent* Event )
    {
        if( Object == volume_ && Event->type() == QEvent::Wheel && session_ )
        {
            const auto Steps = static_cast<QWheelEvent*>( Event )->angleDelta().y() / 120;
            if( Steps ) session_->SetVolume( session_->Volume() + Steps * 5 );
            return true;
        }
        if( Object == parentWidget() && ( Event->type() == QEvent::Resize || Event->type() == QEvent::Show ) ) updateTier();
        if( Object == window() && Event->type() == QEvent::ActivationChange ) refreshTheme();
        return QWidget::eventFilter( Object, Event );
    }
}
