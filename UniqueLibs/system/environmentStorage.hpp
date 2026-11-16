#pragma once

/// 고유 라이브러리
#include "UniqueLibs/registry/registryProtocol.hpp"
/// 공통 라이브러리
#include "commonLibs/system/environmentBackend.hpp"
#include "commonLibs/system/registryValue.hpp"

namespace hc::environment {
registry::RegistryLocation EnvironmentLocation(Scope Source, const QString& CallerSid = {});
registry::RegistryResult<Value> DecodeEnvironmentValue(const registry::RegistryValue& Raw, Scope Source);
registry::RegistryValue EncodeEnvironmentValue(const Value& Item);
struct EnvironmentChange
{
    Scope scope_ = Scope::User;
    QString name_;
    std::optional<Value> before_, after_;
};
struct EnvironmentPreview
{
    registry::RegistryPlan plan_;
    quint32 error_ = 0;
    explicit operator bool() const { return !error_; }
};
struct EnvironmentCommit
{
    registry::RegistryHistoryRecord record_;
    registry::RegistryApplyResult applied_;
    bool restored_ = false;
    quint32 recoveryError_ = 0;
    QString recoveryId_;
};
// All production writes pass through a durable registry plan. The injected
// backend allows host tests to exercise the identical logic without registry I/O.
class EnvironmentStorage final
{
public:
    EnvironmentStorage(registry::IRegistryBackend& Backend, registry::RegistryChangeStore& Store,
        QString CallerSid, QString MachineId, registry::RegistryRemoteSession* Remote = nullptr);
    registry::RegistryResult<std::vector<Value>> ReadScope(Scope Source);
    quint32 RecoverLocalHistory();
    EnvironmentPreview Preview(const QList<EnvironmentChange>& Changes);
    EnvironmentCommit Commit(const EnvironmentPreview& Preview);
    EnvironmentPreview Undo(const registry::RegistryHistoryRecord& Record);
    EnvironmentPreview Redo(const registry::RegistryHistoryRecord& Original, const registry::RegistryHistoryRecord& UndoRecord);
private:
    EnvironmentCommit commit(const EnvironmentPreview& Preview, bool RestoreOnFailure);
    bool allowed(const registry::RegistryStep& Step) const;
    registry::IRegistryBackend& backend_;
    registry::RegistryChangeStore& store_;
    QString callerSid_, machineId_;
    registry::RegistryRemoteSession* remote_;
};
}
