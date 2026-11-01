#pragma once

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QIcon>
#include <QPointer>
#include <QSlider>
#include <QWidget>

class QLabel;
class QToolButton;
namespace hc::audio { class AudioSession; }

namespace hc::ui
{
    enum class MediaGlyph { Previous, Next, Play, Pause, Volume, Mute, Close, Note, Repeat, RepeatOne, Shuffle };
    // 12 단위 상자에 그린 미디어 기호. DPR 1~2의 픽스맵을 함께 담는다.
    QIcon MediaIcon( MediaGlyph Glyph, const QColor& Color, int Px = 12 );
    // 원판 위에 재생·일시정지 기호를 놓은 단추 아이콘.
    QIcon MediaDiscIcon( bool Pause, const QColor& Disc, const QColor& Glyph, int Px );

    // 클릭한 위치로 바로 이동하고, 끄는 동안은 값만 바꾸다가 놓을 때 탐색을 요청한다.
    // 휠과 화살표는 5초씩 움직인다.
    class AudioSeekBar final : public QSlider
    {
        Q_OBJECT
    public:
        explicit AudioSeekBar( QWidget* Parent = nullptr );
        void                SetColors( const QColor& Track, const QColor& Fill, const QColor& Knob, bool KnobAlways );
        void                SetPosition( qint64 PositionMs, qint64 DurationMs );
    signals:
        void                sigSeekRequested( qint64 PositionMs );
    protected:
        void                paintEvent( QPaintEvent* Event ) override;
        void                mousePressEvent( QMouseEvent* Event ) override;
        void                mouseMoveEvent( QMouseEvent* Event ) override;
        void                mouseReleaseEvent( QMouseEvent* Event ) override;
        void                wheelEvent( QWheelEvent* Event ) override;
        void                enterEvent( QEnterEvent* Event ) override;
        void                leaveEvent( QEvent* Event ) override;
    private:
        int                 valueAt( int X ) const;
        QColor              track_, fill_, knob_;
        bool                knobAlways_ = false, hovered_ = false, dragging_ = false;
    };

    // 제목표시줄의 재생 위젯(A안). WindowChrome의 부속 위젯 자리에 들어가며, 재생 중이
    // 아닐 때는 숨는다. 클릭해도 패널 입력 포커스를 빼앗지 않는다.
    class AudioTitleWidget final : public QWidget
    {
        Q_OBJECT
    public:
        enum class Tier { Full, Middle, Compact };
        AudioTitleWidget( hc::audio::AudioSession* Session, QWidget* Chrome );
        Tier                CurrentTier() const { return tier_; }
        QToolButton*        TrackButton() const { return track_; }
    signals:
        void                sigPopoverRequested();
    protected:
        void                paintEvent( QPaintEvent* Event ) override;
        void                mousePressEvent( QMouseEvent* Event ) override;
        void                mouseDoubleClickEvent( QMouseEvent* Event ) override;
        bool                eventFilter( QObject* Object, QEvent* Event ) override;
    private:
        void                refreshTheme();
        void                refreshVolume();
        void                refreshState();
        void                refreshTrack();
        void                refreshPosition();
        void                updateTier();
        int                 tierWidth( Tier Value ) const;
        void                applyTier( Tier Value );
        QToolButton*        button( const char* Name, const QString& Text );
        QWidget*            separator();

        QPointer<hc::audio::AudioSession> session_;
        QToolButton         *prev_ = nullptr, *play_ = nullptr, *next_ = nullptr, *track_ = nullptr, *volume_ = nullptr, *close_ = nullptr, *external_ = nullptr;
        QLabel              *position_ = nullptr, *duration_ = nullptr, *remaining_ = nullptr, *error_ = nullptr;
        AudioSeekBar*       seek_ = nullptr;
        QWidget*            ring_ = nullptr;
        QList<QWidget*>     separators_;
        Tier                tier_ = Tier::Full;
        QColor              foreground_, glyph_, well_, wellLine_, trackColor_;
        int                 wellHeight_ = 24;
        QString             trackText_;
    };
}
