#pragma once
#include "registryHistory.hpp"

namespace hc::registry {
// Lives inside an already authenticated hc.system session. The caller SID is
// obtained from AcceptChannel, never from a registry request's JSON.
class RegistrySessionServer
{
public:
    RegistrySessionServer(IRegistryBackend& Backend, QString CallerSid, QString MachineId, QString TestRoot = {},
        RegistryTransaction::PolicyReader ReadPolicy = {}, RegistryTransaction::PolicyWriter WritePolicy = {});
    QJsonObject Handle(const QString& Kind, const QJsonObject& Request);
private:
    struct Blob { QByteArray bytes, hash; qsizetype expected = 0; bool upload = false; qint64 touched = 0; };
    struct Prepared { RegistryPlan plan; int next = 0; QJsonObject failure; qint64 touched = 0; qsizetype bytes = 0; };
    RegistryResult<RegistryLocation> bind(const QJsonObject& Object) const;
    QJsonObject publish(const QJsonObject& Object);
    void prune();
    qsizetype memory() const;
    IRegistryBackend& backend_;
    RegistryTransaction transaction_;
    QString callerSid_, machineId_, testRoot_;
    QHash<QString, Blob> blobs_;
    QHash<QString, Prepared> prepared_;
};
class RegistryRemoteSession
{
public:
    using Exchange = std::function<QJsonObject(const QString&, QJsonObject)>;
    using PolicyRefresh = std::function<quint32(bool, const std::atomic_bool*)>;
    explicit RegistryRemoteSession(Exchange Send, PolicyRefresh Refresh = {}) : send_(std::move(Send)), refresh_(std::move(Refresh)) {}
    void SetPolicyControl(const std::atomic_bool* Cancel, std::function<void(int)> Progress = {}) { cancel_ = Cancel; progress_ = std::move(Progress); }
    RegistryResult<KeySnapshot> Read(const RegistryLocation& Location);
    RegistryResult<RegistryPlan> Prepare(const QList<RegistryOperation>& Operations);
    RegistryResult<RegistryPlan> PrepareExisting(const RegistryPlan& Plan);
    RegistryResult<RegistryPlan> PrepareRename(const RegistryLocation& Location, const QString& Name);
    RegistryApplyResult Apply(const RegistryPlan& Plan, int Index);
    QJsonObject Status(const QString& PlanId);
    void Release(const QString& PlanId);
private:
    RegistryResult<QJsonObject> uploadAndPrepare(const QJsonObject& Object);
    RegistryResult<QJsonObject> download(const QJsonObject& Reply);
    Exchange send_;
    PolicyRefresh refresh_;
    const std::atomic_bool* cancel_ = nullptr;
    std::function<void(int)> progress_;
};
} // namespace hc::registry
