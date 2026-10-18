#pragma once
#include "directoryRefreshSettings.hpp"
#include <QObject>
#include <QTimer>
#include <memory>

// Native notifications invalidate snapshots. Polling is an opt-in fallback
// for locations where a native notification handle cannot be registered.
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
    bool PollingActive() const { return rescan_.isActive(); }
    void Invalidate();
    void SetIntervals(int DebounceMs, int RescanMs);
signals:
    void RefreshRequired();
private:
    void restartWatch();
    void updateTimers();
    struct NativeWatch;
    std::unique_ptr<NativeWatch> native_;
    hc::refresh::Settings settings_;
    QTimer debounce_, rescan_;
    QString key_, nativePath_, locationPath_;
    bool suspended_ = false, busy_ = false, pending_ = false, excluded_ = false;
};
