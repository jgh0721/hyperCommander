#include "stdafx.h"
#include "directoryPlan.hpp"
#include "requestFile.hpp"
#include "directoryText.hpp"
#include "session.hpp"
#include "UniqueLibs/storageProviders.hpp"
#include "UniqueLibs/nativeWritableStorage.hpp"
#include "commonLibs/document/documentSaveCoordinator.hpp"
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>

namespace hc::compare
{
    namespace
    {
        storage::Locator locator(const QString& Path)
        { return {Path.startsWith("//") ? storage::LocatorKind::Smb : storage::LocatorKind::Local, Path.toUtf8().toStdString()}; }
        template<class T> T require(storage::Result<T> Value)
        { if (!Value.HasValue()) throw Failure(Value.Failure().code_); return std::move(Value.Value()); }
        storage::OperationContext context(const Budget& Limits)
        { storage::OperationContext ctx; ctx.deadline_ = Limits.deadline_; ctx.workerRead_ = true; ctx.denyConcurrentMutation_ = true; return ctx; }
        QString revision(const storage::EntrySnapshot& Entry)
        { if (!Entry.id_.revision_) throw Failure("DirectoryRevisionUnavailable"); return QString::fromStdString(Entry.id_.revision_->token_); }
        QString pathRoot(const QString& Path)
        {
            if (!Path.startsWith("//"))
            {
                if (Path.size() < 3 || !Path[0].isLetter() || Path[1] != ':' || Path[2] != '/') throw Failure("InvalidDirectoryPath");
                return Path.left(3);
            }
            const auto parts = Path.split('/');
            if (parts.size() < 4 || parts[2].isEmpty() || parts[3].isEmpty()) throw Failure("InvalidDirectoryPath");
            return "//" + parts[2] + '/' + parts[3];
        }
        bool textualOverlap(const QString& A, const QString& B)
        { return A == B || A.startsWith(B.endsWith('/') ? B : B + '/') || B.startsWith(A.endsWith('/') ? A : A + '/'); }
        QHash<QString, QString> ancestorIdentities(const QString& Path)
        {
            QHash<QString, QString> result; auto at = Path; QString suffix; const auto root = pathRoot(Path);
            for (;;)
            {
                const auto handle = CreateFileW(reinterpret_cast<const wchar_t*>(QDir::toNativeSeparators(at).utf16()),
                    FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                    OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
                if (handle != INVALID_HANDLE_VALUE)
                {
                    BY_HANDLE_FILE_INFORMATION info{}; const auto ok = GetFileInformationByHandle(handle, &info); CloseHandle(handle);
                    if (!ok) throw Failure("DirectoryIdentityUnavailable");
                    if (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) throw Failure("ReparseDirectoryUnsupported");
                    if (!info.nFileIndexHigh && !info.nFileIndexLow) throw Failure("DirectoryIdentityUnavailable");
                    const auto key = QString::number(info.dwVolumeSerialNumber) + ':' + QString::number(info.nFileIndexHigh) + ':' + QString::number(info.nFileIndexLow);
                    result.insert(key, suffix.toCaseFolded());
                }
                else if (GetLastError() != ERROR_FILE_NOT_FOUND && GetLastError() != ERROR_PATH_NOT_FOUND) throw Failure("DirectoryPathUnavailable");
                if (at.compare(root, Qt::CaseInsensitive) == 0) break;
                suffix = QFileInfo(at).fileName() + (suffix.isEmpty() ? QString{} : '/' + suffix);
                const auto parent = QFileInfo(at).absolutePath();
                if (parent == at || parent.size() >= at.size()) throw Failure("InvalidDirectoryPath");
                at = parent;
            }
            return result;
        }
        QJsonObject inventory(const DirectorySnapshot& Snapshot, const Budget& Limits)
        {
            QJsonArray entries;
            for (const auto& item : Snapshot.entries_)
            {
                Limits.Check(); auto evidence = DirectoryEntryEvidence(item, Limits);
                evidence["path"] = QString::fromStdString(item.hints_.at("relativePath")); entries.append(evidence);
            }
            return {{"path", CheckedDirectoryPath(Snapshot.root_)}, {"exists", true}, {"revision", revision(Snapshot.entry_)}, {"entries", entries}};
        }
    }
    QString CheckedDirectoryPath(const QString& Path)
    {
        if (Path.isEmpty() || Path.contains(QChar(0))) throw Failure("InvalidDirectoryPath");
        auto native = Path; native.replace('\\', '/');
        if (!QDir::isAbsolutePath(native) || native.startsWith("//?/") || native.startsWith("//./")) throw Failure("InvalidDirectoryPath");
        const auto root = pathRoot(native);
        const auto parts = native.split('/');
        for (qsizetype i = native.startsWith("//") ? 2 : 1; i < parts.size(); ++i)
        {
            const auto& part = parts[i]; if (part.isEmpty() && i == parts.size() - 1) continue;
            if (part.isEmpty() || part == "." || part == ".." || part.endsWith('.') || part.endsWith(' ') || part.contains(':') ||
                part.contains('*') || part.contains('?')) throw Failure("InvalidDirectoryPath");
            const auto stem = part.section('.', 0, 0).toUpper();
            if (QStringList{"CON", "PRN", "AUX", "NUL", "CONIN$", "CONOUT$"}.contains(stem) ||
                ((stem.startsWith("COM") || stem.startsWith("LPT")) && stem.size() == 4 && stem[3] >= '1' && stem[3] <= '9'))
                throw Failure("InvalidDirectoryPath");
        }
        auto checked = QDir::cleanPath(native); auto at = checked;
        for (;;)
        {
            const auto attributes = GetFileAttributesW(reinterpret_cast<const wchar_t*>(QDir::toNativeSeparators(at).utf16()));
            if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) throw Failure("ReparseDirectoryUnsupported");
            if (attributes == INVALID_FILE_ATTRIBUTES && GetLastError() != ERROR_FILE_NOT_FOUND && GetLastError() != ERROR_PATH_NOT_FOUND)
                throw Failure("DirectoryPathUnavailable");
            // A UNC server is not a filesystem directory. Stop at its share root.
            if (at.compare(root, Qt::CaseInsensitive) == 0) break;
            const auto parent = QFileInfo(at).absolutePath(); if (parent == at || parent.size() >= at.size()) break; at = parent;
        }
        // Canonicalize the nearest existing ancestor, including short-name aliases.
        QStringList missing; at = checked;
        while (!QFileInfo::exists(at)) { missing.prepend(QFileInfo(at).fileName()); const auto parent = QFileInfo(at).absolutePath(); if (parent == at) throw Failure("DirectoryParentMissing"); at = parent; }
        auto canonical = QFileInfo(at).canonicalFilePath(); if (canonical.isEmpty()) throw Failure("DirectoryPathUnavailable");
        for (const auto& name : missing) canonical = QDir(canonical).filePath(name);
        return QDir::cleanPath(canonical);
    }
    bool DirectoryPathsOverlap(const QString& A, const QString& B)
    {
        const auto a = QDir::cleanPath(QDir::fromNativeSeparators(A)).toCaseFolded(), b = QDir::cleanPath(QDir::fromNativeSeparators(B)).toCaseFolded();
        if (textualOverlap(a, b)) return true;
        if (pathRoot(a) == pathRoot(b)) return false;
        // Drive letters, UNC host aliases and overlapping shares can address the
        // same tree. Compare existing ancestors plus the not-yet-created suffix.
        const auto aa = ancestorIdentities(a), bb = ancestorIdentities(b);
        for (auto it = aa.cbegin(); it != aa.cend(); ++it)
            if (const auto other = bb.constFind(it.key()); other != bb.cend() &&
                (it.value().isEmpty() || other.value().isEmpty() || textualOverlap(it.value(), other.value()))) return true;
        return false;
    }
    QByteArray DirectoryFileDigest(const storage::EntrySnapshot& Entry, const Budget& Limits)
    {
        Limits.Check(); if (Entry.directory_) throw Failure("ExpectedFile");
        const auto provider = storage::MakeNativeReadProvider(Entry.locator_.kind_); const auto ctx = context(Limits);
        const auto bound = require(provider->Resolve(Entry.locator_, ctx).get());
        if (bound.id_.revision_ != Entry.id_.revision_) throw Failure("DirectoryChanged");
        const auto stream = require(provider->OpenRead(bound.id_, ctx).get()); QCryptographicHash hash(QCryptographicHash::Sha256);
        std::uint64_t size = 0;
        for (;;)
        {
            Limits.Check(); const auto chunk = require(stream->Read(1024 * 1024, ctx).get());
            size += chunk.bytes_.size(); hash.addData(QByteArrayView(reinterpret_cast<const char*>(chunk.bytes_.data()), qsizetype(chunk.bytes_.size())));
            if (chunk.eof_) break; if (chunk.bytes_.empty()) throw Failure("ReadMadeNoProgress");
        }
        if (size != Entry.size_ || require(provider->Stat(bound.id_, ctx).get()).id_.revision_ != Entry.id_.revision_) throw Failure("DirectoryChanged");
        return hash.result().toHex();
    }
    QJsonObject DirectoryEntryEvidence(const storage::EntrySnapshot& Entry, const Budget& Limits)
    {
        return {{"directory", Entry.directory_}, {"size", QString::number(Entry.size_)}, {"revision", revision(Entry)},
            {"sha256", Entry.directory_ ? QString{} : QString::fromLatin1(DirectoryFileDigest(Entry, Limits))}};
    }
    QJsonObject MakeDirectoryPlan(const Request& Input, const Result& Comparison, const Budget& Limits)
    {
        CheckProtectedInputs(Input, {Input.output_}, Limits);
        if (!Comparison.directories_ || Comparison.completion_ != "Complete" || Input.directory_.level_ != "bytes") throw Failure("DirectoryPlanRequiresContentComparison");
        if (!Input.takeAll_.isEmpty() && (Comparison.directories_->size() == 3 || (Input.takeAll_ != "left" && Input.takeAll_ != "right"))) throw Failure("InvalidDirectorySelection");
        const auto output = CheckedDirectoryPath(Input.output_); QJsonArray roots;
        if (QDir(output).isRoot()) throw Failure("DirectoryOutputRootUnsupported");
        for (const auto& snapshot : *Comparison.directories_)
        {
            if (DirectoryPathsOverlap(output, CheckedDirectoryPath(snapshot.root_))) throw Failure("DirectoryOutputOverlapsInput");
            roots.append(inventory(snapshot, Limits));
        }
        QJsonObject destination{{"path", output}, {"exists", false}, {"revision", ""}, {"entries", QJsonArray{}}};
        std::optional<DirectorySnapshot> capturedOutput;
        if (QFileInfo::exists(output))
        { capturedOutput = CaptureDirectory(output, {}, Limits); destination = inventory(*capturedOutput, Limits); }
        else if (!QFileInfo(QFileInfo(output).absolutePath()).isDir()) throw Failure("DirectoryParentMissing");
        QJsonArray actions; QSet<QString> selected;
        for (const auto& row : Comparison.entries_)
        {
            Limits.Check(); const auto entry = row.toObject(); const auto path = entry["path"].toString(), status = entry["status"].toString();
            QString choice = "unresolved";
            if (!Input.takeAll_.isEmpty()) choice = Input.takeAll_;
            else if (status == "Equal" || status == "BothSame" || status == "LeftChanged" || status == "LeftAdded") choice = "left";
            else if (status == "RightChanged" || status == "RightAdded") choice = "right";
            else if (status == "LeftDeleted" || status == "RightDeleted" || status == "BothDeleted") choice = "omit";
            if (choice == "unresolved" && Comparison.directories_->size() == 3 && status == "Conflict")
            {
                std::array<storage::EntrySnapshot, 3> inputs; bool eligible = true;
                for (std::size_t side = 0; side < 3; ++side)
                {
                    const auto& tree = (*Comparison.directories_)[side].entries_; const auto at = tree.constFind(path.toCaseFolded());
                    if (at == tree.cend() || at->directory_ || QString::fromStdString(at->hints_.at("relativePath")) != path) { eligible = false; break; }
                    inputs[side] = *at;
                }
                if (eligible && MergeDirectoryText(inputs, Input.encoding_, Limits)) choice = "merge";
            }
            if ((choice == "left" || choice == "right") && !entry[choice].toObject()["present"].toBool()) choice = "omit";
            actions.append(QJsonObject{{"path", path}, {"choice", choice}, {"reason", status}}); selected.insert(path.toCaseFolded());
        }
        if (Input.mirror_ && capturedOutput)
            for (auto it = capturedOutput->entries_.cbegin(); it != capturedOutput->entries_.cend(); ++it)
                if (!selected.contains(it.key())) actions.append(QJsonObject{{"path", QString::fromStdString(it->hints_.at("relativePath"))}, {"choice", "omit"}, {"reason", "MirrorExtra"}});
        for (const auto& snapshot : *Comparison.directories_) ValidateDirectory(snapshot, Input.directory_, Limits);
        if (capturedOutput) ValidateDirectory(*capturedOutput, {}, Limits);
        QJsonObject result{{"kind", "HyperCompareDirectoryPlan"}, {"schemaVersion", 2}, {"textMerge", QJsonObject{{"engine", "myers-bounded-2"}, {"encoding", Input.encoding_}}}, {"roots", roots}, {"output", destination},
            {"include", QJsonArray::fromStringList(Input.directory_.include_)}, {"exclude", QJsonArray::fromStringList(Input.directory_.exclude_)},
            {"overwrite", Input.overwrite_}, {"mirror", Input.mirror_}, {"actions", actions}};
        if (QJsonDocument(result).toJson(QJsonDocument::Compact).size() > directoryPlanLimit) throw Failure("DirectoryPlanSizeLimit");
        return result;
    }
    void SaveDirectoryPlan(const QJsonObject& Plan, const QString& Path, Result& Value, const Budget& Limits)
    {
        const auto destination = CheckedDirectoryPath(Path);
        if (DirectoryPathsOverlap(destination, CheckedDirectoryPath(Plan["output"].toObject()["path"].toString()))) throw Failure("DirectoryArtifactOverlapsRoot");
        for (const auto& snapshot : Plan["roots"].toArray())
            if (DirectoryPathsOverlap(destination, CheckedDirectoryPath(snapshot.toObject()["path"].toString()))) throw Failure("DirectoryArtifactOverlapsRoot");
        auto data = QJsonDocument(Plan).toJson(); data.replace("\n", "\r\n");
        if (data.size() > directoryPlanLimit) throw Failure("DirectoryPlanSizeLimit");
        const auto at = locator(destination); const auto provider = storage::MakeNativeReadProvider(at.kind_);
        const auto writer = storage::MakeNativeWritableStorage(at.kind_, provider);
        if (require(writer->Inspect(at, context(Limits)))) throw Failure("PlanAlreadyExists");
        const auto receipt = require(document::SaveDocumentBytes(*writer, at, {},
            {reinterpret_cast<const std::byte*>(data.constData()), std::size_t(data.size())}, context(Limits)));
        if (!receipt.destinationCommitted_) throw Failure("PlanNotCommitted");
        for (const auto& error : receipt.cleanupErrors_) Value.warnings_.append(QString::fromStdString(error.code_));
        for (const auto& path : receipt.preservedArtifacts_) Value.recovery_.append(QString::fromStdString(path.root_));
        Value.planWritten_ = destination;
    }
    void ExportDirectoryPlan(const Request& Input, Result& Value, const Budget& Limits)
    {
        CheckProtectedInputs(Input, {Input.planOut_, Input.output_}, Limits);
        const auto destination = CheckedDirectoryPath(Input.planOut_);
        if (DirectoryPathsOverlap(destination, CheckedDirectoryPath(Input.output_))) throw Failure("DirectoryArtifactOverlapsRoot");
        for (const auto& snapshot : *Value.directories_)
            if (DirectoryPathsOverlap(destination, CheckedDirectoryPath(snapshot.root_))) throw Failure("DirectoryArtifactOverlapsRoot");
        Value.directoryPlan_ = MakeDirectoryPlan(Input, Value, Limits);
        SaveDirectoryPlan(Value.directoryPlan_, destination, Value, Limits);
        Value.planWritten_ = destination; Value.outcome_ = "Planned";
        qsizetype unresolved = 0;
        for (const auto& action : Value.directoryPlan_["actions"].toArray()) unresolved += action.toObject()["choice"] == "unresolved";
        Value.exitCode_ = unresolved ? 1 : 0;
    }
}
