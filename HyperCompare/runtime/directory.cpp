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
        DirectoryTree enumerate(const QString& Root, const DirectoryPolicy& Policy, const Budget& Limits)
        {
            DirectoryTree result; const auto provider = reader(Root); const auto ctx = context(Limits);
            const auto root = require(provider->Resolve(locator(Root), ctx).get());
            if (!root.directory_) throw Failure("ExpectedDirectory");
            if (root.attributes_ & FILE_ATTRIBUTE_REPARSE_POINT) throw Failure("ReparseDirectoryUnsupported");
            std::vector<std::pair<storage::Locator, QString>> pending{{root.locator_, {}}};
            std::size_t visited = 0;
            while (!pending.empty())
            {
                Limits.Check(); auto [at, prefix] = std::move(pending.back()); pending.pop_back();
                const auto cursor = require(provider->Enumerate(at, ctx).get());
                for (;;)
                {
                    Limits.Check(); auto page = require(cursor->Next(256, ctx).get());
                    for (auto& entry : page.entries_)
                    {
                        if (++visited > 100000) throw Failure("DirectoryEntryLimit");
                        const auto relative = prefix + QString::fromStdString(entry.name_);
                        if (matches(Policy.exclude_, relative)) continue;
                        if (entry.attributes_ & FILE_ATTRIBUTE_REPARSE_POINT) throw Failure("ReparseDirectoryUnsupported");
                        if (relative.count('/') > 128) throw Failure("DirectoryDepthLimit");
                        if (!entry.directory_ && !Policy.include_.isEmpty() && !matches(Policy.include_, relative)) continue;
                        const auto key = relative.toCaseFolded();
                        if (result.contains(key)) throw Failure("CaseCollision");
                        entry.hints_["relativePath"] = relative.toUtf8().toStdString();
                        if (entry.directory_) pending.push_back({entry.locator_, relative + '/'});
                        result.insert(key, std::move(entry));
                    }
                    if (page.end_) break;
                }
            }
            return result;
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
    DirectorySnapshot CaptureDirectory(const QString& Root, const DirectoryPolicy& Policy, const Budget& Limits)
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
        snapshot.entries_ = enumerate(Root, Policy, Limits);
        return snapshot;
    }
    void ValidateDirectory(const DirectorySnapshot& Snapshot, const DirectoryPolicy& Policy, const Budget& Limits)
    {
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
        captured->push_back(CaptureDirectory(Input.file1_, Input.directory_, Limits));
        captured->push_back(CaptureDirectory(Input.file2_, Input.directory_, Limits));
        if (!Input.base_.isEmpty()) captured->push_back(CaptureDirectory(Input.base_, Input.directory_, Limits));
        const bool three = captured->size() == 3;
        Value.operation_ = three ? "directory3" : "directory2";
        Value.directoryLevel_ = Input.directory_.level_;
        Value.directoryFiltered_ = !Input.directory_.include_.isEmpty() || !Input.directory_.exclude_.isEmpty();
        QSet<QString> keys;
        for (const auto& tree : *captured) for (auto it = tree.entries_.cbegin(); it != tree.entries_.cend(); ++it) keys.insert(it.key());
        auto sorted = keys.values(); std::sort(sorted.begin(), sorted.end());
        bool different = false, conflict = false;
        for (const auto& key : sorted)
        {
            Limits.Check(); std::array<const storage::EntrySnapshot*, 3> entries{};
            for (std::size_t i = 0; i < captured->size(); ++i)
            {
                const auto found = (*captured)[i].entries_.constFind(key);
                if (found != (*captured)[i].entries_.cend()) entries[i] = &*found;
            }
            const auto lr = relation(entries[0], entries[1], Input.directory_, Limits);
            QString status = lr, lb, rb;
            if (three)
            {
                lb = relation(entries[0], entries[2], Input.directory_, Limits);
                rb = relation(entries[1], entries[2], Input.directory_, Limits);
                if (lr == "Equal") status = lb == "Equal" ? "Equal" : !entries[0] ? "BothDeleted" : "BothSame";
                else if (lb == "Equal") status = !entries[1] ? "RightDeleted" : !entries[2] ? "RightAdded" : "RightChanged";
                else if (rb == "Equal") status = !entries[0] ? "LeftDeleted" : !entries[2] ? "LeftAdded" : "LeftChanged";
                else status = "Conflict";
            }
            conflict |= status == "Conflict" || status == "TypeConflict";
            different |= status != "Equal";
            const auto* item = entries[0] ? entries[0] : entries[1] ? entries[1] : entries[2];
            Value.entries_.append(QJsonObject{{"path", relative(item)}, {"status", status},
                {"leftRight", lr}, {"leftBase", lb}, {"rightBase", rb},
                {"left", describe(entries[0])}, {"right", describe(entries[1])}, {"base", describe(entries[2])}});
        }
        // Two traversals detect membership/revision changes; this is not an atomic filesystem snapshot.
        for (const auto& snapshot : *captured) ValidateDirectory(snapshot, Input.directory_, Limits);
        Value.directories_ = std::move(captured);
        Value.exitCode_ = different ? 1 : 0; Value.outcome_ = conflict ? "Conflict" : different ? "Different" : "Equal";
    }
}
