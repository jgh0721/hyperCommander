#pragma once

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QFrame>
#include <QPointer>

class QCheckBox;
class QLabel;
class QListWidget;
class QPushButton;
class QSlider;
class QToolButton;
namespace hc::audio { class AudioSession; }

namespace hc::ui
{
    class AudioSeekBar;

    // 제목표시줄 곡명을 누르면 여는 재생 정보: 표지·형식, 큰 진행막대, 반복·무작위,
    // 볼륨, 같은 폴더 대기열. Esc·바깥 클릭으로 닫는다.
    class AudioPopover final : public QFrame
    {
        Q_OBJECT
    public:
        AudioPopover( hc::audio::AudioSession* Session, QWidget* Parent );
        void                ShowBelow( QWidget* Anchor );
    signals:
        void                sigLocateRequested();
    private:
        void                refreshTrack();
        void                refreshState();
        void                refreshPosition();
        void                refreshMode();
        void                refreshQueue();

        QPointer<hc::audio::AudioSession> session_;
        QLabel              *cover_ = nullptr, *title_ = nullptr, *folder_ = nullptr, *format_ = nullptr;
        QLabel              *position_ = nullptr, *remaining_ = nullptr, *queueTitle_ = nullptr, *volumeText_ = nullptr;
        AudioSeekBar*       seek_ = nullptr;
        QToolButton         *repeat_ = nullptr, *prev_ = nullptr, *play_ = nullptr, *next_ = nullptr, *shuffle_ = nullptr;
        QSlider*            volume_ = nullptr;
        QListWidget*        queue_ = nullptr;
        QCheckBox*          autoNext_ = nullptr;
        QPushButton*        locate_ = nullptr;
    };
}
