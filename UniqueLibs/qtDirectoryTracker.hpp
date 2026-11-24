#pragma once
#include "directoryRefreshSettings.hpp"
#include <QObject>
#include <QSet>
#include <QStringList>
#include <QTimer>
#include <atomic>
#include <cstdint>
#include <memory>
#include <stop_token>
#include <string>

// A working native subscription reports child names. Loss of coverage and
// locations without detailed notifications retain the invalidation path.
class QtDirectoryTracker final : public QObject {
    Q_OBJECT
public:
    explicit QtDirectoryTracker(QObject* Parent = nullptr);
    ~QtDirectoryTracker() override;
    struct RemoteCoverage {
        void WaitForWatch(std::stop_token Stop);
        std::atomic_bool armed_ = false;
        std::atomic_bool finished_ = false;
        std::atomic_bool needsBaseline_ = false;
    };

    void SetLocation(const QString& Key, const QString& NativePath = {}, const QString& LocationPath = {}, bool Remote = false);
    void Configure(const hc::refresh::Settings& Settings);
    void SetSuspended(bool Suspended);
    void SetBusy(bool Busy);
    bool NotificationsAvailable() const;
    bool CoversNames() const { return NotificationsAvailable() && settings_.names; }
    bool CoversMetadata() const { return NotificationsAvailable() && settings_.metadata; }
    const QString& LocationKey() const { return key_; }
    std::string CoveredIdentity() const;
    static bool HasDirectoryIdentity(const QString& Path, const std::string& Identity);
    std::shared_ptr<RemoteCoverage> Coverage() const { return remoteCoverage_; }
    void CompleteListing();
    bool PollingActive() const { return rescan_.isActive(); }
    void Invalidate();
    void VerifyNames(const QStringList& Names);
    void SetIntervals(int DebounceMs, int RescanMs);
signals:
    void RefreshRequired();
    void NamesChanged(const QStringList& Names);
private:
    void restartWatch();
    void closeWatch();
    void watchFailed(unsigned long Error);
    void readCompleted();
    void updateTimers();
    struct NativeWatch;
    void attachWatch(std::shared_ptr<NativeWatch> Watch);
    std::shared_ptr<NativeWatch> native_;
    std::shared_ptr<RemoteCoverage> remoteCoverage_;
    std::stop_source watchStop_;
    std::uint64_t watchGeneration_ = 0;
    hc::refresh::Settings settings_;
    QTimer debounce_, rescan_, retry_;
    QString key_, nativePath_, locationPath_;
    QSet<QString> pendingNames_;
    bool suspended_ = false, busy_ = false, pending_ = false, excluded_ = false;
    bool remote_ = false, opening_ = false;
    int retryDelay_ = 2000;
};
