#pragma once
#include "columnContracts.hpp"
#include "metadataColumns.hpp"
#include <QCache>
#include <QElapsedTimer>
#include <QFutureWatcher>
#include <QTimer>
#include <QThread>
#include <QThreadPool>
#include <QtConcurrent/QtConcurrentRun>
#include <functional>
#include <memory>
#include <mutex>
#include <stop_token>

namespace hc::columns
{
    using FieldValues = QMap<QString, CellValue>;
    inline QString QueryFamily(const QString& Field)
    {
        if (Field.startsWith("lua.") && Field.count('.') == 2) return Field.section('.', 0, 1);
        if (IsPluginField(Field)) return Field;
        if (Field.startsWith("file.image")) return "image";
        if (QStringList{"file.fileVersion", "file.productVersion", "file.productName", "file.company", "file.description"}.contains(Field)) return "version";
        // One OS read per family fills every field in it.
        if (Field == "file.allocatedSize" || Field == "file.hardlink.count") return "standard";
        if (Field == "file.linkTarget" || Field == "file.softlink.target" || Field == "file.junction.target") return "link";
        if (Field == "file.alternateDataStream.zone") return "zone";
        if (Field.startsWith("file.alternateDataStream.")) return "streams";
        return {};
    }
    // Settings → file panel → network field conditions (design 6.6): "off", "builtIn" (no plugin fields) or "all".
    inline bool NetworkFieldAllowed(const QString& Mode, bool Network, const QString& Field)
    {
        return Mode == "all" || !Network || (Mode == "builtIn" && !IsPluginField(Field));
    }
    // Folders carry reparse data and named streams, but no payload, version resource or image header.
    // The Mark of the Web (Zone.Identifier) is written on downloaded files, not folders: a folder
    // reads as having no zone without opening a stream, so a folder-only listing queries nothing.
    inline bool QueriesFolders(const QString& Field)
    {
        const auto Family = QueryFamily(Field);
        return Family == "link" || Family == "streams" || IsPluginField(Field);
    }
    // GUI-owned cache and request set. Each engine runs its own worker loops on two process-lifetime
    // pools; a loop takes the next job from the shared queue itself and leaves the result in an inbox.
    // The GUI drains the inbox on one coarse timer: streamed results cost one low-priority timer
    // message per frame instead of several posted events per file, so queued user input is read
    // first. Destroying a tab never joins an OS metadata call or posts to a dead model.
    class QueryEngine final : public QObject
    {
    public:
        using Loader = std::function<FieldValues(const QString&, const QString&, std::stop_token)>;
        explicit QueryEngine(Loader Load, QObject* Parent = nullptr, int CacheLifetimeMs = 30000)
            : QObject(Parent), shared_(std::make_shared<Shared>()), cache_(8192), cacheLifetimeMs_(CacheLifetimeMs)
        {
            shared_->loader_ = std::move(Load);
            // Scale each model's queue to the available logical CPUs; unknown/single CPU keeps two slots.
            shared_->limit_ = qMax(2, QThread::idealThreadCount());
            drain_.setTimerType(Qt::CoarseTimer); drain_.setInterval(16);
            connect(&drain_, &QTimer::timeout, this, [this] { drain(); });
        }
        ~QueryEngine() override { std::lock_guard Lock(shared_->lock_); shared_->stop_.request_stop(); shared_->clearQueues(); }
        std::function<void(const QSet<QString>&)> Changed;
        // While this returns true, finished values wait (at most MaxDeferMs) so pending input is handled first.
        std::function<bool()> DeferWhile;
        static constexpr int MaxDeferMs = 500;
        // Query families of the paths passed to the Changed call in progress.
        const QSet<QString>& ChangedFamilies() const { return reportedFamilies_; }
        int Pending() const { return int(pending_.size()); }
        quint64 Started() const { return started_; }
        bool Paused() const { return paused_; }
        // A sort snapshot must retain early results until the last slow query finishes.
        void BeginSnapshot()
        {
            if (snapshot_) return;
            for (const auto& Key : cache_.keys()) if (cache_.object(Key)->age_.elapsed() >= cacheLifetimeMs_) cache_.remove(Key);
            snapshot_ = true;
        }
        void EndSnapshot() { snapshot_ = false; }
        void SetLoader(Loader Load, bool Clear = true)
        {
            if (Clear) Reset(); else { Cancel(); Resume(); }
            std::lock_guard Lock(shared_->lock_); shared_->loader_ = std::move(Load);
        }
        void Reset() { Cancel(); cache_.clear(); Resume(); }
        void Cancel()
        {
            {
                std::lock_guard Lock(shared_->lock_);
                shared_->stop_.request_stop(); ++shared_->epoch_; shared_->clearQueues(); shared_->done_.clear();
            }
            paused_ = true; snapshot_ = false; pending_.clear(); changedPaths_.clear(); changedFamilies_.clear();
            statusDirty_ = false; deferred_.invalidate(); drain_.stop();
        }
        void Resume() { if (paused_) { std::lock_guard Lock(shared_->lock_); shared_->stop_ = std::stop_source{}; paused_ = false; } }
        CellValue Read(const QString& Path, const QString& Field, bool Request = true, bool Urgent = true, bool SlowStorage = false)
        {
            const auto Family = QueryFamily(Field); if (Family.isEmpty()) return CellValue::Missing(ValueState::Unsupported);
            const auto Key = Path + QChar(0) + Family;
            if (const auto* Entry = cache_.object(Key)) {
                if (snapshot_ || Entry->age_.elapsed() < cacheLifetimeMs_) return Entry->values_.value(Field, Entry->values_.value("__error", CellValue::Missing(ValueState::Unsupported)));
                cache_.remove(Key);
            }
            if (paused_) return CellValue::Missing(ValueState::OnDemand);
            if (Request && !pending_.contains(Key)) {
                if (pending_.size() >= 4096) return CellValue::Missing(ValueState::OnDemand, "query-budget");
                pending_.insert(Key); ++started_; statusDirty_ = true;
                const bool Network = ((Path.startsWith("\\\\") || Path.startsWith("//")) && !Path.startsWith("\\\\?\\") && !Path.startsWith("//?/"))
                    || Path.startsWith("\\\\?\\UNC\\", Qt::CaseInsensitive) || Path.startsWith("//?/UNC/", Qt::CaseInsensitive);
                const bool Slow = SlowStorage || Network ||
                    IsPluginField(Field) || Family.startsWith("lua.") || Family == "image" || Family == "version";
                {
                    std::lock_guard Lock(shared_->lock_);
                    auto& Queue = shared_->queues_[Slow ? 1 : 0];
                    const Job Next{Path, Family, Key, shared_->epoch_};
                    if (Urgent) Queue.prepend(Next); else Queue.append(Next);
                }
                schedule(shared_);
                if (!drain_.isActive()) drain_.start();
            }
            if (Request && Urgent) {
                std::lock_guard Lock(shared_->lock_);
                for (auto& Queue : shared_->queues_)
                    for (int Index = 1; Index < Queue.size(); ++Index) if (Queue[Index].key_ == Key) { Queue.prepend(Queue.takeAt(Index)); break; }
            }
            return CellValue::Missing(ValueState::Pending);
        }
    private:
        struct Cached { FieldValues values_; QElapsedTimer age_; };
        struct Job { QString path_, family_, key_; quint64 epoch_ = 0; };
        struct Done { Job job_; FieldValues values_; };
        // Shared with the worker loops, which outlive the engine when an OS call is still running.
        struct Shared
        {
            std::mutex lock_;
            QList<Job> queues_[2];            // metadata, slow providers
            QList<Done> done_;
            Loader loader_;
            std::stop_source stop_;
            quint64 epoch_ = 0;
            int loops_[2]{0, 0}, starting_[2]{0, 0}, limit_ = 2;
            void clearQueues() { queues_[0].clear(); queues_[1].clear(); }
        };
        static QThreadPool* pool(bool Slow)
        {
            // Process-lifetime pools isolate metadata from slow providers and the colour evaluator.
            // No tab destructor waits for an uninterruptible OS call; workers capture no model.
            static auto* Metadata = new QThreadPool;
            static auto* Providers = new QThreadPool;
            static const bool Initialized = [] {
                Metadata->setMaxThreadCount(qMax(2, QThread::idealThreadCount()));
                Providers->setMaxThreadCount(qMax(2, QThread::idealThreadCount()));
                return true;
            }();
            (void)Initialized;
            return Slow ? Providers : Metadata;
        }
        // Starts a loop for each queued job not yet claimed, within the model budget. Slow providers
        // always leave one slot for metadata, including metadata that arrives later.
        static void schedule(const std::shared_ptr<Shared>& State)
        {
            std::lock_guard Lock(State->lock_);
            const auto Start = [&](int Lane) {
                ++State->loops_[Lane]; ++State->starting_[Lane];
                pool(Lane == 1)->start([State, Lane] { run(State, Lane); });
            };
            while (State->queues_[0].size() > State->starting_[0] && State->loops_[0] + State->loops_[1] < State->limit_) Start(0);
            while (State->queues_[1].size() > State->starting_[1] && State->loops_[1] < State->limit_ - 1
                && State->loops_[0] + State->loops_[1] < State->limit_) Start(1);
        }
        static void run(const std::shared_ptr<Shared>& State, int Lane)
        {
            for (bool First = true;; First = false) {
                Job Next; Loader Load; std::stop_token Stop;
                {
                    std::lock_guard Lock(State->lock_);
                    if (First) --State->starting_[Lane];
                    if (State->queues_[Lane].isEmpty()) { --State->loops_[Lane]; break; }
                    Next = State->queues_[Lane].takeFirst(); Load = State->loader_; Stop = State->stop_.get_token();
                }
                FieldValues Values;
                if (!Stop.stop_requested()) {
                    try { Values = Load(Next.path_, Next.family_, Stop); }
                    catch (...) { Values = FieldValues{{"__error", CellValue::Missing(ValueState::Error, "provider-exception")}}; }
                }
                std::lock_guard Lock(State->lock_);
                if (Next.epoch_ == State->epoch_) State->done_.append(Done{std::move(Next), std::move(Values)});
            }
            // A finished metadata loop frees the slot a waiting slow job may need.
            schedule(State);
        }
        void drain()
        {
            QList<Done> Finished; quint64 Epoch = 0; int Loops = 0;
            {
                std::lock_guard Lock(shared_->lock_);
                Finished.swap(shared_->done_); Epoch = shared_->epoch_; Loops = shared_->loops_[0] + shared_->loops_[1];
            }
            for (auto& Item : Finished) {
                if (Item.job_.epoch_ != Epoch) continue;
                auto* Entry = new Cached{std::move(Item.values_), {}}; Entry->age_.start();
                cache_.insert(Item.job_.key_, Entry); pending_.remove(Item.job_.key_);
                changedPaths_.insert(Item.job_.path_); changedFamilies_.insert(Item.job_.family_);
            }
            if (!changedPaths_.isEmpty() && DeferWhile && DeferWhile()) {
                if (!deferred_.isValid()) deferred_.start();
                if (deferred_.elapsed() < MaxDeferMs) return;
            }
            if (!changedPaths_.isEmpty() || statusDirty_) {
                deferred_.invalidate(); statusDirty_ = false;
                const auto Paths = std::exchange(changedPaths_, {});
                reportedFamilies_ = std::exchange(changedFamilies_, {});
                if (Changed) Changed(Paths);
                reportedFamilies_.clear();
            }
            if (Loops == 0 && pending_.isEmpty() && changedPaths_.isEmpty() && !statusDirty_) drain_.stop();
        }
        std::shared_ptr<Shared> shared_;
        QCache<QString, Cached> cache_;
        QSet<QString> pending_, changedPaths_, changedFamilies_, reportedFamilies_;
        QTimer drain_;
        QElapsedTimer deferred_;
        quint64 started_ = 0;
        int cacheLifetimeMs_ = 30000;
        bool paused_ = false, snapshot_ = false, statusDirty_ = false;
    };
}
