#include "batchRenameJournal.hpp"
#include "settings/appSettings.hpp"
#include <io.h>

namespace hc::filetools {
using namespace storage;
using namespace operations;
namespace {
constexpr qint64 PlanBudget = 128 * 1024 * 1024;
constexpr qint64 LogBudget = 64 * 1024 * 1024;
Error Failure(const QString& Code) { return {ErrorCategory::Unavailable, Code.toStdString(), {"batch-rename"}, {}, false, "inspect-rename-history"}; }
bool ValidId(const QString& Id) { return QRegularExpression("^[0-9a-f]{32}$").match(Id).hasMatch(); }
QString RecordPath(const QString& Directory, const QString& Id, const QString& Suffix) { return QDir(Directory).filePath(Id + Suffix); }
bool Durable(QFileDevice& File) {
    if (!File.flush() || File.handle() < 0) return false;
    const auto Native = _get_osfhandle(File.handle());
    return Native != -1 && FlushFileBuffers(reinterpret_cast<HANDLE>(Native));
}
Result<Unit> SavePlan(const QString& Directory, const RenameRecord& Record) {
    if (!QDir::isAbsolutePath(Directory) || !QDir().mkpath(Directory)) return Failure("rename-journal-directory");
    QJsonArray Rows;
    for (int I = 0; I < Record.rows_.size(); ++I) {
        const auto& Row = Record.rows_[I];
        Rows.append(QJsonObject{{"source", Row.source_.path_}, {"target", Row.target_}, {"temporary", Record.temporary_[I]},
            {"identity", QString::fromLatin1(Row.source_.identity_)}, {"modified", QString::number(Row.source_.modified_.toMSecsSinceEpoch())},
            {"directory", Row.source_.directory_}, {"size", QString::number(Row.source_.size_)}});
    }
    const QJsonObject Object{{"version", 1}, {"id", Record.id_}, {"folder", Record.folder_}, {"folderIdentity", QString::fromLatin1(Record.folderIdentity_)},
        {"created", QString::number(Record.createdUtcMs_)}, {"parent", Record.parentId_}, {"kind", Record.kind_}, {"rows", Rows}};
    const auto Bytes = QJsonDocument(Object).toJson(QJsonDocument::Compact);
    if (Bytes.size() > PlanBudget) return Failure("rename-plan-budget");
    QSaveFile File(RecordPath(Directory, Record.id_, ".json"));
    if (!File.open(QIODevice::WriteOnly) || File.write(Bytes) != Bytes.size() || !Durable(File) || !File.commit()) return Failure("rename-plan-save");
    return Unit{};
}
Result<RenameRecord> Read(const QString& Directory, const QString& Id, bool Summary) {
    if (!QDir::isAbsolutePath(Directory) || !ValidId(Id)) return Failure("rename-record-id");
    QFile File(RecordPath(Directory, Id, ".json"));
    if (!File.open(QIODevice::ReadOnly) || File.size() > PlanBudget) return Failure("rename-plan-read");
    QJsonParseError Parse;
    const auto Object = QJsonDocument::fromJson(File.readAll(), &Parse).object();
    if (Parse.error != QJsonParseError::NoError || Object["version"] != 1 || Object["id"] != Id || !Object["rows"].isArray()) return Failure("rename-plan-format");
    RenameRecord Record; Record.id_ = Id; Record.folder_ = Object["folder"].toString(); Record.folderIdentity_ = Object["folderIdentity"].toString().toLatin1();
    Record.createdUtcMs_ = Object["created"].toString().toLongLong(); Record.parentId_ = Object["parent"].toString(); Record.kind_ = Object["kind"].toString();
    const auto Rows = Object["rows"].toArray(); Record.count_ = Rows.size();
    if (!NativeFolder(Record.folder_) || Record.folderIdentity_.isEmpty() || Rows.isEmpty() || Rows.size() > 100000) return Failure("rename-plan-scope");
    QSet<QString> Sources, Targets, Identities, Temporary;
    for (int I = 0; I < Rows.size(); ++I) {
        const auto Value = Rows[I].toObject(); RenameRow Row; Row.source_.path_ = Value["source"].toString(); Row.target_ = Value["target"].toString();
        Row.source_.identity_ = Value["identity"].toString().toLatin1(); Row.source_.directory_ = Value["directory"].toBool();
        bool ValidSize = false, ValidTime = false; Row.source_.size_ = Value["size"].toString().toULongLong(&ValidSize);
        Row.source_.modified_ = QDateTime::fromMSecsSinceEpoch(Value["modified"].toString().toLongLong(&ValidTime));
        const auto Temp = Value["temporary"].toString();
        for (const auto& Path : {Row.source_.path_, Row.target_, Temp})
            if (QDir::cleanPath(Path) != Path || QFileInfo(Path).absolutePath().compare(Record.folder_, Qt::CaseInsensitive)
                || !NameError(QFileInfo(Path).fileName()).isEmpty()) return Failure("rename-plan-path");
        if (Temp != QDir(Record.folder_).filePath(QString(".hc-rename-%1-%2").arg(Id).arg(I)) || Row.source_.identity_.isEmpty()
            || !ValidSize || !ValidTime || Sources.contains(Row.source_.path_.toCaseFolded()) || Targets.contains(Row.target_.toCaseFolded())
            || Identities.contains(QString::fromLatin1(Row.source_.identity_)) || Temporary.contains(Temp.toCaseFolded())) return Failure("rename-plan-row");
        Sources.insert(Row.source_.path_.toCaseFolded()); Targets.insert(Row.target_.toCaseFolded());
        Identities.insert(QString::fromLatin1(Row.source_.identity_)); Temporary.insert(Temp.toCaseFolded());
        if (!Summary) { Record.rows_.append(Row); Record.temporary_.append(Temp); }
    }
    for (const auto& Temp : Temporary) if (Sources.contains(Temp) || Targets.contains(Temp)) return Failure("rename-plan-temporary-collision");
    QFile Log(RecordPath(Directory, Id, ".log"));
    if (Log.exists()) {
        if (!Log.open(QIODevice::ReadOnly) || Log.size() > LogBudget) return Failure("rename-log-read");
        while (!Log.atEnd()) {
            const auto Line = Log.readLine(8193);
            if (!Line.endsWith('\n')) { if (Log.atEnd()) break; return Failure("rename-log-line"); }
            const auto Event = QJsonDocument::fromJson(Line, &Parse).object();
            if (Parse.error != QJsonParseError::NoError) return Failure("rename-log-format");
            if (Event["event"] == "state") {
                const int State = Event["state"].toInt(-1);
                if (State < 0 || State > int(RenameState::RecoveryRequired)) return Failure("rename-log-state");
                Record.state_ = RenameState(State); Record.error_ = Event["error"].toString(); Record.jobId_ = Event["job"].toString();
            }
        }
    }
    return Record;
}

class RenameTask final : public IJobTask {
public:
    RenameTask(QString Directory, RenameRecord Record, bool Recovery, bool Restore)
        : directory_(std::move(Directory)), record_(std::move(Record)), recovery_(Recovery), restore_(Restore),
          lock_(RecordPath(directory_, record_.id_, ".lock")) { lock_.setStaleLockTime(0); }
    JobStep Advance(JobStepContext Context) override {
        record_.jobId_ = QString::fromStdString(Context.jobId_);
        if (!lockPath_.isEmpty()) {
            if (!Context.answer_) {
                JobStep Step{JobState::WaitingForUser};
                Step.interaction_ = InteractionRequest{"", InteractionKind::RetryFailure, "file-in-use:win32:" + std::to_string(lockCode_),
                    {InteractionChoice::Retry,InteractionChoice::Cancel},{},lockPath_.toStdString(),{}};
                return Step;
            }
            if (Context.answer_->choice_ != InteractionChoice::Retry) return fail("rename-lock-cancelled");
            lockPath_.clear(); pinned_ = 0;
            const auto PinError = PinNativeParents(record_.folder_, parents_);
            if (!PinError.isEmpty()) return fail(PinError);
            if (Identity(parents_.back()->value_) != record_.folderIdentity_) return fail("rename-parent-replaced");
            return running();
        }
        if (!started_) {
            if (!lock_.tryLock()) return fail("rename-record-busy");
            auto Fresh = Read(directory_, record_.id_, false);
            if (!Fresh.HasValue()) return fail(QString::fromStdString(Fresh.Failure().code_));
            // Recovery can only be requested explicitly. Never turn a completed
            // record into a replay because a second worker used a stale UI row.
            if ((recovery_ && (Fresh.Value().state_ == RenameState::Completed || Fresh.Value().state_ == RenameState::Restored))
                || (!recovery_ && Fresh.Value().state_ != RenameState::Queued)) return fail("rename-record-state-changed");
            owns_ = true;
            const auto PinError = PinNativeParents(record_.folder_, parents_);
            if (!PinError.isEmpty()) return fail(PinError);
            if (Identity(parents_.back()->value_) != record_.folderIdentity_) return fail("rename-parent-replaced");
            if (!openLog() || !state(RenameState::Running)) return fail("rename-log-save");
            started_ = true;
            current_.resize(record_.rows_.size());
            if (recovery_) {
                for (const auto& Row : record_.rows_) { candidates_.append(Row.source_.path_); candidates_.append(Row.target_); }
                candidates_.append(record_.temporary_); candidates_.removeDuplicates();
                for (int I = 0; I < record_.rows_.size(); ++I) byIdentity_.insert(record_.rows_[I].source_.identity_, I);
            }
            return running();
        }
        if (recovery_ && candidate_ < candidates_.size()) {
            // Bounded identity discovery. No directory sweep or path-only trust.
            for (int N = 0; N < 32 && candidate_ < candidates_.size(); ++N) {
                const auto Entry = Inspect(candidates_[candidate_++]); const auto Found = byIdentity_.constFind(Entry.identity_);
                if (Found == byIdentity_.cend()) continue;
                const auto I = Found.value();
                if (!current_[I].isEmpty() && current_[I].compare(Entry.path_, Qt::CaseInsensitive)) return fail("rename-identity-ambiguous");
                current_[I] = Entry.path_;
            }
            return running();
        }
        if (pinned_ < record_.rows_.size()) {
            const auto I = pinned_; const auto& Row = record_.rows_[I];
            const auto Path = recovery_ ? current_[I] : Row.source_.path_;
            if (Path.isEmpty()) return fail("rename-source-missing");
            const auto Native = NativePath(Path);
            auto File = std::make_unique<Handle>(CreateFileW(reinterpret_cast<LPCWSTR>(Native.utf16()), DELETE | FILE_READ_ATTRIBUTES, FILE_SHARE_READ,
                nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
            const auto OpenError = File->value_ == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
            if (!mutated_ && Context.operation_.providerInteractions_ && LocalFolder(record_.folder_)
                && (OpenError == ERROR_SHARING_VIOLATION || OpenError == ERROR_LOCK_VIOLATION)) {
                handles_.clear(); parents_.clear(); lockPath_ = Path; lockCode_ = OpenError;
                return running();
            }
            const auto Actual = Inspect(Path);
            if (File->value_ == INVALID_HANDLE_VALUE || Identity(File->value_) != Row.source_.identity_ || Actual.identity_ != Row.source_.identity_
                || Actual.modified_ != Row.source_.modified_ || Actual.size_ != Row.source_.size_ || Actual.directory_ != Row.source_.directory_)
                return fail("rename-source-changed: " + Path);
            current_[I] = Path; handles_.push_back(std::move(File)); ++pinned_; return running();
        }
        // Scheduler checkpoints Committing before entering the actual rename.
        // Pause/cancel is honored again between every two rename operations.
        if (!Context.committing_) { auto Step = running(); Step.next_ = JobState::Committing; return Step; }
        const int Count = record_.rows_.size();
        if (phase_ < 2 && move_ < Count) {
            const auto I = move_++; const auto Desired = restore_ ? record_.rows_[I].source_.path_ : record_.rows_[I].target_;
            const auto Target = phase_ == 0 ? record_.temporary_[I] : Desired;
            if (record_.rows_[I].source_.path_ != record_.rows_[I].target_ && current_[I] != Target) {
                auto Changed = move(I, Target); if (!Changed.isEmpty()) return fail(Changed);
            }
            if (move_ == Count) { ++phase_; move_ = 0; }
            return running();
        }
        if (!completed_) {
            if (!state(restore_ ? RenameState::Restored : RenameState::Completed)) return fail("rename-log-completion");
            completed_ = true;
        }
        if (outcome_ < Count) {
            const auto I = outcome_++; auto Step = running();
            Step.item_ = ItemOutcome{current_[I].toStdString(), ItemOutcome::Status::Completed};
            Step.item_->destinationCommitted_ = true; Step.progress_->currentItem_ = current_[I].toStdString(); return Step;
        }
        auto Step = running(); Step.next_ = JobState::Completed; return Step;
    }
    std::vector<Error> Finalize(JobState State) override {
        std::vector<Error> Errors;
        if (!owns_ && !started_ && !recovery_ && lock_.tryLock()) {
            auto Fresh = Read(directory_, record_.id_, false);
            if (Fresh.HasValue() && Fresh.Value().state_ == RenameState::Queued) owns_ = true;
            else lock_.unlock();
        }
        if (!owns_) return Errors;
        // A durably completed rename is retained even if scheduler publication
        // is interrupted. The independent receipt is the history authority.
        if (!completed_ && State != JobState::Completed) {
            bool Restored = true;
            if (mutated_) {
                for (int I = 0; I < int(handles_.size()); ++I)
                    if (current_[I] != record_.rows_[I].source_.path_ && current_[I] != record_.temporary_[I]) {
                        const auto Error = move(I, record_.temporary_[I]);
                        if (!Error.isEmpty()) { Restored = false; Errors.push_back(Failure(Error)); }
                    }
                for (int I = 0; I < int(handles_.size()); ++I) if (current_[I] != record_.rows_[I].source_.path_) {
                    const auto Error = move(I, record_.rows_[I].source_.path_);
                    if (!Error.isEmpty()) { Restored = false; Errors.push_back(Failure(Error)); }
                }
            } else if (recovery_) Restored = false; // Inspection failure must not hide interrupted evidence.
            if (!state(Restored ? RenameState::Restored : RenameState::RecoveryRequired)) Errors.push_back(Failure("rename-log-finalize"));
        }
        handles_.clear(); parents_.clear(); log_.reset(); lock_.unlock(); owns_ = false; return Errors;
    }
private:
    bool openLog() {
        const auto Path = NativePath(RecordPath(directory_, record_.id_, ".log"));
        log_ = std::make_unique<Handle>(CreateFileW(reinterpret_cast<LPCWSTR>(Path.utf16()), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ,
            nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (log_->value_ == INVALID_HANDLE_VALUE) return false;
        // A process crash may leave only a suffix of the next receipt. Remove
        // that uncommitted suffix before appending; completed lines are immutable.
        QFile ReadLog(RecordPath(directory_, record_.id_, ".log"));
        if (!ReadLog.open(QIODevice::ReadOnly) || ReadLog.size() > LogBudget) return false;
        auto Bytes = ReadLog.readAll(); ReadLog.close(); const auto End = Bytes.lastIndexOf('\n') + 1;
        LARGE_INTEGER At{}; At.QuadPart = End;
        return SetFilePointerEx(log_->value_, At, nullptr, FILE_BEGIN) && SetEndOfFile(log_->value_) && FlushFileBuffers(log_->value_);
    }
    bool append(QJsonObject Event) {
        if (logUncertain_ || !log_ || log_->value_ == INVALID_HANDLE_VALUE) return false;
        const auto Bytes = QJsonDocument(Event).toJson(QJsonDocument::Compact) + '\n';
        LARGE_INTEGER Size{}; if (!GetFileSizeEx(log_->value_, &Size) || Size.QuadPart + Bytes.size() > LogBudget) return false;
        DWORD Written = 0;
        if (WriteFile(log_->value_, Bytes.constData(), DWORD(Bytes.size()), &Written, nullptr) && Written == Bytes.size() && FlushFileBuffers(log_->value_)) return true;
        // Do not append after a partial receipt: that would turn a recoverable
        // suffix into a malformed committed line. Stop all mutation if repair
        // cannot restore the last durable boundary.
        logUncertain_ = !(SetFilePointerEx(log_->value_, Size, nullptr, FILE_BEGIN) && SetEndOfFile(log_->value_) && FlushFileBuffers(log_->value_));
        return false;
    }
    bool state(RenameState State) {
        if (!log_ && !openLog()) return false;
        if (!append({{"event", "state"}, {"state", int(State)}, {"job", record_.jobId_}, {"error", record_.error_.left(2048)}})) return false;
        record_.state_ = State; return true;
    }
    QString move(int I, const QString& Target) {
        // Paths are immutable in the plan. Store the destination role so a
        // 100,000-item plan cannot exhaust receipts just because its path is long.
        const auto Role = Target == record_.rows_[I].source_.path_ ? "source" : Target == record_.temporary_[I] ? "temporary" : "target";
        if (!append({{"event", "intent"}, {"index", I}, {"destination", Role}})) return "rename-log-intent";
        const auto Error = RenameHandle(handles_[I]->value_, Target);
        if (Error) return QString("rename-failed: Windows %1 · %2").arg(Error).arg(Target);
        current_[I] = Target; mutated_ = true;
        if (!append({{"event", "moved"}, {"index", I}, {"destination", Role}})) return "rename-log-receipt";
        return {};
    }
    JobStep running() const {
        JobStep Step; Step.progress_ = JobProgress{}; Step.progress_->plannedItems_ = record_.rows_.size();
        const auto Count = int(record_.rows_.size());
        const int Index = Count ? std::min(phase_ < 2 ? move_ : outcome_, Count - 1) : 0;
        Step.progress_->itemsDone_ = phase_ == 0 ? 0 : phase_ == 1 ? move_ : Count;
        Step.progress_->phase_ = pinned_ < Count ? JobPhase::Inspecting : phase_ == 0 ? JobPhase::TemporaryNames
            : restore_ ? JobPhase::RestoringNames : JobPhase::ApplyingNames;
        if (Count) {
            const auto& Row = record_.rows_[Index];
            Step.progress_->source_ = (Index < current_.size() && !current_[Index].isEmpty() ? current_[Index] : Row.source_.path_).toStdString();
            Step.progress_->destination_ = (restore_ ? Row.source_.path_ : Row.target_).toStdString();
            Step.progress_->currentItem_ = QFileInfo(QString::fromStdString(Step.progress_->source_)).fileName().toStdString();
        }
        return Step;
    }
    JobStep fail(const QString& Error) { record_.error_ = Error; JobStep Step; Step.next_ = JobState::Failed; Step.error_ = Failure(Error); return Step; }
    QString directory_;
    RenameRecord record_;
    bool recovery_ = false, restore_ = false, owns_ = false, started_ = false, mutated_ = false, completed_ = false, logUncertain_ = false;
    QLockFile lock_;
    std::unique_ptr<Handle> log_;
    std::vector<std::unique_ptr<Handle>> parents_, handles_;
    QStringList current_, candidates_;
    QString lockPath_;
    DWORD lockCode_ = 0;
    QHash<QByteArray, int> byIdentity_;
    int candidate_ = 0, pinned_ = 0, phase_ = 0, move_ = 0, outcome_ = 0;
};
PreparedJob Job(const QString& Directory, RenameRecord Record, bool Recovery, bool Restore) {
    PreparedJob Out; Out.request_.resourceLanes_ = {FileOperationLane({NativeFolder(Record.folder_) && !LocalFolder(Record.folder_) ? LocatorKind::Smb : LocatorKind::Local, Record.folder_.toStdString()})};
    Out.request_.metadata_.source_ = Record.folder_.toStdString(); Out.request_.metadata_.destination_ = Record.folder_.toStdString();
    Out.request_.metadata_.options_ = "rename-record=" + Record.id_.toStdString(); Out.request_.maxItemResults_ = 200;
    Out.request_.metadata_.kind_ = Recovery ? (Restore ? JobKind::RenameRestore : JobKind::RenameResume)
        : Record.kind_ == "undo" ? JobKind::RenameUndo : Record.kind_ == "redo" ? JobKind::RenameRedo : JobKind::BatchRename;
    Out.task_ = std::make_unique<RenameTask>(Directory, std::move(Record), Recovery, Restore); return Out;
}
}
QString RenameJournalDirectory() { return QDir(QFileInfo(settings::AppSettings().FileName()).absolutePath()).filePath("operation-journal/batch-rename"); }
Result<RenameRecord> ReadRenameRecord(const QString& Directory, const QString& Id) { return Read(Directory, Id, false); }
Result<QList<RenameRecord>> ReadRenameHistory(const QString& Directory) {
    QList<RenameRecord> Out;
    const auto Files = QDir(Directory).entryInfoList({"*.json"}, QDir::Files, QDir::Time);
    for (const auto& File : Files) {
        if (Out.size() >= 512) break;
        auto Record = Read(Directory, File.completeBaseName(), true); if (!Record.HasValue()) return Record.Failure();
        Out.append(std::move(Record.Value()));
    }
    return Out;
}
Result<PreparedJob> PrepareBatchRename(const QString& Directory, QList<RenameRow> Rows, const QString& ParentId, const QString& Kind) {
    if (Rows.isEmpty() || Rows.size() > 100000) return Failure("rename-plan-count");
    RenameRecord Record; Record.id_ = QUuid::createUuid().toString(QUuid::Id128); Record.rows_ = std::move(Rows);
    Record.folder_ = QFileInfo(Record.rows_.first().source_.path_).absolutePath(); Record.folderIdentity_ = Inspect(Record.folder_).identity_;
    Record.createdUtcMs_ = QDateTime::currentMSecsSinceEpoch(); Record.count_ = Record.rows_.size(); Record.parentId_ = ParentId; Record.kind_ = Kind;
    for (int I = 0; I < Record.rows_.size(); ++I) Record.temporary_.append(QDir(Record.folder_).filePath(QString(".hc-rename-%1-%2").arg(Record.id_).arg(I)));
    for (const auto& Row : Record.rows_) if (!Row.error_.isEmpty()) return Failure("rename-plan-error");
    auto Saved = SavePlan(Directory, Record); if (!Saved.HasValue()) return Saved.Failure();
    auto Validated = Read(Directory, Record.id_, false);
    if (!Validated.HasValue()) { QFile::remove(RecordPath(Directory, Record.id_, ".json")); return Validated.Failure(); }
    return Job(Directory, std::move(Validated.Value()), false, false);
}
Result<PreparedJob> PrepareRenameUndo(const QString& Directory, const QString& Id) {
    auto Record = Read(Directory, Id, false); if (!Record.HasValue()) return Record.Failure();
    if (Record.Value().state_ != RenameState::Completed) return Failure("rename-undo-state");
    auto History = ReadRenameHistory(Directory); if (!History.HasValue()) return History.Failure();
    for (const auto& Later : History.Value()) if (Later.parentId_ == Id && Later.state_ == RenameState::Completed)
        return Failure("rename-already-undone-use-latest-history");
    if (Inspect(Record.Value().folder_).identity_ != Record.Value().folderIdentity_) return Failure("rename-parent-replaced");
    QList<RenameRow> Rows;
    for (const auto& Row : Record.Value().rows_) { auto Entry = Row.source_; Entry.path_ = Row.target_; Rows.append({Entry, Row.source_.path_, {}}); }
    return PrepareBatchRename(Directory, std::move(Rows), Id, Record.Value().kind_ == "undo" ? "redo" : "undo");
}
Result<PreparedJob> PrepareRenameRecovery(const QString& Directory, const QString& Id, bool RestoreOriginal) {
    auto Record = Read(Directory, Id, false); if (!Record.HasValue()) return Record.Failure();
    if (Record.Value().state_ == RenameState::Completed || Record.Value().state_ == RenameState::Restored) return Failure("rename-recovery-state");
    return Job(Directory, std::move(Record.Value()), true, RestoreOriginal);
}
}
