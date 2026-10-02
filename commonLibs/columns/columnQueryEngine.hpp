#pragma once
#include "columnContracts.hpp"
#include "metadataColumns.hpp"
#include <QCache>
#include <QElapsedTimer>
#include <QFutureWatcher>
#include <QTimer>
#include <QtConcurrent/QtConcurrentRun>
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
        if (Field == "file.allocatedSize") return "allocation";
        if (Field == "file.linkTarget") return "link";
        return {};
    }
    // GUI-owned queue/cache. Workers own immutable inputs and a stop token only.
    // Destroying a tab never joins an OS metadata call or posts to a dead model.
    class QueryEngine final : public QObject
    {
    public:
        using Loader = std::function<FieldValues(const QString&, const QString&, std::stop_token)>;
        explicit QueryEngine(Loader Load, QObject* Parent = nullptr, int CacheLifetimeMs = 30000) : QObject(Parent), loader_(std::move(Load)), cache_(8192), cacheLifetimeMs_(CacheLifetimeMs)
        {
            pump_.setSingleShot(true); changed_.setSingleShot(true); changed_.setInterval(16);
            connect(&pump_, &QTimer::timeout, this, [this] { pump(); });
            connect(&changed_, &QTimer::timeout, this, [this] { auto Paths = changedPaths_; changedPaths_.clear(); if (Changed) Changed(Paths); });
        }
        ~QueryEngine() override { stop_.request_stop(); }
        std::function<void(const QSet<QString>&)> Changed;
        int Pending() const { return int(pending_.size()); }
        bool Paused() const { return paused_; }
        // A sort snapshot must retain early results until the last slow query finishes.
        void BeginSnapshot()
        {
            if (snapshot_) return;
            for (const auto& Key : cache_.keys()) if (cache_.object(Key)->age_.elapsed() >= cacheLifetimeMs_) cache_.remove(Key);
            snapshot_ = true;
        }
        void EndSnapshot() { snapshot_ = false; }
        void SetLoader(Loader Load, bool Clear = true) { if (Clear) Reset(); else { Cancel(); Resume(); } loader_ = std::move(Load); }
        void Reset() { Cancel(); cache_.clear(); Resume(); }
        void Cancel()
        {
            stop_.request_stop(); ++epoch_; paused_ = true; snapshot_ = false; queue_.clear(); pending_.clear();
            pump_.stop(); changed_.stop(); changedPaths_.clear();
        }
        void Resume() { if (paused_) { stop_ = std::stop_source{}; paused_ = false; } }
        CellValue Read(const QString& Path, const QString& Field, bool Request = true, bool Urgent = true)
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
                pending_.insert(Key);
                Job Next{Path, Family, Key, epoch_};
                if (Urgent) queue_.prepend(Next); else queue_.append(Next);
                if (!pump_.isActive()) pump_.start(0);
                if (!changed_.isActive()) changed_.start();
            }
            if (Request && Urgent) {
                for (int Index = 1; Index < queue_.size(); ++Index) if (queue_[Index].key_ == Key) { queue_.prepend(queue_.takeAt(Index)); break; }
            }
            return CellValue::Missing(ValueState::Pending);
        }
    private:
        struct Cached { FieldValues values_; QElapsedTimer age_; };
        struct Job { QString path_, family_, key_; quint64 epoch_; };
        void pump()
        {
            while (!paused_ && running_ < 2 && !queue_.isEmpty()) {
                const auto Next = queue_.takeFirst(); ++running_;
                auto* Watcher = new QFutureWatcher<FieldValues>(this);
                connect(Watcher, &QFutureWatcher<FieldValues>::finished, this, [this, Watcher, Next] {
                    --running_;
                    if (Next.epoch_ == epoch_) {
                        auto* Entry = new Cached{Watcher->result(), {}}; Entry->age_.start();
                        cache_.insert(Next.key_, Entry); pending_.remove(Next.key_); changedPaths_.insert(Next.path_);
                        if (!changed_.isActive()) changed_.start();
                    }
                    Watcher->deleteLater(); pump();
                });
                const auto Load = loader_; const auto Stop = stop_.get_token();
                Watcher->setFuture(QtConcurrent::run([Next, Load, Stop] {
                    if (Stop.stop_requested()) return FieldValues{};
                    try { return Load(Next.path_, Next.family_, Stop); }
                    catch (...) { return FieldValues{{"__error", CellValue::Missing(ValueState::Error, "provider-exception")}}; }
                }));
            }
        }
        Loader loader_;
        QCache<QString, Cached> cache_;
        QList<Job> queue_;
        QSet<QString> pending_, changedPaths_;
        QTimer pump_, changed_;
        std::stop_source stop_;
        quint64 epoch_ = 0;
        int running_ = 0;
        int cacheLifetimeMs_ = 30000;
        bool paused_ = false, snapshot_ = false;
    };
}
