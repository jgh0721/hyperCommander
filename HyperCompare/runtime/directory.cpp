#include "stdafx.h"
#include "directory.hpp"
#include "session.hpp"
#include "UniqueLibs/storageProviders.hpp"
#include <QDir>

namespace hc::compare
{
    namespace
    {
        storage::Locator locator(const QString& Path)
        { return {Path.startsWith("//") || Path.startsWith("\\\\") ? storage::LocatorKind::Smb : storage::LocatorKind::Local, Path.toUtf8().toStdString()}; }
        auto reader(const QString& Path) { return storage::MakeNativeReadProvider(locator(Path).kind_); }
        template<class T> T require(storage::Result<T> Value)
        { if (!Value.HasValue()) throw Failure(Value.Failure().code_); return std::move(Value.Value()); }
        storage::OperationContext context(const Budget& Limits)
        {
            storage::OperationContext ctx; ctx.deadline_ = Limits.deadline_; ctx.workerRead_ = true;
            ctx.denyConcurrentMutation_ = true; ctx.incrementalEnumeration_ = true; ctx.maxEnumerationEntries_ = 100000;
            return ctx;
        }
        bool matches(const QStringList& Patterns, const QString& Relative)
        {
            for (const auto& pattern : Patterns)
                if (QDir::match(pattern, pattern.contains('/') ? Relative : Relative.section('/', -1))) return true;
            return false;
        }
        bool recoverable(const Failure& Error)
        { return Error.code_.starts_with("win32:") || Error.code_ == "ChangedDuringRead"; }
        void enumerate(DirectorySnapshot& Snapshot, const DirectoryPolicy& Policy, const Budget& Limits, bool AllowIncomplete)
        {
            auto& result = Snapshot.entries_; const auto provider = reader(Snapshot.root_); auto ctx = context(Limits);
            // Retain listing metadata until filters run; included unavailable entries
            // still fail strict mutation capture below.
            ctx.allowUnavailableChildMetadata_ = true;
            const auto& root = Snapshot.entry_;
            if (!root.directory_) throw Failure("ExpectedDirectory");
            if (root.attributes_ & FILE_ATTRIBUTE_REPARSE_POINT) throw Failure("ReparseDirectoryUnsupported");
            std::vector<std::pair<storage::Locator, QString>> pending{{root.locator_, {}}};
            std::size_t visited = 0;
            while (!pending.empty())
            {
                Limits.Check(); auto [at, prefix] = std::move(pending.back()); pending.pop_back();
                try
                {
                    const auto cursor = require(provider->Enumerate(at, ctx).get());
                    for (;;)
                    {
                        Limits.Check(); auto page = require(cursor->Next(256, ctx).get());
                        for (auto& entry : page.entries_)
                        {
                            if (++visited > 100000)
                            {
                                if (!AllowIncomplete) throw Failure("DirectoryEntryLimit");
                                Snapshot.issues_.insert({}, "DirectoryEntryLimit"); return;
                            }
                            const auto relative = prefix + QString::fromStdString(entry.name_);
                            if (matches(Policy.exclude_, relative)) continue;
                            if (!entry.directory_ && !Policy.include_.isEmpty() && !matches(Policy.include_, relative)) continue;
                            const auto key = relative.toCaseFolded();
                            if (result.contains(key)) throw Failure("CaseCollision");
                            QString error;
                            if (const auto found = entry.hints_.find("comparisonError"); found != entry.hints_.end()) error = QString::fromStdString(found->second);
                            if (entry.attributes_ & FILE_ATTRIBUTE_REPARSE_POINT) error = "ReparseDirectoryUnsupported";
                            if (relative.count('/') > 128) error = "DirectoryDepthLimit";
                            if (!error.isEmpty())
                            {
                                if (!AllowIncomplete) throw Failure(error.toStdString());
                                Snapshot.issues_.insert(key, error);
                            }
                            entry.hints_["relativePath"] = relative.toUtf8().toStdString();
                            if (entry.directory_ && error.isEmpty()) pending.push_back({entry.locator_, relative + '/'});
                            result.insert(key, std::move(entry));
                        }
                        if (page.end_) break;
                    }
                }
                catch (const Failure& error)
                {
                    if (!AllowIncomplete || !recoverable(error)) throw;
                    Snapshot.issues_.insert(prefix.isEmpty() ? QString{} : prefix.chopped(1).toCaseFolded(), QString::fromStdString(error.code_));
                }
            }
        }
        QString issueFor(const DirectorySnapshot& Snapshot, QString Key, bool Present)
        {
            for (;;)
            {
                const auto found = Snapshot.issues_.constFind(Key);
                if (found != Snapshot.issues_.cend() && (!Key.isEmpty() || !Present)) return found.value();
                if (Key.isEmpty()) return {};
                const auto slash = Key.lastIndexOf('/'); Key = slash < 0 ? QString{} : Key.left(slash);
            }
        }
        bool sameContent(const storage::EntrySnapshot& A, const storage::EntrySnapshot& B, const Budget& Limits)
        {
            if (A.size_ != B.size_) return false;
            const auto pa = reader(QString::fromStdString(A.locator_.root_)), pb = reader(QString::fromStdString(B.locator_.root_));
            const auto ctx = context(Limits);
            // Resource keys are provider-session scoped. Rebind locators and verify captured revisions.
            const auto boundA = require(pa->Resolve(A.locator_, ctx).get()), boundB = require(pb->Resolve(B.locator_, ctx).get());
            if (boundA.id_.revision_ != A.id_.revision_ || boundB.id_.revision_ != B.id_.revision_) throw Failure("ChangedDuringRead");
            auto a = require(pa->OpenRead(boundA.id_, ctx).get()), b = require(pb->OpenRead(boundB.id_, ctx).get());
            // Streams may return different short-read sizes, so compare a rolling byte window.
            std::vector<std::byte> aa, bb; std::size_t ai = 0, bi = 0; bool ae = false, be = false, equal = true;
            for (;;)
            {
                Limits.Check();
                if (ai == aa.size() && !ae) { auto chunk = require(a->Read(1024 * 1024, ctx).get()); aa = std::move(chunk.bytes_); ai = 0; ae = chunk.eof_; if (aa.empty() && !ae) throw Failure("ReadMadeNoProgress"); }
                if (bi == bb.size() && !be) { auto chunk = require(b->Read(1024 * 1024, ctx).get()); bb = std::move(chunk.bytes_); bi = 0; be = chunk.eof_; if (bb.empty() && !be) throw Failure("ReadMadeNoProgress"); }
                const auto count = std::min(aa.size() - ai, bb.size() - bi);
                if (count && !std::equal(aa.begin() + ai, aa.begin() + ai + count, bb.begin() + bi)) { equal = false; break; }
                ai += count; bi += count;
                if (ae && ai == aa.size() && be && bi == bb.size()) break;
                if ((ae && ai == aa.size() && bi < bb.size()) || (be && bi == bb.size() && ai < aa.size())) { equal = false; break; }
            }
            if (require(pa->Stat(boundA.id_, ctx).get()).id_.revision_ != A.id_.revision_ ||
                require(pb->Stat(boundB.id_, ctx).get()).id_.revision_ != B.id_.revision_) throw Failure("ChangedDuringRead");
            return equal;
        }

        QString relative(const storage::EntrySnapshot* Entry)
        { return Entry ? QString::fromStdString(Entry->hints_.at("relativePath")) : QString{}; }
        QJsonObject describe(const storage::EntrySnapshot* Entry)
        {
            if (!Entry) return {{"present", false}};
            return {{"present", true}, {"path", relative(Entry)}, {"directory", Entry->directory_},
                {"size", QString::number(Entry->size_)}, {"modifiedUtcMs", QString::number(Entry->modifiedUtcMs_)}};
        }
        QString relation(const storage::EntrySnapshot* A, const storage::EntrySnapshot* B,
            const DirectoryPolicy& Policy, const Budget& Limits)
        {
            if (!A && !B) return "Equal";
            if (!A) return "RightOnly";
            if (!B) return "LeftOnly";
            if (A->directory_ != B->directory_) return "TypeConflict";
            if (relative(A) != relative(B)) return "NameDifferent";
            if (A->directory_) return "Equal";
            if (Policy.level_ == "metadata")
                return A->size_ == B->size_ && A->modifiedUtcMs_ == B->modifiedUtcMs_ ? "Equal" : "Different";
            return sameContent(*A, *B, Limits) ? "Equal" : "Different";
        }
    }
    DirectorySnapshot CaptureDirectory(const QString& Root, const DirectoryPolicy& Policy, const Budget& Limits, bool AllowIncomplete)
    {
        if (Policy.level_ != "bytes" && Policy.level_ != "metadata") throw Failure("InvalidDirectoryLevel");
        for (const auto& patterns : {Policy.include_, Policy.exclude_})
        {
            if (patterns.size() > 32) throw Failure("DirectoryFilterLimit");
            for (const auto& pattern : patterns)
                if (pattern.isEmpty() || pattern.size() > 256 || pattern.contains('\\')) throw Failure("InvalidDirectoryFilter");
        }
        Limits.Check(); DirectorySnapshot snapshot; snapshot.root_ = Root;
        snapshot.entry_ = require(reader(Root)->Resolve(locator(Root), context(Limits)).get());
        enumerate(snapshot, Policy, Limits, AllowIncomplete);
        return snapshot;
    }
    void ValidateDirectory(const DirectorySnapshot& Snapshot, const DirectoryPolicy& Policy, const Budget& Limits)
    {
        if (!Snapshot.issues_.isEmpty()) throw Failure("DirectoryComparisonIncomplete");
        const auto current = CaptureDirectory(Snapshot.root_, Policy, Limits);
        if (current.entry_.id_.revision_ != Snapshot.entry_.id_.revision_ || current.entries_.size() != Snapshot.entries_.size())
            throw Failure("DirectoryChanged");
        auto before = Snapshot.entries_.cbegin(), after = current.entries_.cbegin();
        for (; before != Snapshot.entries_.cend(); ++before, ++after)
        {
            Limits.Check();
            if (before.key() != after.key() || before->id_.revision_ != after->id_.revision_ ||
                before->directory_ != after->directory_ || relative(&*before) != relative(&*after)) throw Failure("DirectoryChanged");
        }
    }
    void CompareDirectory(const Request& Input, Result& Value, const Budget& Limits)
    {
        if (!Input.file3_.isEmpty()) throw Failure("DirectoryIndependentThreeUnsupported");
        auto captured = std::make_shared<std::vector<DirectorySnapshot>>();
        captured->push_back(CaptureDirectory(Input.file1_, Input.directory_, Limits, true));
        captured->push_back(CaptureDirectory(Input.file2_, Input.directory_, Limits, true));
        if (!Input.base_.isEmpty()) captured->push_back(CaptureDirectory(Input.base_, Input.directory_, Limits, true));
        const bool three = captured->size() == 3;
        Value.operation_ = three ? "directory3" : "directory2";
        Value.directoryLevel_ = Input.directory_.level_;
        Value.directoryFiltered_ = !Input.directory_.include_.isEmpty() || !Input.directory_.exclude_.isEmpty();
        QSet<QString> keys;
        for (const auto& tree : *captured) for (auto it = tree.entries_.cbegin(); it != tree.entries_.cend(); ++it) keys.insert(it.key());
        auto sorted = keys.values(); std::sort(sorted.begin(), sorted.end());
        bool different = false, conflict = false;
        for (const auto& snapshot : *captured) for (auto it = snapshot.issues_.cbegin(); it != snapshot.issues_.cend(); ++it)
        {
            Value.directoryComplete_ = false;
            Value.warnings_.append(QJsonObject{{"root", snapshot.root_}, {"path", it.key()}, {"error", it.value()}});
        }
        for (const auto& key : sorted)
        {
            Limits.Check(); std::array<const storage::EntrySnapshot*, 3> entries{};
            for (std::size_t i = 0; i < captured->size(); ++i)
            {
                const auto found = (*captured)[i].entries_.constFind(key);
                if (found != (*captured)[i].entries_.cend()) entries[i] = &*found;
            }
            QJsonArray errors;
            for (std::size_t i = 0; i < captured->size(); ++i)
            {
                const auto issue = issueFor((*captured)[i], key, entries[i] != nullptr);
                if (!issue.isEmpty()) errors.append(QJsonObject{{"side", int(i)}, {"root", (*captured)[i].root_}, {"path", key}, {"error", issue}});
            }
            const auto safeRelation = [&](int A, int B) -> QString {
                if (!errors.isEmpty()) return "Error";
                try { return relation(entries[A], entries[B], Input.directory_, Limits); }
                catch (const Failure& error)
                {
                    if (!recoverable(error)) throw;
                    errors.append(QJsonObject{{"path", key}, {"error", QString::fromStdString(error.code_)}}); return "Error";
                }
            };
            const auto lr = safeRelation(0, 1);
            QString status = lr, lb, rb;
            if (three)
            {
                lb = safeRelation(0, 2);
                rb = safeRelation(1, 2);
                if (lr == "Equal") status = lb == "Equal" ? "Equal" : !entries[0] ? "BothDeleted" : "BothSame";
                else if (lb == "Equal") status = !entries[1] ? "RightDeleted" : !entries[2] ? "RightAdded" : "RightChanged";
                else if (rb == "Equal") status = !entries[0] ? "LeftDeleted" : !entries[2] ? "LeftAdded" : "LeftChanged";
                else status = "Conflict";
            }
            if (!errors.isEmpty()) { status = "Error"; Value.directoryComplete_ = false; }
            conflict |= status == "Conflict" || status == "TypeConflict";
            different |= status != "Equal";
            const auto* item = entries[0] ? entries[0] : entries[1] ? entries[1] : entries[2];
            Value.entries_.append(QJsonObject{{"path", relative(item)}, {"status", status},
                {"leftRight", lr}, {"leftBase", lb}, {"rightBase", rb}, {"errors", errors},
                {"left", describe(entries[0])}, {"right", describe(entries[1])}, {"base", describe(entries[2])}});
        }
        // Two traversals detect membership/revision changes; this is not an atomic filesystem snapshot.
        if (Value.directoryComplete_) for (const auto& snapshot : *captured) ValidateDirectory(snapshot, Input.directory_, Limits);
        Value.directories_ = std::move(captured);
        Value.exitCode_ = Value.directoryComplete_ ? (different ? 1 : 0) : 2;
        Value.outcome_ = Value.directoryComplete_ ? (conflict ? "Conflict" : different ? "Different" : "Equal") : "Partial";
        if (!Value.directoryComplete_) Value.error_ = "DirectoryComparisonIncomplete";
    }
}
