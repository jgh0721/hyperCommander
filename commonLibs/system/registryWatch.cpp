#include "stdafx.h"
#include "registryWatch.hpp"

/// 모듈
/// UIs
/// 기타 헤더파일
/// 고유 라이브러리
/// 공통 라이브러리
/// 외부 라이브러리
/// 선언 헤더파일들
/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QLoggingCategory>

DECLARE_CMNLIBSV2_NAMESPACE
Q_LOGGING_CATEGORY(lcRegistryWatch, "hc.registry.watch")

namespace hc::registry {
RegistryWatcher::RegistryWatcher() : stop_(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}
RegistryWatcher::~RegistryWatcher() { Stop(); if (stop_) CloseHandle(stop_); }
bool RegistryWatcher::Start(const RegistryLocation& Location, bool Subtree, Callback Notify)
{
    Stop();
    if (!stop_ || !Notify) return false;
    ResetEvent(stop_);
    worker_ = std::thread([this, Location, Subtree, Notify = std::move(Notify)] {
        auto Key = WinRegistryBackend::Open(Location, KEY_NOTIFY);
        if (!Key) { Notify(Key.error); return; }
        const HANDLE Changed = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!Changed) { Notify(GetLastError()); return; }
        struct EventCleanup { HANDLE value; ~EventCleanup() { CloseHandle(value); } } Cleanup{Changed};
        constexpr DWORD Filter = REG_NOTIFY_CHANGE_NAME | REG_NOTIFY_CHANGE_LAST_SET | REG_NOTIFY_CHANGE_ATTRIBUTES | REG_NOTIFY_CHANGE_SECURITY;
        DWORD Error = RegNotifyChangeKeyValue(Key.value.Get(), Subtree, Filter, Changed, TRUE);
        if (Error) { Notify(Error); return; }
        const HANDLE Events[]{stop_, Changed};
        for (;;) {
            const DWORD Wait = WaitForMultipleObjects(2, Events, FALSE, INFINITE);
            if (Wait == WAIT_OBJECT_0) break;
            if (Wait != WAIT_OBJECT_0 + 1) { Notify(GetLastError()); break; }
            // Re-arm before notifying. The service coalesces updates and reopens a
            // parent watch on deletion; Win32 does not tell us the changed path.
            Error = RegNotifyChangeKeyValue(Key.value.Get(), Subtree, Filter, Changed, TRUE);
            Notify(Error);
            if (Error) break;
        }
    });
    return true;
}
void RegistryWatcher::Stop()
{
    if (stop_) SetEvent(stop_);
    if (worker_.joinable()) worker_.join();
}
} // namespace hc::registry
