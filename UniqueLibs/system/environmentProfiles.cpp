#include "stdafx.h"
#include "environmentProfiles.hpp"

/// 모듈
/// UIs
/// 기타 헤더파일
/// 고유 라이브러리
/// 공통 라이브러리
#include "commonLibs/system/protectedData.hpp"
/// 외부 라이브러리
/// 선언 헤더파일들
/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QFile>
#include <QSaveFile>
#include <QLockFile>
#include <QJsonDocument>
#include <QCryptographicHash>
#include <QDir>
#include <QUuid>

DECLARE_CMNLIBSV2_NAMESPACE
Q_LOGGING_CATEGORY(lcEnvironmentProfiles, "hc.environment.profiles")
namespace hc::environment {
namespace {
QByteArray Read(const QString& Path) { QFile File(Path); if (!File.open(QIODevice::ReadOnly) || File.size() > 16 * 1024 * 1024) return {}; return File.readAll(); }
QByteArray Hash(const QByteArray& Bytes) { return QCryptographicHash::hash(Bytes, QCryptographicHash::Sha256); }
QJsonArray EncodeValues(const std::vector<Value>& Values)
{
    QJsonArray Result; for (const auto& Item : Values) Result.append(registry::RegistryValueJson(EncodeEnvironmentValue(Item)));
    return Result;
}
bool DecodeValues(const QJsonValue& Json, std::vector<Value>& Values)
{
    if (!Json.isArray() || Json.toArray().size() > 4096) return false;
    for (const auto& Raw : Json.toArray()) {
        const auto Encoded = registry::RegistryValueFromJson(Raw.toObject()); if (!Encoded) return false;
        const auto Item = DecodeEnvironmentValue(Encoded.value, Scope::User); if (!Item) return false;
        Values.push_back(Item.value);
    }
    return true;
}
QJsonArray EncodeBaseline(const ProfileBaseline& Baseline)
{
    QJsonArray Result;
    for (const auto& [Name, Item] : Baseline) Result.append(QJsonObject{{"name", QString::fromStdWString(Name)},
        {"value", Item ? QJsonValue(registry::RegistryValueJson(EncodeEnvironmentValue(*Item))) : QJsonValue(QJsonValue::Null)}});
    return Result;
}
bool DecodeBaseline(const QJsonValue& Json, ProfileBaseline& Baseline)
{
    if (Json.isUndefined()) return true; // Before active-profile editing was supported.
    if (!Json.isArray() || Json.toArray().size() > 4096) return false;
    for (const auto& Raw : Json.toArray()) {
        const auto Object = Raw.toObject(); const auto Name = Object["name"].toString().toStdWString();
        std::optional<Value> Item;
        if (!Object["value"].isNull()) {
            const auto Encoded = registry::RegistryValueFromJson(Object["value"].toObject()); if (!Encoded) return false;
            const auto Decoded = DecodeEnvironmentValue(Encoded.value, Scope::User);
            if (!Decoded || !SameName(Name, Decoded.value.name_)) return false; Item = Decoded.value;
        }
        if (Name.empty() || !Baseline.emplace(Name, Item).second) return false;
    }
    return true;
}
void FinishPending(EnvironmentProfile& Profile)
{
    Profile.active_ = Profile.pendingActive_;
    if (!Profile.pendingName_.isEmpty()) {
        Profile.name_ = std::move(Profile.pendingName_); Profile.values_ = std::move(Profile.pendingValues_);
        Profile.baseline_ = std::move(Profile.pendingBaseline_);
    }
    Profile.backupId_ = Profile.active_ ? Profile.pendingId_ : QString();
    Profile.pendingId_.clear(); Profile.pendingName_.clear(); Profile.pendingValues_.clear(); Profile.pendingBaseline_.clear();
}
bool Valid(const EnvironmentProfile& Profile)
{
    if (QUuid(Profile.id_).isNull() || Profile.name_.trimmed().isEmpty() || Profile.values_.size() > 4096) return false;
    std::set<std::wstring, NameLess> Seen;
    for (const auto& Value : Profile.values_) if (Value.scope_ != Scope::User || ValidateValue(Value) || !Seen.insert(Value.name_).second) return false;
    return true;
}
}
EnvironmentProfiles::EnvironmentProfiles(QString FileName) : fileName_(std::move(FileName)), revision_(Hash({})) {}
quint32 EnvironmentProfiles::Load(registry::RegistryChangeStore& History)
{
    previews_.clear(); desired_.clear(); profiles_.clear(); const auto Bytes = Read(fileName_); revision_ = Hash(Bytes);
    if (Bytes.isEmpty()) return QFile::exists(fileName_) ? ERROR_READ_FAULT : 0;
    const auto Envelope = QJsonDocument::fromJson(Bytes).object();
    if (Envelope["version"].toInt() != 1 || Envelope["protection"].toString() != "dpapi-user-v1") return ERROR_INVALID_DATA;
    const auto Plain = hc::system::RevealUserData(QByteArray::fromBase64(Envelope["payload"].toString().toLatin1()));
    if (Plain.isEmpty()) return ERROR_ACCESS_DENIED;
    QJsonParseError Parse; const auto Document = QJsonDocument::fromJson(Plain, &Parse);
    if (Parse.error != QJsonParseError::NoError || !Document.isArray() || Document.array().size() > 256) return ERROR_INVALID_DATA;
    QSet<QString> Ids;
    for (const auto& Entry : Document.array()) {
        const auto Object = Entry.toObject(); EnvironmentProfile Profile;
        Profile.id_ = Object["id"].toString(); Profile.name_ = Object["name"].toString(); Profile.active_ = Object["active"].toBool();
        Profile.backupId_ = Object["backup"].toString(); Profile.pendingId_ = Object["pending"].toString(); Profile.pendingActive_ = Object["pendingActive"].toBool();
        if (!DecodeValues(Object["values"], Profile.values_) || !DecodeBaseline(Object["baseline"], Profile.baseline_)) return ERROR_INVALID_DATA;
        Profile.pendingName_ = Object["pendingName"].toString();
        if (!Profile.pendingName_.isEmpty() && (!DecodeValues(Object["pendingValues"], Profile.pendingValues_)
            || !DecodeBaseline(Object["pendingBaseline"], Profile.pendingBaseline_))) return ERROR_INVALID_DATA;
        if (!Valid(Profile) || Ids.contains(Profile.id_)) return ERROR_INVALID_DATA; Ids.insert(Profile.id_);
        if (!Profile.pendingId_.isEmpty()) {
            const auto Record = History.Read(Profile.pendingId_);
            if (Record && Record.value.state == "committed") {
                FinishPending(Profile);
            }
            else if (Record && (Record.value.state == "undone" || (Record.value.completed == 0 && Record.value.inFlight < 0
                && (Record.value.state == "partial" || Record.value.state == "conflict")))) {
                // No write survived, or an explicit durable undo restored the
                // previous values. Keep the old definition and activation state.
                Profile.pendingId_.clear(); Profile.pendingName_.clear(); Profile.pendingValues_.clear(); Profile.pendingBaseline_.clear();
            }
            // Missing/partial records remain pending: never guess that a write
            // failed and restore over changes made by another application.
        }
        profiles_.append(std::move(Profile));
    }
    return 0;
}
quint32 EnvironmentProfiles::save()
{
    if (!QDir().mkpath(QFileInfo(fileName_).absolutePath())) return ERROR_PATH_NOT_FOUND;
    QLockFile Lock(fileName_ + ".lock"); if (!Lock.tryLock(1000)) return ERROR_BUSY;
    if (Hash(Read(fileName_)) != revision_) return ERROR_REVISION_MISMATCH;
    QJsonArray Items;
    for (const auto& Profile : profiles_) {
        const auto Values = EncodeValues(Profile.values_);
        Items.append(QJsonObject{{"id", Profile.id_}, {"name", Profile.name_}, {"values", Values}, {"active", Profile.active_},
            {"backup", Profile.backupId_}, {"pending", Profile.pendingId_}, {"pendingActive", Profile.pendingActive_}, {"baseline", EncodeBaseline(Profile.baseline_)},
            {"pendingName", Profile.pendingName_}, {"pendingValues", EncodeValues(Profile.pendingValues_)}, {"pendingBaseline", EncodeBaseline(Profile.pendingBaseline_)}});
    }
    const auto Protected = hc::system::ProtectUserData(QJsonDocument(Items).toJson(QJsonDocument::Compact));
    if (Protected.isEmpty()) return ERROR_ENCRYPTION_FAILED;
    const auto Bytes = QJsonDocument(QJsonObject{{"version", 1}, {"protection", "dpapi-user-v1"}, {"payload", QString::fromLatin1(Protected.toBase64())}}).toJson();
    QSaveFile File(fileName_); File.setDirectWriteFallback(false);
    if (!File.open(QIODevice::WriteOnly) || File.write(Bytes) != Bytes.size() || !File.commit()) return ERROR_WRITE_FAULT;
    revision_ = Hash(Bytes); return 0;
}
quint32 EnvironmentProfiles::Put(EnvironmentProfile Profile)
{
    if (Profile.id_.isEmpty()) Profile.id_ = QUuid::createUuid().toString(QUuid::WithoutBraces);
    if (!Valid(Profile) || Profile.active_ || !Profile.pendingId_.isEmpty() || !Profile.backupId_.isEmpty()) return ERROR_INVALID_PARAMETER;
    previews_.clear(); auto Before = profiles_;
    const auto Found = std::find_if(profiles_.begin(), profiles_.end(), [&](const auto& Item) { return Item.id_ == Profile.id_; });
    if (Found != profiles_.end()) { if (Found->active_ || !Found->pendingId_.isEmpty()) return ERROR_BUSY; *Found = std::move(Profile); }
    else { if (profiles_.size() >= 256) return ERROR_TOO_MANY_NAMES; profiles_.append(std::move(Profile)); }
    const auto Error = save(); if (Error) profiles_ = std::move(Before); return Error;
}
quint32 EnvironmentProfiles::Remove(const QString& Id)
{
    previews_.clear(); auto Before = profiles_;
    for (const auto& Profile : profiles_) if (Profile.id_ == Id && (Profile.active_ || !Profile.pendingId_.isEmpty())) return ERROR_BUSY;
    if (!profiles_.removeIf([&](const auto& Profile) { return Profile.id_ == Id; })) return ERROR_NOT_FOUND;
    const auto Error = save(); if (Error) profiles_ = std::move(Before); return Error;
}
QStringList EnvironmentProfiles::Conflicts(const QString& Id) const
{
    QStringList Result;
    const auto Found = std::find_if(profiles_.cbegin(), profiles_.cend(), [&](const auto& Item) { return Item.id_ == Id; });
    if (Found == profiles_.cend()) return Result;
    for (const auto& Other : profiles_) if (Other.id_ != Id && (Other.active_ || !Other.pendingId_.isEmpty()))
        for (const auto& Left : Found->values_) for (const auto& Right : Other.values_) if (SameName(Left.name_, Right.name_)) Result.append(Other.name_ + " / " + QString::fromStdWString(Left.name_));
    return Result;
}
EnvironmentPreview EnvironmentProfiles::PreviewUpdate(EnvironmentProfile Profile, EnvironmentStorage& Storage)
{
    const auto Found = std::find_if(profiles_.cbegin(), profiles_.cend(), [&](const auto& Item) { return Item.id_ == Profile.id_; });
    if (Found == profiles_.cend() || !Valid(Profile) || !Found->active_) return {{}, ERROR_INVALID_PARAMETER};
    if (!Found->pendingId_.isEmpty()) return {{}, ERROR_BUSY};
    for (const auto& Other : profiles_) if (Other.id_ != Profile.id_ && (Other.active_ || !Other.pendingId_.isEmpty()))
        for (const auto& Left : Profile.values_) for (const auto& Right : Other.values_) if (SameName(Left.name_, Right.name_)) return {{}, ERROR_DUP_NAME};
    const auto Current = Storage.ReadScope(Scope::User); if (!Current) return {{}, Current.error};
    const auto Update = BuildProfileUpdate(Found->values_, Profile.values_, Found->baseline_, Current.value);
    if (!Update) return {{}, Update.error_};
    auto Preview = Storage.Preview(Update.changes_);
    if (Preview) { Profile.active_ = true; Profile.baseline_ = Update.baseline_; desired_[Profile.id_] = Profile; previews_[Profile.id_] = Preview.plan_.Hash(); }
    return Preview;
}
EnvironmentPreview EnvironmentProfiles::PreviewToggle(const QString& Id, EnvironmentStorage& Storage, registry::RegistryChangeStore& History)
{
    const auto Found = std::find_if(profiles_.begin(), profiles_.end(), [&](const auto& Item) { return Item.id_ == Id; });
    if (Found == profiles_.end()) return {{}, ERROR_NOT_FOUND};
    if (!Found->pendingId_.isEmpty()) return {{}, ERROR_BUSY};
    if (!Found->active_ && !Conflicts(Id).isEmpty()) return {{}, ERROR_DUP_NAME};
    const auto Current = Storage.ReadScope(Scope::User); if (!Current) return {{}, Current.error};
    const auto Update = BuildProfileUpdate(Found->active_ ? Found->values_ : std::vector<Value>{},
        Found->active_ ? std::vector<Value>{} : Found->values_, Found->baseline_, Current.value);
    EnvironmentPreview Preview;
    if (!Update && Found->active_ && Found->baseline_.empty()) {
        const auto Record = History.Read(Found->backupId_); Preview = Record ? Storage.Undo(Record.value) : EnvironmentPreview{{}, Record.error};
    } else if (!Update) return {{}, Update.error_};
    else Preview = Storage.Preview(Update.changes_);
    if (Preview) {
        auto Next = *Found; Next.active_ = !Found->active_; Next.baseline_ = Update.baseline_;
        desired_[Id] = std::move(Next); previews_[Id] = Preview.plan_.Hash();
    }
    return Preview;
}
EnvironmentCommit EnvironmentProfiles::Toggle(const QString& Id, const EnvironmentPreview& Preview, EnvironmentStorage& Storage)
{
    const auto Found = std::find_if(profiles_.begin(), profiles_.end(), [&](const auto& Item) { return Item.id_ == Id; });
    if (Found == profiles_.end() || !Preview || !Found->pendingId_.isEmpty()) return {{}, {ERROR_INVALID_STATE}};
    if (previews_.take(Id) != Preview.plan_.Hash() || !desired_.contains(Id)) return {{}, {ERROR_REVISION_MISMATCH}};
    const auto Before = *Found; const auto Next = desired_.take(Id);
    Found->pendingId_ = Preview.plan_.id; Found->pendingActive_ = Next.active_; Found->pendingName_ = Next.name_;
    Found->pendingValues_ = Next.values_; Found->pendingBaseline_ = Next.baseline_;
    if (const auto Error = save()) { *Found = Before; return {{}, {Error}}; }
    auto Result = Storage.Commit(Preview);
    if (!Result.applied_.error) {
        FinishPending(*Found);
        if (const auto Error = save()) { Result.applied_.error = Error; Result.applied_.uncertain = true; }
    } else if (Result.record_.directory.isEmpty() || Result.restored_
        || (!Result.applied_.uncertain && Result.applied_.completed == 0)) {
        *Found = Before; if (save()) Result.applied_.uncertain = true;
    }
    return Result;
}
}