#include "stdafx.h"
#include "directory.hpp"
#include "session.hpp"
#include "UniqueLibs/storageProviders.hpp"
#include "UniqueLibs/document/documentTextCodec.hpp"
#include <QDir>
#include <thread>

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
            ctx.denyConcurrentMutation_ = true; ctx.incrementalEnumeration_ = true;
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
        void enumerate(DirectorySnapshot& Snapshot, const DirectoryPolicy& Policy, const Budget& Limits, bool AllowIncomplete, const std::function<void(const DirectoryEntry&)>& Discovered)
        {
            auto& result = Snapshot.entries_; const auto provider = reader(Snapshot.root_); auto ctx = context(Limits);
            // Retain listing metadata until filters run; included unavailable entries
            // still fail strict mutation capture below.
            ctx.allowUnavailableChildMetadata_ = true;
            const auto& root = Snapshot.entry_;
            if (!root.directory_) throw Failure("ExpectedDirectory");
            if (root.attributes_ & FILE_ATTRIBUTE_REPARSE_POINT) throw Failure("ReparseDirectoryUnsupported");
            std::vector<std::pair<storage::Locator, QString>> pending{{root.locator_, {}}};
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
                            if (entry.directory_ && error.isEmpty()) pending.push_back({entry.locator_, relative + '/'});
                            const auto inserted = result.insert(key, {relative, std::move(entry.id_.revision_), entry.size_, entry.createdUtcMs_, entry.modifiedUtcMs_, entry.attributes_, entry.directory_});
                            if (Discovered) Discovered(*inserted);
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
            // Small SDK headers must not allocate/zero a megabyte for every read and EOF probe.
            const auto readBytes = std::size_t(std::clamp<std::uint64_t>(A.size_, 1, 1024 * 1024));
            // Streams may return different short-read sizes, so compare a rolling byte window.
            std::vector<std::byte> aa, bb; std::size_t ai = 0, bi = 0; bool ae = false, be = false, equal = true;
            for (;;)
            {
                Limits.Check();
                if (ai == aa.size() && !ae) { auto chunk = require(a->Read(readBytes, ctx).get()); aa = std::move(chunk.bytes_); ai = 0; ae = chunk.eof_; if (aa.empty() && !ae) throw Failure("ReadMadeNoProgress"); }
                if (bi == bb.size() && !be) { auto chunk = require(b->Read(readBytes, ctx).get()); bb = std::move(chunk.bytes_); bi = 0; be = chunk.eof_; if (bb.empty() && !be) throw Failure("ReadMadeNoProgress"); }
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

        QString textRelation(const storage::EntrySnapshot& A, const storage::EntrySnapshot& B,
            const QString& EncodingA, const QString& EncodingB, bool RawEqual, const Budget& Limits)
        {
            constexpr qsizetype maxTextBytes = 16 * 1024 * 1024;
            if (A.size_ > maxTextBytes || B.size_ > maxTextBytes) return RawEqual ? "Equal" : "Different";
            const auto decode = [&](const storage::EntrySnapshot& Entry, const QString& Encoding) -> std::optional<QString> {
                Limits.Check(); const auto snapshot = ReadFileSnapshot(QString::fromStdString(Entry.locator_.root_), maxTextBytes, Limits);
                if (snapshot.entry_.id_.revision_ != Entry.id_.revision_) throw Failure("ChangedDuringRead");
                const auto& raw = snapshot.raw_;
                const auto bom = raw.startsWith("\xef\xbb\xbf") ? QString("UTF-8") : raw.startsWith("\xff\xfe") ? QString("UTF-16LE") : raw.startsWith("\xfe\xff") ? QString("UTF-16BE") : QString{};
                if (!Encoding.isEmpty() && !bom.isEmpty() && Encoding != bom) return {};
                const auto decoded = document::DecodeDocumentText(raw, Encoding.isEmpty() ? (bom.isEmpty() ? QString("UTF-8") : bom) : Encoding);
                Limits.Check(); if (!decoded.HasValue()) return {};
                return decoded.Value().text_;
            };
            const auto a = decode(A, EncodingA); if (!a) return RawEqual ? "Equal" : "Different";
            const auto b = decode(B, EncodingB); if (!b) return RawEqual ? "Equal" : "Different";
            return *a == *b ? (RawEqual ? "Equal" : "Equivalent") : "Different";
        }
        QString relative(const DirectoryEntry* Entry)
        { return Entry ? Entry->path_ : QString{}; }
        QJsonObject describe(const DirectoryEntry* Entry)
        {
            if (!Entry) return {{"present", false}};
            return {{"present", true}, {"path", relative(Entry)}, {"directory", Entry->directory_},
                {"size", QString::number(Entry->size_)}, {"modifiedUtcMs", QString::number(Entry->modifiedUtcMs_)}};
        }
        QString relation(const DirectoryEntry* A, const DirectoryEntry* B,
            const DirectorySnapshot& RootA, const DirectorySnapshot& RootB, const Request& Input, int SideA, int SideB, const Budget& Limits)
        {
            if (!A && !B) return "Equal";
            if (!A) return "RightOnly";
            if (!B) return "LeftOnly";
            if (A->directory_ != B->directory_) return "TypeConflict";
            if (relative(A) != relative(B)) return "NameDifferent";
            if (A->directory_) return "Equal";
            if (Input.directory_.level_ == "metadata")
                return A->size_ == B->size_ && A->modifiedUtcMs_ == B->modifiedUtcMs_ ? "Equal" : "Different";
            const auto a = RootA.Expand(*A), b = RootB.Expand(*B);
            const bool rawEqual = sameContent(a, b, Limits);
            const auto codecA = InputEncoding(Input, SideA), codecB = InputEncoding(Input, SideB);
            if (Input.directory_.level_ != "text" || (rawEqual && codecA == codecB)) return rawEqual ? "Equal" : "Different";
            return textRelation(a, b, codecA, codecB, rawEqual, Limits);
        }
    }
    namespace
    {
        const QStringList& statusNames()
        {
            static const QStringList names{"", "Equal", "Different", "LeftOnly", "RightOnly", "TypeConflict", "NameDifferent", "LeftChanged", "RightChanged",
                "LeftAdded", "RightAdded", "LeftDeleted", "RightDeleted", "BothDeleted", "BothSame", "Conflict", "MirrorExtra", "Error", "Pending", "Equivalent"};
            return names;
        }
    }
    void DirectoryProgressChannel::Push(DirectoryProgress Value, const Budget& Limits)
    {
        // Bound queued data even if painting is slower than enumeration. Cancellation
        // remains observable while the UI is closing and waiting for its worker.
        for (;;)
        {
            Limits.Check();
            { std::lock_guard lock(mutex_); if (queue_.size() < 4) { queue_.push_back(std::move(Value)); return; } }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    std::optional<DirectoryProgress> DirectoryProgressChannel::Take()
    {
        std::lock_guard lock(mutex_); if (queue_.empty()) return {};
        auto value = std::move(queue_.front()); queue_.pop_front(); return value;
    }
    DirectoryRow::DirectoryRow(const QJsonObject& Object) : path_(Object["path"].toString()), errors_(Object["errors"].toArray())
    {
        static const QStringList fields{"status", "leftRight", "leftBase", "rightBase"}, sides{"left", "right", "base"};
        for (int i = 0; i < 4; ++i)
        {
            const auto index = statusNames().indexOf(Object[fields[i]].toString());
            if (index < 0) throw Failure("InvalidDirectoryStatus"); statuses_[i] = quint8(index);
        }
        for (int i = 0; i < 3; ++i)
        {
            const auto side = Object[sides[i]].toObject(); auto& target = sides_[i];
            target.present_ = side["present"].toBool(); target.directory_ = side["directory"].toBool();
            const auto path = side["path"].toString(path_); if (path != path_) target.path_ = path;
            target.size_ = side["size"].toString().toULongLong(); target.modified_ = side["modifiedUtcMs"].toString().toLongLong();
        }
    }
    QString DirectoryRow::Status(int Index) const { return statusNames()[statuses_[Index]]; }
    QJsonObject DirectoryRow::toObject() const
    {
        QJsonObject result{{"path", path_}, {"status", Status()}, {"leftRight", Status(1)}, {"leftBase", Status(2)}, {"rightBase", Status(3)}, {"errors", errors_}};
        static const QStringList sides{"left", "right", "base"};
        for (int i = 0; i < 3; ++i)
        {
            const auto& side = sides_[i]; QJsonObject value{{"present", side.present_}};
            if (side.present_) { value["path"] = side.path_.isEmpty() ? path_ : side.path_; value["directory"] = side.directory_;
                value["size"] = QString::number(side.size_); value["modifiedUtcMs"] = QString::number(side.modified_); }
            result[sides[i]] = value;
        }
        return result;
    }
    storage::EntrySnapshot DirectorySnapshot::Expand(const DirectoryEntry& Entry) const
    {
        storage::EntrySnapshot result; result.locator_ = locator(QDir(root_).filePath(Entry.path_));
        result.id_.revision_ = Entry.revision_; result.directory_ = Entry.directory_; result.size_ = Entry.size_;
        result.createdUtcMs_ = Entry.createdUtcMs_; result.modifiedUtcMs_ = Entry.modifiedUtcMs_; result.attributes_ = Entry.attributes_;
        result.name_ = Entry.path_.section('/', -1).toStdString(); result.hints_["relativePath"] = Entry.path_.toStdString(); return result;
    }
    DirectorySnapshot CaptureDirectory(const QString& Root, const DirectoryPolicy& Policy, const Budget& Limits, bool AllowIncomplete, const std::function<void(const DirectoryEntry&)>& Discovered)
    {
        if (Policy.level_ != "bytes" && Policy.level_ != "metadata" && Policy.level_ != "text") throw Failure("InvalidDirectoryLevel");
        for (const auto& patterns : {Policy.include_, Policy.exclude_})
        {
            if (patterns.size() > 32) throw Failure("DirectoryFilterLimit");
            for (const auto& pattern : patterns)
                if (pattern.isEmpty() || pattern.size() > 256 || pattern.contains('\\')) throw Failure("InvalidDirectoryFilter");
        }
        Limits.Check(); DirectorySnapshot snapshot; snapshot.root_ = Root;
        snapshot.entry_ = require(reader(Root)->Resolve(locator(Root), context(Limits)).get());
        enumerate(snapshot, Policy, Limits, AllowIncomplete, Discovered);
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
            if (before.key() != after.key() || before->revision_ != after->revision_ ||
                before->directory_ != after->directory_ || relative(&*before) != relative(&*after)) throw Failure("DirectoryChanged");
        }
    }
    void CompareDirectory(const Request& Input, Result& Value, const Budget& Limits)
    {
        if (!Input.file3_.isEmpty()) throw Failure("DirectoryIndependentThreeUnsupported");
        auto captured = std::make_shared<std::vector<DirectorySnapshot>>();
        DirectoryProgress progress; progress.phase_ = "scan";
        auto flushed = std::chrono::steady_clock::now();
        const auto flush = [&](bool Force) {
            if (!Input.directoryProgress_) return;
            const auto now = std::chrono::steady_clock::now();
            if (!Force && progress.rows_.size() < 512 && now - flushed < std::chrono::milliseconds(100)) return;
            DirectoryProgress batch; batch.phase_ = progress.phase_; batch.side_ = progress.side_; batch.processed_ = progress.processed_; batch.total_ = progress.total_;
            batch.rows_ = std::move(progress.rows_); Input.directoryProgress_->Push(std::move(batch), Limits);
            progress.rows_ = {}; flushed = now;
        };
        const QStringList roots = Input.base_.isEmpty() ? QStringList{Input.file1_, Input.file2_} : QStringList{Input.file1_, Input.file2_, Input.base_};
        for (int side = 0; side < roots.size(); ++side)
        {
            progress.side_ = side; progress.phase_ = "scan";
            captured->push_back(CaptureDirectory(roots[side], Input.directory_, Limits, true, [&](const DirectoryEntry& Entry) {
                ++progress.processed_;
                if (!Input.directoryProgress_) return;
                DirectoryRow row; row.path_ = Entry.path_; row.statuses_[0] = 18;
                row.sides_[side] = {{}, Entry.size_, Entry.modifiedUtcMs_, true, Entry.directory_};
                progress.rows_.append(row); flush(false);
            }));
            flush(true);
        }
        const bool three = captured->size() == 3;
        Value.operation_ = three ? "directory3" : "directory2";
        Value.directoryLevel_ = Input.directory_.level_;
        Value.directoryFiltered_ = !Input.directory_.include_.isEmpty() || !Input.directory_.exclude_.isEmpty();
        QSet<QString> keys;
        for (const auto& tree : *captured) for (auto it = tree.entries_.cbegin(); it != tree.entries_.cend(); ++it) keys.insert(it.key());
        auto sorted = keys.values(); std::sort(sorted.begin(), sorted.end());
        bool different = false, conflict = false;
        progress.phase_ = "compare"; progress.side_ = -1; progress.processed_ = 0; progress.total_ = sorted.size(); flush(true);
        for (const auto& snapshot : *captured) for (auto it = snapshot.issues_.cbegin(); it != snapshot.issues_.cend(); ++it)
        {
            Value.directoryComplete_ = false;
            Value.warnings_.append(QJsonObject{{"root", snapshot.root_}, {"path", it.key()}, {"error", it.value()}});
        }
        for (const auto& key : sorted)
        {
            Limits.Check(); std::array<const DirectoryEntry*, 3> entries{};
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
                try { return relation(entries[A], entries[B], (*captured)[A], (*captured)[B], Input, A, B, Limits); }
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
                if (DirectoryContentEqual(lr)) status = DirectoryContentEqual(lb) ? lr : !entries[0] ? "BothDeleted" : "BothSame";
                else if (DirectoryContentEqual(lb)) status = !entries[1] ? "RightDeleted" : !entries[2] ? "RightAdded" : "RightChanged";
                else if (DirectoryContentEqual(rb)) status = !entries[0] ? "LeftDeleted" : !entries[2] ? "LeftAdded" : "LeftChanged";
                else status = "Conflict";
            }
            if (!errors.isEmpty()) { status = "Error"; Value.directoryComplete_ = false; }
            conflict |= status == "Conflict" || status == "TypeConflict";
            different |= !DirectoryContentEqual(status);
            const auto* item = entries[0] ? entries[0] : entries[1] ? entries[1] : entries[2];
            Value.entries_.append(QJsonObject{{"path", relative(item)}, {"status", status},
                {"leftRight", lr}, {"leftBase", lb}, {"rightBase", rb}, {"errors", errors},
                {"left", describe(entries[0])}, {"right", describe(entries[1])}, {"base", describe(entries[2])}});
            ++progress.processed_; if (Input.directoryProgress_) { progress.rows_.append(Value.entries_.last()); flush(false); }
        }
        flush(true); progress.phase_ = "validate"; flush(true);
        // Two traversals detect membership/revision changes; this is not an atomic filesystem snapshot.
        if (Value.directoryComplete_) for (const auto& snapshot : *captured) ValidateDirectory(snapshot, Input.directory_, Limits);
        Value.directories_ = std::move(captured);
        Value.exitCode_ = Value.directoryComplete_ ? (different ? 1 : 0) : 2;
        Value.outcome_ = Value.directoryComplete_ ? (conflict ? "Conflict" : different ? "Different" : "Equal") : "Partial";
        if (!Value.directoryComplete_) Value.error_ = "DirectoryComparisonIncomplete";
    }
}
