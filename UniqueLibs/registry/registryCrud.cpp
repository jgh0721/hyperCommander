#include "stdafx.h"
#include "registryMutation.hpp"

/// 모듈
/// UIs
/// 기타 헤더파일
/// 고유 라이브러리
/// 공통 라이브러리
/// 외부 라이브러리
/// 선언 헤더파일들
/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )

DECLARE_CMNLIBSV2_NAMESPACE
Q_LOGGING_CATEGORY(lcRegistryCrud, "hc.registry.crud")
namespace hc::registry {
RegistryMutationResult RegistryMutation::CreateKey(const RegistryLocation& Location)
{
    const auto Bound = bind(Location); const bool Elevated = requiresHelper(Bound);
    if (Bound.hive == Hive::ClassesRoot) return commit({{}, ERROR_NOT_SUPPORTED}, false);
    if (Bound.key.isEmpty()) return commit({{}, ERROR_INVALID_PARAMETER}, false);
    if (Elevated && !remote_) return commit({{}, ERROR_ELEVATION_REQUIRED}, true);
    RegistryTransaction Transaction(backend_);
    const QList<RegistryOperation> Operations{{OperationKind::CreateKey, Bound}};
    auto Prepared = Elevated ? remote_->Prepare(Operations) : Transaction.Prepare(Operations, callerSid_, machineId_);
    if (!Prepared) return commit(std::move(Prepared), Elevated);
    // A UI child creation must not revive missing ancestors or accept an
    // existing destination as success after the name dialog was displayed.
    const bool Matches = Prepared.value.steps.size() == 1 && Prepared.value.steps[0].kind == StepKind::Key
        && Prepared.value.steps[0].location == Bound && !Prepared.value.steps[0].existedBefore && Prepared.value.steps[0].existsAfter;
    if (!Matches) {
        if (Elevated) remote_->Release(Prepared.value.id);
        RegistryMutationResult Result; Result.elevated = Elevated;
        Result.applied.error = ERROR_REVISION_MISMATCH; Result.applied.conflict = true; return Result;
    }
    return commit(std::move(Prepared), Elevated);
}
RegistryMutationResult RegistryMutation::DeleteSelected(const QList<RegistryOperation>& Selected)
{
    QList<RegistryOperation> Bound; bool Elevated = false;
    for (auto Operation : Selected) {
        if (Operation.kind != OperationKind::DeleteKey && Operation.kind != OperationKind::DeleteValue)
            return commit({{}, ERROR_INVALID_PARAMETER}, false);
        Operation.location = bind(Operation.location);
        if (Operation.location.hive == Hive::ClassesRoot) return commit({{}, ERROR_NOT_SUPPORTED}, false);
        if (Operation.kind == OperationKind::DeleteKey && Operation.location.key.isEmpty())
            return commit({{}, ERROR_ACCESS_DENIED}, false);
        Elevated |= requiresHelper(Operation.location);
        Bound.append(std::move(Operation));
    }
    // Expanded view can select a key and its descendants together. Keep only
    // the outer key; every affected byte is still captured by recursive prepare.
    QList<RegistryOperation> Operations;
    for (qsizetype Index = 0; Index < Bound.size(); ++Index) {
        const auto& Current = Bound[Index]; bool Covered = false;
        for (qsizetype OtherIndex = 0; OtherIndex < Bound.size(); ++OtherIndex) {
            if (OtherIndex == Index) continue;
            const auto& Other = Bound[OtherIndex];
            if (Other.location.hive != Current.location.hive || Other.location.view != Current.location.view || Other.location.sid != Current.location.sid) continue;
            const bool SameKey = Other.location.key.compare(Current.location.key, Qt::CaseInsensitive) == 0;
            if (Other.kind == OperationKind::DeleteKey
                && ((SameKey && (Current.kind == OperationKind::DeleteValue || OtherIndex < Index))
                    || Current.location.key.startsWith(Other.location.key + '\\', Qt::CaseInsensitive))) { Covered = true; break; }
            if (OtherIndex < Index && SameKey && Other.kind == Current.kind
                && Other.value.name.compare(Current.value.name, Qt::CaseInsensitive) == 0) { Covered = true; break; }
        }
        if (!Covered) Operations.append(Current);
    }
    if (Operations.isEmpty()) return commit({{}, ERROR_INVALID_PARAMETER}, false);
    if (Elevated && !remote_) return commit({{}, ERROR_ELEVATION_REQUIRED}, true);
    RegistryTransaction Transaction(backend_);
    auto Prepared = Elevated ? remote_->Prepare(Operations) : Transaction.Prepare(Operations, callerSid_, machineId_);
    if (!Prepared) return commit(std::move(Prepared), Elevated);
    bool Matches = true;
    for (const auto& Operation : Operations) {
        bool Found = false;
        for (const auto& Step : Prepared.value.steps) {
            if (Step.location != Operation.location) continue;
            if (Operation.kind == OperationKind::DeleteValue && Step.kind == StepKind::Value
                && Step.before == Operation.value && !Step.after) { Found = true; break; }
            if (Operation.kind == OperationKind::DeleteKey && (Step.kind == StepKind::Key || Step.kind == StepKind::Link)
                && Step.existedBefore && !Step.existsAfter) { Found = true; break; }
        }
        if (!Found) { Matches = false; break; }
    }
    if (!Matches) {
        if (Elevated) remote_->Release(Prepared.value.id);
        RegistryMutationResult Result; Result.elevated = Elevated;
        Result.applied.error = ERROR_REVISION_MISMATCH; Result.applied.conflict = true; return Result;
    }
    return commit(std::move(Prepared), Elevated);
}
RegistryMutationResult RegistryMutation::SetCheckedValue(const RegistryLocation& Location, const QString& Name,
    const std::optional<RegistryValue>& Before, const std::optional<RegistryValue>& After, const QList<RegistryStep>& Guards)
{
    const auto Bound = bind(Location); bool Elevated = requiresHelper(Bound);
    if (Bound.hive == Hive::ClassesRoot || (After && After->name.compare(Name, Qt::CaseInsensitive) != 0) || (Before && Before->name.compare(Name, Qt::CaseInsensitive) != 0)) return commit({{}, ERROR_INVALID_PARAMETER}, false);
    auto Checks = Guards;
    for (auto& Check : Checks) {
        Check.location = bind(Check.location); Elevated |= requiresHelper(Check.location);
        if (Check.kind != StepKind::ObserveValue || Check.before != Check.after || Check.location.hive == Hive::ClassesRoot) return commit({{}, ERROR_INVALID_PARAMETER}, false);
    }
    if (Elevated && !remote_) return commit({{}, ERROR_ELEVATION_REQUIRED}, true);
    QList<RegistryOperation> Operations;
    if (After) Operations.append({OperationKind::CreateKey, Bound});
    Operations.append({After ? OperationKind::SetValue : OperationKind::DeleteValue, Bound, After.value_or(RegistryValue{Name})});
    RegistryTransaction Transaction(backend_);
    auto Prepared = Elevated ? remote_->Prepare(Operations) : Transaction.Prepare(Operations, callerSid_, machineId_);
    if (!Prepared) return commit(std::move(Prepared), Elevated);
    bool Matched = false;
    for (const auto& Step : Prepared.value.steps) if (Step.kind == StepKind::Value && Step.location == Bound && Step.name.compare(Name, Qt::CaseInsensitive) == 0)
        Matched = Step.before == Before && Step.after == After;
    if (!Matched) {
        if (Elevated) remote_->Release(Prepared.value.id);
        RegistryMutationResult Result; Result.applied.error = ERROR_REVISION_MISMATCH; Result.applied.conflict = true; return Result;
    }
    if (!Checks.isEmpty()) {
        if (Elevated) remote_->Release(Prepared.value.id);
        Checks.append(Prepared.value.steps); Prepared.value.steps = Checks;
        if (Elevated) Prepared = remote_->PrepareExisting(Prepared.value);
    }
    return commit(std::move(Prepared), Elevated);
}
}