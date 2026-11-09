#include "stdafx.h"
#include "inputGate.hpp"

/// 모듈
/// UIs
/// 기타 헤더파일
/// 고유 라이브러리
/// 공통 라이브러리
/// 외부 라이브러리
/// 선언 헤더파일들
/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <Windows.h>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEvent>
#include <QKeyEvent>
#include <QThread>

DECLARE_CMNLIBSV2_NAMESPACE

namespace hc::ui
{
    namespace
    {
        // Installed once on the application; it only stamps the time of continuous input. A single
        // Enter or click that opens a folder is not continuous: deferring after it would only delay
        // that folder's colors. Held keys (auto-repeat) and wheel scrolling are.
        class InputClock final : public QObject
        {
        public:
            static InputClock& Instance()
            {
                static auto* Clock = new InputClock;
                return *Clock;
            }
            qint64 Since() const { return last_.isValid() ? last_.elapsed() : -1; }
        private:
            InputClock() { if (auto* App = QCoreApplication::instance()) App->installEventFilter(this); }
            bool eventFilter(QObject*, QEvent* Event) override
            {
                if ((Event->type() == QEvent::KeyPress && static_cast<QKeyEvent*>(Event)->isAutoRepeat()) || Event->type() == QEvent::Wheel)
                    last_.start();
                return false;
            }
            QElapsedTimer last_;
        };
    }

    qint64 InputGate::SinceInput()
    {
        Q_ASSERT(QThread::isMainThread());
        return InputClock::Instance().Since();
    }

    bool InputGate::Busy(int QuietMs)
    {
        Q_ASSERT(QThread::isMainThread());
        // Input already queued for this thread is read before the next timer message.
        if (HIWORD(GetQueueStatus(QS_KEY | QS_MOUSEBUTTON)) != 0) return true;
        const auto Since = InputClock::Instance().Since();
        return Since >= 0 && Since < QuietMs;
    }
}
