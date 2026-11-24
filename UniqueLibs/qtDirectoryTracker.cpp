#include "stdafx.h"
#include "qtDirectoryTracker.hpp"
#include "nativeFilePath.hpp"
#include <QWinEventNotifier>
#include <Windows.h>
#include <array>
#include <cstddef>
#include <utility>

struct QtDirectoryTracker::NativeWatch {
    HANDLE handle_ = INVALID_HANDLE_VALUE;
    HANDLE event_ = nullptr;
    OVERLAPPED request_{};
    alignas(DWORD) std::array<std::byte, 64 * 1024> buffer_;
    std::unique_ptr<QWinEventNotifier> notifier_;
    DWORD filter_ = 0;
    bool reading_ = false;

    bool Arm() {
        ResetEvent(event_);
        request_ = {};
        request_.hEvent = event_;
        reading_ = ReadDirectoryChangesW(handle_, buffer_.data(), DWORD(buffer_.size()), FALSE,
            filter_, nullptr, &request_, nullptr) != FALSE;
        return reading_;
    }

    ~NativeWatch() {
        notifier_.reset();
        if (reading_) {
            CancelIoEx(handle_, &request_);
            DWORD Bytes = 0;
            GetOverlappedResult(handle_, &request_, &Bytes, TRUE);
        }
        if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
        if (event_) CloseHandle(event_);
    }
};

QtDirectoryTracker::QtDirectoryTracker(QObject* Parent) : QObject(Parent) {
    debounce_.setSingleShot(true); debounce_.setInterval(180); rescan_.setInterval(15000);
    connect(&rescan_, &QTimer::timeout, this, [this] {
        restartWatch();
        Invalidate();
    });
    connect(&debounce_, &QTimer::timeout, this, [this] {
        if (key_.isEmpty() || excluded_ || suspended_ || busy_) return;
        auto Names = std::exchange(pendingNames_, {}).values();
        if (std::exchange(pending_, false)) emit RefreshRequired();
        else if (!Names.isEmpty()) emit NamesChanged(Names);
    });
}
QtDirectoryTracker::~QtDirectoryTracker() = default;

void QtDirectoryTracker::SetLocation(const QString& Key, const QString& NativePath, const QString& LocationPath) {
    const auto Path = LocationPath.isEmpty() ? NativePath : LocationPath;
    if (key_ == Key && nativePath_ == NativePath && locationPath_ == Path) return;
    debounce_.stop(); rescan_.stop(); pending_ = false; pendingNames_.clear();
    key_ = Key; nativePath_ = NativePath; locationPath_ = Path;
    restartWatch();
}
void QtDirectoryTracker::Configure(const hc::refresh::Settings& Settings) {
    if (settings_ == Settings) return;
    const bool CoverageChanged = settings_.names != Settings.names || settings_.metadata != Settings.metadata
        || excluded_ != Settings.Excludes(locationPath_);
    settings_ = Settings;
    rescan_.setInterval(settings_.fallbackSeconds * 1000);
    if (CoverageChanged) {
        restartWatch();
        if (NotificationsAvailable()) Invalidate();
    } else updateTimers();
}
bool QtDirectoryTracker::NotificationsAvailable() const { return native_ && native_->reading_; }

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
            const auto Path = hc::storage::native::FilePath(nativePath_).toStdWString();
            Watch->handle_ = CreateFileW(Path.c_str(), FILE_LIST_DIRECTORY,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
            Watch->event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            Watch->filter_ = Filter;
            if (Watch->handle_ != INVALID_HANDLE_VALUE && Watch->event_ && Watch->Arm()) {
                Watch->notifier_ = std::make_unique<QWinEventNotifier>(Watch->event_);
                connect(Watch->notifier_.get(), &QWinEventNotifier::activated, this, [this] { readCompleted(); });
                native_ = std::move(Watch);
            }
        }
    }
    updateTimers();
}
void QtDirectoryTracker::readCompleted()
{
    auto& Watch = *native_;
    DWORD Bytes = 0;
    const auto Success = GetOverlappedResult(Watch.handle_, &Watch.request_, &Bytes, FALSE);
    if (!Success && GetLastError() == ERROR_IO_INCOMPLETE) return;
    Watch.reading_ = false;
    QStringList Names;
    bool Complete = Success && Bytes && Bytes <= Watch.buffer_.size();
    constexpr auto Header = offsetof(FILE_NOTIFY_INFORMATION, FileName);
    for (std::size_t Offset = 0; Complete;) {
        const auto Remaining = Bytes - Offset;
        if (Remaining < Header) { Complete = false; break; }
        const auto* Item = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(Watch.buffer_.data() + Offset);
        const auto Length = Item->FileNameLength;
        if (!Length || Length % sizeof(wchar_t) || Length > Remaining - Header
            || Item->Action < FILE_ACTION_ADDED || Item->Action > FILE_ACTION_RENAMED_NEW_NAME) { Complete = false; break; }
        auto Name = QString::fromWCharArray(Item->FileName, Length / sizeof(wchar_t));
        const auto Stream = Name.indexOf(':');
        if (Stream >= 0) Name.truncate(Stream);
        Names.append(std::move(Name));
        const auto Next = Item->NextEntryOffset;
        if (!Next) break;
        if (Next % alignof(DWORD) || Next < Header + Length || Next >= Remaining) { Complete = false; break; }
        Offset += Next;
    }
    if (!Watch.Arm()) {
        Watch.notifier_->setEnabled(false);
        const auto Key = key_;
        QTimer::singleShot(0, this, [this, Key] {
            if (key_ == Key && native_ && !native_->reading_) restartWatch();
        });
        Complete = false;
    }
    if (Complete) VerifyNames(Names);
    else Invalidate();
}
void QtDirectoryTracker::updateTimers() {
    const bool Enabled = !key_.isEmpty() && !excluded_ && !suspended_;
    if (Enabled && settings_.fallbackEnabled && !NotificationsAvailable()) {
        if (!rescan_.isActive()) rescan_.start();
    } else rescan_.stop();
    if (Enabled && !busy_ && (pending_ || !pendingNames_.isEmpty())) {
        if (!debounce_.isActive()) debounce_.start();
    } else debounce_.stop();
}
void QtDirectoryTracker::SetSuspended(bool Suspended) { suspended_ = Suspended; updateTimers(); }
void QtDirectoryTracker::SetBusy(bool Busy) { busy_ = Busy; updateTimers(); }
void QtDirectoryTracker::Invalidate() {
    if (key_.isEmpty() || excluded_) return;
    pendingNames_.clear(); pending_ = true; updateTimers();
}
void QtDirectoryTracker::VerifyNames(const QStringList& Names) {
    if (key_.isEmpty() || excluded_ || pending_) return;
    for (const auto& Name : Names) pendingNames_.insert(Name);
    if (pendingNames_.size() > 4096) Invalidate();
    else updateTimers();
}
void QtDirectoryTracker::SetIntervals(int DebounceMs, int RescanMs) {
    debounce_.setInterval(qMax(1, DebounceMs)); rescan_.setInterval(qMax(1, RescanMs));
}
