#include "stdafx.h"
#include "audioEngine.hpp"

/// 모듈

/// UIs

/// 기타 헤더파일

/// 고유 라이브러리

/// 공통 라이브러리
#include "commonLibs/layout/productLayout.hpp"

/// 외부 라이브러리

/// 선언 헤더파일들

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QAudioDevice>
#include <QAudioOutput>
#include <QCoreApplication>
#include <QDir>
#include <QLoggingCategory>
#include <QMediaDevices>
#include <QMediaMetaData>
#include <QMediaPlayer>
#include <QUrl>
#include <QtMultimedia/qtaudio.h>
#include <Windows.h>

DECLARE_CMNLIBSV2_NAMESPACE

namespace hc::audio
{
    namespace
    {
        Q_LOGGING_CATEGORY( engineLog, "hc.audio.engine" )

        QString MetaText( const QMediaMetaData& Meta, QMediaMetaData::Key Key )
        {
            const auto Text = Meta.stringValue( Key ).trimmed();
            // FFmpeg 백엔드는 QMediaFormat에 없는 코덱을 "Unspecified"로 알린다(실측: aiff·amr·dts·mp2·wma·wv).
            return Text.compare( "Unspecified", Qt::CaseInsensitive ) == 0 ? QString() : Text;
        }

        class QtAudioEngine final : public IAudioEngine
        {
        public:
            bool IsAvailable() override { ensure(); return player_ && player_->isAvailable(); }
            QString UnavailableReason() const override { return unavailable_; }
            void Open( const QString& LocalPath ) override
            {
                if( !IsAvailable() ) { emit sigFailed( ErrorKind::Unavailable, unavailable_ ); return; }
                ended_ = false; emit sigStateChanged( EngineState::Loading );
                player_->setSource( QUrl::fromLocalFile( LocalPath ) );
            }
            void Play() override { if( player_ ) player_->play(); }
            void Pause() override { if( player_ ) player_->pause(); }
            void Stop() override
            {
                if( !player_ ) return;
                // stop()만으로는 FFmpeg가 연 파일 핸들이 남는다(실측 S03). 소스를 비워야 놓는다.
                stopping_ = true; player_->stop(); player_->setSource( QUrl() ); stopping_ = false;
            }
            void Seek( qint64 PositionMs ) override { if( player_ && player_->isSeekable() ) player_->setPosition( std::max<qint64>( 0, PositionMs ) ); }
            void SetVolume( int Percent ) override
            {
                volume_ = std::clamp( Percent, 0, 100 );
                if( output_ ) output_->setVolume( QtAudio::convertVolume( volume_ / 100.0f, QtAudio::LogarithmicVolumeScale, QtAudio::LinearVolumeScale ) );
            }
            void SetMuted( bool Muted ) override { muted_ = Muted; if( output_ ) output_->setMuted( Muted ); }

        private:
            void ensure()
            {
                if( attempted_ ) return;
                attempted_ = true;
                if( !PreloadFfmpeg( &unavailable_ ) ) { qCWarning( engineLog ).noquote() << unavailable_; return; }
                player_ = new QMediaPlayer( this ); output_ = new QAudioOutput( this ); devices_ = new QMediaDevices( this );
                player_->setAudioOutput( output_ );
                SetVolume( volume_ ); SetMuted( muted_ );
                if( !player_->isAvailable() )
                {
                    // ffmpegmediaplugin.dll은 Media Foundation을 정적으로 쓴다. Media Feature Pack이 없는 N 에디션이 여기로 온다.
                    unavailable_ = QObject::tr( "이 Windows에는 미디어 기능이 없어 재생할 수 없습니다. Media Feature Pack을 설치하십시오." );
                    qCWarning( engineLog ).noquote() << "media backend unavailable";
                    return;
                }
                connect( devices_, &QMediaDevices::audioOutputsChanged, this, [ this ] {
                    // 시스템 기본 장치를 따른다(계획 D18).
                    output_->setDevice( QMediaDevices::defaultAudioOutput() );
                } );
                connect( player_, &QMediaPlayer::mediaStatusChanged, this, [ this ]( QMediaPlayer::MediaStatus Status ) {
                    if( stopping_ ) return;
                    if( Status == QMediaPlayer::EndOfMedia && !ended_ ) { ended_ = true; emit sigStateChanged( EngineState::Ended ); }
                    else if( Status == QMediaPlayer::InvalidMedia ) emit sigFailed( ErrorKind::Unsupported, player_->errorString() );
                } );
                connect( player_, &QMediaPlayer::playbackStateChanged, this, [ this ]( QMediaPlayer::PlaybackState State ) {
                    if( stopping_ ) return;
                    if( State == QMediaPlayer::PlayingState ) emit sigStateChanged( EngineState::Playing );
                    else if( State == QMediaPlayer::PausedState ) emit sigStateChanged( EngineState::Paused );
                } );
                connect( player_, &QMediaPlayer::errorOccurred, this, [ this ]( QMediaPlayer::Error Error, const QString& Text ) {
                    if( stopping_ || Error == QMediaPlayer::NoError ) return;
                    qCWarning( engineLog ).noquote() << "player error" << int( Error ) << Text;
                    emit sigFailed( Error == QMediaPlayer::FormatError ? ErrorKind::Unsupported : ErrorKind::Io, Text );
                } );
                connect( player_, &QMediaPlayer::positionChanged, this, &IAudioEngine::sigPositionChanged );
                connect( player_, &QMediaPlayer::durationChanged, this, &IAudioEngine::sigDurationChanged );
                connect( player_, &QMediaPlayer::metaDataChanged, this, [ this ] {
                    const auto Meta = player_->metaData();
                    AudioMetadata Value;
                    Value.title_ = MetaText( Meta, QMediaMetaData::Title );
                    Value.artist_ = MetaText( Meta, QMediaMetaData::ContributingArtist );
                    if( Value.artist_.isEmpty() ) Value.artist_ = MetaText( Meta, QMediaMetaData::AlbumArtist );
                    Value.album_ = MetaText( Meta, QMediaMetaData::AlbumTitle );
                    Value.codec_ = MetaText( Meta, QMediaMetaData::AudioCodec );
                    Value.bitRate_ = Meta.value( QMediaMetaData::AudioBitRate ).toInt();
                    Value.cover_ = Meta.value( QMediaMetaData::CoverArtImage ).value<QImage>();
                    if( Value.cover_.isNull() ) Value.cover_ = Meta.value( QMediaMetaData::ThumbnailImage ).value<QImage>();
                    emit sigMetadataChanged( Value );
                } );
            }

            QMediaPlayer*       player_ = nullptr;
            QAudioOutput*       output_ = nullptr;
            QMediaDevices*      devices_ = nullptr;
            QString             unavailable_;
            int                 volume_ = 70;
            bool                muted_ = false;
            bool                attempted_ = false;
            bool                stopping_ = false;
            bool                ended_ = false;
        };
    }

    bool PreloadFfmpeg( QString* Error )
    {
        static int Loaded = -1;
        static QString Failure;
        if( Loaded < 0 )
        {
            Loaded = 1;
            const auto Directory = QDir( QCoreApplication::applicationDirPath() ).filePath( hc::layout::FfmpegLibraries );
            // 의존 순서대로 올린다. 핸들은 프로세스가 끝날 때까지 유지한다.
            for( const auto* Name : { L"avutil-60.dll", L"swresample-6.dll", L"swscale-9.dll", L"avcodec-62.dll", L"avformat-62.dll" } )
            {
                const auto Path = QDir::toNativeSeparators( QDir( Directory ).filePath( QString::fromWCharArray( Name ) ) ).toStdWString();
                if( LoadLibraryExW( Path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS ) ) continue;
                Loaded = 0;
                Failure = QObject::tr( "재생 구성 요소를 불러오지 못했습니다: %1 (Windows 오류 %2)" ).arg( QString::fromStdWString( Path ) ).arg( GetLastError() );
                break;
            }
        }
        if( Error && !Loaded ) *Error = Failure;
        return Loaded == 1;
    }

    std::unique_ptr<IAudioEngine> MakeQtAudioEngine() { return std::make_unique<QtAudioEngine>(); }
}
