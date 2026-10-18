#include "stdafx.h"
#include "qtDirectoryTracker.hpp"
#include <QWinEventNotifier>
#include <Windows.h>

struct QtDirectoryTracker::NativeWatch {
    HANDLE handle = INVALID_HANDLE_VALUE;
    std::unique_ptr<QWinEventNotifier> notifier;
    ~NativeWatch() {
        notifier.reset();
        if (handle != INVALID_HANDLE_VALUE) FindCloseChangeNotification(handle);
    }
};

QtDirectoryTracker::QtDirectoryTracker(QObject* Parent) : QObject(Parent) {
    debounce_.setSingleShot(true); debounce_.setInterval(180); rescan_.setInterval(15000);
    connect(&rescan_, &QTimer::timeout, this, [this] {
        // Retry a detached watcher only after the user has enabled the fallback.
        restartWatch();
        Invalidate();
    });
    connect(&debounce_, &QTimer::timeout, this, [this] {
        if (key_.isEmpty() || excluded_ || suspended_ || busy_ || !pending_) return;
        pending_ = false;
        emit RefreshRequired();
    });
}
QtDirectoryTracker::~QtDirectoryTracker() = default;

void QtDirectoryTracker::SetLocation(const QString& Key, const QString& NativePath, const QString& LocationPath) {
    const auto Path = LocationPath.isEmpty() ? NativePath : LocationPath;
    if (key_ == Key && nativePath_ == NativePath && locationPath_ == Path) return;
    debounce_.stop(); rescan_.stop(); pending_ = false;
    key_ = Key; nativePath_ = NativePath; locationPath_ = Path;
    restartWatch();
}
void QtDirectoryTracker::Configure(const hc::refresh::Settings& Settings) {
    if (settings_ == Settings) return;
    settings_ = Settings;
    rescan_.setInterval(settings_.fallbackSeconds * 1000);
    restartWatch();
}
bool QtDirectoryTracker::NotificationsAvailable() const { return bool(native_); }

void QtDirectoryTracker::restartWatch() {
    native_.reset();
    excluded_ = settings_.Excludes(locationPath_);
    if (!key_.isEmpty() && !excluded_ && !nativePath_.isEmpty()) {
        DWORD Filter = 0;
        if (settings_.names) Filter |= FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME;
        if (settings_.metadata) Filter |= FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE
            | FILE_NOTIFY_CHANGE_CREATION | FILE_NOTIFY_CHANGE_ATTRIBUTES | FILE_NOTIFY_CHANGE_SECURITY;
        if (Filter) {
            auto Watch = std::make_unique<NativeWatch>();
            Watch->handle = FindFirstChangeNotificationW(reinterpret_cast<LPCWSTR>(nativePath_.utf16()), FALSE, Filter);
            if (Watch->handle != INVALID_HANDLE_VALUE) {
                Watch->notifier = std::make_unique<QWinEventNotifier>(Watch->handle);
                connect(Watch->notifier.get(), &QWinEventNotifier::activated, this, [this] {
                    // Rearm before publishing so changes during enumeration remain pending.
                    native_->notifier->setEnabled(false);
                    const bool Armed = FindNextChangeNotification(native_->handle);
                    if (Armed) native_->notifier->setEnabled(true);
                    else {
                        // Keep the notifier alive until its activation signal has returned.
                        QTimer::singleShot(0, this, [this] { restartWatch(); });
                    }
                    Invalidate();
                });
                native_ = std::move(Watch);
            }
        }
    }
    updateTimers();
}
void QtDirectoryTracker::updateTimers() {
    const bool Enabled = !key_.isEmpty() && !excluded_ && !suspended_;
    if (Enabled && settings_.fallbackEnabled && !NotificationsAvailable()) {
        if (!rescan_.isActive()) rescan_.start();
    } else rescan_.stop();
    if (Enabled && !busy_ && pending_) {
        if (!debounce_.isActive()) debounce_.start();
    } else debounce_.stop();
}
void QtDirectoryTracker::SetSuspended(bool Suspended) { suspended_ = Suspended; updateTimers(); }
void QtDirectoryTracker::SetBusy(bool Busy) { busy_ = Busy; updateTimers(); }
void QtDirectoryTracker::Invalidate() {
    if (key_.isEmpty() || excluded_) return;
    pending_ = true; updateTimers();
}
void QtDirectoryTracker::SetIntervals(int DebounceMs, int RescanMs) {
    debounce_.setInterval(qMax(1, DebounceMs)); rescan_.setInterval(qMax(1, RescanMs));
}
