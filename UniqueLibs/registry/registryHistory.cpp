#include "stdafx.h"
#include "registryHistory.hpp"
#include "commonLibs/system/registryLocalPolicy.hpp"
#include "commonLibs/system/protectedData.hpp"
#include "commonLibs/system/environmentTypes.hpp"

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
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLockFile>
#include <QLoggingCategory>
#include <QSaveFile>
#include <QUuid>
#include <QScopeGuard>
#include <io.h>
#include <algorithm>

DECLARE_CMNLIBSV2_NAMESPACE
Q_LOGGING_CATEGORY(lcRegistryHistory, "hc.registry.history")

namespace hc::registry {
namespace {
constexpr qsizetype PlanLimit = 256 * 1024 * 1024;
bool Missing(quint32 Error) { return Error == ERROR_FILE_NOT_FOUND || Error == ERROR_PATH_NOT_FOUND; }
bool SameValue(const RegistryValue& A, const RegistryValue& B)
{
    return A.name.compare(B.name, Qt::CaseInsensitive) == 0 && A.type == B.type && A.bytes == B.bytes;
}
bool ValidId(const QString& Id) { return Id.size() == 36 && !QUuid(Id).isNull() && !Id.contains('/') && !Id.contains('\\'); }
bool SensitivePlan(const RegistryPlan& Plan)
{
    return std::any_of(Plan.steps.begin(), Plan.steps.end(), [](const auto& Step) {
        return Step.sensitive || hc::environment::SensitiveName(Step.name.toStdWString());
    });
}
QByteArray JsonBytes(const QJsonObject& Object) { return QJsonDocument(Object).toJson(QJsonDocument::Compact); }
QByteArray Digest(const QByteArray& Bytes) { return QCryptographicHash::hash(Bytes, QCryptographicHash::Sha256).toHex(); }
RegistryPlan NewPlan(const QString& Caller, const QString& Machine)
{
    RegistryPlan Plan; Plan.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    Plan.createdUtc = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    Plan.callerSid = Caller; Plan.machineId = Machine; return Plan;
}
quint32 WriteDurable(const QString& Path, const QByteArray& Bytes)
{
    QSaveFile File(Path); File.setDirectWriteFallback(false);
    if (!File.open(QIODevice::WriteOnly) || File.write(Bytes) != Bytes.size() || !File.flush()) return ERROR_WRITE_FAULT;
    const auto Native = _get_osfhandle(File.handle());
    if (Native == -1 || !FlushFileBuffers(reinterpret_cast<HANDLE>(Native)) || !File.commit()) return ERROR_WRITE_FAULT;
    return 0;
}
QJsonObject StepJson(const RegistryStep& Step)
{
    QJsonArray Checks;
    for (const auto& Check : Step.policyChecks) {
        const auto JsonValue = [](const auto& Value) { return Value ? QJsonValue(RegistryValueJson(*Value)) : QJsonValue(QJsonValue::Null); };
        Checks.append(QJsonObject{{"location", RegistryLocationJson(Check.location)}, {"name", Check.name},
            {"observed", JsonValue(Check.observed)}, {"before", JsonValue(Check.before)}, {"after", JsonValue(Check.after)}});
    }
    QJsonObject Result{{"policyPhase", Step.policyPhase}, {"policyChecks", Checks}, {"kind", int(Step.kind)}, {"location", RegistryLocationJson(Step.location)}, {"name", Step.name},
        {"before", Step.before ? QJsonValue(RegistryValueJson(*Step.before)) : QJsonValue(QJsonValue::Null)},
        {"after", Step.after ? QJsonValue(RegistryValueJson(*Step.after)) : QJsonValue(QJsonValue::Null)},
        {"existedBefore", Step.existedBefore}, {"existsAfter", Step.existsAfter}, {"treeHash", QString::fromLatin1(Step.treeHash)}, {"treeBackup", Step.treeBackup}};
    // Omit false to preserve hashes of existing version-1 receipts.
    if (Step.sensitive) Result["sensitive"] = true;
    return Result;
}
RegistryResult<RegistryHistoryRecord> ReadRecord(const QString& Directory)
{
    RegistryResult<RegistryHistoryRecord> Result;
    QFile File(QDir(Directory).filePath("manifest.json"));
    if (!File.open(QIODevice::ReadOnly) || File.size() > PlanLimit) { Result.error = ERROR_READ_FAULT; return Result; }
    QJsonParseError Parse; auto Object = QJsonDocument::fromJson(File.readAll(), &Parse).object();
    if (Parse.error == QJsonParseError::NoError && Object["protection"].toString() == "dpapi-user-v1") {
        const auto Plain = hc::system::RevealUserData(QByteArray::fromBase64(Object["payload"].toString().toLatin1()));
        if (Plain.isEmpty()) { Result.error = ERROR_ACCESS_DENIED; return Result; }
        Object = QJsonDocument::fromJson(Plain, &Parse).object();
    }
    if (Parse.error != QJsonParseError::NoError || Object["version"].toInt() != 1) { Result.error = ERROR_INVALID_DATA; return Result; }
    auto Plan = RegistryPlan::FromJson(Object["plan"].toObject());
    if (!Plan || Plan.value.Hash() != Object["planHash"].toString().toLatin1()) { Result.error = ERROR_INVALID_DATA; return Result; }
    Result.value.plan = std::move(Plan.value); Result.value.state = Object["state"].toString();
    Result.value.completed = Object["completed"].toInt(-1); Result.value.inFlight = Object["inFlight"].toInt(-2);
    Result.value.error = quint32(Object["error"].toInteger()); Result.value.redoAllowed = Object["redoAllowed"].toBool();
    Result.value.sequence = Object["sequence"].toString().toULongLong();
    Result.value.directory = Directory;
    if (Result.value.completed < 0 || Result.value.completed > Result.value.plan.steps.size()
        || Result.value.inFlight < -1 || Result.value.inFlight >= Result.value.plan.steps.size()) Result.error = ERROR_INVALID_DATA;
    return Result;
}
QList<RegistryOperation> ExportSteps(const QList<RegistryStep>& Steps, bool Before)
{
    QList<RegistryOperation> Result;
    for (int Offset = 0; Offset < Steps.size(); ++Offset) {
        const auto& Step = Steps[Before ? Steps.size() - 1 - Offset : Offset];
        if (Step.kind == StepKind::ObserveValue) continue;
        if (Step.kind == StepKind::PolicyFile) {
            // Forensic raw-value backup is separate from policy undo semantics.
            // Replaying this .reg is never used to restore GPO-managed values.
            if (Step.policyPhase == 0) for (const auto& Check : Step.policyChecks) {
                const auto Value = Before ? Check.observed : Check.after;
                Result.append({OperationKind::CreateKey, Check.location});
                Result.append({Value ? OperationKind::SetValue : OperationKind::DeleteValue, Check.location, Value.value_or(RegistryValue{Check.name})});
            }
            continue;
        }
        const auto Value = Before ? Step.before : Step.after;
        const bool Exists = Before ? Step.existedBefore : Step.existsAfter;
        if (Step.kind == StepKind::Value) {
            Result.append({OperationKind::CreateKey, Step.location});
            Result.append({Value ? OperationKind::SetValue : OperationKind::DeleteValue, Step.location, Value.value_or(RegistryValue{Step.name})});
        } else if (Step.kind == StepKind::RenameKey) {
            const auto Old = Before ? Step.location.Parent()->Child(Step.name) : Step.location;
            const auto Restored = Before ? Step.location : Step.location.Parent()->Child(Step.name);
            Result.append({OperationKind::DeleteKey, Old});
            for (const auto& Entry : Step.treeBackup) {
                const auto Snapshot = RegistrySnapshotFromJson(Entry.toObject());
                if (!Snapshot) continue;
                auto Location = Restored; Location.key += Snapshot.value.info.name;
                Result.append({OperationKind::CreateKey, Location});
                for (const auto& Item : Snapshot.value.values) Result.append({OperationKind::SetValue, Location, Item});
            }
        } else {
            Result.append({Exists ? OperationKind::CreateKey : OperationKind::DeleteKey, Step.location});
            if (Value) Result.append({OperationKind::SetValue, Step.location, *Value});
        }
    }
    return Result;
}
}

QJsonObject RegistryLocationJson(const RegistryLocation& Location)
{
    return {{"path", Location.Path()}, {"view", int(Location.view)}, {"sid", Location.sid}, {"origin", int(Location.origin)}};
}
RegistryResult<RegistryLocation> RegistryLocationFromJson(const QJsonObject& Object)
{
    RegistryResult<RegistryLocation> Result;
    const int SelectedView = Object["view"].toInt(-1), Source = Object["origin"].toInt(-1);
    const auto Parsed = RegistryLocation::Parse(Object["path"].toString(), View(SelectedView));
    const QString Sid = Object["sid"].toString();
    if (!Parsed || SelectedView < 0 || SelectedView > 1 || Source < 0 || Source > 3
        || Sid.size() > 184 || Sid.contains('\\') || Sid.contains(QChar(0)) || (!Sid.isEmpty() && !Sid.startsWith("S-1-"))) {
        Result.error = ERROR_INVALID_PARAMETER; return Result;
    }
    Result.value = *Parsed; Result.value.sid = Sid; Result.value.origin = Origin(Source); return Result;
}
QJsonObject RegistryValueJson(const RegistryValue& Value)
{
    return {{"name", Value.name}, {"type", qint64(Value.type)}, {"bytes", QString::fromLatin1(Value.bytes.toBase64())}, {"sha256", QString::fromLatin1(Digest(Value.bytes))}};
}
RegistryResult<RegistryValue> RegistryValueFromJson(const QJsonObject& Object)
{
    RegistryResult<RegistryValue> Result;
    const auto Encoded = Object["bytes"].toString().toLatin1(); const auto Type = Object["type"].toInteger(-1);
    if (!WinRegistryBackend::ValidName(Object["name"].toString(), false) || Type < 0 || Type > 0xffffffffLL
        || Encoded.size() > WinRegistryBackend::MaxValueBytes * 4 / 3 + 4) { Result.error = ERROR_INVALID_DATA; return Result; }
    const auto Decoded = QByteArray::fromBase64Encoding(Encoded, QByteArray::AbortOnBase64DecodingErrors);
    if (!Decoded || Decoded.decoded.size() > WinRegistryBackend::MaxValueBytes || Digest(Decoded.decoded) != Object["sha256"].toString().toLatin1()) {
        Result.error = ERROR_INVALID_DATA; return Result;
    }
    Result.value = {Object["name"].toString(), quint32(Type), Decoded.decoded}; return Result;
}
QJsonObject RegistrySnapshotJson(const KeySnapshot& Snapshot)
{
    QJsonArray Keys, Values;
    for (const auto& Key : Snapshot.keys) Keys.append(QJsonObject{{"name", Key.name}, {"written", QString::number(Key.lastWrite)}, {"keys", int(Key.subkeyCount)}, {"values", int(Key.valueCount)}, {"link", Key.link}});
    for (const auto& Value : Snapshot.values) Values.append(RegistryValueJson(Value));
    return {{"name", Snapshot.info.name}, {"written", QString::number(Snapshot.info.lastWrite)}, {"link", Snapshot.info.link}, {"keys", Keys}, {"values", Values}};
}
RegistryResult<KeySnapshot> RegistrySnapshotFromJson(const QJsonObject& Object)
{
    RegistryResult<KeySnapshot> Result;
    if (!Object["keys"].isArray() || !Object["values"].isArray()) { Result.error = ERROR_INVALID_DATA; return Result; }
    Result.value.info.name = Object["name"].toString(); Result.value.info.lastWrite = Object["written"].toString().toULongLong(); Result.value.info.link = Object["link"].toBool();
    for (const auto& Item : Object["keys"].toArray()) {
        const auto Key = Item.toObject(); const auto Name = Key["name"].toString();
        if (!WinRegistryBackend::ValidName(Name, true)) { Result.error = ERROR_INVALID_DATA; return Result; }
        Result.value.keys.append({Name, Key["written"].toString().toULongLong(), quint32(Key["keys"].toInteger()), quint32(Key["values"].toInteger()), Key["link"].toBool()});
    }
    for (const auto& Item : Object["values"].toArray()) {
        auto Value = RegistryValueFromJson(Item.toObject()); if (!Value) { Result.error = Value.error; return Result; }
        Result.value.values.append(std::move(Value.value));
    }
    Result.value.info.subkeyCount = quint32(Result.value.keys.size()); Result.value.info.valueCount = quint32(Result.value.values.size()); return Result;
}
QJsonObject RegistryPlan::ToJson() const
{
    QJsonArray Steps; for (const auto& Step : steps) Steps.append(StepJson(Step));
    return {{"version", 1}, {"id", id}, {"utc", createdUtc}, {"callerSid", callerSid}, {"machineId", machineId}, {"purpose", purpose}, {"parentId", parentId}, {"steps", Steps}};
}
QByteArray RegistryPlan::Hash() const { return Digest(JsonBytes(ToJson())); }
RegistryResult<RegistryPlan> RegistryPlan::FromJson(const QJsonObject& Object)
{
    RegistryResult<RegistryPlan> Result; auto& Plan = Result.value;
    Plan.id = Object["id"].toString(); Plan.createdUtc = Object["utc"].toString(); Plan.callerSid = Object["callerSid"].toString(); Plan.machineId = Object["machineId"].toString();
    Plan.purpose = Object["purpose"].toString(); Plan.parentId = Object["parentId"].toString();
    if (Object["version"].toInt() != 1 || !ValidId(Plan.id) || !Object["steps"].isArray()
        || Object["steps"].toArray().size() > RegFileParser::MaxOperations || JsonBytes(Object).size() > PlanLimit
        || (!Plan.parentId.isEmpty() && !ValidId(Plan.parentId))
        || (Plan.purpose != "edit" && Plan.purpose != "undo" && Plan.purpose != "redo")
        || !QDateTime::fromString(Plan.createdUtc, Qt::ISODateWithMs).isValid()) { Result.error = ERROR_INVALID_DATA; return Result; }
    for (const auto& Entry : Object["steps"].toArray()) {
        const auto Item = Entry.toObject(); RegistryStep Step;
        const auto Location = RegistryLocationFromJson(Item["location"].toObject()); const int Kind = Item["kind"].toInt(-1);
        if (!Location || Kind < 0 || Kind > 5 || !Item["existedBefore"].isBool() || !Item["existsAfter"].isBool()) { Result.error = ERROR_INVALID_DATA; return Result; }
        Step.location = Location.value; Step.kind = StepKind(Kind); Step.name = Item["name"].toString();
        Step.existedBefore = Item["existedBefore"].toBool(); Step.existsAfter = Item["existsAfter"].toBool(); Step.treeHash = Item["treeHash"].toString().toLatin1();
        Step.treeBackup = Item["treeBackup"].toArray();
        if (Item.contains("sensitive") && !Item["sensitive"].isBool()) return {{}, ERROR_INVALID_DATA};
        Step.sensitive = Item["sensitive"].toBool();
        for (const auto& Pair : {std::pair{QString("before"), &Step.before}, std::pair{QString("after"), &Step.after}}) {
            if (Item[Pair.first].isNull()) continue;
            auto Value = RegistryValueFromJson(Item[Pair.first].toObject());
            if (!Value) { Result.error = Value.error; return Result; } *Pair.second = std::move(Value.value);
        }
        if (Step.kind == StepKind::PolicyFile) {
            if (!Step.location.key.isEmpty() || (Step.location.hive != Hive::LocalMachine && Step.location.hive != Hive::CurrentUser)
                || Step.name != "Registry.pol" || !Step.before || !Step.after
                || Step.before->name != Step.name || Step.after->name != Step.name
                || Step.before->type != REG_BINARY || Step.after->type != REG_BINARY
                || !Step.existedBefore || !Step.existsAfter || !Step.treeBackup.isEmpty() || !Step.treeHash.isEmpty()
                || !LocalPolicyStore::Canonical(Step.before->bytes) || !LocalPolicyStore::Canonical(Step.after->bytes)) {
                Result.error = ERROR_INVALID_DATA; return Result;
            }
            Step.policyPhase = Item["policyPhase"].toInt(-1);
            if (Step.policyPhase < 0 || Step.policyPhase > 2 || !Item["policyChecks"].isArray()) return {{}, ERROR_INVALID_DATA};
            QSet<QString> Seen;
            for (const auto& Entry : Item["policyChecks"].toArray()) {
                const auto Object = Entry.toObject(); RegistryPolicyValueCheck Check;
                const auto Location = RegistryLocationFromJson(Object["location"].toObject());
                Check.name = Object["name"].toString();
                if (!Location || Location.value.hive != Step.location.hive || Location.value.sid != Step.location.sid
                    || Location.value.key.isEmpty() || !WinRegistryBackend::ValidName(Check.name, false)
                    || Check.name.startsWith("**") || Location.value.view != View::Native64) return {{}, ERROR_INVALID_DATA};
                Check.location = Location.value;
                const auto Id = Check.location.Id() + QChar(0) + Check.name.toCaseFolded();
                if (Seen.contains(Id)) return {{}, ERROR_INVALID_DATA}; Seen.insert(Id);
                for (const auto& Pair : {std::pair{QString("observed"), &Check.observed}, std::pair{QString("before"), &Check.before}, std::pair{QString("after"), &Check.after}}) {
                    if (Object[Pair.first].isNull()) continue;
                    const auto Value = RegistryValueFromJson(Object[Pair.first].toObject());
                    if (!Value || Value.value.name.compare(Check.name, Qt::CaseInsensitive) != 0 || Value.value.type == REG_LINK) return {{}, ERROR_INVALID_DATA};
                    *Pair.second = Value.value;
                }
                Step.policyChecks.append(std::move(Check));
            }
            Plan.steps.append(std::move(Step)); continue;
        }
        if ((Step.kind != StepKind::Value && Step.location.key.isEmpty()) || !WinRegistryBackend::ValidName(Step.name, Step.kind == StepKind::RenameKey)
            || (Step.kind == StepKind::RenameKey && (Step.treeHash.size() != 64 || QByteArray::fromHex(Step.treeHash).size() != 32))) { Result.error = ERROR_INVALID_DATA; return Result; }
        if ((Step.kind == StepKind::Value || Step.kind == StepKind::ObserveValue) && ((!Step.existedBefore || !Step.existsAfter)
            || (Step.before && Step.before->name.compare(Step.name, Qt::CaseInsensitive))
            || (Step.after && (Step.after->name.compare(Step.name, Qt::CaseInsensitive) || Step.after->type == REG_LINK)))) { Result.error = ERROR_INVALID_DATA; return Result; }
        if (Step.kind == StepKind::ObserveValue && Step.before != Step.after) return {{}, ERROR_INVALID_DATA};
        if (Step.kind == StepKind::Key && (Step.before || Step.after || Step.existedBefore == Step.existsAfter)) { Result.error = ERROR_INVALID_DATA; return Result; }
        if (Step.kind == StepKind::Link && (Step.existedBefore == Step.existsAfter || bool(Step.before) != Step.existedBefore || bool(Step.after) != Step.existsAfter
            || (Step.before && (Step.before->type != REG_LINK || Step.before->name.compare("SymbolicLinkValue", Qt::CaseInsensitive)))
            || (Step.after && (Step.after->type != REG_LINK || Step.after->name.compare("SymbolicLinkValue", Qt::CaseInsensitive))))) { Result.error = ERROR_INVALID_DATA; return Result; }
        if (Step.kind == StepKind::RenameKey) {
            QCryptographicHash Hash(QCryptographicHash::Sha256);
            if (Step.treeBackup.isEmpty()) { Result.error = ERROR_INVALID_DATA; return Result; }
            for (const auto& Entry : Step.treeBackup) {
                const auto Snapshot = RegistrySnapshotFromJson(Entry.toObject());
                if (!Snapshot || (!Snapshot.value.info.name.isEmpty() && !Snapshot.value.info.name.startsWith('\\'))) { Result.error = ERROR_INVALID_DATA; return Result; }
                Hash.addData(JsonBytes(Entry.toObject()));
            }
            if (Hash.result().toHex() != Step.treeHash) { Result.error = ERROR_INVALID_DATA; return Result; }
        }
        Plan.steps.append(std::move(Step));
    }
    const bool Policy = std::any_of(Plan.steps.cbegin(), Plan.steps.cend(), [](const auto& Step) { return Step.kind == StepKind::PolicyFile; });
    if (Policy) {
        if (Plan.steps.size() != 3) return {{}, ERROR_INVALID_DATA};
        auto Base = StepJson(Plan.steps[0]); Base.remove("policyPhase");
        for (int Phase = 0; Phase < 3; ++Phase) {
            const auto& Step = Plan.steps[Phase]; auto Json = StepJson(Step); Json.remove("policyPhase");
            if (Step.kind != StepKind::PolicyFile || Step.policyPhase != Phase || Json != Base) return {{}, ERROR_INVALID_DATA};
        }
    }
    return Result;
}

RegistryResult<RegistryPlan> RegistryTransaction::Prepare(const QList<RegistryOperation>& Operations, const QString& CallerSid,
    const QString& MachineId, const std::atomic_bool* Cancel, std::function<void(int, int, int)> StepRanges)
{
    RegistryResult<RegistryPlan> Result; Result.value = NewPlan(CallerSid, MachineId);
    if (Operations.size() > RegFileParser::MaxOperations) { Result.error = ERROR_FILE_TOO_LARGE; return Result; }
    struct Node { bool exists = false; KeySnapshot data; };
    QHash<QString, std::shared_ptr<Node>> Cache; qsizetype Budget = 0;
    const auto Load = [&](const RegistryLocation& Location) -> std::shared_ptr<Node> {
        if (Cancel && Cancel->load()) { Result.error = ERROR_CANCELLED; return {}; }
        if (const auto Found = Cache.value(Location.Id())) return Found;
        auto Read = backend_.Enumerate(Location, Cancel);
        if (!Read && (!Missing(Read.error) || !Read.complete)) { Result.error = Read.error; return {}; }
        auto Item = std::make_shared<Node>(); Item->exists = bool(Read); Item->data = std::move(Read.value); Cache.insert(Location.Id(), Item); return Item;
    };
    const auto Append = [&](RegistryStep Step) {
        Budget += JsonBytes(StepJson(Step)).size();
        if (Budget > PlanLimit || Result.value.steps.size() >= RegFileParser::MaxOperations) { Result.error = ERROR_FILE_TOO_LARGE; return false; }
        Result.value.steps.append(std::move(Step)); return true;
    };
    std::function<bool(const RegistryLocation&, int)> Ensure = [&](const RegistryLocation& Location, int Depth) {
        if (Depth > 512) { Result.error = ERROR_STACK_OVERFLOW; return false; }
        auto Item = Load(Location); if (!Item) return false; if (Item->exists) return true;
        const auto Parent = Location.Parent(); if (!Parent) { Result.error = ERROR_PATH_NOT_FOUND; return false; }
        if (!Ensure(*Parent, Depth + 1)) return false;
        auto ParentItem = Load(*Parent); if (!ParentItem || ParentItem->data.info.link) { Result.error = ERROR_CANT_ACCESS_FILE; return false; }
        RegistryStep Step; Step.kind = StepKind::Key; Step.location = Location; Step.existedBefore = false;
        if (!Append(Step)) return false;
        Item->exists = true; Item->data = {}; Item->data.info.name = Location.key.section('\\', -1);
        ParentItem->data.keys.append({Item->data.info.name}); return true;
    };
    std::function<bool(const RegistryLocation&, int)> Remove = [&](const RegistryLocation& Location, int Depth) {
        if (Depth > 512 || Location.key.isEmpty()) { Result.error = ERROR_INVALID_PARAMETER; return false; }
        auto Item = Load(Location); if (!Item) return false; if (!Item->exists) return true;
        auto ParentItem = Load(*Location.Parent()); if (!ParentItem) return false;
        if (Item->data.info.link) {
            RegistryStep Step; Step.kind = StepKind::Link; Step.location = Location; Step.existsAfter = false;
            for (const auto& Value : Item->data.values) if (Value.type == REG_LINK && Value.name.compare("SymbolicLinkValue", Qt::CaseInsensitive) == 0) Step.before = Value;
            if (!Step.before) { Result.error = ERROR_INVALID_DATA; return false; }
            if (!Append(Step)) return false;
        } else {
            const auto Children = Item->data.keys;
            for (const auto& Child : Children) if (!Remove(Location.Child(Child.name), Depth + 1)) return false;
            for (const auto& Value : Item->data.values) {
                RegistryStep Step; Step.location = Location; Step.name = Value.name; Step.before = Value;
                if (!Append(Step)) return false;
            }
            RegistryStep Step; Step.kind = StepKind::Key; Step.location = Location; Step.existsAfter = false;
            if (!Append(Step)) return false;
        }
        Item->exists = false; Item->data = {};
        ParentItem->data.keys.removeIf([&](const KeyInfo& Key) { return Key.name.compare(Location.key.section('\\', -1), Qt::CaseInsensitive) == 0; });
        return true;
    };
    int OperationIndex = 0;
    for (const auto& Operation : Operations) {
        const int At = OperationIndex++, Begin = int(Result.value.steps.size());
        const auto Report = qScopeGuard([&] { if (StepRanges) StepRanges(At, Begin, int(Result.value.steps.size())); });
        if (Operation.kind == OperationKind::CreateKey) { if (!Ensure(Operation.location, 0)) break; continue; }
        if (Operation.kind == OperationKind::DeleteKey) { if (!Remove(Operation.location, 0)) break; continue; }
        auto Item = Load(Operation.location); if (!Item) break;
        if (!Item->exists) { Result.error = ERROR_FILE_NOT_FOUND; break; }
        if (Item->data.info.link || !WinRegistryBackend::ValidName(Operation.value.name, false) || Operation.value.type == REG_LINK) { Result.error = ERROR_INVALID_PARAMETER; break; }
        RegistryStep Step; Step.location = Operation.location; Step.name = Operation.value.name;
        for (const auto& Value : Item->data.values) if (Value.name.compare(Step.name, Qt::CaseInsensitive) == 0) { Step.before = Value; break; }
        if (Operation.kind == OperationKind::SetValue) Step.after = Operation.value;
        if ((!Step.before && !Step.after) || (Step.before && Step.after && SameValue(*Step.before, *Step.after))) continue;
        if (!Append(Step)) break;
        Item->data.values.removeIf([&](const RegistryValue& Value) { return Value.name.compare(Step.name, Qt::CaseInsensitive) == 0; });
        if (Step.after) Item->data.values.append(*Step.after);
    }
    if (Result.error) Result.value.steps.clear(); return Result;
}

RegistryResult<QByteArray> RegistryTransaction::treeHash(const RegistryLocation& Location, const std::atomic_bool* Cancel, QJsonArray* Backup)
{
    RegistryResult<QByteArray> Result; QCryptographicHash Hash(QCryptographicHash::Sha256); qsizetype Budget = 0;
    std::function<bool(const RegistryLocation&, const QString&, int)> Visit = [&](const RegistryLocation& Key, const QString& Relative, int Depth) {
        if (Depth > 512) { Result.error = ERROR_STACK_OVERFLOW; return false; }
        auto Snapshot = backend_.Enumerate(Key, Cancel); if (!Snapshot) { Result.error = Snapshot.error; return false; }
        auto& Data = Snapshot.value; Data.info.lastWrite = 0; Data.info.name = Relative;
        std::sort(Data.keys.begin(), Data.keys.end(), [](const KeyInfo& A, const KeyInfo& B) { return A.name < B.name; });
        std::sort(Data.values.begin(), Data.values.end(), [](const RegistryValue& A, const RegistryValue& B) { return A.name < B.name; });
        for (auto& Child : Data.keys) Child.lastWrite = 0;
        const auto Bytes = JsonBytes(RegistrySnapshotJson(Data)); Budget += Bytes.size();
        if (Budget > PlanLimit) { Result.error = ERROR_FILE_TOO_LARGE; return false; } Hash.addData(Bytes);
        if (Backup) Backup->append(RegistrySnapshotJson(Data));
        if (!Data.info.link) for (const auto& Child : Data.keys) if (!Visit(Key.Child(Child.name), Relative + '\\' + Child.name, Depth + 1)) return false;
        return true;
    };
    if (Visit(Location, {}, 0)) Result.value = Hash.result().toHex(); return Result;
}
RegistryResult<RegistryPlan> RegistryTransaction::PrepareRename(const RegistryLocation& Location, const QString& Name, const QString& CallerSid, const QString& MachineId)
{
    RegistryResult<RegistryPlan> Result; Result.value = NewPlan(CallerSid, MachineId);
    if (Location.key.isEmpty() || !WinRegistryBackend::ValidName(Name, true)) { Result.error = ERROR_INVALID_PARAMETER; return Result; }
    const auto Destination = Location.Parent()->Child(Name);
    auto Existing = backend_.Enumerate(Destination);
    if (Destination.Id() != Location.Id() && (Existing || !Missing(Existing.error) || !Existing.complete)) { Result.error = Existing ? ERROR_ALREADY_EXISTS : Existing.error; return Result; }
    RegistryStep Step; Step.kind = StepKind::RenameKey; Step.location = Location; Step.name = Name;
    const auto Hash = treeHash(Location, nullptr, &Step.treeBackup); if (!Hash) { Result.error = Hash.error; return Result; } Step.treeHash = Hash.value;
    if (Destination.Id() == Location.Id()) {
        if (Name == Location.key.section('\\', -1)) { Result.error = ERROR_ALREADY_EXISTS; return Result; }
        // Windows rejects a case-only RegRenameKey. Both hops must be durable
        // steps so interruption after the temporary name remains recoverable.
        const auto Temporary = Location.Parent()->Child("HyperCommanderRename-" + Result.value.id);
        const auto Check = backend_.Enumerate(Temporary);
        if (Check || !Missing(Check.error) || !Check.complete) { Result.error = Check ? ERROR_ALREADY_EXISTS : Check.error; return Result; }
        auto First = Step; First.name = Temporary.key.section('\\', -1);
        Result.value.steps.append(std::move(First));
        Step.location = Temporary;
    }
    Result.value.steps.append(std::move(Step)); return Result;
}
RegistryResult<bool> RegistryTransaction::Matches(const RegistryStep& Step, bool After)
{
    RegistryResult<bool> Result;
    if (Step.kind == StepKind::PolicyFile) {
        const auto Expected = After || Step.policyPhase > 0 ? Step.after : Step.before;
        if (!Expected) return {{}, ERROR_INVALID_DATA};
        const bool Machine = Step.location.hive == Hive::LocalMachine;
        const auto Actual = readPolicy_ ? readPolicy_(Machine) : LocalPolicyStore::Read(Machine);
        if (!Actual) return {{}, Actual.error};
        const auto Left = LocalPolicyStore::Canonical(Actual.value), Right = LocalPolicyStore::Canonical(Expected->bytes);
        if (!Left || !Right) return {{}, !Left ? Left.error : Right.error};
        if (Left.value != Right.value) return {false};
        const bool Observe = !After && Step.policyPhase == 0;
        const bool Verify = After && Step.policyPhase == 2;
        if (Observe || Verify) for (const auto& Check : Step.policyChecks) {
            const auto& Wanted = Observe ? Check.observed : Check.after;
            const auto Snapshot = backend_.Enumerate(Check.location);
            if (!Snapshot) {
                if (!Missing(Snapshot.error) || !Snapshot.complete) return {{}, Snapshot.error};
                if (Wanted) return {false};
                continue;
            }
            if (Snapshot.value.info.link) return {{}, ERROR_CANT_ACCESS_FILE};
            std::optional<RegistryValue> Found;
            for (const auto& Value : Snapshot.value.values) if (Value.name.compare(Check.name, Qt::CaseInsensitive) == 0) { Found = Value; break; }
            if (bool(Found) != bool(Wanted) || (Found && !SameValue(*Found, *Wanted))) return {false};
        }
        return {true};
    }
    if (Step.kind == StepKind::RenameKey) {
        const auto Location = After ? Step.location.Parent()->Child(Step.name) : Step.location;
        const auto Other = After ? Step.location : Step.location.Parent()->Child(Step.name);
        if (Location.Id() == Other.Id()) {
            const auto Parent = backend_.Enumerate(*Location.Parent());
            if (!Parent) { Result.error = Parent.error; return Result; }
            bool NameMatches = false;
            for (const auto& Child : Parent.value.keys) if (Child.name == Location.key.section('\\', -1)) NameMatches = true;
            if (!NameMatches) return Result;
        } else {
            const auto OtherRead = backend_.Enumerate(Other);
            if (OtherRead) return Result;
            if (!Missing(OtherRead.error) || !OtherRead.complete) { Result.error = OtherRead.error; return Result; }
        }
        const auto Hash = treeHash(Location); Result.error = Hash.error; Result.value = Hash && Hash.value == Step.treeHash; return Result;
    }
    const bool Exists = After ? Step.existsAfter : Step.existedBefore; const auto Expected = After ? Step.after : Step.before;
    auto Key = backend_.Enumerate(Step.location);
    if (!Key) { if (Missing(Key.error) && Key.complete) Result.value = !Exists; else Result.error = Key.error; return Result; }
    if (!Exists) return Result;
    if (Step.kind == StepKind::Key) { Result.value = !Key.value.info.link && Key.value.keys.isEmpty() && Key.value.values.isEmpty(); return Result; }
    if ((Step.kind == StepKind::Link) != Key.value.info.link) return Result;
    const QString Name = Step.kind == StepKind::Link ? QStringLiteral("SymbolicLinkValue") : Step.name;
    std::optional<RegistryValue> Actual;
    for (const auto& Value : Key.value.values) if (Value.name.compare(Name, Qt::CaseInsensitive) == 0) { Actual = Value; break; }
    Result.value = Expected ? Actual && SameValue(*Actual, *Expected) : !Actual; return Result;
}
RegistryApplyResult RegistryTransaction::ApplyStep(const RegistryStep& Step, bool CallerRefreshed)
{
    const auto Before = Matches(Step, false);
    if (!Before || !Before.value) return {Before.error ? Before.error : ERROR_REVISION_MISMATCH, 0, bool(Before), false};
    quint32 Error = 0;
    switch (Step.kind) {
    case StepKind::ObserveValue: break; // Read-only guard: never rewrite the autorun source.
    case StepKind::Value: Error = Step.after ? backend_.Write(Step.location, *Step.after) : backend_.DeleteValue(Step.location, Step.name); break;
    case StepKind::Key: Error = Step.existsAfter ? backend_.Create(Step.location) : backend_.DeleteKey(Step.location); break;
    case StepKind::Link: Error = Step.existsAfter && Step.after ? backend_.CreateLink(Step.location, Step.after->bytes) : backend_.DeleteKey(Step.location); break;
    case StepKind::RenameKey: Error = backend_.RenameKey(Step.location, Step.name); break;
    case StepKind::PolicyFile: {
        if (!Step.before || !Step.after) return {ERROR_INVALID_DATA};
        const bool Machine = Step.location.hive == Hive::LocalMachine;
        if (Step.policyPhase == 0) {
            const auto Applied = writePolicy_ ? writePolicy_(Machine, Step.before->bytes, Step.after->bytes)
                : LocalPolicyStore::Apply(Machine, Step.before->bytes, Step.after->bytes);
            Error = Applied.error;
        } else if (Step.policyPhase == 1 && !CallerRefreshed) Error = refreshPolicy_ ? refreshPolicy_(Machine) : LocalPolicyStore::Refresh(Machine);
        break;
    }
    }
    if (Error) return {Error, 0, false, true};
    const auto After = Matches(Step, true);
    if (!After || !After.value) return {After.error ? After.error : (Step.kind == StepKind::PolicyFile ? ERROR_REVISION_MISMATCH : ERROR_WRITE_FAULT), 0, Step.kind == StepKind::PolicyFile && bool(After), true};
    return {0, 1, false, false};
}
RegistryStep RegistryTransaction::Reverse(const RegistryStep& Step)
{
    RegistryStep Result = Step; std::swap(Result.before, Result.after); std::swap(Result.existedBefore, Result.existsAfter);
    if (Step.kind == StepKind::RenameKey) { Result.location = Step.location.Parent()->Child(Step.name); Result.name = Step.location.key.section('\\', -1); }
    return Result;
}

RegistryChangeStore::RegistryChangeStore(QString BackupDirectory) : directory_(std::move(BackupDirectory)) {}
quint32 RegistryChangeStore::save(const RegistryHistoryRecord& Record) const
{
    const QFileInfo Path(Record.directory);
    if (Path.absolutePath().compare(QDir(directory_).absolutePath(), Qt::CaseInsensitive) || Path.isSymLink()
        || !Path.fileName().endsWith('-' + Record.plan.id) || !ValidId(Record.plan.id)) return ERROR_INVALID_NAME;
    const QJsonObject Object{{"version", 1}, {"plan", Record.plan.ToJson()}, {"planHash", QString::fromLatin1(Record.plan.Hash())},
        {"state", Record.state}, {"completed", Record.completed}, {"inFlight", Record.inFlight}, {"error", qint64(Record.error)}, {"redoAllowed", Record.redoAllowed}, {"sequence", QString::number(Record.sequence)}};
    if (SensitivePlan(Record.plan)) {
        const auto Protected = hc::system::ProtectUserData(JsonBytes(Object));
        if (Protected.isEmpty()) return ERROR_ENCRYPTION_FAILED;
        return WriteDurable(QDir(Record.directory).filePath("manifest.json"), JsonBytes({{"protection", "dpapi-user-v1"}, {"payload", QString::fromLatin1(Protected.toBase64())}}));
    }
    return WriteDurable(QDir(Record.directory).filePath("manifest.json"), JsonBytes(Object));
}
RegistryResult<RegistryHistoryRecord> RegistryChangeStore::SavePrepared(const RegistryPlan& Plan)
{
    RegistryResult<RegistryHistoryRecord> Result;
    const auto Valid = RegistryPlan::FromJson(Plan.ToJson());
    if (!Valid || !QDir::isAbsolutePath(directory_) || !QDir().mkpath(directory_)) { Result.error = ERROR_INVALID_PARAMETER; return Result; }
    QLockFile Lock(QDir(directory_).filePath("writer.lock")); if (!Lock.tryLock(1000)) { Result.error = ERROR_BUSY; return Result; }
    Result.value.plan = Plan;
    Result.value.directory = QDir(directory_).filePath(QDateTime::currentDateTimeUtc().toString("yyyyMMddTHHmmsszzzZ") + '-' + Plan.id);
    if (QFileInfo::exists(Result.value.directory) || !QDir().mkdir(Result.value.directory)) { Result.error = ERROR_ALREADY_EXISTS; return Result; }
    Result.value.state = "preparing"; Result.error = save(Result.value); if (Result.error) return Result;
    const bool Protected = SensitivePlan(Plan);
    for (const bool Before : {true, false}) {
        if (Result.error) break;
        auto Bytes = RegFileWriter::Write(ExportSteps(Plan.steps, Before));
        if (Protected) { Bytes = hc::system::ProtectUserData(Bytes); if (Bytes.isEmpty()) { Result.error = ERROR_ENCRYPTION_FAILED; break; } }
        Result.error = WriteDurable(QDir(Result.value.directory).filePath(QString(Before ? "before.reg" : "after.reg") + (Protected ? ".dpapi" : "")), Bytes);
    }
    for (int I = 0; !Result.error && I < Plan.steps.size(); ++I) {
        const auto& Step = Plan.steps[I]; if (Step.kind != StepKind::PolicyFile || Step.policyPhase != 0) continue;
        Result.error = WriteDurable(QDir(Result.value.directory).filePath(QString("policy-%1-before.pol").arg(I)), Step.before->bytes);
        if (!Result.error) Result.error = WriteDurable(QDir(Result.value.directory).filePath(QString("policy-%1-after.pol").arg(I)), Step.after->bytes);
    }
    if (!Result.error) { Result.value.state = "prepared"; Result.error = save(Result.value); }
    return Result;
}
RegistryApplyResult RegistryChangeStore::Apply(RegistryHistoryRecord& Record, const StepExecutor& Execute)
{
    QLockFile Lock(QDir(directory_).filePath("writer.lock")); if (!Lock.tryLock(1000)) return {ERROR_BUSY};
    if (Record.state != "prepared" || Record.completed != 0 || Record.inFlight != -1 || !Execute) return {ERROR_INVALID_STATE};
    auto Disk = ReadRecord(Record.directory);
    if (!Disk || Disk.value.state != "prepared" || Disk.value.plan.Hash() != Record.plan.Hash()) return {ERROR_INVALID_DATA};
    for (int Index = 0; Index < Record.plan.steps.size(); ++Index) {
        Record.state = "applying"; Record.inFlight = Index;
        if (const auto Error = save(Record)) return {Error, Record.completed};
        const auto Applied = Execute(Record.plan.steps[Index], Index);
        if (Applied.error || Applied.uncertain || Applied.completed != 1) {
            Record.error = Applied.error ? Applied.error : ERROR_WRITE_FAULT;
            Record.state = Applied.uncertain ? "recovery-required" : Applied.conflict ? "conflict" : "partial";
            if (!Applied.uncertain) Record.inFlight = -1;
            const auto Error = save(Record);
            return {Error ? Error : Record.error, Record.completed, Applied.conflict, Applied.uncertain || bool(Error)};
        }
        ++Record.completed; Record.inFlight = -1;
        if (const auto Error = save(Record)) return {Error, Record.completed, false, true};
    }
    Record.state = "committed"; Record.error = 0; Record.sequence = nextSequence();
    const auto Error = save(Record); return {Error, Record.completed, false, bool(Error)};
}
RegistryResult<QList<RegistryHistoryRecord>> RegistryChangeStore::History() const
{
    RegistryResult<QList<RegistryHistoryRecord>> Result;
    for (const auto& Directory : QDir(directory_).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name | QDir::Reversed)) {
        if (Directory.isSymLink()) continue;
        auto Record = ReadRecord(Directory.absoluteFilePath());
        if (!Record) { Result.error = Record.error; Result.complete = false; continue; }
        Result.value.append(std::move(Record.value));
    }
    // Derive branches from committed records as well as persisted flags. A crash
    // after committing undo but before updating its parent cannot expose an
    // incorrect redo branch after restart. Commit sequence, not preparation time,
    // orders branches; two records may have been prepared in the opposite order.
    QList<RegistryHistoryRecord> Ordered = Result.value;
    std::sort(Ordered.begin(), Ordered.end(), [](const auto& A, const auto& B) { return A.sequence < B.sequence; });
    for (const auto& Record : Ordered) {
        if (Record.state != "committed") continue;
        if (Record.plan.purpose == "edit") {
            for (auto& Older : Result.value) if (Older.sequence < Record.sequence) Older.redoAllowed = false;
        } else {
            for (auto& Parent : Result.value) if (Parent.plan.id == Record.plan.parentId) {
                Parent.state = Record.plan.purpose == "undo" ? "undone" : "committed";
                Parent.redoAllowed = Record.plan.purpose == "undo";
            }
        }
    }
    return Result;
}
quint64 RegistryChangeStore::nextSequence() const
{
    quint64 Next = quint64(QDateTime::currentMSecsSinceEpoch()) * 1000;
    // The caller holds writer.lock. Use raw receipts so branch derivation cannot
    // affect sequence allocation, including recovery after a clock adjustment.
    for (const auto& Directory : QDir(directory_).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        if (Directory.isSymLink()) continue;
        const auto Record = ReadRecord(Directory.absoluteFilePath());
        if (Record) Next = std::max(Next, Record.value.sequence + 1);
    }
    return Next;
}
RegistryResult<RegistryHistoryRecord> RegistryChangeStore::Read(const QString& Id) const
{
    if (!ValidId(Id)) return {{}, ERROR_INVALID_PARAMETER};
    auto Records = History();
    for (auto& Record : Records.value) if (Record.plan.id == Id) return {std::move(Record)};
    return {{}, Records.error ? Records.error : ERROR_FILE_NOT_FOUND};
}
quint32 RegistryChangeStore::SetState(RegistryHistoryRecord& Record, const QString& State, bool RedoAllowed)
{
    QLockFile Lock(QDir(directory_).filePath("writer.lock")); if (!Lock.tryLock(1000)) return ERROR_BUSY;
    Record.state = State; Record.redoAllowed = RedoAllowed; return save(Record);
}
quint32 RegistryChangeStore::Recover(RegistryHistoryRecord& Record, RegistryTransaction& Transaction)
{
    QLockFile Lock(QDir(directory_).filePath("writer.lock")); if (!Lock.tryLock(1000)) return ERROR_BUSY;
    // Another panel may have loaded this record while its writer still held
    // the lock. Re-read after acquiring it, never overwrite a later commit with
    // that stale in-flight image.
    const auto Current = ReadRecord(Record.directory);
    if (!Current) return Current.error;
    if (Current.value.plan.Hash() != Record.plan.Hash()) return ERROR_REVISION_MISMATCH;
    Record = Current.value;
    if (Record.inFlight < 0) return 0;
    if (Record.inFlight >= Record.plan.steps.size() || Record.inFlight != Record.completed) return ERROR_INVALID_DATA;
    const auto& Step = Record.plan.steps[Record.inFlight];
    // A crash cannot prove that an in-flight refresh waiter observed completion.
    if (Step.kind == StepKind::PolicyFile && Step.policyPhase == 1) {
        Record.state = "partial"; Record.inFlight = -1; Record.error = ERROR_RETRY; save(Record); return ERROR_RETRY;
    }
    const auto After = Transaction.Matches(Step, true), Before = Transaction.Matches(Step, false);
    if (!After || !Before) return After.error ? After.error : Before.error;
    if (After.value) { ++Record.completed; Record.inFlight = -1; }
    else if (Before.value) Record.inFlight = -1;
    else { Record.state = "recovery-required"; Record.error = ERROR_REVISION_MISMATCH; const auto Error = save(Record); return Error ? Error : ERROR_REVISION_MISMATCH; }
    Record.state = Record.completed == Record.plan.steps.size() ? "committed" : "partial";
    if (Record.state == "committed") { Record.error = 0; if (!Record.sequence) Record.sequence = nextSequence(); }
    return save(Record);
}
RegistryResult<RegistryPlan> RegistryChangeStore::PrepareUndo(const RegistryHistoryRecord& Record, RegistryTransaction&) const
{
    RegistryResult<RegistryPlan> Result;
    if (Record.inFlight >= 0 || Record.completed <= 0 || Record.completed > Record.plan.steps.size()
        || (Record.state != "committed" && Record.state != "partial" && Record.state != "conflict")) { Result.error = ERROR_INVALID_STATE; return Result; }
    Result.value = NewPlan(Record.plan.callerSid, Record.plan.machineId); Result.value.parentId = Record.plan.id; Result.value.purpose = "undo";
    if (!Record.plan.steps.isEmpty() && Record.plan.steps[0].kind == StepKind::PolicyFile) {
        auto Step = Record.plan.steps[0]; std::swap(Step.before, Step.after);
        for (auto& Check : Step.policyChecks) {
            Check.observed = Check.after; std::swap(Check.before, Check.after);
        }
        for (int Phase = 0; Phase < 3; ++Phase) { Step.policyPhase = Phase; Result.value.steps.append(Step); }
        return Result;
    }
    // Reverse steps are checked against their recorded after-image immediately
    // before each write. A conflict stops at that step, preserving later changes.
    for (int Index = 0; Index < Record.completed; ++Index)
        if (Record.plan.steps[Index].kind == StepKind::ObserveValue) Result.value.steps.append(Record.plan.steps[Index]);
    for (int Index = Record.completed - 1; Index >= 0; --Index)
        if (Record.plan.steps[Index].kind != StepKind::ObserveValue) Result.value.steps.append(RegistryTransaction::Reverse(Record.plan.steps[Index]));
    return Result;
}
RegistryResult<RegistryPlan> RegistryChangeStore::PrepareRedo(const RegistryHistoryRecord& Original, const RegistryHistoryRecord& Undo, RegistryTransaction& Transaction) const
{
    const auto Current = Read(Original.plan.id);
    if (!Current || Current.value.state != "undone" || !Current.value.redoAllowed || Undo.state != "committed"
        || Undo.plan.purpose != "undo" || Undo.plan.parentId != Original.plan.id) return {{}, ERROR_INVALID_STATE};
    auto Result = PrepareUndo(Undo, Transaction);
    if (Result) { Result.value.purpose = "redo"; Result.value.parentId = Original.plan.id; }
    return Result;
}
} // namespace hc::registry
