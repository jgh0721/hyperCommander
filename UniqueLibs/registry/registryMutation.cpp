#include "stdafx.h"
#include "registryMutation.hpp"

/// 모듈
/// UIs
/// 기타 헤더파일
/// 고유 라이브러리
/// 공통 라이브러리
#include "commonLibs/system/registryPolicyFile.hpp"
/// 외부 라이브러리
/// 선언 헤더파일들
/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )

DECLARE_CMNLIBSV2_NAMESPACE
Q_LOGGING_CATEGORY(lcRegistryMutation, "hc.registry.mutation")

namespace hc::registry {
RegistryMutation::RegistryMutation(IRegistryBackend& Backend, RegistryChangeStore& Store, QString CallerSid, QString MachineId,
    RegistryRemoteSession* Remote)
    : backend_(Backend), store_(Store), callerSid_(std::move(CallerSid)), machineId_(std::move(MachineId)), remote_(Remote) {}
RegistryLocation RegistryMutation::bind(RegistryLocation Location) const
{
    if (Location.hive == Hive::CurrentUser) Location.sid = callerSid_;
    return Location;
}
bool RegistryMutation::requiresHelper(const RegistryLocation& Location) const
{
    if (Location.hive == Hive::LocalMachine || Location.hive == Hive::CurrentConfig) return true;
    if (Location.hive == Hive::Users) {
        const auto Owner = Location.key.section('\\', 0, 0);
        return Owner.compare(callerSid_, Qt::CaseInsensitive) != 0 && Owner.compare(callerSid_ + "_Classes", Qt::CaseInsensitive) != 0;
    }
    return false;
}
RegistryMutationResult RegistryMutation::commit(RegistryResult<RegistryPlan> Prepared, bool Remote)
{
    RegistryMutationResult Result; Result.elevated = Remote;
    if (!Prepared) { Result.applied.error = Prepared.error; return Result; }
    // No permissive fallback on missing identity or a merged HKCR plan.
    if (callerSid_.isEmpty() || machineId_.isEmpty() || Prepared.value.callerSid != callerSid_ || Prepared.value.machineId != machineId_) {
        Result.applied.error = ERROR_ACCESS_DENIED; return Result;
    }
    if (Remote && !remote_) { Result.applied.error = ERROR_ELEVATION_REQUIRED; return Result; }
    auto Saved = store_.SavePrepared(Prepared.value);
    if (!Saved) {
        if (Remote) remote_->Release(Prepared.value.id);
        Result.applied.error = Saved.error; return Result;
    }
    Result.record = std::move(Saved.value);
    RegistryTransaction Transaction(backend_);
    Result.applied = store_.Apply(Result.record, [&](const RegistryStep& Step, int Index) {
        return Remote ? remote_->Apply(Result.record.plan, Index) : Transaction.ApplyStep(Step);
    });
    if (Remote) remote_->Release(Result.record.plan.id);
    return Result;
}
RegistryMutationResult RegistryMutation::Apply(const QList<RegistryOperation>& Operations)
{
    auto Bound = Operations; bool Elevated = false;
    for (auto& Operation : Bound) {
        Operation.location = bind(Operation.location);
        if (Operation.location.hive == Hive::ClassesRoot) return commit({{}, ERROR_NOT_SUPPORTED}, false);
        Elevated |= requiresHelper(Operation.location);
    }
    if (Elevated && !remote_) return commit({{}, ERROR_ELEVATION_REQUIRED}, true);
    RegistryTransaction Transaction(backend_);
    return commit(Elevated ? remote_->Prepare(Bound) : Transaction.Prepare(Bound, callerSid_, machineId_), Elevated);
}
RegistryMutationResult RegistryMutation::ApplyPreview(const QList<RegistryOperation>& Operations, const RegistryPlan& Expected)
{
    if (Expected.callerSid != callerSid_ || Expected.machineId != machineId_) return commit({{}, ERROR_ACCESS_DENIED}, false);
    auto Bound = Operations; bool Elevated = false;
    for (auto& Operation : Bound) {
        Operation.location = bind(Operation.location);
        if (Operation.location.hive == Hive::ClassesRoot) return commit({{}, ERROR_NOT_SUPPORTED}, false);
        Elevated |= requiresHelper(Operation.location);
    }
    if (Elevated && !remote_) return commit({{}, ERROR_ELEVATION_REQUIRED}, true);
    RegistryTransaction Transaction(backend_);
    auto Prepared = Elevated ? remote_->Prepare(Bound) : Transaction.Prepare(Bound, callerSid_, machineId_);
    if (!Prepared) return commit(std::move(Prepared), Elevated);
    // Compare every ordered before/after step before the first write. A new
    // preparation must not silently approve changes made after the preview.
    if (Prepared.value.ToJson()["steps"] != Expected.ToJson()["steps"]) {
        if (Elevated) remote_->Release(Prepared.value.id);
        RegistryMutationResult Result; Result.elevated = Elevated;
        Result.applied.error = ERROR_REVISION_MISMATCH; Result.applied.conflict = true; return Result;
    }
    if (Prepared.value.steps.isEmpty()) {
        if (Elevated) remote_->Release(Prepared.value.id);
        return {};
    }
    return commit(std::move(Prepared), Elevated);
}
RegistryMutationResult RegistryMutation::ApplyPolicy(bool Machine, const QByteArray& ExpectedPolicy, const QByteArray& DesiredPolicy,
    const QList<RegistryOperation>& LiveOperations, const QList<RegistryStep>& ExpectedValues)
{
    if (!remote_) return commit({{}, ERROR_ELEVATION_REQUIRED}, true);
    Q_UNUSED(LiveOperations); // Preview only; GPO-managed values are never written directly.
    auto Prepared = remote_->Prepare({});
    if (!Prepared) return commit(std::move(Prepared), true);
    remote_->Release(Prepared.value.id);
    const auto Before = ExpectedPolicy.isEmpty() ? RegistryPolResult{} : RegistryPolCodec::Read(ExpectedPolicy);
    const auto After = DesiredPolicy.isEmpty() ? RegistryPolResult{} : RegistryPolCodec::Read(DesiredPolicy);
    if (!Before || !After) return commit({{}, ERROR_INVALID_DATA}, true);
    RegistryStep Policy; Policy.kind = StepKind::PolicyFile;
    Policy.location = bind(RegistryLocation{Machine ? Hive::LocalMachine : Hive::CurrentUser});
    Policy.name = "Registry.pol";
    Policy.before = RegistryValue{Policy.name, REG_BINARY, ExpectedPolicy};
    Policy.after = RegistryValue{Policy.name, REG_BINARY, DesiredPolicy};
    for (const auto& Value : ExpectedValues) {
        RegistryPolicyValueCheck Check; Check.location = bind(Value.location); Check.name = Value.name; Check.observed = Value.before;
        if (Check.location.hive != Policy.location.hive) return commit({{}, ERROR_INVALID_PARAMETER}, true);
        const auto Soft = [&](const auto& Records) {
            for (const auto& Record : Records) if (Record.Directive() == PolicyDirective::SoftValue
                && Record.key.compare(Check.location.key, Qt::CaseInsensitive) == 0
                && Record.value.name.mid(7).compare(Check.name, Qt::CaseInsensitive) == 0) return true;
            return false;
        };
        const bool HasSoft = Soft(Before.records) || Soft(After.records);
        const auto Expected = [&](const auto& Records) -> std::optional<RegistryValue> {
            for (const auto& Record : Records) {
                if (Record.key.compare(Check.location.key, Qt::CaseInsensitive) != 0) continue;
                if (Record.Directive() == PolicyDirective::Value && Record.value.name.compare(Check.name, Qt::CaseInsensitive) == 0) return Record.value;
                if (Record.Directive() == PolicyDirective::DeleteValue && Record.value.name.mid(6).compare(Check.name, Qt::CaseInsensitive) == 0) return {};
                if (Record.Directive() == PolicyDirective::SoftValue && Record.value.name.mid(7).compare(Check.name, Qt::CaseInsensitive) == 0) {
                    if (Check.observed) return Check.observed;
                    auto Result = Record.value; Result.name = Check.name; return Result;
                }
            }
            // Unconfiguration follows the old policy file, not a manual raw
            // before-image; soft defaults are the exception because they never
            // take ownership of an already-present manual value.
            return HasSoft ? Check.observed : std::nullopt;
        };
        Check.before = Expected(Before.records); Check.after = Expected(After.records);
        Policy.policyChecks.append(std::move(Check));
    }
    for (int Phase = 0; Phase < 3; ++Phase) { Policy.policyPhase = Phase; Prepared.value.steps.append(Policy); }
    const auto Valid = RegistryPlan::FromJson(Prepared.value.ToJson());
    if (!Valid) return commit({{}, Valid.error}, true);
    return commit(remote_->PrepareExisting(Prepared.value), true);
}
RegistryMutationResult RegistryMutation::SaveValue(const RegistryLocation& Location, const std::optional<RegistryValue>& Original, const RegistryValue& Draft)
{
    const auto Bound = bind(Location); const bool Elevated = requiresHelper(Bound);
    if (Bound.hive == Hive::ClassesRoot) return commit({{}, ERROR_NOT_SUPPORTED}, false);
    if (Original && Original->name.compare(Draft.name, Qt::CaseInsensitive) != 0) return commit({{}, ERROR_INVALID_PARAMETER}, false);
    if (Elevated && !remote_) return commit({{}, ERROR_ELEVATION_REQUIRED}, true);
    RegistryTransaction Transaction(backend_);
    const QList<RegistryOperation> Operations{{OperationKind::SetValue, Bound, Draft}};
    auto Prepared = Elevated ? remote_->Prepare(Operations) : Transaction.Prepare(Operations, callerSid_, machineId_);
    if (!Prepared) return commit(std::move(Prepared), Elevated);
    // Compare the prepared before-image with the image the editor actually
    // showed. A fresh preparation must never overwrite an intervening edit.
    const bool Matches = Prepared.value.steps.size() == 1 && Prepared.value.steps[0].kind == StepKind::Value
        && Prepared.value.steps[0].before == Original;
    if (!Matches) {
        if (Elevated) remote_->Release(Prepared.value.id);
        RegistryMutationResult Result; Result.elevated = Elevated;
        Result.applied.error = ERROR_REVISION_MISMATCH; Result.applied.conflict = true; return Result;
    }
    return commit(std::move(Prepared), Elevated);
}
RegistryMutationResult RegistryMutation::RenameValue(const RegistryLocation& Location, const RegistryValue& Original, const QString& Name)
{
    const auto Bound = bind(Location); const bool Elevated = requiresHelper(Bound);
    if (Bound.hive == Hive::ClassesRoot) return commit({{}, ERROR_NOT_SUPPORTED}, false);
    if (!WinRegistryBackend::ValidName(Name, false) || Original.name.isEmpty() || Name.isEmpty())
        return commit({{}, ERROR_INVALID_PARAMETER}, false);
    if (Original.name == Name) return {};
    if (Elevated && !remote_) return commit({{}, ERROR_ELEVATION_REQUIRED}, true);
    auto Renamed = Original; Renamed.name = Name;
    const bool CaseOnly = Original.name.compare(Name, Qt::CaseInsensitive) == 0;
    // Windows keeps existing value-name casing on RegSetValueEx. A case-only
    // rename must delete then recreate; the durable journal retains the data
    // across that interval. Other renames retain the source until the copy exists.
    const RegistryOperation Remove{OperationKind::DeleteValue, Bound, Original};
    const RegistryOperation Create{OperationKind::SetValue, Bound, Renamed};
    const QList<RegistryOperation> Operations = CaseOnly ? QList{Remove, Create} : QList{Create, Remove};
    RegistryTransaction Transaction(backend_);
    auto Prepared = Elevated ? remote_->Prepare(Operations) : Transaction.Prepare(Operations, callerSid_, machineId_);
    if (!Prepared) return commit(std::move(Prepared), Elevated);
    bool Matches = Prepared.value.steps.size() == 2;
    if (Matches) {
        const auto& Deleted = Prepared.value.steps[CaseOnly ? 0 : 1];
        const auto& Created = Prepared.value.steps[CaseOnly ? 1 : 0];
        Matches = Deleted.kind == StepKind::Value && Deleted.before == Original && !Deleted.after
            && Created.kind == StepKind::Value && !Created.before && Created.after == Renamed;
    }
    if (!Matches) {
        if (Elevated) remote_->Release(Prepared.value.id);
        RegistryMutationResult Result; Result.elevated = Elevated;
        Result.applied.error = ERROR_REVISION_MISMATCH; Result.applied.conflict = true; return Result;
    }
    return commit(std::move(Prepared), Elevated);
}
RegistryMutationResult RegistryMutation::Rename(const RegistryLocation& Location, const QString& Name)
{
    const auto Bound = bind(Location); const bool Elevated = requiresHelper(Bound);
    if (Bound.hive == Hive::ClassesRoot) return commit({{}, ERROR_NOT_SUPPORTED}, false);
    if (Elevated && !remote_) return commit({{}, ERROR_ELEVATION_REQUIRED}, true);
    RegistryTransaction Transaction(backend_);
    return commit(Elevated ? remote_->PrepareRename(Bound, Name) : Transaction.PrepareRename(Bound, Name, callerSid_, machineId_), Elevated);
}
RegistryMutationResult RegistryMutation::Undo(const RegistryHistoryRecord& Record)
{
    if (Record.plan.callerSid != callerSid_ || Record.plan.machineId != machineId_) return commit({{}, ERROR_ACCESS_DENIED}, false);
    RegistryTransaction Transaction(backend_); auto Prepared = store_.PrepareUndo(Record, Transaction);
    bool Elevated = false; for (const auto& Step : Record.plan.steps) Elevated |= Step.kind == StepKind::PolicyFile || requiresHelper(Step.location);
    if (Prepared && Elevated) {
        if (!remote_) return commit({{}, ERROR_ELEVATION_REQUIRED}, true);
        Prepared = remote_->PrepareExisting(Prepared.value);
    }
    return commit(std::move(Prepared), Elevated);
}
RegistryMutationResult RegistryMutation::Redo(const RegistryHistoryRecord& Original, const RegistryHistoryRecord& UndoRecord)
{
    if (Original.plan.callerSid != callerSid_ || Original.plan.machineId != machineId_) return commit({{}, ERROR_ACCESS_DENIED}, false);
    RegistryTransaction Transaction(backend_); auto Prepared = store_.PrepareRedo(Original, UndoRecord, Transaction);
    bool Elevated = false; for (const auto& Step : Original.plan.steps) Elevated |= Step.kind == StepKind::PolicyFile || requiresHelper(Step.location);
    if (Prepared && Elevated) {
        if (!remote_) return commit({{}, ERROR_ELEVATION_REQUIRED}, true);
        Prepared = remote_->PrepareExisting(Prepared.value);
    }
    return commit(std::move(Prepared), Elevated);
}
} // namespace hc::registry
