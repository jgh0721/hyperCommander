#pragma once
/// 고유 라이브러리
#include "registryAdmx.hpp"
#include "registryCatalog.hpp"
/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QObject>
#include <QFileSystemWatcher>
#include <QTimer>

namespace hc::registry {
struct RegistryExplanation
{
    RegistryDescription builtin;
    QList<int> policyIndexes;
    QString title, text;
    QStringList sources, warnings;
    bool Known() const { return bool(builtin) || !policyIndexes.isEmpty(); }
};
class RegistryDescriptions final : public QObject
{
    Q_OBJECT
public:
    explicit RegistryDescriptions(QString Directory, QString Language, QString CacheDirectory, QObject* Parent = nullptr);
    ~RegistryDescriptions() override;
    static RegistryDescriptions* Shared(const QString& CacheDirectory);
    void Reload();
    bool Loading() const { return loading_; }
    RegistryExplanation Find(const RegistryLocation& Location, const std::optional<QString>& Name = {},
        std::optional<quint32> Type = {}) const;
    std::shared_ptr<const AdmxCatalog> Policies() const { return policies_; }
    const BuiltinRegistryCatalog& Builtin() const { return builtin_; }
    QString Language() const { return language_; }
signals:
    void Changed();
private:
    QString directory_, language_, cacheDirectory_;
    bool loading_ = false, reloadRequested_ = false;
    quint64 generation_ = 0;
    std::shared_ptr<std::atomic_bool> cancel_;
    std::shared_ptr<const AdmxCatalog> policies_;
    BuiltinRegistryCatalog builtin_;
    mutable QHash<QString, RegistryExplanation> lookup_;
    QFileSystemWatcher watcher_;
    QTimer debounce_;
};
} // namespace hc::registry
