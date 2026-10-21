#include "documentRecoveryStore.hpp"
#include "commonLibs/document/documentHostProtocol.hpp"
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QLockFile>
#include <QRegularExpression>
#include <QSaveFile>
#include <QUuid>
#include <set>
#include <algorithm>
#include <map>
#include <utility>

namespace hc::document
{
    using namespace storage;
    namespace
    {
        Error Failure(const char* Code) { return {ErrorCategory::Unavailable, Code, {"document-recovery"}, {}, true}; }
        bool IdValid(const std::string& Id)
        { return Id.size() == 32 && QRegularExpression("^[0-9a-f]{32}$").match(QString::fromStdString(Id)).hasMatch(); }
        bool Unsigned(const QJsonValue& Value, std::uint64_t& Number)
        {
            if (!Value.isString()) return false;
            bool Valid = false; const auto Text = Value.toString();
            Number = Text.toULongLong(&Valid);
            return Valid && QString::number(Number) == Text;
        }
        bool Hex(const std::string& Text)
        {
            return Text.size() == 64 && std::all_of(Text.begin(), Text.end(),
                [](unsigned char Ch) { return (Ch >= '0' && Ch <= '9') || (Ch >= 'a' && Ch <= 'f'); });
        }
        bool TextValid(const QJsonValue& Value, int Limit = 32768)
        {
            if (!Value.isString()) return false;
            const auto Text = Value.toString();
            return Text.size() <= Limit && !Text.contains(QChar(0)) && QString::fromUtf8(Text.toUtf8()) == Text;
        }
        bool NativeTextValid(const std::string& Text, int Limit = 32768)
        {
            const auto Decoded = QString::fromStdString(Text);
            return Decoded.toStdString() == Text && TextValid(Decoded, Limit);
        }
        bool NativeSourceValid(const SourceIdentity& Identity)
        {
            if (!NativeTextValid(Identity.localPath_) || !NativeTextValid(Identity.sourceToken_)) return false;
            if (!Identity.entry_) return true;
            const auto& Entry = *Identity.entry_;
            if (!NativeTextValid(Entry.id_.providerId_.value_) || !NativeTextValid(Entry.id_.key_)
                || (Entry.id_.revision_ && !NativeTextValid(Entry.id_.revision_->token_))
                || !NativeTextValid(Entry.locator_.root_)) return false;
            return std::all_of(Entry.locator_.nesting_.begin(), Entry.locator_.nesting_.end(),
                [](const auto& Hop) { return NativeTextValid(Hop.provider_) && NativeTextValid(Hop.entry_); });
        }
        bool ModelValid(const ExternalModelRevision& Model)
        {
            return Model.epoch_ && !Model.savepoint_.empty() && Model.savepoint_.size() <= 128
                && std::all_of(Model.savepoint_.begin(), Model.savepoint_.end(),
                    [](unsigned char Ch) { return Ch >= 0x21 && Ch <= 0x7e; });
        }
        bool ExternalValid(const RecoverySnapshot& Snapshot)
        {
            const auto& Document = Snapshot.document_;
            const bool Dirty = Document.model_.epoch_ != Document.savedModel_.epoch_
                || Document.model_.savepoint_ != Document.savedModel_.savepoint_;
            return Document.contentMode_ == DocumentContentMode::ExternalModel
                && Hex(Snapshot.bundleSha256_) && (Snapshot.format_ == "hwp" || Snapshot.format_ == "hwpx")
                && Snapshot.engineSchema_ == RecoveryEngineSchema && !Snapshot.bytes_.empty()
                && Document.generation_ && Document.revision_ && Document.savedRevision_ <= Document.revision_
                && ModelValid(Document.model_) && ModelValid(Document.savedModel_)
                && (Document.model_.epoch_ != Document.savedModel_.epoch_
                    || Document.savedModel_.changeSequence_ <= Document.model_.changeSequence_)
                && Document.dirty_ == Dirty && !Snapshot.viewId_.empty()
                && NativeTextValid(Snapshot.viewId_, 128) && NativeSourceValid(Document.source_);
        }
        QJsonObject Model(const ExternalModelRevision& Revision)
        {
            return {{"epoch", QString::number(Revision.epoch_)},
                {"changeSequence", QString::number(Revision.changeSequence_)},
                {"savepoint", QString::fromStdString(Revision.savepoint_)}};
        }
        bool ReadModel(const QJsonValue& Value, ExternalModelRevision& Revision)
        {
            if (!Value.isObject()) return false;
            const auto Json = Value.toObject();
            if (Json.size() != 3 || !Unsigned(Json["epoch"], Revision.epoch_)
                || !Unsigned(Json["changeSequence"], Revision.changeSequence_)
                || !TextValid(Json["savepoint"], 128)) return false;
            Revision.savepoint_ = Json["savepoint"].toString().toStdString(); return ModelValid(Revision);
        }
        bool ExternalSourceValid(const QJsonObject& Json, const QJsonObject& Record)
        {
            if (!TextValid(Json["localPath"]) || !TextValid(Json["sourceToken"])
                || !TextValid(Json["inputSha256"], 64) || Json["localPath"] != Record["sourcePath"]
                || Json["sourceToken"] != Record["sourceToken"]) return false;
            const auto InputHash = Json["inputSha256"].toString().toStdString();
            if (!InputHash.empty() && !Hex(InputHash)) return false;
            if (!Json.contains("provider")) return Json.size() == 3;
            if (Json.size() != 10 || !TextValid(Json["provider"]) || !TextValid(Json["key"])
                || !TextValid(Json["revision"]) || !TextValid(Json["locatorRoot"])
                || !Json["nesting"].isArray() || Json["nesting"].toArray().size() > 128) return false;
            const auto Scope = Json["scope"], Kind = Json["locatorKind"];
            if ((Scope != QJsonValue(int(IdentityScope::Persistent)) && Scope != QJsonValue(int(IdentityScope::NavigationSession)))
                || !Kind.isDouble() || Kind.toDouble(-1) != Kind.toInt(-1)
                || Kind.toInt(-1) < 0 || Kind.toInt(-1) > int(LocatorKind::Plugin)) return false;
            for (const auto& Hop : Json["nesting"].toArray()) {
                if (!Hop.isObject() || Hop.toObject().size() != 2
                    || !TextValid(Hop.toObject()["provider"]) || !TextValid(Hop.toObject()["entry"])) return false;
            }
            return true;
        }
        auto Stamp(const DocumentSnapshot& Document)
        { return std::pair{Document.generation_, Document.revision_}; }
        bool IndexedAtLeast(const QJsonObject& Item, const DocumentSnapshot& Document)
        {
            std::uint64_t Generation = 1, Revision = 0;
            // Legacy indexes have no generation; use their record when readable.
            if (!Unsigned(Item["revision"], Revision)) return false;
            if (Item.contains("generation") && (!Unsigned(Item["generation"], Generation) || !Generation)) return false;
            return std::pair{Generation, Revision} >= Stamp(Document);
        }
        bool StoreBytes(const QString& Path, const QByteArray& Bytes)
        {
            QSaveFile File(Path); File.setDirectWriteFallback(false);
            return File.open(QIODevice::WriteOnly) && File.write(Bytes) == Bytes.size() && File.commit();
        }
        Result<QJsonArray> Index(const QString& Root)
        {
            QFile File(Root + "/index.json"); if (!File.exists()) return QJsonArray{};
            if (!File.open(QIODevice::ReadOnly)) return Failure("document-recovery-index-read");
            QJsonParseError Parse; const auto Document = QJsonDocument::fromJson(File.readAll(), &Parse);
            const auto Json = Document.object();
            if (Parse.error != QJsonParseError::NoError || !Document.isObject() || Json["schemaVersion"] != QJsonValue(1) || !Json["entries"].isArray()) return Failure("document-recovery-index-schema");
            for (const auto& Value : Json["entries"].toArray()) {
                if (!Value.isObject()) return Failure("document-recovery-index-schema");
                const auto Item = Value.toObject(); std::uint64_t Revision = 0, Generation = 0;
                const auto Name = Item["path"].toString();
                if ((Item.size() != 3 && Item.size() != 4) || !IdValid(Item["documentId"].toString().toStdString())
                    || !Unsigned(Item["revision"], Revision) || !TextValid(Item["path"])
                    || QFileInfo(Name).fileName() != Name || !Name.startsWith(Item["documentId"].toString() + "-")
                    || !Name.endsWith(".snapshot.json")
                    || (Item.size() == 4 && (!Unsigned(Item["generation"], Generation) || !Generation)))
                    return Failure("document-recovery-index-schema");
            }
            return Json["entries"].toArray();
        }
        bool PutIndex(const QString& Root, const QJsonArray& Entries)
        { return StoreBytes(Root + "/index.json", QJsonDocument(QJsonObject{{"schemaVersion", 1}, {"entries", Entries}}).toJson(QJsonDocument::Compact)); }
        QJsonObject Source(const SourceIdentity& Identity)
        {
            QJsonObject Json{{"localPath", QString::fromStdString(Identity.localPath_)},
                {"sourceToken", QString::fromStdString(Identity.sourceToken_)}};
            if (Identity.entry_) {
                const auto& Entry = *Identity.entry_; QJsonArray Nesting;
                for (const auto& Hop : Entry.locator_.nesting_) Nesting.append(QJsonObject{{"provider", QString::fromStdString(Hop.provider_)}, {"entry", QString::fromStdString(Hop.entry_)}});
                Json["provider"] = QString::fromStdString(Entry.id_.providerId_.value_);
                Json["key"] = QString::fromStdString(Entry.id_.key_);
                Json["revision"] = Entry.id_.revision_ ? QString::fromStdString(Entry.id_.revision_->token_) : QString{};
                Json["scope"] = int(Entry.id_.scope_); Json["locatorKind"] = int(Entry.locator_.kind_);
                Json["locatorRoot"] = QString::fromStdString(Entry.locator_.root_); Json["nesting"] = Nesting;
            }
            return Json;
        }
    }
    Result<RecoverySnapshot> DocumentRecoveryStore::Store(RecoverySnapshot Snapshot)
    {
        if (!IdValid(Snapshot.document_.id_)) return Failure("document-recovery-budget");
        if (!Snapshot.document_.generation_ || Snapshot.document_.savedRevision_ > Snapshot.document_.revision_)
            return Failure("document-recovery-generation");
        const bool External = Snapshot.document_.contentMode_ == DocumentContentMode::ExternalModel;
        if ((Snapshot.document_.contentMode_ != DocumentContentMode::InlineBytes && !External)
            || (External && !ExternalValid(Snapshot))
            || (!External && (!Snapshot.bundleSha256_.empty() || !Snapshot.format_.empty() || Snapshot.engineSchema_)))
            return Failure("document-recovery-external-model");
        if (!QDir().mkpath(root_)) return Failure("document-recovery-directory");
        QLockFile Lock(root_ + "/index.lock"); if (!Lock.tryLock(2000)) return Failure("document-recovery-locked");
        auto Existing = Index(root_); if (!Existing.HasValue()) return Existing.Failure();
        const auto Id = QString::fromStdString(Snapshot.document_.id_);
        // Native generation/revision order wins over completion time. Reject
        // stale/equal completions before publishing files, index or P0 alias.
        for (const auto& Value : Existing.Value()) {
            auto Item = Value.toObject();
            if (Item["documentId"] != Id) continue;
            if (!Item.contains("generation")) {
                auto Previous = Read((root_ + "/" + Item["path"].toString()).toStdString());
                if (Previous.HasValue()) Item["generation"] = QString::number(Previous.Value().document_.generation_);
            }
            if (IndexedAtLeast(Item, Snapshot.document_))
                return Error{ErrorCategory::StaleRevision, "document-recovery-stale"};
        }
        auto Alias = Read((root_ + "/" + Id + ".json").toStdString());
        if (Alias.HasValue() && Alias.Value().document_.id_ == Snapshot.document_.id_
            && Stamp(Alias.Value().document_) >= Stamp(Snapshot.document_))
            return Error{ErrorCategory::StaleRevision, "document-recovery-stale"};
        const auto Bytes = QByteArray::fromStdString(Snapshot.bytes_);
        Snapshot.checksum_ = QCryptographicHash::hash(Bytes, QCryptographicHash::Sha256).toHex().toStdString();
        Snapshot.utcMs_ = QDateTime::currentMSecsSinceEpoch();
        const auto Name = Id + "-" + QString::number(Snapshot.document_.revision_) + "-" + QUuid::createUuid().toString(QUuid::Id128) + ".snapshot.json";
        Snapshot.snapshotPath_ = (root_ + "/" + Name).toStdString();
        QJsonObject Record{{"schemaVersion", 2}, {"documentId", Id}, {"viewId", QString::fromStdString(Snapshot.viewId_)},
            {"sourcePath", QString::fromStdString(Snapshot.document_.source_.localPath_)},
            {"sourceToken", QString::fromStdString(Snapshot.document_.source_.sourceToken_)},
            {"source", Source(Snapshot.document_.source_)}, {"generation", QString::number(Snapshot.document_.generation_)},
            {"revision", QString::number(Snapshot.document_.revision_)}, {"savedRevision", QString::number(Snapshot.document_.savedRevision_)},
            {"utcMs", QString::number(Snapshot.utcMs_)}, {"sha256", QString::fromStdString(Snapshot.checksum_)},
            {"bytes", QString::fromLatin1(Bytes.toBase64())}};
        if (External) {
            auto Identity = Record["source"].toObject();
            Identity["inputSha256"] = QString::fromStdString(Snapshot.document_.source_.inputSha256_);
            Record["source"] = Identity;
            Record["schemaVersion"] = 3; Record["contentMode"] = "externalModel";
            Record["bundleSha256"] = QString::fromStdString(Snapshot.bundleSha256_);
            Record["format"] = QString::fromStdString(Snapshot.format_);
            Record["engineSchema"] = int(Snapshot.engineSchema_);
            Record["model"] = Model(Snapshot.document_.model_);
            Record["savedModel"] = Model(Snapshot.document_.savedModel_);
            Record["dirty"] = Snapshot.document_.dirty_;
            if (!ExternalSourceValid(Identity, Record)) return Failure("document-recovery-source");
        }
        const auto Payload = QJsonDocument(Record).toJson(QJsonDocument::Compact);
        if (!StoreBytes(QString::fromStdString(Snapshot.snapshotPath_), Payload)) return Failure("document-recovery-write");
        QJsonArray Next; Next.append(QJsonObject{{"documentId", Id}, {"generation", QString::number(Snapshot.document_.generation_)}, {"revision", QString::number(Snapshot.document_.revision_)}, {"path", Name}});
        QStringList Retired; int Previous = 0;
        for (const auto& Value : Existing.Value()) {
            const auto Item = Value.toObject();
            if (Item["documentId"] == Id && ++Previous >= 3) Retired.append(Item["path"].toString());
            else Next.append(Item);
        }
        // A snapshot is immutable and complete before its index is published.
        // A failed index update leaves the previous indexed generations intact.
        if (!PutIndex(root_, Next)) return Failure("document-recovery-index-write");
        // Compatibility alias for the P0 host harness; recovery enumeration uses
        // indexed immutable generations and never trusts incomplete orphan files.
        StoreBytes(root_ + "/" + Id + ".json", Payload);
        for (const auto& Old : Retired) {
            if (QFileInfo(Old).fileName() == Old && Old.endsWith(".snapshot.json")) QFile::remove(root_ + "/" + Old);
        }
        return Snapshot;
    }
    Result<RecoverySnapshot> DocumentRecoveryStore::Read(const std::string& Path)
    {
        const auto Canonical = QFileInfo(QString::fromStdString(Path)).canonicalFilePath();
        const auto Root = QFileInfo(root_).canonicalFilePath();
        if (Root.isEmpty() || Canonical.isEmpty() || QFileInfo(Canonical).absolutePath() != Root || !Canonical.endsWith(".json"))
            return Failure("document-recovery-path");
        QFile File(Canonical);
        if (!File.open(QIODevice::ReadOnly)) return Failure("document-recovery-read");
        QJsonParseError Parse; const auto Document = QJsonDocument::fromJson(File.readAll(), &Parse);
        const auto Json = Document.object(); const auto Schema = Json["schemaVersion"].toInt();
        const auto Bytes = QByteArray::fromBase64(Json["bytes"].toString().toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
        const auto Id = Json["documentId"].toString().toStdString(); bool RevisionValid = false;
        const auto Revision = Json["revision"].toString().toULongLong(&RevisionValid);
        if (Parse.error != QJsonParseError::NoError || !Document.isObject() || (Schema != 1 && Schema != 2 && Schema != 3)
            || !IdValid(Id) || !RevisionValid || Bytes.toBase64() != Json["bytes"].toString().toLatin1()
            || QCryptographicHash::hash(Bytes, QCryptographicHash::Sha256).toHex() != Json["sha256"].toString().toLatin1())
            return Failure("document-recovery-checksum-or-schema");
        RecoverySnapshot Result; Result.document_.id_ = Id; Result.document_.revision_ = Revision;
        if (Schema == 2 || Schema == 3) {
            bool GenerationValid = false, SavedValid = false;
            Result.document_.generation_ = Json["generation"].toString().toULongLong(&GenerationValid);
            Result.document_.savedRevision_ = Json["savedRevision"].toString().toULongLong(&SavedValid);
            if (!GenerationValid || !Result.document_.generation_ || !SavedValid || Result.document_.savedRevision_ > Revision) return Failure("document-recovery-generation");
        }
        Result.document_.source_.localPath_ = Json["sourcePath"].toString().toStdString();
        Result.document_.source_.sourceToken_ = Json["sourceToken"].toString().toStdString();
        const auto Identity = Json["source"].toObject();
        // Bound nested origin metadata before constructing the native locator.
        if (Schema == 3 && !ExternalSourceValid(Identity, Json)) return Failure("document-recovery-external-schema");
        if (Identity.contains("provider")) {
            EntrySnapshot Entry; Entry.id_.providerId_ = {Identity["provider"].toString().toStdString()}; Entry.id_.key_ = Identity["key"].toString().toStdString();
            Entry.id_.scope_ = Identity["scope"].toInt() == int(IdentityScope::Persistent) ? IdentityScope::Persistent : IdentityScope::NavigationSession;
            if (!Identity["revision"].toString().isEmpty()) Entry.id_.revision_ = ResourceRevision{Identity["revision"].toString().toStdString()};
            const auto Kind = Identity["locatorKind"].toInt(-1);
            if (Kind < 0 || Kind > int(LocatorKind::Plugin)) return Failure("document-recovery-source");
            Entry.locator_.kind_ = LocatorKind(Kind); Entry.locator_.root_ = Identity["locatorRoot"].toString().toStdString();
            for (auto Value : Identity["nesting"].toArray()) Entry.locator_.nesting_.push_back({Value.toObject()["provider"].toString().toStdString(), Value.toObject()["entry"].toString().toStdString()});
            Result.document_.source_.entry_ = std::move(Entry);
        }
        if (Schema == 3) {
            std::uint64_t Generation = 0, Saved = 0, CanonicalRevision = 0;
            bool TimeValid = false; const auto TimeText = Json["utcMs"].toString();
            const auto Time = TimeText.toLongLong(&TimeValid);
            Result.document_.contentMode_ = DocumentContentMode::ExternalModel;
            Result.bundleSha256_ = Json["bundleSha256"].toString().toStdString();
            Result.format_ = Json["format"].toString().toStdString();
            Result.engineSchema_ = RecoveryEngineSchema;
            Result.document_.dirty_ = Json["dirty"].toBool();
            Result.viewId_ = Json["viewId"].toString().toStdString();
            Result.bytes_ = Bytes.toStdString();
            if (Json.size() != 19 || Json["schemaVersion"] != QJsonValue(3)
                || Json["contentMode"] != QJsonValue("externalModel")
                || Json["engineSchema"] != QJsonValue(int(RecoveryEngineSchema))
                || !TextValid(Json["bundleSha256"], 64) || !TextValid(Json["format"], 4)
                || !TextValid(Json["viewId"], 128) || !Json["dirty"].isBool()
                || !TextValid(Json["sourcePath"]) || !TextValid(Json["sourceToken"])
                || !Unsigned(Json["generation"], Generation) || !Unsigned(Json["revision"], CanonicalRevision)
                || !Unsigned(Json["savedRevision"], Saved) || !TimeValid || Time < 1 || QString::number(Time) != TimeText
                || !ReadModel(Json["model"], Result.document_.model_)
                || !ReadModel(Json["savedModel"], Result.document_.savedModel_)
                || !ExternalSourceValid(Identity, Json) || !ExternalValid(Result))
                return Failure("document-recovery-external-schema");
            Result.document_.source_.inputSha256_ = Identity["inputSha256"].toString().toStdString();
            // Serialized provider/token metadata cannot restore a live grant.
            Result.document_.source_.sourceToken_.clear();
        } else if (Json.contains("contentMode") || Json.contains("model") || Json.contains("savedModel")
            || Json.contains("bundleSha256") || Json.contains("engineSchema")
            || Json["viewId"] == QJsonValue("rhwp") || Json["viewId"] == QJsonValue("hwp")
            || Json["viewId"] == QJsonValue("hwpx")) {
            return Failure("document-recovery-external-schema");
        }
        // Persisted identity is information, never a renewed provider authority.
        Result.document_.source_.originalWritable_ = false;
        Result.bytes_ = Bytes.toStdString(); Result.checksum_ = Json["sha256"].toString().toStdString();
        Result.snapshotPath_ = Canonical.toStdString(); Result.utcMs_ = Json["utcMs"].toString().toLongLong();
        Result.viewId_ = Json["viewId"].toString("text").toStdString(); return Result;
    }
    Result<std::vector<RecoverySnapshot>> DocumentRecoveryStore::List()
    {
        auto Entries = Index(root_); if (!Entries.HasValue()) return Entries.Failure();
        std::set<DocumentId> Seen; std::vector<RecoverySnapshot> Result;
        std::map<DocumentId, std::size_t> Selected;
        for (const auto& Value : Entries.Value()) {
            const auto Item = Value.toObject(); const auto Name = Item["path"].toString();
            auto Record = Read((root_ + "/" + Name).toStdString());
            std::uint64_t Revision = 0, Generation = 0;
            if (!Record.HasValue() || Record.Value().document_.id_ != Item["documentId"].toString().toStdString()
                || !Unsigned(Item["revision"], Revision) || Record.Value().document_.revision_ != Revision
                || (Item.contains("generation") && (!Unsigned(Item["generation"], Generation)
                    || Record.Value().document_.generation_ != Generation))) continue;
            const auto Id = Record.Value().document_.id_;
            const auto Previous = Selected.find(Id);
            if (Previous == Selected.end()) {
                Selected[Id] = Result.size(); Seen.insert(Id); Result.push_back(std::move(Record.Value()));
            } else if (Stamp(Record.Value().document_) > Stamp(Result[Previous->second].document_))
                Result[Previous->second] = std::move(Record.Value());
        }
        // Import valid P0 aliases without exposing unindexed P1 generations.
        for (const auto& Name : QDir(root_).entryList({"*.json"}, QDir::Files)) {
            if (!QRegularExpression("^[0-9a-f]{32}\\.json$").match(Name).hasMatch()) continue;
            auto Record = Read((root_ + "/" + Name).toStdString());
            if (Record.HasValue() && Seen.insert(Record.Value().document_.id_).second) Result.push_back(std::move(Record.Value()));
        }
        return Result;
    }
    Result<Unit> DocumentRecoveryStore::Remove(const DocumentId& Id, std::uint64_t ThroughRevision)
    {
        if (!IdValid(Id)) return Failure("document-recovery-id");
        if (!QFileInfo::exists(root_)) return Unit{};
        QLockFile Lock(root_ + "/index.lock"); if (!Lock.tryLock(2000)) return Failure("document-recovery-locked");
        auto Entries = Index(root_); if (!Entries.HasValue()) return Entries.Failure();
        QJsonArray Next; QStringList Retired; bool Remaining = false;
        for (auto Value : Entries.Value()) {
            const auto Item = Value.toObject();
            if (Item["documentId"].toString().toStdString() == Id && Item["revision"].toString().toULongLong() <= ThroughRevision)
                Retired.append(Item["path"].toString());
            else { Next.append(Item); if (Item["documentId"].toString().toStdString() == Id) Remaining = true; }
        }
        if (!PutIndex(root_, Next)) return Failure("document-recovery-index-write");
        for (const auto& Name : Retired) if (QFileInfo(Name).fileName() == Name && Name.startsWith(QString::fromStdString(Id) + "-") && Name.endsWith(".snapshot.json")) {
            if (QFileInfo::exists(root_ + "/" + Name) && !QFile::remove(root_ + "/" + Name)) return Failure("document-recovery-cleanup");
        }
        const auto Alias = root_ + "/" + QString::fromStdString(Id) + ".json";
        if (!Remaining && QFileInfo::exists(Alias)) {
            auto Record = Read(Alias.toStdString());
            if (Record.HasValue() && Record.Value().document_.revision_ <= ThroughRevision && !QFile::remove(Alias)) return Failure("document-recovery-cleanup");
        }
        return Unit{};
    }
}
