#pragma once

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QImage>
#include <QObject>
#include <QString>
#include <memory>

namespace hc::audio
{
    enum class EngineState { Idle, Loading, Playing, Paused, Ended, Failed };
    enum class ErrorKind { None, Unavailable, Unsupported, Io, Device, TooLarge, Cancelled };

    struct AudioMetadata
    {
        QString     title_, artist_, album_, codec_;
        int         bitRate_ = 0;
        QImage      cover_;
    };

    // 세션·위젯·명령은 이 경계만 안다. 2단계 엔진(FFmpeg 직접 디코드 + QAudioSink)은
    // 같은 인터페이스로 교체한다(계획 문서 14절).
    class IAudioEngine : public QObject
    {
        Q_OBJECT
    public:
        explicit IAudioEngine( QObject* Parent = nullptr ) : QObject( Parent ) {}
        ~IAudioEngine() override = default;
        // 처음 부를 때 재생 기반을 올린다. N 에디션처럼 쓸 수 없으면 false.
        virtual bool                IsAvailable() = 0;
        virtual QString             UnavailableReason() const = 0;
        virtual void                Open( const QString& LocalPath ) = 0;
        virtual void                Play() = 0;
        virtual void                Pause() = 0;
        // 파일 핸들까지 놓는다. 그 뒤에야 삭제·이동·임시 파일 정리가 된다.
        virtual void                Stop() = 0;
        virtual void                Seek( qint64 PositionMs ) = 0;
        virtual void                SetVolume( int Percent ) = 0;
        virtual void                SetMuted( bool Muted ) = 0;

    signals:
        void                        sigStateChanged( hc::audio::EngineState State );
        void                        sigPositionChanged( qint64 PositionMs );
        void                        sigDurationChanged( qint64 DurationMs );
        void                        sigMetadataChanged( const hc::audio::AudioMetadata& Metadata );
        void                        sigFailed( hc::audio::ErrorKind Kind, const QString& Detail );
    };

    // Libs/ffmpeg의 DLL을 전체 경로로 먼저 올린다. 플러그인은 이미 올라온 같은 이름의
    // 모듈로 import를 해결하므로 루트에 FFmpeg DLL을 두지 않아도 된다(계획 D19).
    bool                            PreloadFfmpeg( QString* Error = nullptr );
    std::unique_ptr<IAudioEngine>   MakeQtAudioEngine();
}
