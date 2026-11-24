#include "stdafx.h"
#include "qtDirectoryTracker.hpp"
#include "nativeFilePath.hpp"
#include "nativeStorageRevision.hpp"
#include <QFutureWatcher>
#include <QScopeGuard>
#include <QThreadPool>
#include <QWinEventNotifier>
#include <QtConcurrent/QtConcurrentRun>
#include <Windows.h>
#include <array>
#include <cstddef>
#include <thread>
#include <utility>

namespace {
QThreadPool* watchIoPool()
{
    // A disconnected redirector can block on open, rearm or cancellation.
    // Keep those requests off the GUI and away from listing and metadata workers.
    static auto* Pool = [] {
        auto* Value = new QThreadPool;
        Value->setMaxThreadCount(8);
        return Value;
    }();
    return Pool;
}

std::string directoryIdentity(HANDLE Folder, const std::wstring& Path)
{
    BY_HANDLE_FILE_INFORMATION Info{};
    if (!GetFileInformationByHandle(Folder, &Info) || !(Info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        || (Info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) return {};
    auto Revision = hc::storage::native::HandleRevision(Folder, Info, Path);
    return Revision.token_.starts_with("directory:identity:") ? std::move(Revision.token_) : std::string{};
}
}

struct QtDirectoryTracker::NativeWatch {
    HANDLE handle_ = INVALID_HANDLE_VALUE;
    HANDLE event_ = nullptr;
    OVERLAPPED request_{};
    alignas(DWORD) std::array<std::byte, 64 * 1024> buffer_;
    std::unique_ptr<QWinEventNotifier> notifier_;
    std::string identity_;
    DWORD filter_ = 0;
    bool reading_ = false;

    DWORD Arm() {
        ResetEvent(event_);
        request_ = {};
        request_.hEvent = event_;
        if (ReadDirectoryChangesW(handle_, buffer_.data(), DWORD(buffer_.size()), FALSE,
            filter_, nullptr, &request_, nullptr)) {
            reading_ = true;
            return ERROR_SUCCESS;
        }
        const auto Error = GetLastError();
        reading_ = Error == ERROR_IO_PENDING;
        return reading_ ? ERROR_SUCCESS : Error;
    }

    static std::pair<std::shared_ptr<NativeWatch>, DWORD> Open(const QString& Name, DWORD Filter, bool Remote) {
        auto Watch = std::shared_ptr<NativeWatch>(new NativeWatch, [Remote](NativeWatch* Value) {
            if (Remote) watchIoPool()->start([Value] { delete Value; });
            else delete Value;
        });
        const auto Path = hc::storage::native::FilePath(Name).toStdWString();
        Watch->handle_ = CreateFileW(Path.c_str(), FILE_LIST_DIRECTORY,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
        if (Watch->handle_ == INVALID_HANDLE_VALUE) return {{}, GetLastError()};
        Watch->event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!Watch->event_) return {{}, GetLastError()};
        Watch->filter_ = Filter;
        const auto Error = Watch->Arm();
        if (Error) return {{}, Error};
        if (!Remote) Watch->identity_ = directoryIdentity(Watch->handle_, Path);
        return {std::move(Watch), ERROR_SUCCESS};
    }

    ~NativeWatch() {
        if (reading_) {
            CancelIoEx(handle_, &request_);
            DWORD Bytes = 0;
            GetOverlappedResult(handle_, &request_, &Bytes, TRUE);
        }
        if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
        if (event_) CloseHandle(event_);
    }
};

void QtDirectoryTracker::RemoteCoverage::WaitForWatch(std::stop_token Stop)
{
    using namespace std::chrono_literals;
    const auto Deadline = std::chrono::steady_clock::now() + 220ms;
    while (!finished_ && !Stop.stop_requested() && std::chrono::steady_clock::now() < Deadline)
        std::this_thread::sleep_for(5ms);
    if (!Stop.stop_requested() && !armed_) needsBaseline_ = true;
}

QtDirectoryTracker::QtDirectoryTracker(QObject* Parent) : QObject(Parent) {
    debounce_.setSingleShot(true); debounce_.setInterval(180); rescan_.setInterval(15000);
    retry_.setSingleShot(true);
    connect(&retry_, &QTimer::timeout, this, &QtDirectoryTracker::restartWatch);
    connect(&rescan_, &QTimer::timeout, this, [this] {
        if (remote_) {
            if (!busy_) emit RefreshRequired();
        } else {
            restartWatch();
            Invalidate();
        }
    });
    connect(&debounce_, &QTimer::timeout, this, [this] {
        if (key_.isEmpty() || excluded_ || suspended_ || busy_) return;
        auto Names = std::exchange(pendingNames_, {}).values();
        if (std::exchange(pending_, false)) emit RefreshRequired();
        else if (!Names.isEmpty()) emit NamesChanged(Names);
    });
}
QtDirectoryTracker::~QtDirectoryTracker() { closeWatch(); }

void QtDirectoryTracker::SetLocation(const QString& Key, const QString& NativePath, const QString& LocationPath, bool Remote)
{
    const auto Path = LocationPath.isEmpty() ? NativePath : LocationPath;
    if (key_ == Key && nativePath_ == NativePath && locationPath_ == Path && remote_ == Remote) return;
    debounce_.stop(); rescan_.stop(); retry_.stop(); pending_ = false; pendingNames_.clear();
    key_ = Key; nativePath_ = NativePath; locationPath_ = Path; remote_ = Remote;
    remoteCoverage_ = Remote && !NativePath.isEmpty() ? std::make_shared<RemoteCoverage>() : nullptr;
    retryDelay_ = 2000;
    restartWatch();
}

void QtDirectoryTracker::Configure(const hc::refresh::Settings& Settings)
{
    if (settings_ == Settings) return;
    const bool CoverageChanged = settings_.names != Settings.names || settings_.metadata != Settings.metadata
        || excluded_ != Settings.Excludes(locationPath_);
    settings_ = Settings;
    rescan_.setInterval(settings_.fallbackSeconds * 1000);
    if (CoverageChanged) {
        if (remoteCoverage_) remoteCoverage_->needsBaseline_ = true;
        restartWatch();
        if (NotificationsAvailable()) Invalidate();
    } else updateTimers();
}

bool QtDirectoryTracker::NotificationsAvailable() const
{
    // The OS buffers changes on a live handle between notification reads.
    return bool(native_);
}

std::string QtDirectoryTracker::CoveredIdentity() const
{
    return CoversNames() && CoversMetadata() && !pending_ ? native_->identity_ : std::string{};
}

bool QtDirectoryTracker::HasDirectoryIdentity(const QString& Name, const std::string& Identity)
{
    const auto Path = hc::storage::native::FilePath(Name).toStdWString();
    // Recheck list access as well as identity. The parked handle may still have
    // its original permissions or refer to an earlier object at this path.
    const auto Folder = CreateFileW(Path.c_str(), FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (Folder == INVALID_HANDLE_VALUE) return false;
    const auto Close = qScopeGuard([Folder] { CloseHandle(Folder); });
    return !Identity.empty() && directoryIdentity(Folder, Path) == Identity;
}

void QtDirectoryTracker::CompleteListing()
{
    if (native_ && remoteCoverage_ && remoteCoverage_->needsBaseline_.exchange(false)) Invalidate();
}

void QtDirectoryTracker::closeWatch()
{
    ++watchGeneration_;
    watchStop_.request_stop();
    opening_ = false;
    if (native_) {
        if (auto* Notifier = native_->notifier_.release()) {
            Notifier->setEnabled(false);
            Notifier->deleteLater();
        }
        native_.reset();
    }
}

void QtDirectoryTracker::restartWatch()
{
    retry_.stop();
    closeWatch();
    excluded_ = settings_.Excludes(locationPath_);
    DWORD Filter = 0;
    if (settings_.names) Filter |= FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME;
    if (settings_.metadata) Filter |= FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE
        | FILE_NOTIFY_CHANGE_CREATION | FILE_NOTIFY_CHANGE_ATTRIBUTES | FILE_NOTIFY_CHANGE_SECURITY;
    if (key_.isEmpty() || excluded_ || nativePath_.isEmpty() || !Filter) {
        if (remoteCoverage_) remoteCoverage_->finished_ = true;
        updateTimers();
        return;
    }
    if (!remote_) {
        auto [Watch, Error] = NativeWatch::Open(nativePath_, Filter, false);
        if (Watch) attachWatch(std::move(Watch));
        else updateTimers();
        return;
    }

    opening_ = true;
    watchStop_ = {};
    const auto Epoch = watchGeneration_;
    const auto Coverage = remoteCoverage_;
    Coverage->armed_ = false;
    Coverage->finished_ = false;
    using OpenResult = std::pair<std::shared_ptr<NativeWatch>, DWORD>;
    auto* Opening = new QFutureWatcher<OpenResult>(this);
    connect(Opening, &QFutureWatcher<OpenResult>::finished, this, [this, Opening, Epoch] {
        auto [Watch, Error] = Opening->future().takeResult();
        Opening->deleteLater();
        if (Epoch != watchGeneration_) return;
        opening_ = false;
        if (Watch) attachWatch(std::move(Watch));
        else watchFailed(Error);
    });
    Opening->setFuture(QtConcurrent::run(watchIoPool(), [Path = nativePath_, Filter, Coverage, Stop = watchStop_.get_token()]() -> OpenResult {
        if (Stop.stop_requested()) return {{}, ERROR_OPERATION_ABORTED};
        auto Result = NativeWatch::Open(Path, Filter, true);
        if (Stop.stop_requested()) return {{}, ERROR_OPERATION_ABORTED};
        Coverage->armed_ = bool(Result.first);
        Coverage->finished_ = true;
        return Result;
    }));
    updateTimers();
}

void QtDirectoryTracker::attachWatch(std::shared_ptr<NativeWatch> Watch)
{
    retryDelay_ = 2000;
    Watch->notifier_ = std::make_unique<QWinEventNotifier>(Watch->event_, this);
    connect(Watch->notifier_.get(), &QWinEventNotifier::activated, this, [this, Registered = Watch.get()] {
        if (native_.get() == Registered) readCompleted();
    });
    native_ = std::move(Watch);
    CompleteListing();
    updateTimers();
}

void QtDirectoryTracker::watchFailed(unsigned long Error)
{
    const bool WasWatching = NotificationsAvailable();
    closeWatch();
    if (remoteCoverage_) {
        remoteCoverage_->armed_ = false;
        remoteCoverage_->finished_ = true;
        if (WasWatching) {
            pendingNames_.clear();
            remoteCoverage_->needsBaseline_ = true;
        }
        if (Error != ERROR_INVALID_FUNCTION && Error != ERROR_NOT_SUPPORTED && Error != ERROR_ACCESS_DENIED
            && Error != ERROR_INVALID_PARAMETER) {
            retry_.start(retryDelay_);
            retryDelay_ = std::min(retryDelay_ * 2, 60000);
        } else if (WasWatching) Invalidate();
    } else if (WasWatching) {
        const auto Epoch = watchGeneration_;
        QTimer::singleShot(0, this, [this, Epoch] { if (Epoch == watchGeneration_) restartWatch(); });
        Invalidate();
    }
    updateTimers();
}

void QtDirectoryTracker::readCompleted()
{
    auto& Watch = *native_;
    DWORD Bytes = 0;
    const auto Success = GetOverlappedResult(Watch.handle_, &Watch.request_, &Bytes, FALSE);
    const auto Error = Success ? ERROR_SUCCESS : GetLastError();
    if (Error == ERROR_IO_INCOMPLETE) return;
    Watch.reading_ = false;
    if (Error && Error != ERROR_NOTIFY_ENUM_DIR) { watchFailed(Error); return; }
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
    if (!remote_) {
        if (const auto Rearm = Watch.Arm(); Rearm) { watchFailed(Rearm); return; }
    } else {
        opening_ = true;
        Watch.notifier_->setEnabled(false);
        const auto Epoch = watchGeneration_;
        auto* Arming = new QFutureWatcher<DWORD>(this);
        connect(Arming, &QFutureWatcher<DWORD>::finished, this, [this, Arming, Epoch] {
            const auto Error = Arming->result();
            Arming->deleteLater();
            if (Epoch != watchGeneration_ || !native_) return;
            opening_ = false;
            if (Error) watchFailed(Error);
            else {
                native_->notifier_->setEnabled(true);
                updateTimers();
            }
        });
        Arming->setFuture(QtConcurrent::run(watchIoPool(), [Watch = native_, Stop = watchStop_.get_token()] {
            return Stop.stop_requested() ? DWORD(ERROR_OPERATION_ABORTED) : Watch->Arm();
        }));
    }
    if (Complete) VerifyNames(Names);
    else Invalidate();
}

void QtDirectoryTracker::updateTimers() {
    const bool Enabled = !key_.isEmpty() && !excluded_ && !suspended_;
    if (Enabled && settings_.fallbackEnabled && !NotificationsAvailable() && !opening_) {
        if (!rescan_.isActive()) rescan_.start();
    } else rescan_.stop();
    // Remote providers can report an empty or partial notification just before
    // the disconnect error. Deliver the pending refresh once coverage returns.
    if (Enabled && !busy_ && !opening_ && !retry_.isActive() && (pending_ || !pendingNames_.isEmpty())) {
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
