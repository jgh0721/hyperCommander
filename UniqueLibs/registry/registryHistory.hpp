#pragma once

/// 공통 라이브러리
#include "commonLibs/system/registryFile.hpp"
/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QJsonObject>
#include <QJsonArray>
#include <QHash>
#include <functional>

namespace hc::registry {
enum class StepKind { Value, Key, Link, RenameKey, PolicyFile, ObserveValue };
struct RegistryPolicyValueCheck
{
    RegistryLocation location;
    QString name;
    std::optional<RegistryValue> observed, before, after;
};
struct RegistryStep
{
    StepKind kind = StepKind::Value;
    RegistryLocation location;
    QString name;
    std::optional<RegistryValue> before, after;
    bool existedBefore = true, existsAfter = true;
    QByteArray treeHash; // Rename guards the complete subtree, excluding timestamps/ACLs.
    QJsonArray treeBackup;
    // One policy transaction has Save / Refresh / Verify phases (0 / 1 / 2).
    // These values are observations and assertions, never direct writes.
    int policyPhase = 0;
    QList<RegistryPolicyValueCheck> policyChecks;
};
struct RegistryPlan
{
    QString id, createdUtc, callerSid, machineId;
    QList<RegistryStep> steps;
    QString parentId, purpose = "edit";
    QByteArray Hash() const;
    QJsonObject ToJson() const;
    static RegistryResult<RegistryPlan> FromJson(const QJsonObject& Object);
};
struct RegistryApplyResult
{
    quint32 error = 0;
    int completed = 0;
    bool conflict = false;
    bool uncertain = false;
};
// Worker-thread-only planner and compare-before-write executor. Prepare never
// writes; each step is independently journaled before crossing a side effect.
class RegistryTransaction
{
public:
    using PolicyReader = std::function<RegistryResult<QByteArray>(bool)>;
    using PolicyWriter = std::function<RegistryResult<QByteArray>(bool, const QByteArray&, const QByteArray&)>;
    using PolicyRefresher = std::function<quint32(bool)>;
    explicit RegistryTransaction(IRegistryBackend& Backend, PolicyReader ReadPolicy = {}, PolicyWriter WritePolicy = {}, PolicyRefresher RefreshPolicy = {})
        : backend_(Backend), readPolicy_(std::move(ReadPolicy)), writePolicy_(std::move(WritePolicy)), refreshPolicy_(std::move(RefreshPolicy)) {}
    RegistryResult<RegistryPlan> Prepare(const QList<RegistryOperation>& Operations, const QString& CallerSid,
        const QString& MachineId, const std::atomic_bool* Cancel = nullptr,
        std::function<void(int, int, int)> StepRanges = {});
    RegistryResult<RegistryPlan> PrepareRename(const RegistryLocation& Location, const QString& Name,
        const QString& CallerSid, const QString& MachineId);
    RegistryApplyResult ApplyStep(const RegistryStep& Step, bool CallerRefreshed = false);
    RegistryResult<bool> Matches(const RegistryStep& Step, bool After);
    static RegistryStep Reverse(const RegistryStep& Step);
private:
    RegistryResult<QByteArray> treeHash(const RegistryLocation& Location, const std::atomic_bool* Cancel = nullptr, QJsonArray* Backup = nullptr);
    IRegistryBackend& backend_;
    PolicyReader readPolicy_;
    PolicyWriter writePolicy_;
    PolicyRefresher refreshPolicy_;
};
struct RegistryHistoryRecord
{
    RegistryPlan plan;
    QString state = "prepared";
    int completed = 0, inFlight = -1;
    quint32 error = 0;
    quint64 sequence = 0;
    bool redoAllowed = false;
    QString directory;
};
class RegistryChangeStore
{
public:
    using StepExecutor = std::function<RegistryApplyResult(const RegistryStep&, int)>;
    explicit RegistryChangeStore(QString BackupDirectory);
    RegistryResult<RegistryHistoryRecord> SavePrepared(const RegistryPlan& Plan);
    RegistryApplyResult Apply(RegistryHistoryRecord& Record, const StepExecutor& Execute);
    RegistryResult<QList<RegistryHistoryRecord>> History() const;
    RegistryResult<RegistryHistoryRecord> Read(const QString& Id) const;
    quint32 Recover(RegistryHistoryRecord& Record, RegistryTransaction& Transaction);
    RegistryResult<RegistryPlan> PrepareUndo(const RegistryHistoryRecord& Record, RegistryTransaction& Transaction) const;
    RegistryResult<RegistryPlan> PrepareRedo(const RegistryHistoryRecord& Original, const RegistryHistoryRecord& Undo, RegistryTransaction& Transaction) const;
    quint32 SetState(RegistryHistoryRecord& Record, const QString& State, bool RedoAllowed = false);
    QString Directory() const { return directory_; }
private:
    quint32 save(const RegistryHistoryRecord& Record) const;
    quint64 nextSequence() const;
    QString directory_;
};

QJsonObject RegistryLocationJson(const RegistryLocation& Location);
RegistryResult<RegistryLocation> RegistryLocationFromJson(const QJsonObject& Object);
QJsonObject RegistryValueJson(const RegistryValue& Value);
RegistryResult<RegistryValue> RegistryValueFromJson(const QJsonObject& Object);
QJsonObject RegistrySnapshotJson(const KeySnapshot& Snapshot);
RegistryResult<KeySnapshot> RegistrySnapshotFromJson(const QJsonObject& Object);
} // namespace hc::registry
