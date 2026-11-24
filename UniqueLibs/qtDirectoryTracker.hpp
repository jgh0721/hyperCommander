#pragma once
#include "directoryRefreshSettings.hpp"
#include <QObject>
#include <QSet>
#include <QStringList>
#include <QTimer>
#include <memory>

// A working native subscription reports child names. Loss of coverage and
// locations without detailed notifications retain the invalidation path.
class QtDirectoryTracker final : public QObject {
    Q_OBJECT
public:
    explicit QtDirectoryTracker(QObject* Parent = nullptr);
    ~QtDirectoryTracker() override;
    void SetLocation(const QString& Key, const QString& NativePath = {}, const QString& LocationPath = {});
    void Configure(const hc::refresh::Settings& Settings);
    void SetSuspended(bool Suspended);
    void SetBusy(bool Busy);
    bool NotificationsAvailable() const;
    bool CoversNames() const { return NotificationsAvailable() && settings_.names; }
    bool CoversMetadata() const { return NotificationsAvailable() && settings_.metadata; }
    bool PollingActive() const { return rescan_.isActive(); }
    void Invalidate();
    void VerifyNames(const QStringList& Names);
    void SetIntervals(int DebounceMs, int RescanMs);
signals:
    void RefreshRequired();
    void NamesChanged(const QStringList& Names);
private:
    void restartWatch();
    void readCompleted();
    void updateTimers();
    struct NativeWatch;
    std::unique_ptr<NativeWatch> native_;
    hc::refresh::Settings settings_;
    QTimer debounce_, rescan_;
    QString key_, nativePath_, locationPath_;
    QSet<QString> pendingNames_;
    bool suspended_ = false, busy_ = false, pending_ = false, excluded_ = false;
};
