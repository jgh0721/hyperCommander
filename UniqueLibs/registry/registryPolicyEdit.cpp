#include "stdafx.h"
#include "registryPolicyEdit.hpp"

/// 모듈
/// UIs
/// 기타 헤더파일
/// 고유 라이브러리
/// 공통 라이브러리
#include "commonLibs/system/registryLocalPolicy.hpp"
#include "commonLibs/system/registryValue.hpp"
/// 외부 라이브러리
/// 선언 헤더파일들
/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )

DECLARE_CMNLIBSV2_NAMESPACE
Q_LOGGING_CATEGORY(lcRegistryPolicyEdit, "hc.registry.policyEdit")
namespace hc::registry {
namespace {
QString Address(const QString& Key, const QString& Name) { return Key.toCaseFolded() + QChar(0) + Name.toCaseFolded(); }
bool Same(const std::optional<RegistryValue>& A, const std::optional<RegistryValue>& B)
{
    return bool(A) == bool(B) && (!A || (A->type == B->type && A->bytes == B->bytes));
}
}
RegistryPolicyEditPlan RegistryPolicyEdit::Prepare(const AdmxPolicy& Policy, bool Machine, PolicyState State,
    const QVariantMap& Options, const PolicyObservation& Observation, const QByteArray& LocalPolicy)
{
    RegistryPolicyEditPlan Result; Result.beforePolicy = LocalPolicy;
    if (Policy.scope != "Both" && Policy.scope != (Machine ? "Machine" : "User")) { Result.error = QObject::tr("정책의 적용 범위가 일치하지 않습니다."); return Result; }
    const auto Program = RegistryPolicyDefinition::Compile(Policy, State, Options);
    if (!Program) { Result.error = Program.error; return Result; }
    if (!LocalPolicyStore::Canonical(LocalPolicy)) { Result.error = QObject::tr("원본 Registry.pol을 손실 없이 편집할 수 없습니다."); return Result; }
    const auto Parsed = LocalPolicy.isEmpty() ? RegistryPolResult{} : RegistryPolCodec::Read(LocalPolicy);
    QMap<QString, PolicyAssignment> Desired;
    for (const auto& Assignment : Program.assignments) Desired.insert(Address(Assignment.key, Assignment.name), Assignment);
    for (const auto& Key : Program.clearKeys) {
        const auto Found = Observation.keys.constFind(Key.toCaseFolded());
        if (Found == Observation.keys.cend()) { Result.error = QObject::tr("목록 키를 먼저 읽어야 합니다."); return Result; }
        for (const auto& Value : Found->value.values) {
            const auto Id = Address(Key, Value.name);
            if (!Desired.contains(Id)) Desired.insert(Id, {Key, Value.name});
        }
    }
    // Additive lists replace their stored list but retain values from other GPOs
    // or manual configuration. Only entries previously stored by this local GPO
    // are candidates for removal from the live key.
    for (const auto& Record : Parsed.records) if (Program.additiveKeys.contains(Record.key, Qt::CaseInsensitive)
        && Record.Directive() == PolicyDirective::Value) {
        const auto Id = Address(Record.key, Record.value.name);
        if (!Desired.contains(Id)) Desired.insert(Id, {Record.key, Record.value.name});
    }
    const auto Owns = [&](const RegistryPolRecord& Record) {
        if (Program.clearKeys.contains(Record.key, Qt::CaseInsensitive) || Program.additiveKeys.contains(Record.key, Qt::CaseInsensitive)) return true;
        const auto Name = Record.Directive() == PolicyDirective::DeleteValue ? Record.value.name.mid(6) : Record.Directive() == PolicyDirective::SoftValue ? Record.value.name.mid(7) : Record.value.name;
        return Desired.contains(Address(Record.key, Name));
    };
    QList<RegistryPolRecord> After;
    for (const auto& Record : Parsed.records) {
        // Complex key-wide directives may alter more than the selected policy.
        // Preserve them byte-for-byte on unrelated keys; block overlapping edits.
        const auto Kind = Record.Directive();
        const bool Simple = Kind == PolicyDirective::Value || Kind == PolicyDirective::DeleteValue || Kind == PolicyDirective::DeleteAllValues || Kind == PolicyDirective::SoftValue;
        bool AffectedKey = Program.clearKeys.contains(Record.key, Qt::CaseInsensitive) || Program.additiveKeys.contains(Record.key, Qt::CaseInsensitive);
        for (const auto& Assignment : Program.assignments) AffectedKey |= Assignment.key.compare(Record.key, Qt::CaseInsensitive) == 0;
        if (AffectedKey && !Simple) { Result.error = QObject::tr("겹치는 정책 지시문을 먼저 확인하세요: %1").arg(Record.value.name); return Result; }
        if (!Owns(Record)) After.append(Record);
    }
    // The list's delete directive precedes replacement entries in Registry.pol.
    // Unconfiguration removes the directive instead of persisting a deletion.
    if (State == PolicyState::Enabled) for (const auto& Key : Program.clearKeys)
        After.append({Key, {"**DelVals.", REG_SZ, RegistryValueCodec::EncodeText(REG_SZ, " ").value}});
    QSet<QString> Created;
    for (const auto& Assignment : Desired) {
        const auto Location = RegistryLocation::Parse((Machine ? "HKLM\\" : "HKCU\\") + Assignment.key);
        const auto Snapshot = Observation.keys.constFind(Assignment.key.toCaseFolded());
        if (!Location || Snapshot == Observation.keys.cend()) { Result.error = QObject::tr("정책 값의 원본을 읽어야 합니다."); return Result; }
        if (!Snapshot.value() && Snapshot->error != ERROR_FILE_NOT_FOUND && Snapshot->error != ERROR_PATH_NOT_FOUND) {
            Result.error = QObject::tr("정책 키 읽기 실패 (%1)").arg(Snapshot->error); return Result;
        }
        if (Snapshot->value.info.link) { Result.error = QObject::tr("링크 키 정책은 편집할 수 없습니다."); return Result; }
        std::optional<RegistryValue> Before;
        for (const auto& Value : Snapshot->value.values) if (Value.name.compare(Assignment.name, Qt::CaseInsensitive) == 0) { Before = Value; break; }
        const auto LiveAfter = Assignment.soft && (Before || !Assignment.value) ? Before : Assignment.value;
        if (Assignment.value) {
            auto Stored = *Assignment.value;
            if (Assignment.soft) Stored.name.prepend("**soft.");
            After.append({Assignment.key, Stored});
        }
        else if (Assignment.explicitDeletion && State != PolicyState::NotConfigured)
            After.append({Assignment.key, {"**Del." + Assignment.name, REG_SZ, RegistryValueCodec::EncodeText(REG_SZ, " ").value}});
        RegistryStep Expected; Expected.location = *Location; Expected.name = Assignment.name;
        Expected.before = Before; Expected.after = LiveAfter; Result.expectedValues.append(std::move(Expected));
        if (Same(Before, LiveAfter)) continue;
        if (LiveAfter && !Created.contains(Location->Id())) {
            Result.operations.append({OperationKind::CreateKey, *Location}); Created.insert(Location->Id());
        }
        Result.operations.append({LiveAfter ? OperationKind::SetValue : OperationKind::DeleteValue,
            *Location, LiveAfter.value_or(RegistryValue{Assignment.name})});

    }
    const auto Bytes = RegistryPolCodec::Write(After);
    if (!Bytes) { Result.error = QObject::tr("Registry.pol 인코딩 실패 (%1)").arg(Bytes.error); return Result; }
    Result.afterPolicy = Bytes.value;
    return Result;
}
} // namespace hc::registry
