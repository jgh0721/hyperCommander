#include "stdafx.h"
#include "directoryApply.hpp"
#include "session.hpp"
#include "UniqueLibs/storageProviders.hpp"
#include "UniqueLibs/nativeWritableStorage.hpp"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QLockFile>
#include <io.h>

namespace hc::compare
{
    namespace
    {
        using Evidence = QMap<QString, QJsonObject>;
        struct Action
        {
            QString path_, kind_;
            std::optional<storage::EntrySnapshot> source_;
            QJsonObject desired_;
        };
        storage::Locator locator(const QString& Path)
        { return {Path.startsWith("//") ? storage::LocatorKind::Smb : storage::LocatorKind::Local, Path.toUtf8().toStdString()}; }
        template<class T> T require(storage::Result<T> Value)
        { if (!Value.HasValue()) throw Failure(Value.Failure().code_); return std::move(Value.Value()); }
        storage::OperationContext context(const Budget& Limits)
        { storage::OperationContext ctx; ctx.deadline_ = Limits.deadline_; ctx.workerRead_ = true; ctx.denyConcurrentMutation_ = true; return ctx; }
        auto writer(const QString& Path)
        { const auto at = locator(Path); return storage::MakeNativeWritableStorage(at.kind_, storage::MakeNativeReadProvider(at.kind_)); }
        QJsonObject json(const QByteArray& Data)
        {
            QJsonParseError error; const auto doc = QJsonDocument::fromJson(Data, &error);
            if (error.error != QJsonParseError::NoError || !doc.isObject()) throw Failure("DirectoryJsonInvalid"); return doc.object();
        }
        void keys(const QJsonObject& Object, const QStringList& Expected)
        { auto actual = Object.keys(), wanted = Expected; actual.sort(); wanted.sort(); if (actual != wanted) throw Failure("DirectorySchemaInvalid"); }
        QString text(const QJsonObject& Object, const QString& Key)
        { if (!Object[Key].isString()) throw Failure("DirectorySchemaInvalid"); return Object[Key].toString(); }
        QString relative(const QJsonObject& Object)
        {
            const auto path = text(Object, "path");
            if (path.isEmpty() || path.size() > 32760 || path.startsWith('/') || path.contains('\\') || path.contains(':')) throw Failure("DirectoryRelativePathInvalid");
            // Reuse the native component validator without requiring this path to exist.
            const auto checked = CheckedDirectoryPath(QDir::tempPath() + '/' + path);
            if (!checked.startsWith(CheckedDirectoryPath(QDir::tempPath()) + '/', Qt::CaseInsensitive)) throw Failure("DirectoryRelativePathInvalid");
            return path;
        }
        QString identity(const QString& Revision)
        {
            // Native directory revisions already contain creation time + volume/file ID and omit child mtime.
            if (Revision.startsWith("directory:")) return Revision;
            const auto at = Revision.indexOf(":identity:"); if (at < 0) throw Failure("DirectoryIdentityUnavailable"); return Revision.mid(at);
        }
        bool sameEvidence(const QJsonObject& A, const QJsonObject& B)
        {
            if (A.isEmpty() || B.isEmpty()) return A.isEmpty() == B.isEmpty();
            if (A["directory"] != B["directory"]) return false;
            if (A["directory"].toBool()) return identity(A["revision"].toString()) == identity(B["revision"].toString());
            return A == B;
        }
        bool desired(const Action& Action, const QJsonObject& Evidence)
        {
            if (Action.kind_ == "remove") return Evidence.isEmpty();
            if (Action.kind_ == "mkdir") return Evidence["directory"].toBool();
            return !Evidence.isEmpty() && !Evidence["directory"].toBool() && Evidence["sha256"] == Action.desired_["sha256"] && Evidence["size"] == Action.desired_["size"];
        }
        Evidence inventory(const QJsonObject& Object)
        {
            keys(Object, {"path", "exists", "revision", "entries"});
            if (!Object["exists"].isBool() || !Object["entries"].isArray()) throw Failure("DirectorySchemaInvalid");
            Evidence result;
            const auto entries = Object["entries"].toArray(); if (entries.size() > 100000) throw Failure("DirectoryEntryLimit");
            if (Object["exists"].toBool())
                result.insert({}, QJsonObject{{"directory", true}, {"revision", text(Object, "revision")}, {"size", "0"}, {"sha256", ""}});
            else if (!entries.isEmpty() || !text(Object, "revision").isEmpty()) throw Failure("DirectorySchemaInvalid");
            for (const auto& value : entries)
            {
                if (!value.isObject()) throw Failure("DirectorySchemaInvalid"); auto entry = value.toObject();
                keys(entry, {"path", "directory", "size", "revision", "sha256"});
                const auto path = relative(entry).toCaseFolded();
                if (result.contains(path) || !entry["directory"].isBool()) throw Failure("DirectorySchemaInvalid");
                bool ok = false; const auto size = text(entry, "size"); size.toULongLong(&ok); if (!ok) throw Failure("DirectorySchemaInvalid");
                identity(text(entry, "revision")); const auto digest = text(entry, "sha256");
                if (entry["directory"].toBool() ? !digest.isEmpty() : digest.size() != 64) throw Failure("DirectorySchemaInvalid");
                entry.remove("path"); result.insert(path, entry);
            }
            return result;
        }
        Evidence capture(const DirectorySnapshot& Snapshot, const Budget& Limits)
        {
            Evidence result;
            result.insert({}, DirectoryEntryEvidence(Snapshot.entry_, Limits));
            for (auto it = Snapshot.entries_.cbegin(); it != Snapshot.entries_.cend(); ++it)
                result.insert(it.key(), DirectoryEntryEvidence(*it, Limits));
            return result;
        }
        void validateEvidence(const Evidence& Expected, const Evidence& Actual)
        {
            if (Expected.size() != Actual.size()) throw Failure("DirectorySnapshotMismatch");
            for (auto it = Expected.cbegin(); it != Expected.cend(); ++it)
                if (!Actual.contains(it.key()) || !sameEvidence(it.value(), Actual.value(it.key()))) throw Failure("DirectorySnapshotMismatch");
        }
        QJsonObject inspect(const QString& Path, const Budget& Limits)
        {
            CheckedDirectoryPath(Path); const auto entry = require(writer(Path)->Inspect(locator(Path), context(Limits)));
            return entry ? DirectoryEntryEvidence(*entry, Limits) : QJsonObject{};
        }
        void append(QFile& File, const QJsonObject& Event)
        {
            const auto bytes = QJsonDocument(Event).toJson(QJsonDocument::Compact) + "\r\n";
            if (File.size() + bytes.size() > directoryPlanLimit || File.write(bytes) != bytes.size() || !File.flush() || File.handle() < 0)
                throw Failure("DirectoryJournalWriteFailed");
            const auto handle = _get_osfhandle(File.handle());
            if (handle == -1 || !FlushFileBuffers(reinterpret_cast<HANDLE>(handle))) throw Failure("DirectoryJournalFlushFailed");
        }
        void copy(const Action& Action, const QString& Destination, const QJsonObject& Before, Result& Value, const Budget& Limits)
        {
            const auto& source = *Action.source_; const auto provider = storage::MakeNativeReadProvider(source.locator_.kind_); auto ctx = context(Limits);
            ctx.expectedWriteBytes_ = source.size_;
            const auto bound = require(provider->Resolve(source.locator_, ctx).get());
            if (bound.id_.revision_ != source.id_.revision_) throw Failure("DirectorySourceChanged");
            const auto input = require(provider->OpenRead(bound.id_, ctx).get());
            auto destination = writer(Destination); storage::DestinationExpectation expected;
            if (!Before.isEmpty()) expected = {true, storage::ResourceRevision{Before["revision"].toString().toStdString()}};
            storage::CopyOptions options; options.verify_ = true; options.metadata_ = false;
            auto output = require(destination->BeginCopy(locator(Destination), expected, options, ctx));
            try
            {
                QCryptographicHash hash(QCryptographicHash::Sha256); std::uint64_t size = 0;
                for (;;)
                {
                    Limits.Check(); const auto chunk = require(input->Read(1024 * 1024, ctx).get());
                    if (!chunk.bytes_.empty())
                    { require(output->Append(chunk.bytes_, ctx)); require(output->RecordSourceBytes(chunk.bytes_, ctx)); }
                    hash.addData(QByteArrayView(reinterpret_cast<const char*>(chunk.bytes_.data()), qsizetype(chunk.bytes_.size()))); size += chunk.bytes_.size();
                    if (chunk.eof_) break; if (chunk.bytes_.empty()) throw Failure("ReadMadeNoProgress");
                }
                if (QString::number(size) != Action.desired_["size"] || QString::fromLatin1(hash.result().toHex()) != Action.desired_["sha256"] ||
                    require(provider->Stat(bound.id_, ctx).get()).id_.revision_ != source.id_.revision_) throw Failure("DirectorySourceChanged");
                while (!require(output->AdvanceCopyExtras(0, ctx)).done_) Limits.Check();
                while (!require(output->VerifyContent(ctx))) Limits.Check();
                Limits.Check(); const auto receipt = require(output->Commit(ctx));
                if (!receipt.destinationCommitted_) throw Failure("NotCommitted");
                Value.commit_ = "Partial";
                for (const auto& error : receipt.cleanupErrors_) Value.warnings_.append(QString::fromStdString(error.code_));
                for (const auto& artifact : receipt.preservedArtifacts_) Value.recovery_.append(QString::fromStdString(artifact.root_));
            }
            catch (...)
            {
                const auto aborted = output->Abort();
                if (!aborted.HasValue()) Value.warnings_.append(QString::fromStdString(aborted.Failure().code_));
                throw;
            }
        }
    }
    void ApplyDirectoryPlan(const Request& Input, Result& Value, const Budget& Limits, const std::function<void(std::size_t)>& AfterCommit)
    {
        Value.operation_ = "directory-apply";
        const auto planPath = CheckedDirectoryPath(Input.applyPlan_);
        QFile file(planPath); if (!file.open(QIODevice::ReadOnly) || file.size() > directoryPlanLimit) throw Failure("DirectoryPlanReadFailed");
        const auto bytes = file.readAll(); if (file.error() != QFileDevice::NoError) throw Failure("DirectoryPlanReadFailed"); file.close();
        const auto plan = json(bytes); Value.directoryPlan_ = plan;
        if (!Input.applyPlanDigest_.isEmpty() && Input.applyPlanDigest_ != QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(plan).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex()))
            throw Failure("DirectoryPlanChanged");
        keys(plan, {"kind", "schemaVersion", "roots", "output", "include", "exclude", "overwrite", "mirror", "actions"});
        if (plan["kind"] != "HyperCompareDirectoryPlan" || plan["schemaVersion"] != 1 || !plan["roots"].isArray() ||
            !plan["output"].isObject() || !plan["actions"].isArray() || !plan["overwrite"].isBool() || !plan["mirror"].isBool()) throw Failure("DirectorySchemaInvalid");
        DirectoryPolicy policy;
        for (const auto& key : {QString("include"), QString("exclude")})
        {
            if (!plan[key].isArray()) throw Failure("DirectorySchemaInvalid");
            for (const auto& pattern : plan[key].toArray())
            { if (!pattern.isString()) throw Failure("DirectorySchemaInvalid"); (key == "include" ? policy.include_ : policy.exclude_).append(pattern.toString()); }
        }
        const auto destination = plan["output"].toObject(); const auto output = CheckedDirectoryPath(text(destination, "path"));
        if (QDir(output).isRoot()) throw Failure("DirectoryOutputRootUnsupported");
        const auto journalPath = CheckedDirectoryPath(Input.journal_.isEmpty() ? planPath + ".journal" : Input.journal_);
        if (journalPath.compare(planPath, Qt::CaseInsensitive) == 0 || DirectoryPathsOverlap(output, journalPath) || DirectoryPathsOverlap(output, planPath)) throw Failure("DirectoryArtifactOverlapsRoot");
        auto expected = inventory(destination); std::vector<DirectorySnapshot> roots; std::vector<Evidence> evidence;
        const auto rootList = plan["roots"].toArray(); if (rootList.size() < 2 || rootList.size() > 3) throw Failure("DirectorySchemaInvalid");
        QSet<QString> candidates;
        for (const auto& value : rootList)
        {
            if (!value.isObject()) throw Failure("DirectorySchemaInvalid"); const auto root = value.toObject();
            const auto path = CheckedDirectoryPath(text(root, "path"));
            if (DirectoryPathsOverlap(path, output) || DirectoryPathsOverlap(path, journalPath) || DirectoryPathsOverlap(path, planPath)) throw Failure("DirectoryOutputOverlapsInput");
            roots.push_back(CaptureDirectory(path, policy, Limits)); evidence.push_back(inventory(root));
            validateEvidence(evidence.back(), capture(roots.back(), Limits));
            for (auto it = roots.back().entries_.cbegin(); it != roots.back().entries_.cend(); ++it) candidates.insert(it.key());
        }
        if (plan["mirror"].toBool()) for (auto it = expected.cbegin(); it != expected.cend(); ++it) if (!it.key().isEmpty()) candidates.insert(it.key());
        std::vector<Action> removals, directories, copies;
        if (!expected.contains({})) directories.push_back({{}, "mkdir", {}, {}});
        QSet<QString> seen; QMap<QString, bool> chosen;
        const auto actions = plan["actions"].toArray(); if (actions.size() > 300000) throw Failure("DirectoryEntryLimit");
        for (const auto& value : actions)
        {
            Limits.Check(); if (!value.isObject()) throw Failure("DirectorySchemaInvalid"); const auto item = value.toObject();
            keys(item, {"path", "choice", "reason"}); const auto path = relative(item), key = path.toCaseFolded(), choice = text(item, "choice");
            text(item, "reason");
            if (seen.contains(key) || !candidates.contains(key)) throw Failure("DirectoryActionScopeInvalid"); seen.insert(key);
            if (choice == "unresolved") throw Failure("DirectoryDecisionsRequired");
            const QStringList roles{"left", "right", "base"}; const auto side = roles.indexOf(choice);
            std::optional<storage::EntrySnapshot> source;
            QJsonObject desiredEntry;
            if (choice != "omit")
            {
                if (side < 0 || std::size_t(side) >= roots.size() || !roots[std::size_t(side)].entries_.contains(key)) throw Failure("DirectoryChoiceInvalid");
                source = roots[std::size_t(side)].entries_.value(key); desiredEntry = evidence[std::size_t(side)].value(key);
                chosen.insert(key, source->directory_);
            }
            const auto before = expected.value(key);
            const bool remove = !before.isEmpty() && (!source || before["directory"].toBool() != source->directory_);
            if (remove)
            {
                if (!Input.allowDelete_) throw Failure("DirectoryDeletionAuthorizationRequired");
                removals.push_back({path, "remove", {}, {}});
            }
            if (!source) continue;
            if (source->directory_)
            { if (before.isEmpty() || remove) directories.push_back({path, "mkdir", {}, {}}); }
            else
            {
                if (!before.isEmpty() && !remove && before["sha256"] == desiredEntry["sha256"] && before["size"] == desiredEntry["size"]) continue;
                if (!before.isEmpty() && !plan["overwrite"].toBool()) throw Failure("OutputExists");
                copies.push_back({path, "copy", source, desiredEntry});
            }
        }
        if (seen != candidates) throw Failure("DirectoryActionScopeInvalid");
        // Every selected child must have a selected directory parent. No implicit resurrection after a deletion choice.
        for (auto it = chosen.cbegin(); it != chosen.cend(); ++it)
        {
            auto parent = it.key();
            while (parent.contains('/'))
            { parent = parent.left(parent.lastIndexOf('/')); if (!chosen.contains(parent) || !chosen.value(parent)) throw Failure("DirectoryParentChoiceConflict"); }
        }
        QSet<QString> removed; for (const auto& action : removals) removed.insert(action.path_.toCaseFolded());
        for (auto it = expected.cbegin(); it != expected.cend(); ++it)
        {
            auto parent = it.key();
            while (parent.contains('/'))
            { parent = parent.left(parent.lastIndexOf('/')); if (removed.contains(parent) && !removed.contains(it.key())) throw Failure("DirectoryPreservedChildConflict"); }
        }
        auto depthOrder = [](const Action& A, const Action& B) {
            const auto ad = A.path_.count('/'), bd = B.path_.count('/'); return ad == bd ? A.path_ < B.path_ : ad < bd;
        };
        std::sort(removals.begin(), removals.end(), [&](const Action& A, const Action& B) { return depthOrder(B, A); });
        std::sort(directories.begin(), directories.end(), depthOrder);
        std::sort(copies.begin(), copies.end(), [](const Action& A, const Action& B) { return A.path_ < B.path_; });
        std::vector<Action> schedule = std::move(removals); schedule.insert(schedule.end(), directories.begin(), directories.end()); schedule.insert(schedule.end(), copies.begin(), copies.end());
        const auto pathOf = [&](const Action& Action) { return Action.path_.isEmpty() ? output : output + '/' + Action.path_; };
        // Serialize all plans targeting this output, including plans with different journal locations.
        const auto lockName = QFileInfo(output).absolutePath() + "/.hypercompare-" + QString::fromLatin1(QCryptographicHash::hash(output.toCaseFolded().toUtf8(), QCryptographicHash::Sha256).toHex()) + ".lock";
        CheckedDirectoryPath(lockName); QLockFile lock(lockName); if (!lock.tryLock(0)) throw Failure("DirectoryOutputBusy");
        const auto planDigest = QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(plan).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex());
        QFile journal(journalPath); std::size_t completed = 0; bool pending = false; qint64 validBytes = 0;
        if (Input.resume_)
        {
            if (!journal.open(QIODevice::ReadOnly) || journal.size() > directoryPlanLimit) throw Failure("DirectoryJournalReadFailed");
            const auto header = json(journal.readLine(4096));
            if (header["kind"] != "HyperCompareDirectoryJournal" || header["schemaVersion"] != 1 || header["planSha256"] != planDigest) throw Failure("DirectoryJournalMismatch");
            validBytes = journal.pos();
            while (!journal.atEnd())
            {
                Limits.Check(); const auto line = journal.readLine(32768);
                if (!line.endsWith('\n')) { if (!journal.atEnd()) throw Failure("DirectoryJournalInvalid"); break; }
                const auto event = json(line); keys(event, {"index", "phase", "after"});
                if (event["index"].toInteger(-1) != qint64(completed) || completed >= schedule.size()) throw Failure("DirectoryJournalInvalid");
                if (event["phase"] == "pending" && !pending) pending = true;
                else if (event["phase"] == "done" && pending && event["after"].isObject())
                {
                    const auto after = event["after"].toObject(); if (!desired(schedule[completed], after)) throw Failure("DirectoryJournalInvalid");
                    const auto key = schedule[completed].path_.toCaseFolded(); if (after.isEmpty()) expected.remove(key); else expected[key] = after;
                    ++completed; pending = false;
                }
                else throw Failure("DirectoryJournalInvalid");
                validBytes = journal.pos();
            }
            journal.close();
            if (pending)
            {
                const auto& action = schedule[completed]; const auto actual = inspect(pathOf(action), Limits);
                if (!sameEvidence(expected.value(action.path_.toCaseFolded()), actual))
                {
                    if (!desired(action, actual)) throw Failure("DirectoryRecoveryRequired");
                    const auto key = action.path_.toCaseFolded(); if (actual.isEmpty()) expected.remove(key); else expected[key] = actual;
                }
            }
        }
        else if (QFileInfo::exists(journalPath)) throw Failure("DirectoryJournalExistsUseResume");
        Evidence actual;
        if (QFileInfo::exists(output)) actual = capture(CaptureDirectory(output, {}, Limits), Limits);
        validateEvidence(expected, actual);
        for (const auto& snapshot : roots) ValidateDirectory(snapshot, policy, Limits);
        if (Input.dryRun_) { Value.outcome_ = "Validated"; Value.exitCode_ = 0; Value.completion_ = "Complete"; return; }
        if (!journal.open(Input.resume_ ? QIODevice::ReadWrite : QIODevice::WriteOnly | QIODevice::NewOnly)) throw Failure("DirectoryJournalOpenFailed");
        if (Input.resume_) { if (!journal.resize(validBytes) || !journal.seek(validBytes)) throw Failure("DirectoryJournalWriteFailed"); }
        else append(journal, {{"kind", "HyperCompareDirectoryJournal"}, {"schemaVersion", 1}, {"planSha256", planDigest}});
        Value.recovery_.append(journalPath);
        if (completed || pending) Value.commit_ = "Partial";
        for (; completed < schedule.size(); ++completed)
        {
            Limits.Check(); const auto& action = schedule[completed]; const auto path = pathOf(action), key = action.path_.toCaseFolded();
            const auto before = inspect(path, Limits);
            if (!sameEvidence(expected.value(key), before)) throw Failure("DirectoryTargetChanged");
            if (!pending) append(journal, {{"index", qint64(completed)}, {"phase", "pending"}, {"after", QJsonObject{}}});
            if (!(pending && desired(action, before)))
            {
                if (action.kind_ == "copy") copy(action, path, before, Value, Limits);
                else if (action.kind_ == "mkdir") require(writer(path)->MakeDirectory(locator(path), context(Limits)));
                else
                {
                    auto destinationWriter = writer(path); const auto entry = require(destinationWriter->Inspect(locator(path), context(Limits)));
                    if (!entry || !sameEvidence(before, DirectoryEntryEvidence(*entry, Limits))) throw Failure("DirectoryTargetChanged");
                    require(destinationWriter->Remove(*entry, context(Limits)));
                }
                Value.commit_ = "Partial";
            }
            // Once committed, record evidence even when cancellation arrives between commit and journaling.
            auto recordBudget = Limits; recordBudget.canceled_ = nullptr;
            recordBudget.deadline_ = std::max(Limits.deadline_, std::chrono::steady_clock::now() + std::chrono::seconds(10));
            const auto after = inspect(path, recordBudget); if (!desired(action, after)) throw Failure("DirectoryCommitVerificationFailed");
            append(journal, {{"index", qint64(completed)}, {"phase", "done"}, {"after", after}});
            if (after.isEmpty()) expected.remove(key); else expected[key] = after; pending = false;
            if (AfterCommit) AfterCommit(completed + 1);
        }
        Value.commit_ = "Committed"; Value.outcome_ = "Saved"; Value.exitCode_ = 0; Value.completion_ = "Complete";
    }
}
