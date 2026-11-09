#pragma once

/// 모듈
/// UIs
/// 기타 헤더파일
/// 고유 라이브러리
/// 공통 라이브러리
/// 외부 라이브러리
/// 선언 헤더파일들
/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QtGlobal>

namespace hc::ui
{
    // Background result application asks this before doing GUI-thread work. It is busy while
    // keyboard or mouse button input waits in the GUI thread's queue, or for QuietMs after the
    // last continuous input (a held key's auto-repeat, wheel scrolling). A single key press or
    // click does not defer anything. Callers bound how long they defer. GUI thread only.
    struct InputGate
    {
        static constexpr int DefaultQuietMs = 120;
        static bool Busy(int QuietMs = DefaultQuietMs);
        // Milliseconds since the last auto-repeated key or wheel event, or -1 before any.
        static qint64 SinceInput();
    };
}
