#pragma once
#include <QFileSystemWatcher>
#include <QObject>
#include <QTimer>

// Watch notifications invalidate a snapshot; they are never treated as a full
// change journal. Periodic rescan also covers lost events and remote providers.
class QtDirectoryTracker final : public QObject {
    Q_OBJECT
public:
    explicit QtDirectoryTracker(QObject* Parent = nullptr);
    void SetLocation(const QString& Key, const QString& NativePath = {});
    void Invalidate();
    void SetIntervals(int DebounceMs, int RescanMs);
signals:
    void RefreshRequired();
private:
    QFileSystemWatcher watcher_;
    QTimer debounce_, rescan_;
    QString key_, nativePath_;
};
