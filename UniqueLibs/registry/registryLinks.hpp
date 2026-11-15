#pragma once
/// 공통 라이브러리
#include "commonLibs/system/registryAccess.hpp"
/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QObject>
#include <QHash>
#include <QSet>
#include <QProcessEnvironment>

namespace hc::registry {
struct RegistryLink
{
    enum class Kind { Key, File, Text, Guid };
    Kind kind = Kind::Key;
    RegistryLocation source;
    QString sourceName, label, text, file;
    std::optional<RegistryLocation> target;
    quint32 error = 0;
    bool fallback = false;
    bool operator==(const RegistryLink&) const = default;
};
struct RegistryLinkResult
{
    QList<RegistryLink> forward, chain;
    QList<RegistryLocation> physicalClasses;
    QStringList diagnostics;
};
struct RegistryLinkIndex
{
    QHash<QString, QList<RegistryLink>> incoming;
    QStringList diagnostics;
    qsizetype keys = 0;
    bool complete = true;
};
class RegistryLinkResolver final
{
public:
    static RegistryResult<RegistryLocation> PhysicalClasses(IRegistryBackend& Backend, Hive HiveId, View Bits);
    static QString CanonicalKey(const RegistryLocation& Location);
    static QString FilePath(const QString& Text, const QProcessEnvironment& Environment);
    static QList<RegistryLink> ValueLinks(IRegistryBackend& Backend, const RegistryLocation& Location,
        const RegistryValue& Value, const QProcessEnvironment& Environment, bool InspectFiles = true, bool ValidateTargets = true);
    static RegistryLinkResult Resolve(IRegistryBackend& Backend, const RegistryLocation& Location,
        const std::optional<RegistryValue>& Value, const QProcessEnvironment& Environment);
    static RegistryLinkIndex BuildIndex(IRegistryBackend& Backend, View SelectedView,
        const std::atomic_bool* Cancel = nullptr, qsizetype MaxKeys = 100000,
        QList<RegistryLocation> Roots = {}, std::atomic<qsizetype>* Progress = nullptr);
};
class RegistryLinks final : public QObject
{
    Q_OBJECT
public:
    explicit RegistryLinks(QObject* Parent = nullptr);
    ~RegistryLinks() override;
    void Select(const RegistryLocation& Location, std::optional<RegistryValue> Value = {});
    void Refresh();
    void Suspend();
    void SetCacheDirectory(QString Directory) { cacheDirectory_ = std::move(Directory); }
    bool FastIndexComplete() const { return fastComplete_; }
    qsizetype IndexedKeys() const { return progress_ ? progress_->load() : index_.keys; }
    RegistryLinkResult Result() const { return result_; }
    QList<RegistryLink> BackReferences() const;
    bool Loading() const { return loading_; }
    bool IndexLoading() const { return indexLoading_; }
    const RegistryLinkIndex& Index() const { return index_; }
signals:
    void Changed();
    void ProgressChanged();
private:
    void startIndex();
    RegistryLinkResult result_;
    RegistryLinkIndex index_;
    struct IndexWorker;
    std::shared_ptr<IndexWorker> indexWorker_;
    QString cacheDirectory_;
    bool fastComplete_ = false, indexJob_ = false;
    std::shared_ptr<std::atomic<qsizetype>> progress_;
    std::optional<RegistryLocation> location_;
    std::optional<RegistryValue> value_;
    View indexView_ = View::Native64;
    bool loading_ = false, indexLoading_ = false, indexed_ = false;
    quint64 generation_ = 0, indexGeneration_ = 0;
    std::shared_ptr<std::atomic_bool> cancel_;
    struct Watches;
    std::unique_ptr<Watches> watches_;
};
} // namespace hc::registry
