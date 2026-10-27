#include "qtJobJournal.hpp"
#include "operationMetrics.hpp"
#include <QDir>
#include <QFile>
#include <QSaveFile>
#include <QThread>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QLockFile>
#include "qtItemOutcomeStore.hpp"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

namespace hc::operations
{
    using namespace storage;
    namespace
    {
        Error Failure(const char* Code) { return {ErrorCategory::Unavailable, Code, {"operation-journal"}, {}, false, "check-journal-directory"}; }
        struct FileStamp { uint64_t size_=0,modified_=0; DWORD attributes_=0; bool operator==(const FileStamp&) const = default; };
        std::optional<FileStamp> Stamp(const QString& Path) {
            WIN32_FILE_ATTRIBUTE_DATA Data{};
            if (!GetFileAttributesExW(reinterpret_cast<LPCWSTR>(Path.utf16()),GetFileExInfoStandard,&Data)) return {};
            return FileStamp{(uint64_t(Data.nFileSizeHigh)<<32)|Data.nFileSizeLow,
                (uint64_t(Data.ftLastWriteTime.dwHighDateTime)<<32)|Data.ftLastWriteTime.dwLowDateTime,Data.dwFileAttributes};
        }
        QString S(const std::string& Value) { return QString::fromStdString(Value); }
        bool Id(const std::string& Value)
        { return Value.starts_with("hc-job-") && Value.size() < 128 && std::ranges::all_of(Value, [](char Ch) { return (Ch >= 'a' && Ch <= 'z') || (Ch >= '0' && Ch <= '9') || Ch == '-'; }); }
        QJsonObject EncodeError(const Error& Value)
        { return {{"category", int(Value.category_)}, {"code", S(Value.code_)}, {"provider", S(Value.provider_.value_)}, {"action", S(Value.userAction_)}}; }
        Error DecodeError(QJsonObject Value)
        { return {ErrorCategory(Value["category"].toInt(int(ErrorCategory::Internal))), Value["code"].toString().toStdString(), {Value["provider"].toString().toStdString()}, {}, false, Value["action"].toString().toStdString()}; }
        QJsonObject Encode(const JobResult& Result, bool SaveProgress)
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
            Out.insert("metadata", QJsonObject{{"label", S(Snapshot.metadata_.label_)}, {"source", S(Snapshot.metadata_.source_)}, {"destination", S(Snapshot.metadata_.destination_)}, {"options", S(Snapshot.metadata_.options_)}, {"retryOf", S(Snapshot.metadata_.retryOf_)}});
            auto Presentation = Out["metadata"].toObject(); Presentation.insert("kind", int(Snapshot.metadata_.kind_)); Out.insert("metadata", Presentation);
            Out.insert("currentSource", S(Snapshot.progress_.source_)); Out.insert("currentDestination", S(Snapshot.progress_.destination_));
            Out.insert("phase", int(Snapshot.progress_.phase_)); Out.insert("activeFiles", int(Snapshot.progress_.activeFiles_));
            if (Snapshot.progress_.itemsDone_) Out.insert("itemsDone", QString::number(*Snapshot.progress_.itemsDone_));
            if (Snapshot.progress_.outputIndex_) Out.insert("outputIndex", QString::number(*Snapshot.progress_.outputIndex_));
            if (Snapshot.progress_.outputCount_) Out.insert("outputCount", QString::number(*Snapshot.progress_.outputCount_));
            Out.insert("deferred", Snapshot.deferred_); Out.insert("order", QString::number(Snapshot.queueOrder_));
            Out.insert("submittedUtcMs", QString::number(Snapshot.submittedUtcMs_)); Out.insert("retriedAs", S(Snapshot.retriedAs_));
            Out.insert("samplePolicy", "recent-600-at-500ms; rate-window-2000ms; samples-session-only");
            if (Snapshot.progress_.bytesTotal_) Out.insert("total", QString::number(*Snapshot.progress_.bytesTotal_));
            if (Snapshot.progress_.plannedItems_) Out.insert("planned", QString::number(*Snapshot.progress_.plannedItems_));
            if (Snapshot.progress_.discoveredItems_) Out.insert("discoveredItems", QString::number(*Snapshot.progress_.discoveredItems_));
            if (Snapshot.progress_.discoveredFiles_) Out.insert("discoveredFiles", QString::number(*Snapshot.progress_.discoveredFiles_));
            if (Snapshot.progress_.deletedFiles_) Out.insert("deletedFiles", QString::number(*Snapshot.progress_.deletedFiles_));
            Out.insert("currentBytes", QString::number(Snapshot.progress_.currentBytes_));
            if (Snapshot.progress_.currentTotal_) Out.insert("currentTotal", QString::number(*Snapshot.progress_.currentTotal_));
            if (Snapshot.error_) Out.insert("error", EncodeError(*Snapshot.error_));
            Out.insert("saveProgress", SaveProgress);
            if (!SaveProgress && !IsTerminal(Snapshot.state_)) {
                for (const auto* Key : {"current", "bytes", "total", "planned", "currentBytes", "currentTotal", "discoveredItems", "discoveredFiles", "deletedFiles", "currentSource", "currentDestination", "phase", "activeFiles", "itemsDone", "outputIndex", "outputCount"}) Out.remove(Key);
                // Version 2 reconstructs counts from the durable outcome ledger.
                // Version 1 embeds outcomes and must retain their matching counts.
                if (Result.outcomes_) for (const auto* Key : {"completed", "skipped", "failed"}) Out.remove(Key);
            }
            return Out;
        }
        Result<JobResult> Decode(QJsonObject Object)
        {
            if ((Object["version"] != 1 && Object["version"] != 2) || !Id(Object["id"].toString().toStdString()) || !Object["items"].isArray() || !Object["cleanup"].isArray()) return Failure("job-journal-format");
            const int State = Object["state"].toInt(-1); if (State < 0 || State > int(JobState::Cancelled)) return Failure("job-journal-state");
            JobResult Out; auto& Snapshot = Out.snapshot_; Snapshot.id_ = Object["id"].toString().toStdString(); Snapshot.correlationId_ = Object["correlation"].toString().toStdString();
            Snapshot.state_ = JobState(State); Snapshot.progress_.currentItem_ = Object["current"].toString().toStdString();
            Snapshot.progress_.bytesDone_ = Object["bytes"].toString().toULongLong(); Snapshot.completedItems_ = Object["completed"].toString().toULongLong();
            Snapshot.skippedItems_ = Object["skipped"].toString().toULongLong(); Snapshot.failedItems_ = Object["failed"].toString().toULongLong();
            const auto Metadata = Object["metadata"].toObject();
            Snapshot.metadata_ = {Metadata["label"].toString().toStdString(), Metadata["source"].toString().toStdString(), Metadata["destination"].toString().toStdString(), Metadata["options"].toString().toStdString(), Metadata["retryOf"].toString().toStdString()};
            const auto Kind = Metadata["kind"].toInt();
            if (Kind >= 0 && Kind <= int(JobKind::SecurityRestore)) Snapshot.metadata_.kind_ = JobKind(Kind);
            Snapshot.progress_.source_ = Object["currentSource"].toString().toStdString();
            Snapshot.progress_.destination_ = Object["currentDestination"].toString().toStdString();
            const auto Phase = Object["phase"].toInt();
            if (Phase >= 0 && Phase <= int(JobPhase::RestoringSecurity)) Snapshot.progress_.phase_ = JobPhase(Phase);
            Snapshot.progress_.activeFiles_ = uint32_t(std::max(0, Object["activeFiles"].toInt()));
            if (Object.contains("itemsDone")) Snapshot.progress_.itemsDone_ = Object["itemsDone"].toString().toULongLong();
            if (Object.contains("outputIndex")) Snapshot.progress_.outputIndex_ = Object["outputIndex"].toString().toULongLong();
            if (Object.contains("outputCount")) Snapshot.progress_.outputCount_ = Object["outputCount"].toString().toULongLong();
            Snapshot.deferred_ = Object["deferred"].toBool(); Snapshot.queueOrder_ = Object["order"].toString().toULongLong();
            Snapshot.submittedUtcMs_ = Object["submittedUtcMs"].toString().toLongLong(); Snapshot.retriedAs_ = Object["retriedAs"].toString().toStdString();
            if (Object.contains("total")) Snapshot.progress_.bytesTotal_ = Object["total"].toString().toULongLong();
            if (Object.contains("planned")) Snapshot.progress_.plannedItems_ = Object["planned"].toString().toULongLong();
            if (Object.contains("discoveredItems")) Snapshot.progress_.discoveredItems_ = Object["discoveredItems"].toString().toULongLong();
            if (Object.contains("discoveredFiles")) Snapshot.progress_.discoveredFiles_ = Object["discoveredFiles"].toString().toULongLong();
            if (Object.contains("deletedFiles")) Snapshot.progress_.deletedFiles_ = Object["deletedFiles"].toString().toULongLong();
            Snapshot.progress_.currentBytes_ = Object["currentBytes"].toString().toULongLong();
            if (Object.contains("currentTotal")) Snapshot.progress_.currentTotal_ = Object["currentTotal"].toString().toULongLong();
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
            { Snapshot.deferred_ = false; Snapshot.state_ = JobState::Failed; Snapshot.error_ = Error{ErrorCategory::Unavailable, "job-interrupted", {"operation-journal"}, {}, false, "inspect-destination-before-retry"}; }
            return Out;
        }
        class FileJournal final : public IJobJournal
        {
        public:
            explicit FileJournal(QString Directory, bool SaveProgress, bool OptimizeRecoveryIo, bool EnableRecovery) : directory_(std::move(Directory)), optimizeRecoveryIo_(OptimizeRecoveryIo), saveProgress_(SaveProgress), enableRecovery_(EnableRecovery) {}
            void SetProgressPersistence(bool SaveProgress) { std::lock_guard Lock(mutex_); saveProgress_ = SaveProgress; }
            void SetPersistence(bool EnableRecovery, bool SaveProgress) { std::lock_guard Lock(mutex_); enableRecovery_=EnableRecovery; saveProgress_=SaveProgress; }
            Result<std::shared_ptr<IItemOutcomeStore>> OpenItems(const std::string& JobId) override
            {
                if (!Id(JobId)) return Failure("job-journal-id");
                std::lock_guard Lock(mutex_);
                auto Policy=policy(JobId); if (!Policy.HasValue()) return Policy.Failure();
                if (!Policy.Value().enableRecovery_) {
                    if (auto Existing=stores_[JobId].lock()) return Existing;
                    // Bounded, session-only result pages retain review and undo
                    // without authoritative frames or durability barriers.
                    auto Opened=PagedItemOutcomeStore::Create(QDir::tempPath());
                    if (Opened.HasValue()) stores_[JobId]=Opened.Value();
                    return Opened;
                }
                if (!QDir::isAbsolutePath(directory_) || !QDir().mkpath(directory_)) return Failure("job-journal-directory");
                return openItems(JobId);
            }
            Result<std::shared_ptr<IItemOutcomeStore>> OpenWrites(const std::string& JobId) override
            {
                if (!Id(JobId)) return Failure("job-journal-id");
                std::lock_guard Lock(mutex_);
                auto Policy=policy(JobId); if (!Policy.HasValue()) return Policy.Failure();
                if (!Policy.Value().enableRecovery_) return std::shared_ptr<IItemOutcomeStore>{};
                if (!QDir::isAbsolutePath(directory_) || !QDir().mkpath(directory_)) return Failure("job-journal-directory");
                return openItems(JobId+"-writes");
            }
            Result<Unit> Checkpoint(const JobResult& Result) override
            {
                OperationMetric Timing("checkpoint.inclusive");
                if (!Id(Result.snapshot_.id_)) return Failure("job-journal-id");
                std::lock_guard Lock(mutex_);
                auto Policy=policy(Result.snapshot_.id_); if (!Policy.HasValue()) return Policy.Failure();
                if (!Policy.Value().enableRecovery_) { OperationMetric Disabled("checkpoint.disabled"); return Unit{}; }
                const auto Bytes = QJsonDocument(Encode(Result, Policy.Value().saveProgress_)).toJson(QJsonDocument::Compact);
                if (Bytes.size() > 16 * 1024 * 1024) return Failure("job-journal-record-budget");
                const auto EnsureDirectory=[this] { OperationMetric DirectoryTiming("checkpoint.directory.nested"); return QDir().mkpath(directory_); };
                if (!QDir::isAbsolutePath(directory_) || (!optimizeRecoveryIo_ && !EnsureDirectory())) return Failure("job-journal-directory");
                const auto Path = QDir(directory_).filePath(S(Result.snapshot_.id_) + ".json");

                // Keep independent exact records so interleaved jobs also skip
                // progress-only saves. Total cache memory is bounded to 16 MiB.
                // External disk changes and writer locks always invalidate reuse.
                const auto Cached = checkpoints_.find(Result.snapshot_.id_);
                bool Unchanged=false;
                if (Cached!=checkpoints_.end() && Cached->second.bytes_==Bytes && !QFile::exists(QDir(directory_).filePath("writer.lock"))) {
                    OperationMetric MetadataTiming("checkpoint.metadata.nested");
                    if (optimizeRecoveryIo_) { const auto Current=Stamp(Path); Unchanged=Current && Cached->second.stamp_==Current
                        && !(Current->attributes_ & (FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY)) && Current->size_==uint64_t(Bytes.size()); }
                    else { const QFileInfo Existing(Path); Unchanged=Existing.exists() && !Existing.isSymLink() && Existing.size()==Bytes.size() && Existing.lastModified()==Cached->second.modified_; }
                }
                // A successful exact-record check already proves the parent exists.
                // Preserve external-change/lock validation before skipping any save.
                if (Unchanged) { OperationMetric Skipped("checkpoint.unchanged"); return Unit{}; }
                if (optimizeRecoveryIo_ && !EnsureDirectory()) return Failure("job-journal-directory");
                QLockFile Guard(QDir(directory_).filePath("writer.lock")); if (!Guard.tryLock(1000)) return Failure("job-journal-busy");
                if (!QFile::exists(Path) && QDir(directory_).entryList({"*.json"}, QDir::Files).size() >= 1024) return Failure("job-journal-history-full");
                // QSaveFile has no direct-write fallback: a failed checkpoint
                // leaves the previous evidence intact, including commit intent.
                for (int Attempt=0; Attempt<5; ++Attempt) {
                    OperationMetric SaveTiming("checkpoint.save",Bytes.size());
                    QSaveFile File(Path); File.setDirectWriteFallback(false);
                    if (File.open(QIODevice::WriteOnly) && File.write(Bytes)==Bytes.size() && File.commit()) {
                        if (Cached!=checkpoints_.end()) { cachedSize_-=Cached->second.bytes_.size(); checkpoints_.erase(Cached); }
                        if (cachedSize_+Bytes.size()>16*1024*1024 || checkpoints_.size()>=1024) { checkpoints_.clear(); cachedSize_=0; }
                        checkpoints_[Result.snapshot_.id_]={Bytes,optimizeRecoveryIo_?QDateTime{}:QFileInfo(Path).lastModified(),optimizeRecoveryIo_?Stamp(Path):std::optional<FileStamp>{}}; cachedSize_+=Bytes.size(); return Unit{};
                    }
                    const auto Error=File.error();
                    // A Windows reader/scanner can briefly deny atomic replace.
                    // Keep the old evidence and retry only bounded metadata errors.
                    if (Attempt==4 || (Error!=QFileDevice::RenameError && Error!=QFileDevice::PermissionsError && Error!=QFileDevice::OpenError)) {
                        qWarning() << "Job checkpoint failed" << Error << File.errorString(); return Failure("job-journal-write");
                    }
                    QThread::msleep(20*(Attempt+1));
                }
                return Failure("job-journal-write");
            }
            Result<std::vector<JobResult>> Recover() override
            {
                std::lock_guard Lock(mutex_); std::vector<JobResult> Results;
                // Disabled startup leaves old records intact for a later enabled start.
                if (!enableRecovery_ || !QDir(directory_).exists()) return Results;
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
                stores_.erase(JobId); stores_.erase(JobId + "-writes"); policies_.erase(JobId);
                if (!QDir(directory_).exists()) return Unit{};
                QLockFile Guard(QDir(directory_).filePath("writer.lock")); if (!Guard.tryLock(1000)) return Failure("job-journal-busy");
                if (QFile::exists(Path) && !QFile::remove(Path)) return Failure("job-journal-remove");
                const auto Items = QDir(directory_).filePath(S(JobId) + ".items");
                if (QFile::exists(Items) && !QFile::remove(Items)) return Failure("job-outcomes-remove");
                const auto Writes = QDir(directory_).filePath(S(JobId) + "-writes.items");
                if (QFile::exists(Writes) && !QFile::remove(Writes)) return Failure("job-writes-remove");
                stores_.erase(JobId); stores_.erase(JobId + "-writes"); policies_.erase(JobId);
                if (const auto Cached=checkpoints_.find(JobId); Cached!=checkpoints_.end()) { cachedSize_-=Cached->second.bytes_.size(); checkpoints_.erase(Cached); }
                return Unit{};
            }
        private:
            struct PersistencePolicy { bool saveProgress_=false,enableRecovery_=true; };
            Result<PersistencePolicy> policy(const std::string& JobId)
            {
                if (!policies_.contains(JobId) && policies_.size()>=1024) return Failure("job-journal-history-full");
                return policies_.try_emplace(JobId,PersistencePolicy{saveProgress_,enableRecovery_}).first->second;
            }
            Result<std::shared_ptr<IItemOutcomeStore>> openItems(const std::string& JobId)
            {
                if (auto Existing = stores_[JobId].lock()) return Existing;
                auto Opened = OpenDurableOutcomes(QDir(directory_).filePath(S(JobId) + ".items"),optimizeRecoveryIo_);
                if (Opened.HasValue()) stores_[JobId] = Opened.Value();
                return Opened;
            }
            std::map<std::string, std::weak_ptr<IItemOutcomeStore>> stores_;
            QString directory_; std::mutex mutex_;
            bool optimizeRecoveryIo_=true;
            bool saveProgress_ = false,enableRecovery_=true; std::map<std::string, PersistencePolicy> policies_;
            struct CachedCheckpoint { QByteArray bytes_; QDateTime modified_; std::optional<FileStamp> stamp_; };
            std::map<std::string,CachedCheckpoint> checkpoints_; qsizetype cachedSize_=0;
        };
    }
    std::shared_ptr<IJobJournal> MakeFileJobJournal(QString Directory, bool SaveProgress, bool OptimizeRecoveryIo, bool EnableRecovery) { return std::make_shared<FileJournal>(std::move(Directory), SaveProgress,OptimizeRecoveryIo,EnableRecovery); }
    void SetFileJobPersistence(const std::shared_ptr<IJobJournal>& Journal, bool EnableRecovery, bool SaveProgress)
    { if (auto File=std::dynamic_pointer_cast<FileJournal>(Journal)) File->SetPersistence(EnableRecovery,SaveProgress); }
    void SetFileJobProgressPersistence(const std::shared_ptr<IJobJournal>& Journal, bool SaveProgress)
    { if (auto File = std::dynamic_pointer_cast<FileJournal>(Journal)) File->SetProgressPersistence(SaveProgress); }
}
