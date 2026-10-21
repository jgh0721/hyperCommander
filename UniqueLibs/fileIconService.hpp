#pragma once

#include <QCache>
#include <QElapsedTimer>
#include <QFutureWatcher>
#include <QHash>
#include <QImage>
#include <QObject>
#include <QPixmap>
#include <QPointer>
#include <QSet>
#include <QThreadPool>
#include <QTimer>
#include <QFileSystemWatcher>
#include <QAbstractNativeEventFilter>
#include <functional>

class QWindow;

namespace hc::icons {
enum class Kind { File, Folder, Associated, Item, Overlay };
enum class State { Ready, NoEmbeddedIcon, NoOverlay, NoCustomIcon, TransientFailure };
struct Request {
    Kind kind_ = Kind::File;
    QString path_, extension_, revision_;
    int pixels_ = 32;
    bool directory_ = false, virtual_ = false, remote_ = false, knownCustomFolder_ = false;
    QString Family() const;
    QString Key() const;
    bool Remote() const;
};
struct Result {
    QImage image_;
    State state_ = State::Ready;
    QStringList dependencies_;
    bool customFolder_ = false;
    QByteArray digest_;
};
struct Value {
    QPixmap pixmap_;
    State state_ = State::Ready;
};
struct Statistics {
    quint64 loads_ = 0, hits_ = 0, merged_ = 0, conversions_ = 0, discarded_ = 0;
    qint64 loadMs_ = 0, maxApplyUs_ = 0;
    int maxPending_ = 0;
};

// GUI-owned cache and subscriptions; workers receive immutable requests only.
class FileIconService final : public QObject, public QAbstractNativeEventFilter {
public:
    using Loader = std::function<Result(const Request&)>;
    using Clock = std::function<qint64()>;
    explicit FileIconService(QObject* Parent = nullptr, Loader Load = {}, Clock Now = {});
    ~FileIconService() override;
    static FileIconService& Instance();
    void Subscribe(QObject* Owner, std::function<void()> Changed);
    void Unsubscribe(QObject* Owner);
    void BeginRequests(QObject* Owner);
    void SetAutomaticRefresh(QObject* Owner, bool Enabled);
    Value Read(const Request& Query, QObject* Owner = nullptr, bool Explicit = false);
    QPixmap DefaultIcon(bool Directory, int Pixels, bool Explorer, QObject* Owner = nullptr);
    QPixmap Compose(const QPixmap& Base, const Value& Overlay);
    void Invalidate(const QSet<QString>& Keys, int SettleMs = 0);
    void InvalidatePaths(const QSet<QString>& Paths, int SettleMs = 0);
    void InvalidateSystem();
    void CheckExpired();
    const Statistics& Stats() const { return stats_; }
    int Pending() const { return int(pending_.size()); }
    int CacheCost() const { return cache_.totalCost() + composed_.totalCost() + defaults_.totalCost(); }
protected:
    bool eventFilter(QObject* Watched, QEvent* Event) override;
    bool nativeEventFilter(const QByteArray& Type, void* Message, qintptr* Result) override;
private:
    struct Cached { Request query_; Value value_; qint64 expires_ = 0, defer_ = 0; int attempts_ = 0; bool force_ = false; QByteArray digest_; };
    struct Job { Request query_; quint64 serial_ = 0; };
    struct Subscription { QSet<QString> keys_; std::function<void()> changed_; bool automatic_ = true; };
    void enqueue(const Request& Query);
    void pump();
    void publish(const Job& Work, Result Output, qint64 Elapsed);
    void scheduleExpiry();
    void updateExpiry();
    void notify(const QString& Key);
    void watch(const QString& Key, const QStringList& Paths);
    void enableNativeNotifications();
    QElapsedTimer age_;
    Loader loader_;
    Clock now_;
    QThreadPool pool_;
    QCache<QString, Cached> cache_{24 * 1024};
    QCache<QString, QPixmap> composed_{6 * 1024}, defaults_{2 * 1024};
    QHash<QString, Job> pending_;
    QList<QString> queue_;
    QHash<QObject*, Subscription> subscribers_;
    QSet<QString> changed_;
    QHash<QString, QSet<QString>> dependencies_;
    QCache<QString, bool> customFolders_{4096};
    QFileSystemWatcher watcher_;
    QTimer pumpTimer_, changedTimer_, expiryTimer_;
    QTimer systemTimer_;
    QTimer expirySchedule_;
    QWindow* shellWindow_ = nullptr;
    quint32 shellRegistration_ = 0;
    qint64 dependencyPrune_ = -1000;
    quint64 serial_ = 0;
    int running_ = 0, remoteRunning_ = 0;
    bool queueBudgetHit_ = false;
    Statistics stats_;
};
}
