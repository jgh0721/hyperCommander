#include "stdafx.h"
#include "audioSession.hpp"

/// 모듈

/// UIs

/// 기타 헤더파일

/// 고유 라이브러리
#include "UniqueLibs/nativeMaterializer.hpp"
#include "UniqueLibs/settings/appSettings.hpp"

/// 공통 라이브러리
#include "commonLibs/storage/materializedContent.hpp"

/// 외부 라이브러리

/// 선언 헤더파일들

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QtConcurrent/QtConcurrentRun>
#include <QDir>
#include <QFutureWatcher>
#include <QLoggingCategory>
#include <QRandomGenerator>

DECLARE_CMNLIBSV2_NAMESPACE

namespace hc::audio
{
    namespace
    {
        Q_LOGGING_CATEGORY( sessionLog, "hc.audio.session" )

        // 실체화 결과. QFuture가 기본 생성·복사할 수 있도록 Result를 풀어 담는다.
        struct Resolved
        {
            std::shared_ptr<hc::storage::IMaterializedFileLease> lease_;
            QString error_;
            bool cancelled_ = false;
        };

        QString TemporaryBase() { return QDir( QDir::tempPath() ).filePath( "hc-audio-preview" ); }

        QString NativeKey( const QString& Path ) { return QDir::toNativeSeparators( QDir::cleanPath( Path ) ).toLower(); }

        void LogCleanup( const char* Stage, const hc::storage::MaterializationCleanup& Report )
        {
            if( !Report.removed_ && !Report.busy_ && Report.errors_.empty() ) return;
            qCInfo( sessionLog ).noquote() << Stage << "removed" << Report.removed_ << "busy" << Report.busy_ << "errors" << Report.errors_.size();
            // 모르는 항목이 든 임대는 지우지 않고 경로만 남긴다(unexpected-lease-entry).
            for( const auto& Error : Report.errors_ )
                qCWarning( sessionLog ).noquote() << Stage << QString::fromStdString( Error.code_ ) << QString::fromStdString( Error.userAction_ );
        }
    }

    AudioSession::AudioSession( QObject* Parent, EngineFactory Factory )
        : QObject( Parent ), factory_( Factory ? std::move( Factory ) : EngineFactory( [] { return MakeQtAudioEngine(); } ) )
    {
        saveTimer_.setSingleShot( true ); saveTimer_.setInterval( 1000 );
        connect( &saveTimer_, &QTimer::timeout, this, &AudioSession::save );
        ReloadSettings();
    }

    AudioSession::~AudioSession() { Shutdown(); }

    void AudioSession::ReloadSettings()
    {
        const auto Settings = hc::settings::AppSettings().Read( hc::settings::Section::General ).toObject().value( "audioPreview" ).toObject();
        extensions_ = ParseExtensions( Settings.value( "extensions" ).toString() );
        if( extensions_.isEmpty() ) extensions_ = DefaultExtensions();
        volume_ = std::clamp( Settings.value( "volume" ).toInt( 70 ), 0, 100 );
        muted_ = Settings.value( "muted" ).toBool( false );
        autoNext_ = Settings.value( "autoNext" ).toBool( true );
        animateNowPlaying_ = Settings.value( "animateNowPlaying" ).toBool( true );
        materializeLimitBytes_ = qint64( std::clamp( Settings.value( "materializeLimitMiB" ).toInt( 1024 ), 16, 8192 ) ) * 1024 * 1024;
        queue_.SetRepeat( RepeatModeFromName( Settings.value( "repeat" ).toString() ) );
        if( queue_.Shuffle() != Settings.value( "shuffle" ).toBool( false ) ) queue_.SetShuffle( !queue_.Shuffle(), QRandomGenerator::global()->generate() );
        if( engine_ ) { engine_->SetVolume( volume_ ); engine_->SetMuted( muted_ ); }
        emit sigModeChanged();
    }

    bool AudioSession::OpenInternally() const
    {
        return hc::settings::AppSettings().Read( hc::settings::Section::Options ).toObject().value( "audioOpenInternally" ).toBool( false );
    }

    bool AudioSession::IsAvailable() { return engine() != nullptr; }

    IAudioEngine* AudioSession::engine()
    {
        if( engineChecked_ ) return engine_.get();
        engineChecked_ = true;
        auto Engine = factory_();
        if( !Engine || !Engine->IsAvailable() )
        {
            unavailable_ = Engine && !Engine->UnavailableReason().isEmpty() ? Engine->UnavailableReason() : tr( "이 시스템에서는 오디오를 재생할 수 없습니다." );
            qCWarning( sessionLog ).noquote() << "engine unavailable:" << unavailable_;
            return nullptr;
        }
        engine_ = std::move( Engine );
        engine_->SetVolume( volume_ ); engine_->SetMuted( muted_ );
        connect( engine_.get(), &IAudioEngine::sigStateChanged, this, [ this ]( EngineState State ) {
            if( state_ == SessionState::Idle || state_ == SessionState::Failed ) return;
            switch( State )
            {
                case EngineState::Loading: setState( SessionState::Loading ); break;
                case EngineState::Playing: setState( SessionState::Playing ); break;
                case EngineState::Paused: setState( SessionState::Paused ); break;
                case EngineState::Ended: finishTrack(); break;
                default: break;
            }
        } );
        connect( engine_.get(), &IAudioEngine::sigPositionChanged, this, [ this ]( qint64 Position ) {
            if( state_ == SessionState::Idle ) return;
            position_ = Position; emit sigPositionChanged( position_ );
        } );
        connect( engine_.get(), &IAudioEngine::sigDurationChanged, this, [ this ]( qint64 Duration ) {
            duration_ = Duration > 0 ? Duration : -1; emit sigPositionChanged( position_ );
        } );
        connect( engine_.get(), &IAudioEngine::sigMetadataChanged, this, [ this ]( const AudioMetadata& Metadata ) {
            metadata_ = Metadata; emit sigTrackChanged();
        } );
        connect( engine_.get(), &IAudioEngine::sigFailed, this, [ this ]( ErrorKind Kind, const QString& Detail ) {
            if( state_ != SessionState::Idle ) fail( Kind, Detail );
        } );
        return engine_.get();
    }

    void AudioSession::Start( std::vector<AudioTrack> Tracks, qsizetype Start )
    {
        if( Tracks.empty() ) return;
        queue_.Reset( std::move( Tracks ), Start );
        if( queue_.Shuffle() ) queue_.SetShuffle( true, QRandomGenerator::global()->generate() );
        emit sigModeChanged();
        openCurrent();
    }

    void AudioSession::openCurrent()
    {
        const auto* Track = queue_.Current();
        if( !Track ) { Stop(); return; }
        releaseCurrent();
        ++generation_;
        position_ = 0; duration_ = -1; metadata_ = {}; error_.clear();
        // 엔진 상태 신호보다 먼저 세션 상태를 둔다. Idle에서는 엔진 신호를 무시한다.
        state_ = SessionState::Loading;
        emit sigTrackChanged(); emit sigPositionChanged( 0 );
        if( !engine() ) { fail( ErrorKind::Unavailable, unavailable_ ); return; }
        if( !Track->localPath_.isEmpty() ) { playLocal( Track->localPath_ ); return; }
        if( !Track->source_ ) { fail( ErrorKind::Io, {} ); return; }
        if( Track->size_ > quint64( materializeLimitBytes_ ) ) { fail( ErrorKind::TooLarge, {} ); return; }
        auto Store = materializer();
        if( !Store ) { fail( ErrorKind::Io, error_ ); return; }
        setState( SessionState::Resolving );
        resolving_ = std::stop_source();
        const auto Generation = generation_;
        auto* Watcher = new QFutureWatcher<Resolved>( this );
        connect( Watcher, &QFutureWatcher<Resolved>::finished, this, [ this, Watcher, Generation ] {
            const auto Value = Watcher->result(); Watcher->deleteLater();
            // 늦게 끝난 실체화는 버린다. 임대가 여기서 풀리면 저장소가 임시 파일을 지운다.
            if( Generation != generation_ ) return;
            if( Value.cancelled_ ) return;
            if( !Value.lease_ ) { fail( ErrorKind::Io, Value.error_ ); return; }
            lease_ = Value.lease_;
            playLocal( QString::fromStdString( lease_->LocalPath() ) );
        } );
        Watcher->setFuture( QtConcurrent::run( [ Store, Source = Track->source_, Token = resolving_.get_token() ] {
            hc::storage::OperationContext Context; Context.stopToken_ = Token;
            auto Lease = Store->Materialize( { Source }, Context ).get();
            Resolved Result;
            if( Lease.HasValue() ) Result.lease_ = Lease.Value();
            else { Result.error_ = QString::fromStdString( Lease.Failure().code_ ); Result.cancelled_ = Token.stop_requested(); }
            return Result;
        } ) );
    }

    void AudioSession::playLocal( const QString& Path )
    {
        auto* Engine = engine();
        if( !Engine ) { fail( ErrorKind::Unavailable, unavailable_ ); return; }
        setState( SessionState::Loading );
        Engine->Open( Path );
        Engine->Play();
    }

    void AudioSession::finishTrack()
    {
        const auto Next = autoNext_ || queue_.Repeat() == RepeatMode::One ? queue_.NextAfterEnd() : std::nullopt;
        if( !Next ) { Stop(); return; }
        queue_.Select( *Next );
        openCurrent();
    }

    void AudioSession::fail( ErrorKind Kind, const QString& Detail )
    {
        releaseCurrent();
        if( Kind == ErrorKind::Cancelled ) { Stop(); return; }
        switch( Kind )
        {
            case ErrorKind::Unavailable: error_ = Detail.isEmpty() ? unavailable_ : Detail; break;
            case ErrorKind::Unsupported: error_ = tr( "재생할 수 없는 형식입니다" ); break;
            case ErrorKind::TooLarge: error_ = tr( "%1 MiB보다 큰 파일은 꺼내서 재생하지 않습니다" ).arg( materializeLimitBytes_ / 1024 / 1024 ); break;
            default: error_ = tr( "파일을 읽을 수 없습니다" ); break;
        }
        qCWarning( sessionLog ).noquote() << "playback failed:" << error_ << Detail;
        setState( SessionState::Failed );
    }

    void AudioSession::releaseCurrent()
    {
        resolving_.request_stop();
        // 엔진이 파일을 먼저 놓아야 임시 파일 삭제가 성공한다.
        if( engine_ ) engine_->Stop();
        lease_.reset();
    }

    void AudioSession::setState( SessionState State )
    {
        const auto Changed = state_ != State;
        state_ = State;
        const auto Key = State == SessionState::Idle ? QString() : Current() ? Current()->key_ : QString();
        if( Changed ) emit sigStateChanged();
        if( Key != nowPlayingKey_ || Changed ) { nowPlayingKey_ = Key; emit sigNowPlayingChanged( nowPlayingKey_, NowPlayingState() ); }
    }

    int AudioSession::NowPlayingState() const
    {
        switch( state_ )
        {
            case SessionState::Idle: return 0;
            case SessionState::Paused: case SessionState::Failed: return 2;
            default: return 1;
        }
    }

    void AudioSession::PlayPause()
    {
        if( state_ == SessionState::Playing ) { if( engine_ ) engine_->Pause(); }
        else if( state_ == SessionState::Paused ) { if( engine_ ) engine_->Play(); }
        else if( state_ == SessionState::Failed ) openCurrent();
    }

    void AudioSession::Stop()
    {
        ++generation_;
        releaseCurrent();
        queue_.Clear(); metadata_ = {}; error_.clear(); position_ = 0; duration_ = -1;
        setState( SessionState::Idle );
        emit sigTrackChanged();
    }

    void AudioSession::Next()
    {
        if( const auto Index = queue_.Next() ) { queue_.Select( *Index ); openCurrent(); }
    }

    void AudioSession::Previous()
    {
        const auto Index = queue_.Previous( position_ );
        if( !Index ) return;
        if( *Index == queue_.CurrentIndex() && ( state_ == SessionState::Playing || state_ == SessionState::Paused ) ) { Seek( 0 ); return; }
        queue_.Select( *Index ); openCurrent();
    }

    void AudioSession::SelectTrack( qsizetype Index )
    {
        if( !queue_.At( Index ) ) return;
        queue_.Select( Index ); openCurrent();
    }

    void AudioSession::Seek( qint64 PositionMs )
    {
        if( !engine_ || ( state_ != SessionState::Playing && state_ != SessionState::Paused ) ) return;
        const auto Target = std::clamp<qint64>( PositionMs, 0, duration_ > 0 ? duration_ : PositionMs );
        engine_->Seek( Target ); position_ = Target; emit sigPositionChanged( position_ );
    }

    void AudioSession::SeekBy( qint64 DeltaMs ) { Seek( position_ + DeltaMs ); }

    void AudioSession::SetVolume( int Percent )
    {
        volume_ = std::clamp( Percent, 0, 100 );
        if( engine_ ) engine_->SetVolume( volume_ );
        emit sigModeChanged(); scheduleSave();
    }

    void AudioSession::SetMuted( bool Muted )
    {
        muted_ = Muted;
        if( engine_ ) engine_->SetMuted( muted_ );
        emit sigModeChanged(); scheduleSave();
    }

    void AudioSession::SetRepeat( RepeatMode Mode ) { queue_.SetRepeat( Mode ); emit sigModeChanged(); scheduleSave(); }

    void AudioSession::SetShuffle( bool Enabled )
    {
        queue_.SetShuffle( Enabled, QRandomGenerator::global()->generate() );
        emit sigModeChanged(); scheduleSave();
    }

    void AudioSession::SetAutoNext( bool Enabled ) { autoNext_ = Enabled; emit sigModeChanged(); scheduleSave(); }

    bool AudioSession::ReleaseIfTarget( const QStringList& Paths )
    {
        const auto* Track = Current();
        if( !Track || !IsActive() ) return false;
        for( const auto& Candidate : { Track->path_, Track->localPath_ } )
        {
            if( Candidate.isEmpty() ) continue;
            const auto Playing = NativeKey( Candidate );
            for( const auto& Path : Paths )
            {
                const auto Target = NativeKey( Path );
                if( Target.isEmpty() || ( Playing != Target && !Playing.startsWith( Target + '\\' ) ) ) continue;
                qCInfo( sessionLog ) << "stopping playback before a file operation on the playing file";
                Stop();
                return true;
            }
        }
        return false;
    }

    std::shared_ptr<hc::storage::IMaterializer> AudioSession::materializer()
    {
        if( materializer_ ) return materializer_;
        const auto Base = TemporaryBase();
        if( !QDir().mkpath( Base ) ) { error_ = tr( "임시 폴더를 만들 수 없습니다: %1" ).arg( Base ); return {}; }
        // 크기 상한은 설정이 바뀌어도 같은 저장소를 쓰도록 세션이 직접 검사한다.
        hc::storage::MaterializationLimits Limits; Limits.maxActiveFiles_ = 4;
        auto Store = hc::storage::MakeNativeMaterializer( QDir::toNativeSeparators( Base ).toStdString(), Limits );
        if( !Store.HasValue() ) { error_ = QString::fromStdString( Store.Failure().code_ ); qCWarning( sessionLog ).noquote() << "materializer:" << error_; return {}; }
        materializer_ = Store.Value();
        return materializer_;
    }

    void AudioSession::RecoverTemporaryFiles()
    {
        // 없으면 만들지 않는다. 남은 것이 있을 때만 저장소를 열어 잠금이 풀린 임대를 지운다.
        if( !QDir( TemporaryBase() ).exists( "hc-materialized-v1" ) ) return;
        auto Store = materializer();
        if( !Store ) return;
        (void)QtConcurrent::run( [ Store ] { LogCleanup( "recover", Store->Recover() ); } );
    }

    void AudioSession::Shutdown()
    {
        ++generation_;
        releaseCurrent();
        if( saveTimer_.isActive() ) { saveTimer_.stop(); save(); }
        if( !materializer_ ) return;
        auto Store = std::move( materializer_ );
        QtConcurrent::run( [ Store ] { LogCleanup( "shutdown", Store->DrainCleanup() ); } ).waitForFinished();
    }

    void AudioSession::scheduleSave() { saveTimer_.start(); }

    void AudioSession::save()
    {
        auto& Settings = hc::settings::AppSettings();
        // 환경 설정 대화상자가 편집 중이면 그 초안을 건드리지 않는다. 다음 변경 때 다시 저장한다.
        if( Settings.Editing() ) { saveTimer_.start(); return; }
        auto General = Settings.Read( hc::settings::Section::General ).toObject();
        auto Audio = General.value( "audioPreview" ).toObject();
        Audio[ "volume" ] = volume_; Audio[ "muted" ] = muted_; Audio[ "autoNext" ] = autoNext_;
        Audio[ "repeat" ] = RepeatModeName( queue_.Repeat() ); Audio[ "shuffle" ] = queue_.Shuffle();
        General[ "audioPreview" ] = Audio;
        if( !Settings.Put( hc::settings::Section::General, General ) ) qCWarning( sessionLog ) << "saving playback settings failed";
    }
}
