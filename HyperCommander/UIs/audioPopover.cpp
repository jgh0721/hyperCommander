#include "stdafx.h"
#include "audioPopover.hpp"

/// 모듈

/// UIs
#include "audioTitleWidget.hpp"

/// 기타 헤더파일

/// 고유 라이브러리
#include "UniqueLibs/audio/audioSession.hpp"

/// 공통 라이브러리

/// 외부 라이브러리
#include <fmstyle/StylePaint.h>

/// 선언 헤더파일들

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QCheckBox>
#include <QDir>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QImageReader>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QScreen>
#include <QSlider>
#include <QToolButton>
#include <QVBoxLayout>

DECLARE_CMNLIBSV2_NAMESPACE

namespace hc::ui
{
    namespace
    {
        QString SizeText( std::uint64_t Bytes )
        {
            return Bytes >= 1024 * 1024 ? QString( "%1 MB" ).arg( Bytes / 1048576.0, 0, 'f', 1 ) : QString( "%1 KB" ).arg( std::max<std::uint64_t>( 1, Bytes / 1024 ) );
        }

        // 태그에 표지가 없으면 같은 폴더의 cover·folder·front 그림을 쓴다(로컬 경로만).
        QImage FolderCover( const QString& Folder )
        {
            if( Folder.isEmpty() ) return {};
            const QDir Directory( Folder );
            for( const auto& Name : Directory.entryList( { "cover.*", "folder.*", "front.*" }, QDir::Files, QDir::Name ) )
            {
                QImageReader Reader( Directory.filePath( Name ) );
                if( !Reader.canRead() ) continue;
                Reader.setScaledSize( Reader.size().scaled( 112, 112, Qt::KeepAspectRatio ) );
                if( const auto Image = Reader.read(); !Image.isNull() ) return Image;
            }
            return {};
        }
    }

    AudioPopover::AudioPopover( hc::audio::AudioSession* Session, QWidget* Parent )
        : QFrame( Parent, Qt::Popup ), session_( Session )
    {
        setObjectName( "hc.audio.popover" ); setAccessibleName( tr( "재생 정보" ) );
        setFrameShape( QFrame::StyledPanel ); setFixedWidth( 400 );
        auto* Layout = new QVBoxLayout( this ); Layout->setContentsMargins( 16, 16, 16, 14 ); Layout->setSpacing( 12 );

        auto* Header = new QHBoxLayout; Header->setSpacing( 12 );
        cover_ = new QLabel( this ); cover_->setFixedSize( 56, 56 ); cover_->setAlignment( Qt::AlignCenter ); cover_->setObjectName( "hc.audio.cover" );
        auto* Texts = new QVBoxLayout; Texts->setSpacing( 2 );
        title_ = new QLabel( this ); title_->setFont( fm::style::pixelFont( font(), 15, QFont::DemiBold ) ); title_->setTextFormat( Qt::PlainText );
        folder_ = new QLabel( this ); folder_->setTextFormat( Qt::PlainText ); folder_->setForegroundRole( QPalette::PlaceholderText );
        format_ = new QLabel( this ); format_->setTextFormat( Qt::PlainText ); format_->setFont( fm::style::pixelFont( font(), 11 ) ); format_->setForegroundRole( QPalette::PlaceholderText );
        Texts->addWidget( title_ ); Texts->addWidget( folder_ ); Texts->addWidget( format_ );
        Header->addWidget( cover_ ); Header->addLayout( Texts, 1 );
        Layout->addLayout( Header );

        seek_ = new AudioSeekBar( this ); seek_->setObjectName( "hc.audio.popover.seek" ); seek_->setFixedHeight( 18 );
        auto Small = fm::style::pixelFont( font(), 11 ); Small.setFeature( QFont::Tag( "tnum" ), 1 );
        position_ = new QLabel( this ); remaining_ = new QLabel( this ); remaining_->setAlignment( Qt::AlignRight );
        for( auto* Label : { position_, remaining_ } ) { Label->setFont( Small ); Label->setForegroundRole( QPalette::PlaceholderText ); }
        auto* Times = new QHBoxLayout; Times->addWidget( position_ ); Times->addStretch(); Times->addWidget( remaining_ );
        auto* Seek = new QVBoxLayout; Seek->setSpacing( 2 ); Seek->addWidget( seek_ ); Seek->addLayout( Times );
        Layout->addLayout( Seek );

        const auto Tool = [ this ]( const char* Name, const QString& Text, int Size ) {
            auto* Button = new QToolButton( this ); Button->setObjectName( Name ); Button->setAccessibleName( Text ); Button->setToolTip( Text );
            Button->setAutoRaise( true ); Button->setFixedSize( Size, Size ); Button->setIconSize( QSize( 14, 14 ) ); return Button;
        };
        repeat_ = Tool( "hc.audio.repeat", tr( "반복" ), 32 ); repeat_->setCheckable( true );
        prev_ = Tool( "hc.audio.popover.prev", tr( "이전 곡" ), 32 );
        play_ = Tool( "hc.audio.popover.playPause", tr( "재생" ), 40 ); play_->setIconSize( QSize( 36, 36 ) );
        next_ = Tool( "hc.audio.popover.next", tr( "다음 곡" ), 32 );
        shuffle_ = Tool( "hc.audio.shuffle", tr( "무작위 재생" ), 32 ); shuffle_->setCheckable( true );
        volume_ = new QSlider( Qt::Horizontal, this ); volume_->setObjectName( "hc.audio.volumeSlider" ); volume_->setAccessibleName( tr( "볼륨" ) );
        volume_->setRange( 0, 100 ); volume_->setSingleStep( 5 ); volume_->setPageStep( 10 ); volume_->setFixedWidth( 90 );
        volumeText_ = new QLabel( this ); volumeText_->setFont( Small ); volumeText_->setFixedWidth( 34 );
        auto* Controls = new QHBoxLayout; Controls->setSpacing( 4 );
        for( auto* Button : { repeat_, prev_, play_, next_, shuffle_ } ) Controls->addWidget( Button );
        Controls->addStretch(); Controls->addWidget( volume_ ); Controls->addWidget( volumeText_ );
        Layout->addLayout( Controls );

        auto* Line = new QFrame( this ); Line->setFrameShape( QFrame::HLine ); Line->setFrameShadow( QFrame::Plain ); Line->setForegroundRole( QPalette::Mid );
        Layout->addWidget( Line );
        queueTitle_ = new QLabel( this ); queueTitle_->setFont( fm::style::pixelFont( font(), 12, QFont::DemiBold ) );
        locate_ = new QPushButton( tr( "패널에서 위치 보기" ), this ); locate_->setObjectName( "hc.audio.locate" ); locate_->setFlat( true ); locate_->setCursor( Qt::PointingHandCursor );
        auto* QueueHeader = new QHBoxLayout; QueueHeader->addWidget( queueTitle_ ); QueueHeader->addStretch(); QueueHeader->addWidget( locate_ );
        Layout->addLayout( QueueHeader );
        queue_ = new QListWidget( this ); queue_->setObjectName( "hc.audio.queue" ); queue_->setAccessibleName( tr( "대기열" ) );
        queue_->setUniformItemSizes( true ); queue_->setFixedHeight( 7 * 26 + 4 ); queue_->setFrameShape( QFrame::NoFrame );
        Layout->addWidget( queue_ );
        autoNext_ = new QCheckBox( tr( "곡이 끝나면 같은 폴더의 다음 오디오 파일 재생" ), this ); autoNext_->setObjectName( "hc.audio.autoNext" );
        Layout->addWidget( autoNext_ );

        connect( seek_, &AudioSeekBar::sigSeekRequested, this, [ this ]( qint64 Position ) { if( session_ ) session_->Seek( Position ); } );
        connect( prev_, &QToolButton::clicked, this, [ this ] { if( session_ ) session_->Previous(); } );
        connect( play_, &QToolButton::clicked, this, [ this ] { if( session_ ) session_->PlayPause(); } );
        connect( next_, &QToolButton::clicked, this, [ this ] { if( session_ ) session_->Next(); } );
        connect( repeat_, &QToolButton::clicked, this, [ this ] {
            if( !session_ ) return;
            using hc::audio::RepeatMode;
            const auto Mode = session_->Queue().Repeat();
            session_->SetRepeat( Mode == RepeatMode::Off ? RepeatMode::Folder : Mode == RepeatMode::Folder ? RepeatMode::One : RepeatMode::Off );
        } );
        connect( shuffle_, &QToolButton::clicked, this, [ this ] { if( session_ ) session_->SetShuffle( !session_->Queue().Shuffle() ); } );
        connect( volume_, &QSlider::valueChanged, this, [ this ]( int Value ) { if( session_ && session_->Volume() != Value ) session_->SetVolume( Value ); } );
        connect( autoNext_, &QCheckBox::toggled, this, [ this ]( bool Checked ) { if( session_ && session_->AutoNext() != Checked ) session_->SetAutoNext( Checked ); } );
        connect( queue_, &QListWidget::itemActivated, this, [ this ]( QListWidgetItem* Item ) { if( session_ && Item ) session_->SelectTrack( Item->data( Qt::UserRole ).toLongLong() ); } );
        connect( locate_, &QPushButton::clicked, this, [ this ] { hide(); emit sigLocateRequested(); } );
        if( Session )
        {
            connect( Session, &hc::audio::AudioSession::sigTrackChanged, this, &AudioPopover::refreshTrack );
            connect( Session, &hc::audio::AudioSession::sigStateChanged, this, &AudioPopover::refreshState );
            connect( Session, &hc::audio::AudioSession::sigPositionChanged, this, &AudioPopover::refreshPosition );
            connect( Session, &hc::audio::AudioSession::sigModeChanged, this, &AudioPopover::refreshMode );
        }
    }

    void AudioPopover::ShowBelow( QWidget* Anchor )
    {
        refreshTrack(); refreshMode(); refreshState();
        auto Position = Anchor ? Anchor->mapToGlobal( QPoint( 0, Anchor->height() + 4 ) ) : QCursor::pos();
        adjustSize();
        if( const auto* Screen = Anchor ? Anchor->screen() : screen() )
        {
            const auto Area = Screen->availableGeometry();
            Position.setX( std::clamp( Position.x(), Area.left() + 4, Area.right() - width() - 4 ) );
            Position.setY( std::min( Position.y(), Area.bottom() - height() - 4 ) );
        }
        move( Position );
        show();
        queue_->setFocus( Qt::PopupFocusReason );
    }

    void AudioPopover::refreshTrack()
    {
        const auto* Track = session_ ? session_->Current() : nullptr;
        if( !Track ) { if( isVisible() ) hide(); return; }
        const auto& Meta = session_->Metadata();
        title_->setText( fontMetrics().elidedText( Meta.title_.isEmpty() ? QFileInfo( Track->name_ ).completeBaseName() : Meta.title_, Qt::ElideRight, 300 ) );
        const auto Folder = QDir::toNativeSeparators( Track->folder_ );
        folder_->setText( folder_->fontMetrics().elidedText( Meta.artist_.isEmpty() ? Folder : Meta.artist_ + QStringLiteral( " · " ) + Folder, Qt::ElideMiddle, 300 ) );
        folder_->setToolTip( Track->path_ );
        QStringList Format;
        Format << ( Meta.codec_.isEmpty() ? QFileInfo( Track->name_ ).suffix().toUpper() : Meta.codec_ );
        if( Meta.bitRate_ > 0 ) Format << tr( "%1 kbps" ).arg( Meta.bitRate_ / 1000 );
        if( Track->size_ ) Format << SizeText( Track->size_ );
        format_->setText( Format.join( QStringLiteral( " · " ) ) );
        auto Cover = Meta.cover_;
        if( Cover.isNull() && !Track->localPath_.isEmpty() ) Cover = FolderCover( QFileInfo( Track->localPath_ ).absolutePath() );
        if( Cover.isNull() ) { cover_->setPixmap( MediaIcon( MediaGlyph::Note, palette().color( QPalette::Highlight ), 24 ).pixmap( 24, 24 ) ); cover_->setStyleSheet( QString( "background:%1;border-radius:4px;" ).arg( palette().color( QPalette::AlternateBase ).name() ) ); }
        else { const auto Dpr = devicePixelRatioF(); auto Pixmap = QPixmap::fromImage( Cover.scaled( QSize( 56, 56 ) * Dpr, Qt::KeepAspectRatio, Qt::SmoothTransformation ) ); Pixmap.setDevicePixelRatio( Dpr ); cover_->setPixmap( Pixmap ); cover_->setStyleSheet( {} ); }
        refreshQueue();
        refreshPosition();
    }

    void AudioPopover::refreshState()
    {
        if( !session_ || !session_->IsActive() ) { if( isVisible() ) hide(); return; }
        const bool Pause = session_->State() == hc::audio::SessionState::Playing || session_->State() == hc::audio::SessionState::Loading || session_->State() == hc::audio::SessionState::Resolving;
        play_->setIcon( MediaDiscIcon( Pause, palette().color( QPalette::Highlight ), palette().color( QPalette::HighlightedText ), 36 ) );
        play_->setAccessibleName( Pause ? tr( "일시정지" ) : tr( "재생" ) ); play_->setToolTip( play_->accessibleName() );
        if( session_->State() == hc::audio::SessionState::Failed ) format_->setText( session_->ErrorText() );
        refreshQueue();
    }

    void AudioPopover::refreshPosition()
    {
        if( !session_ || !isVisible() ) return;
        const auto Position = session_->Position(), Duration = session_->Duration();
        seek_->SetPosition( Position, Duration );
        position_->setText( hc::audio::FormatDuration( Position ) );
        remaining_->setText( Duration > 0 ? QStringLiteral( "-" ) + hc::audio::FormatDuration( std::max<qint64>( 0, Duration - Position ) ) : QString() );
    }

    void AudioPopover::refreshMode()
    {
        if( !session_ ) return;
        const auto Text = palette().color( QPalette::WindowText ), Accent = palette().color( QPalette::Highlight );
        using hc::audio::RepeatMode;
        const auto Repeat = session_->Queue().Repeat();
        repeat_->setChecked( Repeat != RepeatMode::Off );
        repeat_->setIcon( MediaIcon( Repeat == RepeatMode::One ? MediaGlyph::RepeatOne : MediaGlyph::Repeat, Repeat == RepeatMode::Off ? Text : Accent, 14 ) );
        repeat_->setAccessibleName( Repeat == RepeatMode::One ? tr( "한 곡 반복" ) : Repeat == RepeatMode::Folder ? tr( "폴더 반복" ) : tr( "반복 안 함" ) );
        repeat_->setToolTip( repeat_->accessibleName() );
        shuffle_->setChecked( session_->Queue().Shuffle() );
        shuffle_->setIcon( MediaIcon( MediaGlyph::Shuffle, session_->Queue().Shuffle() ? Accent : Text, 14 ) );
        prev_->setIcon( MediaIcon( MediaGlyph::Previous, Text, 14 ) ); next_->setIcon( MediaIcon( MediaGlyph::Next, Text, 14 ) );
        const QSignalBlocker Volume( volume_ ), Auto( autoNext_ );
        volume_->setValue( session_->Volume() ); volumeText_->setText( session_->Muted() ? tr( "음소거" ) : QString( "%1%" ).arg( session_->Volume() ) );
        autoNext_->setChecked( session_->AutoNext() );
    }

    void AudioPopover::refreshQueue()
    {
        if( !session_ ) return;
        const auto& Queue = session_->Queue();
        queueTitle_->setText( tr( "대기열 — 이 폴더의 오디오 %1개" ).arg( Queue.Size() ) );
        const auto Accent = palette().color( QPalette::Highlight );
        const bool Rebuild = queue_->count() != Queue.Size();
        if( Rebuild ) queue_->clear();
        for( qsizetype Index = 0; Index < Queue.Size(); ++Index )
        {
            auto* Item = Rebuild ? new QListWidgetItem( QFileInfo( Queue.At( Index )->name_ ).completeBaseName(), queue_ ) : queue_->item( int( Index ) );
            Item->setData( Qt::UserRole, qlonglong( Index ) ); Item->setSizeHint( QSize( 0, 26 ) );
            const bool Current = Index == Queue.CurrentIndex();
            auto Font = queue_->font(); Font.setBold( Current ); Item->setFont( Font );
            Item->setIcon( Current ? MediaIcon( session_->State() == hc::audio::SessionState::Playing ? MediaGlyph::Volume : MediaGlyph::Pause, Accent, 14 ) : QIcon() );
            Item->setForeground( Current ? Accent : palette().color( QPalette::WindowText ) );
            Item->setToolTip( Queue.At( Index )->path_ );
            if( Current ) queue_->scrollToItem( Item );
        }
    }
}
