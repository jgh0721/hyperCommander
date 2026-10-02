#include "qtJobJournal.hpp"
#include <QDir>
#include <QFile>
#include <QSaveFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QLockFile>
#include "qtItemOutcomeStore.hpp"

namespace hc::operations
{
    using namespace storage;
    namespace
    {
        Error Failure(const char* Code) { return {ErrorCategory::Unavailable, Code, {"operation-journal"}, {}, false, "check-journal-directory"}; }
        QString S(const std::string& Value) { return QString::fromStdString(Value); }
        bool Id(const std::string& Value)
        { return Value.starts_with("hc-job-") && Value.size() < 128 && std::ranges::all_of(Value, [](char Ch) { return (Ch >= 'a' && Ch <= 'z') || (Ch >= '0' && Ch <= '9') || Ch == '-'; }); }
        QJsonObject EncodeError(const Error& Value)
        { return {{"category", int(Value.category_)}, {"code", S(Value.code_)}, {"provider", S(Value.provider_.value_)}, {"action", S(Value.userAction_)}}; }
        Error DecodeError(QJsonObject Value)
        { return {ErrorCategory(Value["category"].toInt(int(ErrorCategory::Internal))), Value["code"].toString().toStdString(), {Value["provider"].toString().toStdString()}, {}, false, Value["action"].toString().toStdString()}; }
        QJsonObject Encode(const JobResult& Result)
        {
            const auto& Snapshot = Result.snapshot_; QJsonArray Items, Cleanup;
            for (const auto& Item : Result.outcomes_ ? std::vector<ItemOutcome>{} : Result.items_)
            {
                QJsonObject Value{{"item", S(Item.item_)}, {"status", int(Item.status_)}, {"destinationCommitted", Item.destinationCommitted_}, {"sourceRemoved", Item.sourceRemoved_}};
                if (Item.error_) Value.insert("error", EncodeError(*Item.error_));
                if (Item.source_) { Value.insert("sourceId", S(Serialize(Item.source_->id_))); Value.insert("sourceLocator", S(Serialize(Item.source_->locator_))); }
                Items.append(Value);
            }
            for (const auto& Error : Result.cleanupErrors_) Cleanup.append(EncodeError(Error));
            QJsonObject Out{{"version", Result.outcomes_ ? 2 : 1}, {"id", S(Snapshot.id_)}, {"correlation", S(Snapshot.correlationId_)}, {"state", int(Snapshot.state_)},
                {"current", S(Snapshot.progress_.currentItem_)}, {"bytes", QString::number(Snapshot.progress_.bytesDone_)},
                {"completed", QString::number(Snapshot.completedItems_)}, {"skipped", QString::number(Snapshot.skippedItems_)}, {"failed", QString::number(Snapshot.failedItems_)},
                {"items", Items}, {"cleanup", Cleanup}};
            Out.insert("writes", bool(Result.writes_));
            if (Snapshot.error_) Out.insert("error", EncodeError(*Snapshot.error_)); return Out;
        }
        Result<JobResult> Decode(QJsonObject Object)
        {
            if ((Object["version"] != 1 && Object["version"] != 2) || !Id(Object["id"].toString().toStdString()) || !Object["items"].isArray() || !Object["cleanup"].isArray()) return Failure("job-journal-format");
            const int State = Object["state"].toInt(-1); if (State < 0 || State > int(JobState::Cancelled)) return Failure("job-journal-state");
            JobResult Out; auto& Snapshot = Out.snapshot_; Snapshot.id_ = Object["id"].toString().toStdString(); Snapshot.correlationId_ = Object["correlation"].toString().toStdString();
            Snapshot.state_ = JobState(State); Snapshot.progress_.currentItem_ = Object["current"].toString().toStdString();
            Snapshot.progress_.bytesDone_ = Object["bytes"].toString().toULongLong(); Snapshot.completedItems_ = Object["completed"].toString().toULongLong();
            Snapshot.skippedItems_ = Object["skipped"].toString().toULongLong(); Snapshot.failedItems_ = Object["failed"].toString().toULongLong();
            if (Object.contains("error")) Snapshot.error_ = DecodeError(Object["error"].toObject());
            for (const auto& Raw : Object["items"].toArray())
            {
                const auto Value = Raw.toObject(); const auto Status = Value["status"].toInt(-1);
                if (Status < 0 || Status > int(ItemOutcome::Status::Failed) || !Value["item"].isString()) return Failure("job-journal-item");
                ItemOutcome Item; Item.item_ = Value["item"].toString().toStdString(); Item.status_ = ItemOutcome::Status(Status);
                Item.destinationCommitted_ = Value["destinationCommitted"].toBool(); Item.sourceRemoved_ = Value["sourceRemoved"].toBool();
                if (Value.contains("error")) Item.error_ = DecodeError(Value["error"].toObject());
                if (Value.contains("sourceId"))
                {
                    auto Resource = ParseResourceId(Value["sourceId"].toString().toStdString()); auto At = ParseLocator(Value["sourceLocator"].toString().toStdString());
                    if (!Resource.HasValue() || !At.HasValue()) return Failure("job-journal-source");
                    Item.source_ = EntrySnapshot{}; Item.source_->id_ = Resource.Value(); Item.source_->locator_ = At.Value();
                }
                Out.items_.push_back(std::move(Item));
            }
            for (const auto& Item : Object["cleanup"].toArray()) Out.cleanupErrors_.push_back(DecodeError(Item.toObject()));
            // An interrupted commit has unknown effects. Preserve its item
            // evidence and require inspection; never replay or remove sources.
            if (!IsTerminal(Snapshot.state_))
            { Snapshot.state_ = JobState::Failed; Snapshot.error_ = Error{ErrorCategory::Unavailable, "job-interrupted", {"operation-journal"}, {}, false, "inspect-destination-before-retry"}; }
            return Out;
        }
        class FileJournal final : public IJobJournal
        {
        public:
            explicit FileJournal(QString Directory) : directory_(std::move(Directory)) {}
            Result<std::shared_ptr<IItemOutcomeStore>> OpenItems(const std::string& JobId) override
            {
                if (!Id(JobId)) return Failure("job-journal-id");
                std::lock_guard Lock(mutex_);
                if (!QDir::isAbsolutePath(directory_) || !QDir().mkpath(directory_)) return Failure("job-journal-directory");
                return openItems(JobId);
            }
            Result<std::shared_ptr<IItemOutcomeStore>> OpenWrites(const std::string& JobId) override
            { return OpenItems(JobId + "-writes"); }
            Result<Unit> Checkpoint(const JobResult& Result) override
            {
                if (!Id(Result.snapshot_.id_)) return Failure("job-journal-id");
                const auto Bytes = QJsonDocument(Encode(Result)).toJson(QJsonDocument::Compact);
                if (Bytes.size() > 16 * 1024 * 1024) return Failure("job-journal-record-budget");
                std::lock_guard Lock(mutex_);
                if (!QDir::isAbsolutePath(directory_) || !QDir().mkpath(directory_)) return Failure("job-journal-directory");
                QLockFile Guard(QDir(directory_).filePath("writer.lock")); if (!Guard.tryLock(1000)) return Failure("job-journal-busy");
                const auto Path = QDir(directory_).filePath(S(Result.snapshot_.id_) + ".json");
                if (!QFile::exists(Path) && QDir(directory_).entryList({"*.json"}, QDir::Files).size() >= 1024) return Failure("job-journal-history-full");
                // QSaveFile has no direct-write fallback: a failed checkpoint
                // leaves the previous evidence intact, including commit intent.
                QSaveFile File(Path); if (!File.open(QIODevice::WriteOnly) || File.write(Bytes) != Bytes.size() || !File.commit()) return Failure("job-journal-write");
                return Unit{};
            }
            Result<std::vector<JobResult>> Recover() override
            {
                std::lock_guard Lock(mutex_); std::vector<JobResult> Results;
                if (!QDir(directory_).exists()) return Results;
                QLockFile Guard(QDir(directory_).filePath("writer.lock")); if (!Guard.tryLock(1000)) return Failure("job-journal-busy");
                const auto Files = QDir(directory_).entryInfoList({"*.json"}, QDir::Files, QDir::Time);
                if (Files.size() > 1024) return Failure("job-journal-history-full");
                for (const auto& Info : Files)
                {
                    QFile File(Info.absoluteFilePath()); if (Info.isSymLink() || Info.size() > 16 * 1024 * 1024 || !File.open(QIODevice::ReadOnly)) return Failure("job-journal-read");
                    QJsonParseError Error; const auto Document = QJsonDocument::fromJson(File.readAll(), &Error);
                    if (Error.error != QJsonParseError::NoError || !Document.isObject()) return Failure("job-journal-json");
                    auto Decoded = Decode(Document.object()); if (!Decoded.HasValue()) return Decoded.Failure();
                    if (S(Decoded.Value().snapshot_.id_) + ".json" != Info.fileName()) return Failure("job-journal-id-mismatch");
                    if (Document.object()["version"] == 2) {
                        if (!QFile::exists(QDir(directory_).filePath(S(Decoded.Value().snapshot_.id_) + ".items"))) return Failure("job-outcomes-missing");
                        auto Store = openItems(Decoded.Value().snapshot_.id_); if (!Store.HasValue()) return Store.Failure();
                        Decoded.Value().outcomes_ = Store.Value();
                        auto& Snapshot = Decoded.Value().snapshot_;
                        Snapshot.completedItems_ = Snapshot.skippedItems_ = Snapshot.failedItems_ = 0;
                        for (uint64_t Row = 0; Row < Store.Value()->Count();) {
                            auto Page = Store.Value()->Read(Row, 256); if (!Page.HasValue()) return Page.Failure();
                            for (const auto& Item : Page.Value().items_) {
                                if (Item.status_ == ItemOutcome::Status::Completed) ++Snapshot.completedItems_;
                                else if (Item.status_ == ItemOutcome::Status::Skipped) ++Snapshot.skippedItems_;
                                else ++Snapshot.failedItems_;
                                if (Decoded.Value().items_.size() < 200) Decoded.Value().items_.push_back(Item);
                            }
                            Row += Page.Value().items_.size();
                        }
                    }
                    if (Document.object()["writes"].toBool()) {
                        const auto Key = Decoded.Value().snapshot_.id_ + "-writes";
                        if (!QFile::exists(QDir(directory_).filePath(S(Key) + ".items"))) return Failure("job-writes-missing");
                        auto Writes = openItems(Key); if (!Writes.HasValue()) return Writes.Failure();
                        Decoded.Value().writes_ = Writes.Value();
                    }
                    Results.push_back(std::move(Decoded.Value()));
                }
                return Results;
            }
            Result<Unit> Forget(const std::string& JobId) override
            {
                if (!Id(JobId)) return Failure("job-journal-id"); std::lock_guard Lock(mutex_);
                const auto Path = QDir(directory_).filePath(S(JobId) + ".json");
                if (!QDir(directory_).exists()) return Unit{};
                QLockFile Guard(QDir(directory_).filePath("writer.lock")); if (!Guard.tryLock(1000)) return Failure("job-journal-busy");
                if (QFile::exists(Path) && !QFile::remove(Path)) return Failure("job-journal-remove");
                const auto Items = QDir(directory_).filePath(S(JobId) + ".items");
                if (QFile::exists(Items) && !QFile::remove(Items)) return Failure("job-outcomes-remove");
                const auto Writes = QDir(directory_).filePath(S(JobId) + "-writes.items");
                if (QFile::exists(Writes) && !QFile::remove(Writes)) return Failure("job-writes-remove");
                stores_.erase(JobId); stores_.erase(JobId + "-writes"); return Unit{};
            }
        private:
            Result<std::shared_ptr<IItemOutcomeStore>> openItems(const std::string& JobId)
            {
                if (auto Existing = stores_[JobId].lock()) return Existing;
                auto Opened = OpenDurableOutcomes(QDir(directory_).filePath(S(JobId) + ".items"));
                if (Opened.HasValue()) stores_[JobId] = Opened.Value();
                return Opened;
            }
            std::map<std::string, std::weak_ptr<IItemOutcomeStore>> stores_;
            QString directory_; std::mutex mutex_;
        };
    }
    std::shared_ptr<IJobJournal> MakeFileJobJournal(QString Directory) { return std::make_shared<FileJournal>(std::move(Directory)); }
}
