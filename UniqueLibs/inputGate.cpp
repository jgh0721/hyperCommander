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
#include <QAbstractNativeEventFilter>
#include <QElapsedTimer>
#include <QEvent>
#include <QKeyEvent>
#include <QThread>
#include <QPointer>
#include <QTimer>
#include <QWidget>
#include <utility>

DECLARE_CMNLIBSV2_NAMESPACE

namespace hc::ui
{
    namespace
    {
        // Protect navigation taps as well as repeats; Enter/click still lets a newly opened
        // folder publish its initial results immediately.
        class InputClock final : public QObject, public QAbstractNativeEventFilter
        {
        public:
            static InputClock& Instance()
            {
                static auto* Clock = new InputClock;
                return *Clock;
            }
            qint64 Since() const { return last_.isValid() ? last_.elapsed() : -1; }
        private:
            InputClock() {
                paintDeadline_.setSingleShot(true); paintDeadline_.setInterval(16);
                connect(&paintDeadline_, &QTimer::timeout, this, [this] { releasePaints(); });
                if (auto* App = QCoreApplication::instance()) {
                    App->installEventFilter(this);
                    App->installNativeEventFilter(this);
                }
            }
            bool nativeEventFilter(const QByteArray&, void* Message, qintptr*) override
            {
                // Qt can defer a translated key until after timers and paint events.
                // Once Win32 removed it from the queue, GetQueueStatus no longer
                // protects that gap. Start the quiet period at native receipt too.
                const auto* Native = static_cast<const MSG*>(Message);
                if (Native->message == WM_KEYDOWN || Native->message == WM_SYSKEYDOWN) {
                    const auto Key = Native->wParam;
                    if ((Native->lParam & (LPARAM(1) << 30)) || Key == VK_UP || Key == VK_DOWN
                        || Key == VK_LEFT || Key == VK_RIGHT || Key == VK_PRIOR || Key == VK_NEXT
                        || Key == VK_HOME || Key == VK_END || Key == VK_TAB) {
                        last_.start(); nativeWaiting_.start(); awaitingQt_ = true;
                    }
                }
                else if (Native->message == WM_MOUSEWHEEL || Native->message == WM_MOUSEHWHEEL) last_.start();
                return false;
            }
            bool eventFilter(QObject* Watched, QEvent* Event) override
            {
                bool Navigation = false;
                if (Event->type() == QEvent::KeyPress) {
                    const auto* Key = static_cast<QKeyEvent*>(Event);
                    Navigation = Key->isAutoRepeat() || Key->key() == Qt::Key_Up || Key->key() == Qt::Key_Down
                        || Key->key() == Qt::Key_Left || Key->key() == Qt::Key_Right || Key->key() == Qt::Key_PageUp
                        || Key->key() == Qt::Key_PageDown || Key->key() == Qt::Key_Home || Key->key() == Qt::Key_End
                        || Key->key() == Qt::Key_Tab || Key->key() == Qt::Key_Backtab;
                }
                if (Navigation || Event->type() == QEvent::Wheel) last_.start();
                if (Event->type() == QEvent::KeyPress) releasePaints();
                if (Event->type() == QEvent::UpdateRequest && awaitingQt_
                    && Watched->property("hc.navigationWindow").toBool()) {
                    if (nativeWaiting_.elapsed() >= 16) { releasePaints(); return false; }
                    // Win32 has already delivered navigation, but Qt can paint the old
                    // window before dispatching KeyPress. Defer backing-store sync before
                    // any background is erased; replay the update after the key. A 16 ms fallback covers
                    // shortcuts consumed without KeyPress and prevents paint starvation.
                    if (auto* Widget = qobject_cast<QWidget*>(Watched)) {
                        if (!deferredPaints_.contains(Widget)) deferredPaints_.append(Widget);
                        if (!paintDeadline_.isActive()) paintDeadline_.start();
                        return true;
                    }
                }
                return false;
            }
            void releasePaints()
            {
                awaitingQt_ = false; paintDeadline_.stop();
                const auto Pending = std::exchange(deferredPaints_, {});
                for (const auto& Widget : Pending) if (Widget)
                    QCoreApplication::postEvent(Widget, new QEvent(QEvent::UpdateRequest), Qt::LowEventPriority);
            }
            QElapsedTimer last_, nativeWaiting_;
            QTimer paintDeadline_;
            QList<QPointer<QWidget>> deferredPaints_;
            bool awaitingQt_ = false;
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
