#pragma once

/// 공통 라이브러리
#include "commonLibs/system/registryAccess.hpp"
/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QObject>
#include <QHash>
#include <QTimer>
#include <mutex>

namespace hc::registry {
// One service is shared by the key tree and item tree. UI model accessors only
// inspect this cache; enumeration never runs in data/rowCount/hasChildren.
class RegistryService final : public QObject
{
    Q_OBJECT
public:
    struct Entry {
        RegistryLocation location;
        RegistryResult<KeySnapshot> snapshot;
        quint64 generation = 0;
        bool loaded = false, pending = false;
        std::shared_ptr<std::atomic_bool> cancel;
    };
    explicit RegistryService(std::shared_ptr<IRegistryBackend> Backend = {}, QObject* Parent = nullptr);
    ~RegistryService() override;
    const Entry* Cached(const RegistryLocation& Location) const;
    void Request(const RegistryLocation& Location, bool Refresh = false);
    void Invalidate(const RegistryLocation& Location, bool Descendants = false);
    void Cancel();
    // Only visible locations are watched; each gets a shallow parent watch so
    // deletion/recreation can invalidate the cache without polling registry data.
    void SetWatched(const QList<RegistryLocation>& Locations);
signals:
    void Changed(const QString& Id);
private:
    struct Worker { std::shared_ptr<IRegistryBackend> backend; std::mutex mutex; };
    struct Watches;
    struct WatchControl;
    void drainNotifications();
    std::shared_ptr<Worker> worker_;
    std::shared_ptr<Watches> watches_;
    std::shared_ptr<WatchControl> watchControl_;
    QList<RegistryLocation> watchedLocations_;
    QTimer watchTimer_;
    quint64 watchGeneration_ = 0;
    QHash<QString, Entry> cache_;
    quint64 generation_ = 0;
};
} // namespace hc::registry
