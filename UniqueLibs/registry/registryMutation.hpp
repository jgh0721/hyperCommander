#pragma once

/// 고유 라이브러리
#include "registryProtocol.hpp"

namespace hc::registry {
struct RegistryMutationResult
{
    RegistryHistoryRecord record;
    RegistryApplyResult applied;
    bool elevated = false;
    quint32 notificationError = 0;
};
// Synchronous worker-only adapter for UI commands. Both paths create a durable
// backup before the first write and retain partial/uncertain outcomes verbatim.
class RegistryMutation final
{
public:
    RegistryMutation(IRegistryBackend& Backend, RegistryChangeStore& Store, QString CallerSid, QString MachineId,
        RegistryRemoteSession* Remote = nullptr);
    RegistryMutationResult CreateKey(const RegistryLocation& Location);
    RegistryMutationResult DeleteSelected(const QList<RegistryOperation>& Selected);
    RegistryMutationResult Apply(const QList<RegistryOperation>& Operations);
    RegistryMutationResult ApplyPreview(const QList<RegistryOperation>& Operations, const RegistryPlan& Expected);
    RegistryMutationResult ApplyPolicy(bool Machine, const QByteArray& ExpectedPolicy, const QByteArray& DesiredPolicy,
        const QList<RegistryOperation>& LiveOperations, const QList<RegistryStep>& ExpectedValues);
    RegistryMutationResult SaveValue(const RegistryLocation& Location, const std::optional<RegistryValue>& Original, const RegistryValue& Draft);
    RegistryMutationResult SetCheckedValue(const RegistryLocation& Location, const QString& Name,
        const std::optional<RegistryValue>& Before, const std::optional<RegistryValue>& After, const QList<RegistryStep>& Guards = {});
    RegistryMutationResult RenameValue(const RegistryLocation& Location, const RegistryValue& Original, const QString& Name);
    RegistryMutationResult Rename(const RegistryLocation& Location, const QString& Name);
    RegistryMutationResult Undo(const RegistryHistoryRecord& Record);
    RegistryMutationResult Redo(const RegistryHistoryRecord& Original, const RegistryHistoryRecord& UndoRecord);
private:
    RegistryLocation bind(RegistryLocation Location) const;
    bool requiresHelper(const RegistryLocation& Location) const;
    RegistryMutationResult commit(RegistryResult<RegistryPlan> Prepared, bool Remote);
    IRegistryBackend& backend_;
    RegistryChangeStore& store_;
    QString callerSid_, machineId_;
    RegistryRemoteSession* remote_;
};
} // namespace hc::registry
