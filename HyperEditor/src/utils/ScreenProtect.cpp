#include "utils/ScreenProtect.h"

#ifdef Q_OS_WIN
#include <Windows.h>
#endif

namespace ScreenProtect {

#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

void setProtectionMode(QWidget* window, ProtectionMode mode)
{
    if (!window) return;
#ifdef Q_OS_WIN
    HWND hwnd = reinterpret_cast<HWND>(window->window()->winId());
    if (hwnd) {
        DWORD affinity = WDA_NONE;
        switch (mode) {
            case ProtectionMode::Monitor:
                affinity = WDA_MONITOR;
                break;
            case ProtectionMode::ExcludeFromCapture:
                affinity = WDA_EXCLUDEFROMCAPTURE;
                break;
            case ProtectionMode::Off:
            default:
                affinity = WDA_NONE;
                break;
        }
        ::SetWindowDisplayAffinity(hwnd, affinity);
    }
#else
    Q_UNUSED(mode)
#endif
}

void setProtection(QWidget* window, bool enabled)
{
    setProtectionMode(window, enabled ? ProtectionMode::ExcludeFromCapture : ProtectionMode::Off);
}

ProtectionMode protectionMode(QWidget* window)
{
    if (!window) return ProtectionMode::Off;
#ifdef Q_OS_WIN
    HWND hwnd = reinterpret_cast<HWND>(window->window()->winId());
    DWORD affinity = 0;
    if (hwnd && ::GetWindowDisplayAffinity(hwnd, &affinity)) {
        if (affinity == WDA_EXCLUDEFROMCAPTURE)
            return ProtectionMode::ExcludeFromCapture;
        if (affinity == WDA_MONITOR)
            return ProtectionMode::Monitor;
    }
#endif
    return ProtectionMode::Off;
}

bool isProtected(QWidget* window)
{
    return protectionMode(window) != ProtectionMode::Off;
}

} // namespace ScreenProtect

