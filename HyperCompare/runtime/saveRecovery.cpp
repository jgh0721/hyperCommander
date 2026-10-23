#include "stdafx.h"
#include "saveRecovery.hpp"
#include "requestFile.hpp"
#include "UniqueLibs/nativeWritableStorage.hpp"
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QJsonDocument>
#include <QStandardPaths>
#include <QUuid>
#include <io.h>
#include <Windows.h>

namespace hc::compare
{
    namespace
    {
        constexpr qsizetype journalLimit = 1024 * 1024;
        const QStringList phases{"writing", "prepared", "backedUp", "published", "finished", "aborted"};
        QJsonObject encode(const storage::WriteRecoveryRecord& Record, std::uint64_t Sequence)
        {
            const auto text = [](const std::string& Value) { return QString::fromStdString(Value); };
            return {{"schema", "hypercompare.save-recovery.v1"}, {"sequence", qint64(Sequence)},
                {"phase", phases.at(int(Record.phase_))}, {"transaction", text(Record.transaction_)},
                {"kind", Record.destination_.kind_ == storage::LocatorKind::Smb ? "smb" : "local"},
                {"destination", text(Record.destination_.root_)}, {"stage", text(Record.stage_.root_)},
                {"backup", text(Record.backup_.root_)}, {"stageIdentity", text(Record.stageIdentity_)},
                {"originalIdentity", text(Record.originalIdentity_)}};
        }
        storage::WriteRecoveryRecord decode(const QJsonObject& Object)
        {
            const QStringList keys{"schema", "sequence", "phase", "transaction", "kind", "destination", "stage", "backup", "stageIdentity", "originalIdentity"};
            if (Object.size() != keys.size() || Object["schema"] != "hypercompare.save-recovery.v1") throw Failure("SaveRecoverySchema");
            for (const auto& key : keys)
            {
                if (key == "sequence") continue;
                if (!Object[key].isString() || Object[key].toString().size() > 32767 || Object[key].toString().contains(QChar{})) throw Failure("SaveRecoveryField");
                if (key != "originalIdentity" && Object[key].toString().isEmpty()) throw Failure("SaveRecoveryField");
            }
            const auto phase = phases.indexOf(Object["phase"].toString());
            if (phase < 0 || !QStringList{"local", "smb"}.contains(Object["kind"].toString())) throw Failure("SaveRecoveryField");
            storage::WriteRecoveryRecord record;
            record.transaction_ = Object["transaction"].toString().toStdString(); record.phase_ = storage::WritePhase(phase);
            record.stageIdentity_ = Object["stageIdentity"].toString().toStdString(); record.originalIdentity_ = Object["originalIdentity"].toString().toStdString();
            const auto kind = Object["kind"] == "smb" ? storage::LocatorKind::Smb : storage::LocatorKind::Local;
            record.destination_ = {kind, Object["destination"].toString().toStdString()};
            record.stage_ = {kind, Object["stage"].toString().toStdString()}; record.backup_ = {kind, Object["backup"].toString().toStdString()};
            return record;
        }
        bool transition(storage::WritePhase Before, storage::WritePhase After)
        {
            using P = storage::WritePhase;
            if (After == P::Aborted) return Before == P::Writing || Before == P::Prepared || Before == P::BackedUp;
            return (Before == P::Writing && After == P::Prepared) || (Before == P::Prepared && (After == P::BackedUp || After == P::Published)) ||
                (Before == P::BackedUp && After == P::Published) || (Before == P::Published && After == P::Finished);
        }
    }
    QString SaveRecoveryDirectory()
    {
        const auto base = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
        if (base.isEmpty()) throw Failure("SaveRecoveryDirectory"); return base + "/recovery";
    }
    SaveRecoveryJournal::SaveRecoveryJournal(const QString& Directory)
    {
        const auto directory = Directory.isEmpty() ? SaveRecoveryDirectory() : Directory;
        if (directory.isEmpty() || !QDir{}.mkpath(directory)) throw Failure("SaveRecoveryDirectory");
        file_.setFileName(QDir(directory).filePath(QUuid::createUuid().toString(QUuid::WithoutBraces) + ".hcsave"));
    }
    storage::WriteCheckpoint SaveRecoveryJournal::Checkpoint()
    {
        return [this](const storage::WriteRecoveryRecord& Record, std::optional<std::uint64_t> Previous) -> storage::Result<std::uint64_t> {
            const auto fail = [this](const char* Code) { failed_ = true; return storage::Error{storage::ErrorCategory::Unavailable, Code}; };
            if (failed_) return fail("save-journal-failed");
            if ((!sequence_ && Previous) || (sequence_ && Previous != sequence_)) return fail("save-journal-sequence");
            if (!file_.isOpen() && !file_.open(QIODevice::WriteOnly | QIODevice::NewOnly)) return fail("save-journal-open");
            const auto bytes = QJsonDocument(encode(Record, sequence_ + 1)).toJson(QJsonDocument::Compact) + "\r\n";
            if (file_.size() + bytes.size() > journalLimit || file_.write(bytes) != bytes.size() || !file_.flush() || file_.handle() < 0) return fail("save-journal-write");
            const auto handle = _get_osfhandle(file_.handle());
            if (handle == -1 || !FlushFileBuffers(reinterpret_cast<HANDLE>(handle))) return fail("save-journal-flush");
            return ++sequence_;
        };
    }
    bool SaveRecoveryJournal::Remove() { file_.close(); return !Exists() || file_.remove(); }
    void SaveRecoveryJournal::Track(QJsonArray& Recovery) const
    { if (Exists() && !Recovery.contains(Path())) Recovery.append(Path()); }
    void SaveRecoveryJournal::Complete(const storage::WriteReceipt& Receipt, QJsonArray& Warnings, QJsonArray& Recovery, bool Verified)
    {
        Track(Recovery);
        for (const auto& error : Receipt.cleanupErrors_) Warnings.append(QString::fromStdString(error.code_));
        for (const auto& artifact : Receipt.preservedArtifacts_) Recovery.append(QString::fromStdString(artifact.root_));
        if (Verified && Receipt.destinationCommitted_ && Receipt.cleanupErrors_.empty() && Receipt.preservedArtifacts_.empty())
        {
            if (Remove()) { for (qsizetype i = Recovery.size(); i-- > 0;) if (Recovery[i] == Path()) Recovery.removeAt(i); }
            else Warnings.append("SaveRecoveryJournalCleanupFailed");
        }
    }
    void RecoverSavedFile(const Request& Input, Result& Value, const Budget& Limits)
    {
        Value.operation_ = "recover";
        if (!Input.report_.isEmpty() && Input.report_ != "-") throw Failure("SaveRecoveryRequiresStdout");
        if (Input.journal_.isEmpty())
        {
            const auto directory = SaveRecoveryDirectory(); Value.saveRecovery_ = {{"directory", directory}, {"identitiesVerified", false}};
            QDirIterator files(directory, {"*.hcsave"}, QDir::Files | QDir::NoSymLinks);
            while (files.hasNext())
            {
                Limits.Check(); if (Value.recovery_.size() >= 1000) throw Failure("SaveRecoveryListLimit");
                Value.recovery_.append(files.next());
            }
            Value.outcome_ = "RecoveryJournals"; Value.completion_ = "Complete"; Value.exitCode_ = 0; return;
        }
        Value.recovery_.append(Input.journal_);
        // Snapshot reader rejects reparse points, bounds size, and detects concurrent mutation.
        const auto bytes = ReadFileSnapshot(Input.journal_, journalLimit, Limits).raw_;
        std::optional<storage::WriteRecoveryRecord> previous; QJsonObject latest; qsizetype offset = 0; qint64 sequence = 0;
        while (offset < bytes.size())
        {
            Limits.Check(); const auto end = bytes.indexOf('\n', offset); if (end < 0) break;
            if (end - offset > journalLimit) throw Failure("SaveRecoveryLineLimit");
            QJsonParseError error; const auto json = QJsonDocument::fromJson(bytes.mid(offset, end - offset), &error); offset = end + 1;
            if (error.error != QJsonParseError::NoError || !json.isObject()) throw Failure("SaveRecoveryCorrupt");
            latest = json.object(); auto record = decode(latest);
            if (latest["sequence"] != QJsonValue(++sequence)) throw Failure("SaveRecoverySequence");
            if (!previous) { if (record.phase_ != storage::WritePhase::Writing) throw Failure("SaveRecoveryFirstPhase"); }
            else
            {
                const auto& old = *previous;
                if (!transition(old.phase_, record.phase_) || old.transaction_ != record.transaction_ || old.stageIdentity_ != record.stageIdentity_ ||
                    old.destination_.kind_ != record.destination_.kind_ || old.destination_.root_ != record.destination_.root_ || old.stage_.root_ != record.stage_.root_ || old.backup_.root_ != record.backup_.root_ ||
                    (!old.originalIdentity_.empty() && old.originalIdentity_ != record.originalIdentity_)) throw Failure("SaveRecoveryInconsistent");
            }
            previous = std::move(record);
        }
        if (!previous) throw Failure("SaveRecoveryEmpty");
        if (offset != bytes.size()) Value.warnings_.append("SaveRecoveryIgnoredTornTail");
        Value.saveRecovery_ = latest; Value.saveRecovery_["identitiesVerified"] = false;
        const auto& record = *previous;
        CheckProtectedInputs(Input, {QString::fromStdString(record.destination_.root_), QString::fromStdString(record.stage_.root_), QString::fromStdString(record.backup_.root_)}, Limits);
        if (Input.dryRun_) { Value.outcome_ = "RecoveryPreview"; Value.completion_ = "Complete"; Value.exitCode_ = 0; return; }
        Limits.Check(); const auto recovered = storage::RecoverNativeWrite(record);
        if (!recovered.HasValue()) throw Failure(recovered.Failure().code_);
        const auto& receipt = recovered.Value(); Value.saveRecovery_["identitiesVerified"] = true;
        Value.commit_ = receipt.destinationCommitted_ ? "Committed" : "NotCommitted";
        Value.saveRecovery_["destinationPresent"] = !receipt.destinationIdentity_.empty();
        for (const auto& artifact : receipt.preservedArtifacts_) Value.recovery_.append(QString::fromStdString(artifact.root_));
        for (const auto& error : receipt.cleanupErrors_) Value.warnings_.append(QString::fromStdString(error.code_));
        Value.exitCode_ = receipt.cleanupErrors_.empty() ? 0 : 2;
        Value.outcome_ = Value.exitCode_ == 0 ? "RecoveryChecked" : "RecoveryIncomplete";
        Value.completion_ = Value.exitCode_ == 0 ? "Complete" : "Partial";
        // Keep the journal and all uncommitted content for explicit human review.
    }
}
