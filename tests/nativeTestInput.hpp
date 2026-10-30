#pragma once
#include <Windows.h>
#include <QtTest>

// Test-only input-queue attachment grants foreground ownership to the fixture.
// Do not send a key unless this returns true; production focus code stays separate.
inline bool ActivateTestWindow(HWND Window)
{
    RECT Bounds{};
    if (!IsWindow(Window) || !GetWindowRect(Window, &Bounds)) return false;
    SetForegroundWindow(Window);
    if (GetForegroundWindow() == Window) return true;
    const DWORD ForegroundThread = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
    const DWORD CurrentThread = GetCurrentThreadId();
    const bool Attached = ForegroundThread && ForegroundThread != CurrentThread
        && AttachThreadInput(CurrentThread, ForegroundThread, TRUE);
    BringWindowToTop(Window);
    SetForegroundWindow(Window);
    SetActiveWindow(Window);
    if (Attached) AttachThreadInput(CurrentThread, ForegroundThread, FALSE);
    return QTest::qWaitFor([&] { return GetForegroundWindow() == Window; }, 1500);
}

// A document helper owns one visible top-level window; its WLX bridge stays hidden.
inline HWND VisibleTestProcessWindow(DWORD Process)
{
    struct Search { DWORD process; HWND window = nullptr; unsigned count = 0; } Result{Process};
    EnumWindows([](HWND Window, LPARAM Data) -> BOOL {
        auto& Found = *reinterpret_cast<Search*>(Data); DWORD Owner = 0;
        GetWindowThreadProcessId(Window, &Owner);
        if (Owner == Found.process && IsWindowVisible(Window)) { Found.window = Window; ++Found.count; }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&Result));
    return Result.count == 1 ? Result.window : nullptr;
}
