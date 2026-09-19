#include "qtDirectoryTracker.hpp"
#include <QDir>

QtDirectoryTracker::QtDirectoryTracker(QObject* Parent) : QObject(Parent) {
    debounce_.setSingleShot(true); debounce_.setInterval(180); rescan_.setInterval(15000);
    connect(&watcher_, &QFileSystemWatcher::directoryChanged, this, [this] { Invalidate(); });
    connect(&watcher_, &QFileSystemWatcher::fileChanged, this, [this] { Invalidate(); });
    connect(&rescan_, &QTimer::timeout, this, [this] { Invalidate(); });
    connect(&debounce_, &QTimer::timeout, this, [this] {
        if (key_.isEmpty()) return;
        // Replacement/removal can detach a watch. Retry on every rescan.
        if (!nativePath_.isEmpty() && watcher_.directories().isEmpty() && watcher_.files().isEmpty()) watcher_.addPath(nativePath_);
        emit RefreshRequired();
    });
}
void QtDirectoryTracker::SetLocation(const QString& Key, const QString& NativePath) {
    if (key_ == Key && nativePath_ == NativePath) return;
    debounce_.stop(); rescan_.stop();
    const auto Paths = watcher_.directories() + watcher_.files();
    if (!Paths.isEmpty()) watcher_.removePaths(Paths);
    key_ = Key; nativePath_ = NativePath;
    if (key_.isEmpty()) return;
    // Callers pass a native path only for local storage: watcher registration
    // must never reconnect an offline SMB share on the GUI thread.
    if (!nativePath_.isEmpty()) watcher_.addPath(nativePath_);
    rescan_.start();
}
void QtDirectoryTracker::Invalidate() {
    if (!key_.isEmpty() && !debounce_.isActive()) debounce_.start();
}
void QtDirectoryTracker::SetIntervals(int DebounceMs, int RescanMs) {
    debounce_.setInterval(qMax(1, DebounceMs)); rescan_.setInterval(qMax(1, RescanMs));
}
