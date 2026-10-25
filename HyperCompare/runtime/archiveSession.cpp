#include "stdafx.h"
#include "nativeArchive.hpp"
#include "engineRegistry.hpp"
#include "workerHost.hpp"
#include "saveRecovery.hpp"
#include "directoryPlan.hpp"
#include "requestFile.hpp"
#include "UniqueLibs/storageProviders.hpp"
#include "UniqueLibs/nativeWritableStorage.hpp"
#include <QCryptographicHash>
#include <QFile>
#include <QRegularExpression>

namespace hc::compare
{
    namespace
    {
        template<class T> T require(storage::Result<T> Value) { if (!Value.HasValue()) throw Failure(Value.Failure().code_); return std::move(Value.Value()); }
        bool archiveFormat(const QString& Format) { return QStringList{"zip", "7z", "rar"}.contains(Format); }
    }
    QJsonObject RunArchiveWorker(const QVector<Snapshot>& Inputs, const QJsonArray& Passwords, const std::array<QJsonArray, 2>& Members,
        const Budget& Limits, const QJsonObject& Operation)
    {
        std::uint64_t inputBytes = 0; for (const auto& input : Inputs) inputBytes += std::uint64_t(input.raw_.size());
        auto preparation = Limits.memoryReserve_ ? Limits.memoryReserve_(inputBytes * 6 + 8 * 1024 * 1024) : std::shared_ptr<void>{};
        QJsonArray resources; for (const auto& input : Inputs) { if (input.raw_.size() > 16 * 1024 * 1024) throw Failure("ArchiveInputLimit"); resources.append(QString::fromLatin1(input.raw_.toBase64())); }
        auto payload = Operation; payload["resources"] = resources; payload["passwords"] = Passwords; payload["members"] = QJsonArray{Members[0], Members[1]};
        preparation.reset();
        WorkerPolicy policy; policy.timeoutMs_ = 30000; policy.memoryBytes_ = 512 * 1024 * 1024; policy.requestBytes_ = policy.responseBytes_ = 48 * 1024 * 1024;
        auto result = RunWorker("container.7zip", payload, Limits, policy); result.remove("digest");
        if (result["status"] != "Complete") throw Failure(result["reason"].toString("ArchiveWorkerFailed").toStdString());
        if (Operation.isEmpty() || Operation["operation"] == "compare") {
            if (result["payloadSchema"] != "archive.entries.v1" || result["documents"].toArray().size() != Inputs.size() || result["entries"].toArray().size() > 20000) throw Failure("ArchiveWorkerProtocol");
        }
        return result;
    }
    QJsonObject ArchiveOperation(const Request& Input, ArchiveSession& Session, const QJsonObject& Operation, const Budget& Limits)
    {
        QVector<Snapshot> inputs = Session.originals_; for (int side = 0; side < inputs.size(); ++side) inputs[side].raw_ = Session.working_[side];
        QMap<QString, int> prompts;
        for (int attempt = 0;; ++attempt) {
            try { return RunArchiveWorker(inputs, Session.passwords_, Session.members_, Limits, Operation); }
            catch (const Failure& error) {
                const auto match = QRegularExpression("^ArchivePasswordRequired:([01]):([0-4])$").match(QString::fromStdString(error.code_));
                if (!match.hasMatch() || !Input.archivePasswordPrompt_ || attempt >= 30 || ++prompts[match.captured(0)] > 3) throw;
                const auto side = match.captured(1).toInt(), depth = match.captured(2).toInt(); const auto password = Input.archivePasswordPrompt_(side, depth, Limits);
                Limits.Check(); if (password.isNull()) throw Failure("Canceled");
                while (Session.passwords_.size() <= side) Session.passwords_.append(QJsonArray{}); auto passwords = Session.passwords_[side].toArray(); while (passwords.size() <= depth) passwords.append(QJsonValue{}); passwords[depth] = password; Session.passwords_[side] = passwords;
            }
        }
    }
    bool TryArchiveComparison(const Request& Input, Result& Value, const Budget& Limits, bool Save, const QString& Format)
    {
        if (!archiveFormat(Format)) return false;
        if (!Input.base_.isEmpty() || !Input.file3_.isEmpty()) throw Failure("ArchiveThreeUnsupported");
        if (!archiveFormat(InspectResourceFormat(Input.file2_, Limits))) throw Failure("MixedInputFormats");
        auto session = std::make_shared<ArchiveSession>(); session->members_ = Input.archiveMembers_;
        for (int side = 0; side < 2; ++side) {
            session->originals_.append(ReadFileSnapshot(side ? Input.file2_ : Input.file1_, 16 * 1024 * 1024, Limits)); session->working_[side] = session->originals_.last().raw_;
            QJsonArray passwords;
            if (!Input.archivePasswordFiles_[side].isEmpty()) {
                QFile file(Input.archivePasswordFiles_[side]); if (!file.open(QIODevice::ReadOnly) || file.size() > 16384) throw Failure("ArchivePasswordFileInvalid"); auto bytes = file.readAll(); if (!bytes.isValidUtf8() || bytes.contains('\0')) throw Failure("ArchivePasswordFileInvalid");
                if (bytes.startsWith("[")) { const auto document = QJsonDocument::fromJson(bytes); if (!document.isArray() || document.array().size() > 5) throw Failure("ArchivePasswordFileInvalid"); passwords = document.array(); for (const auto& value : passwords) if (!value.isString() || value.toString().size() > 4096) throw Failure("ArchivePasswordFileInvalid"); }
                else { if (bytes.endsWith('\n')) bytes.chop(1); if (bytes.endsWith('\r')) bytes.chop(1); if (bytes.size() > 4096) throw Failure("ArchivePasswordFileInvalid"); passwords.append(QString::fromUtf8(bytes)); }
                bytes.fill('\0');
            }
            session->passwords_.append(passwords);
        }
        session->comparison_ = ArchiveOperation(Input, *session, {}, Limits);
        Value.inputs_ = session->originals_; Value.extension_ = session->comparison_; Value.archive_ = session; Value.operation_ = "compare-archive";
        Value.exitCode_ = Value.extension_["rawEqual"].toBool() ? 0 : 1; Value.outcome_ = Value.extension_["contentsEqual"].toBool() ? (Value.extension_["rawEqual"].toBool() ? "Equal" : "Equivalent") : "Different";
        if (Input.mode_ == "merge") {
            int side = Input.takeAll_ == "left" ? 0 : 1;
            if (!Input.decisions_.isEmpty()) {
                const auto snapshot = Value.decisionsSnapshot_ ? *Value.decisionsSnapshot_ : ReadFileSnapshot(Input.decisions_, 1024 * 1024, Limits); if (snapshot.raw_.size() > 1024 * 1024) throw Failure("ArchiveMergePlanInvalid"); const auto plan = QJsonDocument::fromJson(snapshot.raw_).object(); const auto hashes = plan["inputSha256"].toArray(); side = plan["target"].toInt(-1);
                if (plan["kind"] != "HyperCompareArchiveMerge" || plan["schemaVersion"] != 1 || hashes.size() != 2 || side < 0 || side > 1) throw Failure("ArchiveMergePlanInvalid");
                for (int input = 0; input < 2; ++input) if (hashes[input].toString().toLatin1() != QCryptographicHash::hash(session->working_[input], QCryptographicHash::Sha256).toHex()) throw Failure("ArchiveMergePlanInputChanged");
                const auto rewritten = ArchiveOperation(Input, *session, {{"operation", "rewrite"}, {"target", side}, {"changes", plan["changes"]}}, Limits);
                auto decoded = QByteArray::fromBase64Encoding(rewritten["bytes"].toString().toLatin1(), QByteArray::AbortOnBase64DecodingErrors); if (!decoded || decoded.decoded.size() > 16 * 1024 * 1024 || QCryptographicHash::hash(decoded.decoded, QCryptographicHash::Sha256).toHex() != rewritten["sha256"].toString().toLatin1()) throw Failure("ArchiveWorkerProtocol");
                session->working_[side] = std::move(decoded.decoded); session->members_[side] = rewritten["members"].toArray(); session->comparison_ = ArchiveOperation(Input, *session, {}, Limits); Value.extension_ = session->comparison_;
            } else if (Input.takeAll_.isEmpty() || !QStringList{"left", "right"}.contains(Input.takeAll_)) { if (!Input.headless_) return true; throw Failure("ArchiveMergeChoicesRequired"); }
            auto expected = session->originals_[side]; expected.path_ = Input.output_;
            if (expected.path_.isEmpty()) throw Failure("ArchiveOutputRequired");
            if (QFileInfo::exists(expected.path_)) { if (!Input.overwrite_) throw Failure("OverwriteRequired"); expected = ReadFileSnapshot(expected.path_, 16 * 1024 * 1024, Limits); } else { expected.exists_ = false; expected.entry_ = {}; }
            Value.operation_ = "merge-archive"; Value.output_ = session->working_[side]; Value.exitCode_ = 0; Value.outcome_ = "Planned";
            if (Save && Input.headless_ && !Input.dryRun_) {
                for (const auto& source : session->originals_) { const auto current = ReadFileSnapshot(source.path_, 16 * 1024 * 1024, Limits); if (current.entry_.id_.revision_ != source.entry_.id_.revision_ || current.raw_ != source.raw_) throw Failure("ArchiveInputChanged"); }
                auto protectedInput = Input; protectedInput.protectedInputs_.append({Input.file1_, Input.file2_});
                if (!Input.decisions_.isEmpty()) { protectedInput.protectedInputs_.append(Input.decisions_); const auto current = ReadFileSnapshot(Input.decisions_, 1024 * 1024, Limits); if (Value.decisionsSnapshot_ && (current.entry_.id_.revision_ != Value.decisionsSnapshot_->entry_.id_.revision_ || current.raw_ != Value.decisionsSnapshot_->raw_)) throw Failure("ArchiveMergePlanChanged"); }
                SaveArchiveSnapshot(protectedInput, expected, Value.output_, Limits); Value.commit_ = "Committed"; Value.outcome_ = "Saved";
            }
            if (!Input.headless_) { session->working_[1] = Value.output_; if (side == 0) { session->working_[0] = session->originals_[0].raw_; session->members_[1] = session->members_[0]; session->passwords_[1] = session->passwords_[0]; } session->comparison_ = ArchiveOperation(Input, *session, {}, Limits); Value.extension_ = session->comparison_; }
        }
        return true;
    }
    Snapshot SaveArchiveSnapshot(const Request& Input, const Snapshot& Expected, const QByteArray& Bytes, const Budget& Limits)
    {
        const auto suffix = QFileInfo(Expected.path_).suffix().toLower(); const auto contentFormat = DetectResourceFormat({}, Bytes.left(4096));
        if (archiveFormat(contentFormat) && archiveFormat(suffix) && contentFormat != suffix) throw Failure("ArchiveOutputFormatMismatch");
        Limits.Check(); const auto path = CheckedDirectoryPath(Expected.path_); auto protectedInput = Input; for (const auto& password : Input.archivePasswordFiles_) if (!password.isEmpty()) protectedInput.protectedInputs_.append(password); CheckProtectedInputs(protectedInput, {path}, Limits);
        const storage::Locator at{path.startsWith("//") || path.startsWith("\\\\") ? storage::LocatorKind::Smb : storage::LocatorKind::Local, path.toUtf8().toStdString()};
        const auto reader = storage::MakeNativeReadProvider(at.kind_); const auto writer = storage::MakeNativeWritableStorage(at.kind_, reader); storage::OperationContext context; context.deadline_ = Limits.deadline_; context.workerRead_ = true; context.denyConcurrentMutation_ = true;
        const auto current = require(writer->Inspect(at, context)); if (bool(current) != Expected.exists_ || (current && current->id_.revision_ != Expected.entry_.id_.revision_)) throw Failure("ArchiveChangedBeforeSave");
        SaveRecoveryJournal journal; context.writeCheckpoint_ = journal.Checkpoint(); std::unique_ptr<storage::IWriteSession> output; bool committed = false;
        try {
            output = require(writer->BeginWrite(at, {Expected.exists_, Expected.entry_.id_.revision_}, context));
            for (qsizetype offset = 0; offset < Bytes.size(); offset += 64 * 1024) { Limits.Check(); require(output->Append({reinterpret_cast<const std::byte*>(Bytes.constData() + offset), std::size_t(std::min<qsizetype>(64 * 1024, Bytes.size() - offset))}, context)); }
            Limits.Check(); const auto receipt = require(output->Commit(context)); committed = receipt.destinationCommitted_; if (!committed) throw Failure("NotCommitted");
            auto result = ReadFileSnapshot(path, 16 * 1024 * 1024, Limits); if (result.raw_ != Bytes) throw Failure("ArchiveSavedHashMismatch"); QJsonArray warnings, recovery; journal.Complete(receipt, warnings, recovery); return result;
        } catch (const std::exception& error) { if (output) output->Abort(); QJsonArray recovery; journal.Track(recovery); throw SaveRecoveryFailure(committed ? std::string("ArchiveCommittedVerificationFailed:") + error.what() : error.what(), recovery); }
    }
}
