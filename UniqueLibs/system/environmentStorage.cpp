#include "stdafx.h"
#include "environmentStorage.hpp"

/// 모듈
/// UIs
/// 기타 헤더파일
/// 고유 라이브러리
/// 공통 라이브러리
#include "commonLibs/system/environmentSensitivity.hpp"
/// 외부 라이브러리
/// 선언 헤더파일들
/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QDateTime>
#include <QUuid>
#include <QSet>
#include <QScopeGuard>

DECLARE_CMNLIBSV2_NAMESPACE
Q_LOGGING_CATEGORY(lcEnvironmentStorage, "hc.environment.storage")

namespace hc::environment {
using namespace hc::registry;
RegistryLocation EnvironmentLocation(Scope Source, const QString& CallerSid)
{
    return {Source == Scope::System ? Hive::LocalMachine : Hive::CurrentUser,
        Source == Scope::System ? QStringLiteral("SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment") : QStringLiteral("Environment"),
        View::Native64, Source == Scope::System ? QString() : CallerSid};
}
RegistryResult<Value> DecodeEnvironmentValue(const RegistryValue& Raw, Scope Source)
{
    if (Raw.type != REG_SZ && Raw.type != REG_EXPAND_SZ) return {{}, ERROR_UNSUPPORTED_TYPE};
    const auto Text = RegistryValueCodec::DecodeText(Raw, false);
    Value Item{Raw.name.toStdWString(), Text.text.toStdWString(), ValueType(Raw.type), Source};
    if (!Text.lossless || ValidateValue(Item)) return {{}, ERROR_INVALID_DATA};
    return {std::move(Item)};
}
RegistryValue EncodeEnvironmentValue(const Value& Item)
{
    return {QString::fromStdWString(Item.name_), quint32(Item.type_), RegistryValueCodec::EncodeText(quint32(Item.type_), QString::fromStdWString(Item.raw_)).value};
}
EnvironmentStorage::EnvironmentStorage(IRegistryBackend& Backend, RegistryChangeStore& Store, QString CallerSid,
    QString MachineId, RegistryRemoteSession* Remote)
    : backend_(Backend), store_(Store), callerSid_(std::move(CallerSid)), machineId_(std::move(MachineId)), remote_(Remote) {}
RegistryResult<std::vector<Value>> EnvironmentStorage::ReadScope(Scope Source)
{
    if (Source != Scope::User && Source != Scope::System) return {{}, ERROR_INVALID_PARAMETER};
    const auto Read = backend_.Enumerate(EnvironmentLocation(Source, callerSid_));
    if (!Read) {
        if (Source == Scope::User && Read.complete && (Read.error == ERROR_FILE_NOT_FOUND || Read.error == ERROR_PATH_NOT_FOUND)) return {};
        return {{}, Read.error, Read.complete};
    }
    RegistryResult<std::vector<Value>> Result;
    for (const auto& Raw : Read.value.values) {
        const auto Item = DecodeEnvironmentValue(Raw, Source);
        if (!Item) { Result.error = Item.error; Result.complete = false; continue; }
        Result.value.push_back(Item.value);
    }
    return Result;
}
EnvironmentPreview EnvironmentStorage::Preview(const QList<EnvironmentChange>& Changes)
{
    EnvironmentPreview Result;
    if (callerSid_.isEmpty() || machineId_.isEmpty() || Changes.isEmpty() || Changes.size() > 4096) { Result.error_ = ERROR_INVALID_PARAMETER; return Result; }
    Result.plan_.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    Result.plan_.createdUtc = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    Result.plan_.callerSid = callerSid_; Result.plan_.machineId = machineId_; Result.plan_.purpose = "edit";
    QSet<QString> Seen, MissingKeys;
    QList<RegistryStep> CreateKeys;
    std::vector<Value> Definitions;
    for (const auto Source : {Scope::System, Scope::User}) {
        const auto Read = ReadScope(Source); if (Read) Definitions.insert(Definitions.end(), Read.value.begin(), Read.value.end());
    }
    for (const auto& Change : Changes) for (const auto& Item : {Change.before_, Change.after_}) if (Item) Definitions.push_back(*Item);
    const auto Sensitive = SensitiveVariables(Definitions);
    for (const auto& Change : Changes) {
        if ((Change.scope_ != Scope::User && Change.scope_ != Scope::System) || Change.name_.isEmpty()) { Result.error_ = ERROR_INVALID_PARAMETER; return Result; }
        const auto Location = EnvironmentLocation(Change.scope_, callerSid_);
        const auto Id = Location.Id() + QChar(0) + Change.name_.toCaseFolded();
        if (Seen.contains(Id)) { Result.error_ = ERROR_DUP_NAME; return Result; } Seen.insert(Id);
        RegistryStep Step; Step.location = Location; Step.name = Change.name_; Step.sensitive = Sensitive.contains(Change.name_.toStdWString());
        for (const auto& Pair : {std::pair{&Change.before_, &Step.before}, std::pair{&Change.after_, &Step.after}}) {
            if (!*Pair.first) continue;
            const auto& Value = **Pair.first;
            if (ValidateValue(Value) || Value.scope_ != Change.scope_ || !SameName(Value.name_, Change.name_.toStdWString())) { Result.error_ = ERROR_INVALID_DATA; return Result; }
            *Pair.second = EncodeEnvironmentValue(Value);
        }
        if (Step.before == Step.after) Step.kind = StepKind::ObserveValue;
        const auto Snapshot = backend_.Enumerate(Location);
        const bool Missing = !Snapshot && Snapshot.complete && (Snapshot.error == ERROR_FILE_NOT_FOUND || Snapshot.error == ERROR_PATH_NOT_FOUND);
        if (Missing && Change.scope_ == Scope::User && !Change.before_ && Change.after_) {
            if (!MissingKeys.contains(Location.Id())) {
                RegistryStep Create; Create.kind = StepKind::Key; Create.location = Location; Create.existedBefore = false; Create.existsAfter = true;
                CreateKeys.append(Create); MissingKeys.insert(Location.Id());
            }
        } else if (!Snapshot || Snapshot.value.info.link) { Result.error_ = Snapshot.error ? Snapshot.error : ERROR_CANT_ACCESS_FILE; return Result; }
        std::optional<RegistryValue> Current;
        for (const auto& Raw : Snapshot.value.values) if (Raw.name.compare(Change.name_, Qt::CaseInsensitive) == 0) { Current = Raw; break; }
        if (Current != Step.before) { Result.error_ = ERROR_REVISION_MISMATCH; return Result; }
        Result.plan_.steps.append(std::move(Step));
    }
    // Destination values are written and verified before source deletions.
    std::stable_partition(Result.plan_.steps.begin(), Result.plan_.steps.end(), [](const RegistryStep& Step) { return Step.after.has_value(); });
    Result.plan_.steps = CreateKeys + Result.plan_.steps;
    return Result;
}
bool EnvironmentStorage::allowed(const RegistryStep& Step) const
{
    if (Step.kind == StepKind::Key) return Step.location == EnvironmentLocation(Scope::User, callerSid_)
        && Step.name.isEmpty() && !Step.before && !Step.after && Step.existedBefore != Step.existsAfter
        && Step.treeBackup.isEmpty() && Step.treeHash.isEmpty();
    if ((Step.kind != StepKind::Value && Step.kind != StepKind::ObserveValue) || (Step.kind == StepKind::ObserveValue && Step.before != Step.after) || !Step.existedBefore || !Step.existsAfter || Step.name.isEmpty()) return false;
    const auto Source = Step.location.hive == Hive::LocalMachine ? Scope::System : Scope::User;
    if (Step.location != EnvironmentLocation(Source, callerSid_)) return false;
    for (const auto& Raw : {Step.before, Step.after}) if (Raw && !DecodeEnvironmentValue(*Raw, Source)) return false;
    return true;
}
EnvironmentCommit EnvironmentStorage::Commit(const EnvironmentPreview& Preview) { return commit(Preview, true); }
EnvironmentCommit EnvironmentStorage::commit(const EnvironmentPreview& Preview, bool RestoreOnFailure)
{
    EnvironmentCommit Result;
    if (!Preview) { Result.applied_.error = Preview.error_; return Result; }
    const auto& Plan = Preview.plan_;
    if (Plan.callerSid != callerSid_ || Plan.machineId != machineId_ || Plan.steps.isEmpty()) { Result.applied_.error = ERROR_INVALID_PARAMETER; return Result; }
    RegistryPlan SystemPlan = Plan; SystemPlan.steps.clear();
    RegistryTransaction Local(backend_); QSet<QString> Created;
    for (const auto& Step : Plan.steps) {
        if (!allowed(Step)) { Result.applied_.error = ERROR_ACCESS_DENIED; return Result; }
        // A value following this plan's key creation has no key yet. The
        // executor still verifies it immediately before writing. Reverse key
        // deletion is similarly guarded after all owned values are removed.
        if (Step.kind == StepKind::Key && Step.existsAfter && !Step.existedBefore) Created.insert(Step.location.Id());
        const bool Deferred = (Step.kind == StepKind::Value && !Step.before && Created.contains(Step.location.Id()))
            || (Step.kind == StepKind::Key && Step.existedBefore && !Step.existsAfter);
        const auto Match = Deferred ? RegistryResult<bool>{true} : Local.Matches(Step, false);
        if (!Match || !Match.value) { Result.applied_ = {Match.error ? Match.error : ERROR_REVISION_MISMATCH, 0, bool(Match)}; return Result; }
        if (Step.location.hive == Hive::LocalMachine) SystemPlan.steps.append(Step);
    }
    const bool Elevated = !SystemPlan.steps.isEmpty();
    if (Elevated && !remote_) { Result.applied_.error = ERROR_ELEVATION_REQUIRED; return Result; }
    if (Elevated) {
        const auto Prepared = remote_->PrepareExisting(SystemPlan);
        if (!Prepared) { Result.applied_.error = Prepared.error; return Result; }
        SystemPlan = Prepared.value;
    }
    const auto Release = qScopeGuard([&] { if (Elevated) remote_->Release(SystemPlan.id); });
    auto Saved = store_.SavePrepared(Plan);
    if (!Saved) { Result.applied_.error = Saved.error; return Result; }
    Result.record_ = std::move(Saved.value); int SystemIndex = 0;
    Result.applied_ = store_.Apply(Result.record_, [&](const RegistryStep& Step, int) {
        // HKCU always runs with HC's token, including mixed scope moves.
        return Step.location.hive == Hive::LocalMachine ? remote_->Apply(SystemPlan, SystemIndex++) : Local.ApplyStep(Step);
    });
    if (RestoreOnFailure && Result.applied_.error && Result.applied_.uncertain && Result.record_.inFlight >= 0
        && Result.record_.inFlight < Plan.steps.size() && Plan.steps[Result.record_.inFlight].location.hive != Hive::LocalMachine) {
        // A synchronous caller-side API has returned, so read-back can resolve
        // its unknown outcome. Never do this for a disconnected helper which
        // could still be applying a machine-side request.
        const auto Recovered = store_.Recover(Result.record_, Local);
        if (!Recovered && Result.record_.inFlight < 0) {
            Result.applied_.uncertain = false; Result.applied_.completed = Result.record_.completed;
        }
    }
    if (RestoreOnFailure && Result.applied_.error && !Result.applied_.uncertain && Result.applied_.completed > 0) {
        // The reverse plan compares each current value with exactly what this
        // operation wrote; an intervening edit blocks restoration instead of
        // being silently overwritten. Its writes have their own durable receipt.
        if (Elevated) remote_->Release(SystemPlan.id);
        const auto Reverse = Undo(Result.record_);
        Result.recoveryError_ = Reverse.error_;
        if (Reverse) {
            const auto Recovery = commit(Reverse, false);
            Result.recoveryError_ = Recovery.applied_.error;
            Result.restored_ = !Recovery.applied_.error;
            Result.recoveryId_ = Recovery.record_.plan.id;
            if (Recovery.applied_.uncertain) Result.applied_.uncertain = true;
        }
    }
    return Result;
}
quint32 EnvironmentStorage::RecoverLocalHistory()
{
    auto Records = store_.History(); if (!Records) return Records.error;
    RegistryTransaction Transaction(backend_);
    for (auto& Record : Records.value) {
        if (Record.inFlight < 0 || Record.plan.callerSid != callerSid_ || Record.plan.machineId != machineId_) continue;
        // A previous helper can still be finishing a remote request after a
        // timeout. Only caller-side writes are safe to resolve by local read-back.
        if (!std::all_of(Record.plan.steps.begin(), Record.plan.steps.end(), [&](const auto& Step) {
            return allowed(Step) && Step.location == EnvironmentLocation(Scope::User, callerSid_);
        })) continue;
        if (const auto Error = store_.Recover(Record, Transaction)) return Error;
    }
    return 0;
}
EnvironmentPreview EnvironmentStorage::Undo(const RegistryHistoryRecord& Record)
{
    RegistryTransaction Transaction(backend_); const auto Plan = store_.PrepareUndo(Record, Transaction);
    return {Plan.value, Plan.error};
}
EnvironmentPreview EnvironmentStorage::Redo(const RegistryHistoryRecord& Original, const RegistryHistoryRecord& UndoRecord)
{
    RegistryTransaction Transaction(backend_); const auto Plan = store_.PrepareRedo(Original, UndoRecord, Transaction);
    return {Plan.value, Plan.error};
}
}
