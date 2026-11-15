#include "stdafx.h"
#include "registryService.hpp"

/// 모듈
/// UIs
/// 기타 헤더파일
/// 고유 라이브러리
/// 공통 라이브러리
#include "commonLibs/system/registryWatch.hpp"
/// 외부 라이브러리
/// 선언 헤더파일들
/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QtConcurrentRun>
#include <QFutureWatcher>
#include <QThread>
#include <QSet>
#include <vector>

DECLARE_CMNLIBSV2_NAMESPACE
Q_LOGGING_CATEGORY(lcRegistryService, "hc.registry.service")

namespace hc::registry {
struct RegistryService::Watches
{
    struct Notification { std::atomic_bool dirty{false}; std::atomic_uint32_t error{0}; };
    struct Item { RegistryLocation location; std::shared_ptr<Notification> notification; std::unique_ptr<RegistryWatcher> watcher; };
    std::vector<Item> items;
};
struct RegistryService::WatchControl
{
    std::mutex mutex;
    std::atomic<quint64> generation{0};
    std::shared_ptr<Watches> active;
};
RegistryService::RegistryService(std::shared_ptr<IRegistryBackend> Backend, QObject* Parent) : QObject(Parent), worker_(std::make_shared<Worker>())
{
    watchControl_ = std::make_shared<WatchControl>();
    worker_->backend = Backend ? std::move(Backend) : std::make_shared<WinRegistryBackend>();
    watchTimer_.setInterval(200);
    connect(&watchTimer_, &QTimer::timeout, this, &RegistryService::drainNotifications);
}
RegistryService::~RegistryService() { Cancel(); SetWatched({}); }
const RegistryService::Entry* RegistryService::Cached(const RegistryLocation& Location) const
{
    const auto It = cache_.constFind(Location.Id()); return It == cache_.cend() ? nullptr : &It.value();
}
void RegistryService::Request(const RegistryLocation& Location, bool Refresh)
{
    Q_ASSERT(QThread::currentThread() == thread());
    auto& Entry = cache_[Location.Id()];
    if (!Refresh && (Entry.pending || Entry.loaded)) return;
    if (Entry.cancel) Entry.cancel->store(true);
    Entry.location = Location; Entry.generation = ++generation_; Entry.pending = true;
    Entry.cancel = std::make_shared<std::atomic_bool>(false);
    const auto Generation = Entry.generation; const auto Cancelled = Entry.cancel;
    auto* Watcher = new QFutureWatcher<RegistryResult<KeySnapshot>>(this);
    connect(Watcher, &QFutureWatcherBase::finished, this, [this, Watcher, Location, Generation, Cancelled] {
        const auto Result = Watcher->result(); Watcher->deleteLater();
        auto It = cache_.find(Location.Id());
        if (Cancelled->load() || It == cache_.end() || It->generation != Generation) return;
        It->snapshot = Result; It->loaded = true; It->pending = false;
        emit Changed(Location.Id());
    });
    Watcher->setFuture(QtConcurrent::run([Worker = worker_, Location, Cancelled] {
        std::lock_guard Lock(Worker->mutex);
        if (Cancelled->load()) return RegistryResult<KeySnapshot>{{}, ERROR_CANCELLED};
        return Worker->backend->Enumerate(Location, Cancelled.get());
    }));
    emit Changed(Location.Id());
}
void RegistryService::Invalidate(const RegistryLocation& Location, bool Descendants)
{
    const auto Id = Location.Id(); QList<RegistryLocation> Reload;
    for (auto It = cache_.begin(); It != cache_.end(); ++It) {
        const bool Child = It->location.hive == Location.hive && It->location.sid == Location.sid && It->location.view == Location.view
            && (Location.key.isEmpty() || It->location.key.startsWith(Location.key + '\\', Qt::CaseInsensitive));
        if (It.key() == Id || (Descendants && Child)) Reload.append(It->location);
    }
    for (const auto& Key : Reload) Request(Key, true);
}
void RegistryService::Cancel()
{
    for (auto& Entry : cache_) {
        if (Entry.cancel) Entry.cancel->store(true);
        Entry.pending = false; Entry.generation = ++generation_;
    }
}
void RegistryService::SetWatched(const QList<RegistryLocation>& Locations)
{
    const auto Requested = Locations.mid(0, 16);
    if (Requested == watchedLocations_ && watchGeneration_) return;
    watchedLocations_ = Requested;
    const auto Generation = ++watchGeneration_;
    watchControl_->generation.store(Generation);
    watches_.reset();
    watchTimer_.stop();
    auto* Future = new QFutureWatcher<std::shared_ptr<Watches>>(this);
    connect(Future, &QFutureWatcherBase::finished, this, [this, Future, Generation] {
        const auto Result = Future->result(); Future->deleteLater();
        if (Generation != watchGeneration_) return;
        watches_ = Result; if (Result) watchTimer_.start();
    });
    Future->setFuture(QtConcurrent::run([Locations = watchedLocations_, Control = watchControl_, Generation] {
        // Serialize replacement and stop the prior set before creating another.
        // Rapid navigation cannot accumulate generations of native watch threads.
        std::lock_guard Lock(Control->mutex);
        if (Control->generation.load() != Generation) return std::shared_ptr<Watches>();
        if (Control->active) for (const auto& Item : Control->active->items) Item.watcher->Stop();
        Control->active.reset();
        if (Locations.isEmpty()) return std::shared_ptr<Watches>();
        // Stopping/joining native watchers is also off the GUI thread, including
        // when a view closes while this future is still preparing its handles.
        auto Result = std::shared_ptr<Watches>(new Watches, [](Watches* Value) {
            (void)QtConcurrent::run([Value] { delete Value; });
        });
        QSet<QString> Seen;
        for (const auto& Location : Locations) {
            if (Control->generation.load() != Generation) break;
            QList<RegistryLocation> Keys{Location}; if (Location.Parent()) Keys.append(*Location.Parent());
            for (const auto& Key : Keys) {
                if (Seen.contains(Key.Id())) continue; Seen.insert(Key.Id());
                auto Notification = std::make_shared<Watches::Notification>();
                auto Watcher = std::make_unique<RegistryWatcher>();
                Watcher->Start(Key, false, [Notification](quint32 Error) { Notification->error.store(Error); Notification->dirty.store(true); });
                Result->items.push_back({Key, std::move(Notification), std::move(Watcher)});
            }
        }
        Control->active = Result;
        return Result;
    }));
}
void RegistryService::drainNotifications()
{
    if (!watches_) return;
    bool ParentChanged = false, Failed = false;
    for (const auto& Item : watches_->items) {
        if (!Item.notification->dirty.exchange(false)) continue;
        Invalidate(Item.location, true);
        Failed |= Item.notification->error.load() != 0;
        for (const auto& Key : watchedLocations_) if (Key.Parent() && Key.Parent()->Id() == Item.location.Id()) ParentChanged = true;
    }
    // A missing child watch is dormant, not a retry loop. Its parent's next
    // notification reopens it after recreation.
    if (ParentChanged && !Failed) {
        const auto Desired = watchedLocations_; watchedLocations_.clear(); SetWatched(Desired);
    }
}
} // namespace hc::registry
