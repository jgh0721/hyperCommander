#include "stdafx.h"
#include "registryProtocol.hpp"
#include "commonLibs/system/registryLocalPolicy.hpp"

/// 모듈
/// UIs
/// 기타 헤더파일
/// 고유 라이브러리
/// 공통 라이브러리
/// 외부 라이브러리
/// 선언 헤더파일들
/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QCryptographicHash>
#include <QDateTime>
#include <QJsonDocument>
#include <QLoggingCategory>
#include <QUuid>

DECLARE_CMNLIBSV2_NAMESPACE
Q_LOGGING_CATEGORY(lcRegistryProtocol, "hc.registry.protocol")

namespace hc::registry {
namespace {
constexpr qsizetype ChunkBytes = 24 * 1024, TransferLimit = 256 * 1024 * 1024, MemoryLimit = 512 * 1024 * 1024;
QByteArray Digest(const QByteArray& Bytes) { return QCryptographicHash::hash(Bytes, QCryptographicHash::Sha256).toHex(); }
QJsonObject Reply(quint32 Error = 0) { return {{"outcome", Error ? "Failed" : "Completed"}, {"nativeCode", QString::number(Error)}}; }
quint32 Code(const QJsonObject& Object)
{
    bool Valid = false; const auto Error = Object["nativeCode"].toString().toUInt(&Valid);
    return Valid ? Error : ERROR_INVALID_DATA;
}
qint64 Now() { return QDateTime::currentMSecsSinceEpoch(); }
}
RegistrySessionServer::RegistrySessionServer(IRegistryBackend& Backend, QString CallerSid, QString MachineId, QString TestRoot,
    RegistryTransaction::PolicyReader ReadPolicy, RegistryTransaction::PolicyWriter WritePolicy)
    : backend_(Backend), transaction_(Backend, std::move(ReadPolicy), std::move(WritePolicy)), callerSid_(std::move(CallerSid)), machineId_(std::move(MachineId)), testRoot_(std::move(TestRoot)) {}
RegistryResult<RegistryLocation> RegistrySessionServer::bind(const QJsonObject& Object) const
{
    auto Result = RegistryLocationFromJson(Object); if (!Result) return Result;
    if (Result.value.hive == Hive::ClassesRoot) { Result.error = ERROR_INVALID_PARAMETER; return Result; }
    if (Result.value.hive == Hive::CurrentUser) {
        if (callerSid_.isEmpty()) { Result.error = ERROR_ACCESS_DENIED; return Result; }
        Result.value.sid = callerSid_;
    }
    if (!testRoot_.isEmpty() && Result.value.Path().compare(testRoot_, Qt::CaseInsensitive)
        && !Result.value.Path().startsWith(testRoot_ + '\\', Qt::CaseInsensitive)) Result.error = ERROR_ACCESS_DENIED;
    return Result;
}
qsizetype RegistrySessionServer::memory() const
{
    qsizetype Bytes = 0;
    for (const auto& Blob : blobs_) Bytes += Blob.expected;
    for (const auto& Plan : prepared_) Bytes += Plan.bytes;
    return Bytes;
}
void RegistrySessionServer::prune()
{
    const auto Cutoff = Now() - 15 * 60 * 1000;
    blobs_.removeIf([&](const auto& Item) { return Item.value().touched < Cutoff; });
    prepared_.removeIf([&](const auto& Item) { return Item.value().touched < Cutoff; });
}
QJsonObject RegistrySessionServer::publish(const QJsonObject& Object)
{
    const auto Bytes = QJsonDocument(Object).toJson(QJsonDocument::Compact);
    if (Bytes.size() > TransferLimit || memory() + Bytes.size() > MemoryLimit || blobs_.size() >= 32) return Reply(ERROR_NOT_ENOUGH_MEMORY);
    const QString Id = QUuid::createUuid().toString(QUuid::WithoutBraces); const auto Hash = Digest(Bytes);
    blobs_.insert(Id, {Bytes, Hash, Bytes.size(), false, Now()});
    auto Result = Reply(); Result["blobId"] = Id; Result["bytes"] = qint64(Bytes.size()); Result["sha256"] = QString::fromLatin1(Hash); return Result;
}
QJsonObject RegistrySessionServer::Handle(const QString& Kind, const QJsonObject& Request)
{
    prune();
    if (Kind == "registry.upload.begin") {
        const auto Size = Request["bytes"].toInteger(-1); const auto Hash = Request["sha256"].toString().toLatin1();
        if (Size < 0 || Size > TransferLimit || Hash.size() != 64 || QByteArray::fromHex(Hash).size() != 32) return Reply(ERROR_INVALID_PARAMETER);
        if (memory() + Size > MemoryLimit || blobs_.size() >= 32) return Reply(ERROR_NOT_ENOUGH_MEMORY);
        const auto Id = QUuid::createUuid().toString(QUuid::WithoutBraces); blobs_.insert(Id, {{}, Hash, Size, true, Now()});
        auto Result = Reply(); Result["blobId"] = Id; return Result;
    }
    if (Kind == "registry.upload.chunk" || Kind == "registry.download.chunk") {
        auto Found = blobs_.find(Request["blobId"].toString());
        if (Found == blobs_.end()) return Reply(ERROR_NOT_FOUND);
        const auto Offset = Request["offset"].toInteger(-1); Found->touched = Now();
        if (Offset < 0 || Offset > Found->bytes.size()) return Reply(ERROR_INVALID_PARAMETER);
        if (Kind == "registry.upload.chunk") {
            const auto Text = Request["data"].toString().toLatin1();
            if (!Found->upload || Text.size() > ChunkBytes * 4 / 3 + 4) return Reply(ERROR_INVALID_PARAMETER);
            const auto Data = QByteArray::fromBase64Encoding(Text, QByteArray::AbortOnBase64DecodingErrors);
            if (!Data || Data.decoded.size() > ChunkBytes || Offset + Data.decoded.size() > Found->expected) return Reply(ERROR_INVALID_DATA);
            if (Offset < Found->bytes.size()) return Reply(Found->bytes.mid(Offset, Data.decoded.size()) == Data.decoded ? 0 : ERROR_INVALID_DATA);
            Found->bytes += Data.decoded; return Reply();
        }
        if (Found->upload) return Reply(ERROR_INVALID_PARAMETER);
        auto Result = Reply(); Result["data"] = QString::fromLatin1(Found->bytes.mid(Offset, ChunkBytes).toBase64()); return Result;
    }
    if (Kind == "registry.release") { blobs_.remove(Request["blobId"].toString()); prepared_.remove(Request["operationId"].toString()); return Reply(); }
    if (Kind == "registry.read") {
        const auto Location = bind(Request["location"].toObject()); if (!Location) return Reply(Location.error);
        const auto Snapshot = backend_.Enumerate(Location.value); if (!Snapshot) return Reply(Snapshot.error);
        return publish(RegistrySnapshotJson(Snapshot.value));
    }
    if (Kind == "registry.prepare") {
        auto Found = blobs_.find(Request["blobId"].toString());
        if (Found == blobs_.end() || !Found->upload || Found->bytes.size() != Found->expected || Digest(Found->bytes) != Found->hash) return Reply(ERROR_INVALID_DATA);
        QJsonParseError Parse; const auto Object = QJsonDocument::fromJson(Found->bytes, &Parse).object(); blobs_.erase(Found);
        if (Parse.error != QJsonParseError::NoError) return Reply(ERROR_INVALID_DATA);
        RegistryResult<RegistryPlan> Plan;
        if (Object["format"] == "operations") {
            if (!Object["operations"].isArray() || Object["operations"].toArray().size() > RegFileParser::MaxOperations) return Reply(ERROR_INVALID_DATA);
            QList<RegistryOperation> Operations;
            for (const auto& Entry : Object["operations"].toArray()) {
                const auto Item = Entry.toObject(); const int Type = Item["kind"].toInt(-1);
                const auto Location = bind(Item["location"].toObject());
                if (!Location || Type < 0 || Type > 3) return Reply(ERROR_INVALID_PARAMETER);
                RegistryOperation Operation; Operation.kind = OperationKind(Type); Operation.location = Location.value;
                if (Operation.kind == OperationKind::SetValue || Operation.kind == OperationKind::DeleteValue) {
                    const auto Value = RegistryValueFromJson(Item["value"].toObject()); if (!Value) return Reply(Value.error); Operation.value = Value.value;
                }
                Operations.append(std::move(Operation));
            }
            Plan = transaction_.Prepare(Operations, callerSid_, machineId_);
        } else if (Object["format"] == "rename") {
            const auto Location = bind(Object["location"].toObject()); if (!Location) return Reply(Location.error);
            Plan = transaction_.PrepareRename(Location.value, Object["name"].toString(), callerSid_, machineId_);
        } else if (Object["format"] == "plan") {
            Plan = RegistryPlan::FromJson(Object["plan"].toObject());
            if (Plan) {
                if (Plan.value.callerSid != callerSid_ || Plan.value.machineId != machineId_) return Reply(ERROR_ACCESS_DENIED);
                for (auto& Step : Plan.value.steps) {
                    const auto Location = bind(RegistryLocationJson(Step.location)); if (!Location) return Reply(Location.error); Step.location = Location.value;
                    for (auto& Check : Step.policyChecks) {
                        const auto Target = bind(RegistryLocationJson(Check.location)); if (!Target) return Reply(Target.error); Check.location = Target.value;
                    }
                }
            }
        } else return Reply(ERROR_INVALID_DATA);
        if (!Plan) return Reply(Plan.error);
        for (const auto& Step : Plan.value.steps) if (Step.kind == StepKind::PolicyFile && Step.policyPhase == 0) {
            const auto Match = transaction_.Matches(Step, false);
            if (!Match || !Match.value) return Reply(!Match ? Match.error : ERROR_REVISION_MISMATCH);
        }
        const auto Bytes = QJsonDocument(Plan.value.ToJson()).toJson(QJsonDocument::Compact).size();
        if (prepared_.size() >= 16 || memory() + Bytes * 2 > MemoryLimit) return Reply(ERROR_NOT_ENOUGH_MEMORY);
        if (prepared_.contains(Plan.value.id)) return Reply(ERROR_ALREADY_EXISTS);
        prepared_.insert(Plan.value.id, {Plan.value, 0, {}, Now(), Bytes});
        return publish(Plan.value.ToJson());
    }
    if (Kind == "registry.status" || Kind == "registry.apply") {
        auto Found = prepared_.find(Request["operationId"].toString());
        if (Found == prepared_.end()) { auto Result = Reply(ERROR_NOT_FOUND); Result["outcome"] = "Unknown"; return Result; }
        Found->touched = Now();
        if (Kind == "registry.status") {
            auto Result = Reply(); Result["next"] = Found->next; Result["hash"] = QString::fromLatin1(Found->plan.Hash()); Result["failure"] = Found->failure; return Result;
        }
        const int Index = Request["index"].toInt(-1); const auto Hash = QString::fromLatin1(Found->plan.Hash());
        if (Request["hash"] != Hash || Request["backupHash"] != Hash || Index < 0 || Index >= Found->plan.steps.size() || Index > Found->next) return Reply(ERROR_INVALID_PARAMETER);
        if (Index < Found->next) { auto Result = Reply(); Result["completed"] = 1; return Result; }
        if (!Found->failure.isEmpty()) return Found->failure;
        // Recheck both the policy file and observed values before saving .pol.
        // The later phases refresh and verify; they never write managed values.
        if (Index == 0) for (const auto& Step : Found->plan.steps) if (Step.kind == StepKind::PolicyFile && Step.policyPhase == 0) {
            const auto Match = transaction_.Matches(Step, false);
            if (!Match || !Match.value) {
                auto Result = Reply(!Match ? Match.error : ERROR_REVISION_MISMATCH);
                Result["completed"] = 0; Result["conflict"] = bool(Match); Result["uncertain"] = false;
                Found->failure = Result; return Result;
            }
        }
        const auto& Step = Found->plan.steps[Index];
        const bool Refresh = Step.kind == StepKind::PolicyFile && Step.policyPhase == 1;
        if (Refresh && !Request["callerRefreshed"].toBool()) return Reply(ERROR_INVALID_PARAMETER);
        // User refresh runs in the authenticated original caller, not the
        // potentially different account used for over-the-shoulder elevation.
        const auto Applied = transaction_.ApplyStep(Step, Refresh);
        auto Result = Reply(Applied.error); Result["completed"] = Applied.completed; Result["conflict"] = Applied.conflict; Result["uncertain"] = Applied.uncertain;
        if (Applied.error || Applied.uncertain) Found->failure = Result;
        else ++Found->next;
        return Result;
    }
    return Reply(ERROR_INVALID_FUNCTION);
}

RegistryResult<QJsonObject> RegistryRemoteSession::download(const QJsonObject& Reply)
{
    RegistryResult<QJsonObject> Result; Result.error = Code(Reply); if (Result.error) return Result;
    const auto Id = Reply["blobId"].toString(); const auto Size = Reply["bytes"].toInteger(-1);
    if (Id.isEmpty() || Size < 0 || Size > TransferLimit) { Result.error = ERROR_INVALID_DATA; return Result; }
    QByteArray Bytes;
    for (qsizetype Offset = 0; Offset < Size;) {
        const auto Chunk = send_("registry.download.chunk", {{"blobId", Id}, {"offset", qint64(Offset)}});
        Result.error = Code(Chunk); if (Result.error) break;
        const auto Data = QByteArray::fromBase64Encoding(Chunk["data"].toString().toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
        if (!Data || Data.decoded.isEmpty() || Data.decoded.size() > ChunkBytes || Offset + Data.decoded.size() > Size) { Result.error = ERROR_INVALID_DATA; break; }
        Bytes += Data.decoded; Offset += Data.decoded.size();
    }
    send_("registry.release", {{"blobId", Id}});
    if (Result.error) return Result;
    if (Digest(Bytes) != Reply["sha256"].toString().toLatin1()) { Result.error = ERROR_INVALID_DATA; return Result; }
    QJsonParseError Parse; Result.value = QJsonDocument::fromJson(Bytes, &Parse).object();
    if (Parse.error != QJsonParseError::NoError) Result.error = ERROR_INVALID_DATA; return Result;
}
RegistryResult<QJsonObject> RegistryRemoteSession::uploadAndPrepare(const QJsonObject& Object)
{
    const auto Bytes = QJsonDocument(Object).toJson(QJsonDocument::Compact);
    if (Bytes.size() > TransferLimit) return {{}, ERROR_FILE_TOO_LARGE};
    const auto Started = send_("registry.upload.begin", {{"bytes", qint64(Bytes.size())}, {"sha256", QString::fromLatin1(Digest(Bytes))}});
    if (const auto Error = Code(Started)) return {{}, Error};
    const auto Id = Started["blobId"].toString(); if (Id.isEmpty()) return {{}, ERROR_INVALID_DATA};
    for (qsizetype Offset = 0; Offset < Bytes.size(); Offset += ChunkBytes) {
        const auto Reply = send_("registry.upload.chunk", {{"blobId", Id}, {"offset", qint64(Offset)}, {"data", QString::fromLatin1(Bytes.mid(Offset, ChunkBytes).toBase64())}});
        if (const auto Error = Code(Reply)) { send_("registry.release", {{"blobId", Id}}); return {{}, Error}; }
    }
    return download(send_("registry.prepare", {{"blobId", Id}}));
}
RegistryResult<KeySnapshot> RegistryRemoteSession::Read(const RegistryLocation& Location)
{
    const auto Result = download(send_("registry.read", {{"location", RegistryLocationJson(Location)}}));
    return Result ? RegistrySnapshotFromJson(Result.value) : RegistryResult<KeySnapshot>{{}, Result.error};
}
RegistryResult<RegistryPlan> RegistryRemoteSession::Prepare(const QList<RegistryOperation>& Operations)
{
    QJsonArray Items;
    for (const auto& Operation : Operations) Items.append(QJsonObject{{"kind", int(Operation.kind)}, {"location", RegistryLocationJson(Operation.location)}, {"value", RegistryValueJson(Operation.value)}});
    const auto Result = uploadAndPrepare({{"format", "operations"}, {"operations", Items}});
    return Result ? RegistryPlan::FromJson(Result.value) : RegistryResult<RegistryPlan>{{}, Result.error};
}
RegistryResult<RegistryPlan> RegistryRemoteSession::PrepareExisting(const RegistryPlan& Plan)
{
    const auto Result = uploadAndPrepare({{"format", "plan"}, {"plan", Plan.ToJson()}});
    return Result ? RegistryPlan::FromJson(Result.value) : RegistryResult<RegistryPlan>{{}, Result.error};
}
RegistryResult<RegistryPlan> RegistryRemoteSession::PrepareRename(const RegistryLocation& Location, const QString& Name)
{
    const auto Result = uploadAndPrepare({{"format", "rename"}, {"location", RegistryLocationJson(Location)}, {"name", Name}});
    return Result ? RegistryPlan::FromJson(Result.value) : RegistryResult<RegistryPlan>{{}, Result.error};
}
RegistryApplyResult RegistryRemoteSession::Apply(const RegistryPlan& Plan, int Index)
{
    const auto Hash = QString::fromLatin1(Plan.Hash());
    if (Index < 0 || Index >= Plan.steps.size()) return {ERROR_INVALID_PARAMETER};
    const auto& Step = Plan.steps[Index];
    const bool Policy = Step.kind == StepKind::PolicyFile;
    if (Policy && progress_) progress_(Step.policyPhase);
    if (Policy && cancel_ && cancel_->load()) return {ERROR_CANCELLED, 0, false, Index > 0};
    if (Policy && Step.policyPhase == 1) {
        const auto Error = refresh_ ? refresh_(Step.location.hive == Hive::LocalMachine, cancel_)
            : LocalPolicyStore::Refresh(Step.location.hive == Hive::LocalMachine, cancel_);
        if (Error) return {Error, 0, false, true};
    }
    const auto Reply = send_("registry.apply", {{"operationId", Plan.id}, {"index", Index}, {"hash", Hash}, {"backupHash", Hash},
        {"callerRefreshed", Policy && Step.policyPhase == 1}});
    const auto Error = Code(Reply);
    return {Error, Reply["completed"].toInt(), Reply["conflict"].toBool(), Reply["uncertain"].toBool() || Reply["outcome"] == "Unknown"};
}
QJsonObject RegistryRemoteSession::Status(const QString& PlanId) { return send_("registry.status", {{"operationId", PlanId}}); }
void RegistryRemoteSession::Release(const QString& PlanId) { send_("registry.release", {{"operationId", PlanId}}); }
} // namespace hc::registry
