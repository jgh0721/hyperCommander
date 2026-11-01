#pragma once

/// 고유 라이브러리
#include "audioEngine.hpp"
#include "audioQueue.hpp"

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QObject>
#include <QTimer>
#include <functional>
#include <memory>
#include <stop_token>

namespace hc::storage { class IMaterializer; class IMaterializedFileLease; }

namespace hc::audio
{
    enum class SessionState { Idle, Resolving, Loading, Playing, Paused, Failed };

    // 제목표시줄 미리 듣기의 재생 상태. UI를 모르며 위젯·팝오버·명령·패널 표시는
    // 신호만 구독한다. 실체화한 임시 파일의 수명도 여기서 책임진다(계획 D20).
    class AudioSession final : public QObject
    {
        Q_OBJECT
    public:
        using EngineFactory = std::function<std::unique_ptr<IAudioEngine>()>;
        explicit AudioSession( QObject* Parent, EngineFactory Factory = {} );
        ~AudioSession() override;

        // 재생 엔진을 쓸 수 있는지. 처음 부를 때 FFmpeg와 백엔드를 올린다.
        bool                        IsAvailable();
        QString                     UnavailableText() const { return unavailable_; }

        void                        Start( std::vector<AudioTrack> Tracks, qsizetype Start );
        void                        PlayPause();
        void                        Stop();
        void                        Next();
        void                        Previous();
        void                        SelectTrack( qsizetype Index );
        void                        Seek( qint64 PositionMs );
        void                        SeekBy( qint64 DeltaMs );
        void                        SetVolume( int Percent );
        void                        SetMuted( bool Muted );
        void                        SetRepeat( RepeatMode Mode );
        void                        SetShuffle( bool Enabled );
        void                        SetAutoNext( bool Enabled );
        // 삭제·이동·이름 변경 대상에 재생 중 파일(또는 그 상위 폴더)이 있으면 정지한다(계획 D05).
        bool                        ReleaseIfTarget( const QStringList& Paths );
        // 강제 종료로 남은 임시 파일을 지운다. 세션 생성 직후 백그라운드에서 한 번 부른다.
        void                        RecoverTemporaryFiles();
        // 앱 종료. 엔진을 멈추고 임시 파일을 모두 지운다.
        void                        Shutdown();

        SessionState                State() const { return state_; }
        bool                        IsActive() const { return state_ != SessionState::Idle; }
        const AudioQueue&           Queue() const { return queue_; }
        const AudioTrack*           Current() const { return queue_.Current(); }
        const AudioMetadata&        Metadata() const { return metadata_; }
        qint64                      Position() const { return position_; }
        qint64                      Duration() const { return duration_; }
        int                         Volume() const { return volume_; }
        bool                        Muted() const { return muted_; }
        bool                        AutoNext() const { return autoNext_; }
        // 파일 패널의 재생 중 막대를 움직일지(환경 설정). 끄면 정지된 막대로 그린다.
        bool                        AnimateNowPlaying() const { return animateNowPlaying_; }
        QString                     ErrorText() const { return error_; }
        QStringList                 Extensions() const { return extensions_; }
        bool                        OpenInternally() const;
        // 패널 행 표시: 0 없음, 1 재생, 2 일시정지.
        int                         NowPlayingState() const;
        void                        ReloadSettings();

    signals:
        void                        sigStateChanged();
        void                        sigTrackChanged();
        void                        sigPositionChanged( qint64 PositionMs );
        void                        sigModeChanged();
        void                        sigNowPlayingChanged( const QString& Key, int State );

    private:
        IAudioEngine*               engine();
        void                        openCurrent();
        void                        playLocal( const QString& Path );
        void                        finishTrack();
        void                        fail( ErrorKind Kind, const QString& Detail );
        void                        setState( SessionState State );
        void                        releaseCurrent();
        void                        scheduleSave();
        void                        save();
        std::shared_ptr<hc::storage::IMaterializer> materializer();

        EngineFactory               factory_;
        std::unique_ptr<IAudioEngine> engine_;
        bool                        engineChecked_ = false;
        QString                     unavailable_;
        AudioQueue                  queue_;
        AudioMetadata               metadata_;
        SessionState                state_ = SessionState::Idle;
        qint64                      position_ = 0, duration_ = -1;
        int                         volume_ = 70;
        bool                        muted_ = false, autoNext_ = true, animateNowPlaying_ = true;
        QStringList                 extensions_;
        qint64                      materializeLimitBytes_ = 0;
        QString                     error_;
        QString                     nowPlayingKey_;
        // 실체화 결과가 늦게 와도 이미 바뀐 곡에 쓰지 않도록 세대를 센다.
        quint64                     generation_ = 0;
        std::stop_source            resolving_;
        std::shared_ptr<hc::storage::IMaterializer> materializer_;
        std::shared_ptr<hc::storage::IMaterializedFileLease> lease_;
        QTimer                      saveTimer_;
    };
}
