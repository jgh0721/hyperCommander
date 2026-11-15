#pragma once
/// 고유 라이브러리
#include "registryPolicyState.hpp"
#include "registryResultModel.hpp"

namespace hc::registry {
struct RegistryPolicyEntry
{
    AdmxPolicy policy;
    PolicyObservation observation;
    bool machine = false;
    QByteArray localPolicy;
    quint32 localError = 0;
};
struct RegistryPolicyScan
{
    QList<RegistryResultRow> rows;
    QHash<QString, RegistryPolicyEntry> entries;
    QStringList diagnostics;
};
class RegistryPolicies final : public QObject
{
    Q_OBJECT
public:
    explicit RegistryPolicies(QObject* Parent = nullptr);
    ~RegistryPolicies() override;
    RegistryResultModel* Model() const { return model_; }
    bool Loading() const { return loading_; }
    QStringList Diagnostics() const { return data_.diagnostics; }
    std::optional<RegistryPolicyEntry> Entry(const QString& Id) const;
    void Reload(std::shared_ptr<const AdmxCatalog> Catalog);
    static RegistryPolicyScan Scan(const AdmxCatalog& Catalog, IRegistryBackend& Backend,
        const RegistryResult<QByteArray>& MachinePolicy, const RegistryResult<QByteArray>& UserPolicy,
        const std::atomic_bool* Cancel = nullptr);
signals:
    void Changed();
private:
    RegistryResultModel* model_;
    RegistryPolicyScan data_;
    bool loading_ = false;
    quint64 generation_ = 0;
    std::shared_ptr<std::atomic_bool> cancel_;
};
} // namespace hc::registry
