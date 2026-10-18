#include "wcxMutation.hpp"
#include "wcxBackend.hpp"
#include "wcxStorage.hpp"
#include "nativeMaterializer.hpp"
#include "nativeWritableStorage.hpp"
#include "storageProviders.hpp"
#include <QtCore>
#include <set>
#include <array>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

namespace hc::storage
{
    namespace
    {
        Error Invalid(std::string Code) { return {ErrorCategory::InvalidRequest, std::move(Code), {"wcx"}}; }
        std::wstring Native(const QString& Path) { return QDir::toNativeSeparators(Path).toStdWString(); }
        bool Inside(const std::string& Path, const std::string& Parent) { return Path == Parent || Path.starts_with(Parent + "/"); }
        std::string Parent(const std::string& Path) { const auto Slash = Path.find_last_of('/'); return Slash == 0 ? "/" : Path.substr(0, Slash); }
        Result<std::string> SafePath(std::string Path, std::size_t MaxDepth)
        {
            if (Path.size() < 2 || Path.front() != '/' || Path.back() == '/' || Path.find('\0') != std::string::npos)
                return Invalid("invalid-archive-mutation-path");
            const auto Text = QString::fromStdString(Path);
            if (Text.toUtf8().toStdString() != Path || Text.size() > 1000) return Invalid("archive-mutation-path-encoding-or-length");
            const auto Parts = Text.mid(1).split('/');
            if (static_cast<std::size_t>(Parts.size()) > MaxDepth) return Error{ErrorCategory::BudgetExceeded, "archive-depth-budget"};
            for (const auto& Part : Parts)
            {
                const auto Stem = Part.section('.', 0, 0).toUpper();
                if (Part.isEmpty() || Part == "." || Part == ".." || Part.endsWith('.') || Part.endsWith(' ')
                    || Part.size() > 255 || Part.contains(QRegularExpression("[\\\\:*?\"<>|\\x{0000}-\\x{001f}]"))
                    || Stem == "CON" || Stem == "PRN" || Stem == "AUX" || Stem == "NUL"
                    || Stem.contains(QRegularExpression("^(COM|LPT)[1-9¹²³]$")))
                    return Invalid("archive-mutation-name-not-representable");
            }
            return Path;
        }
        std::uint32_t Crc(std::uint32_t Value, const QByteArray& Bytes)
        {
            static const auto Table = [] { std::array<std::uint32_t, 256> Values{}; for (std::uint32_t Index = 0; Index < 256; ++Index) {
                auto Value = Index; for (int Bit = 0; Bit < 8; ++Bit) Value = (Value >> 1) ^ ((Value & 1) ? 0xedb88320u : 0); Values[Index] = Value; } return Values; }();
            for (const auto Byte : Bytes) Value = (Value >> 8) ^ Table[(Value ^ static_cast<unsigned char>(Byte)) & 255];
            return Value;
        }
        struct DraftEntry
        {
            EntrySnapshot entry_;
            std::shared_ptr<IContentSource> content_;
            bool changed_ = false;
            bool reuseData_ = false;
            std::string metadataOrigin_;
        };
        class Session final : public IArchiveMutationSession
        {
        public:
            Session(std::string Plugin, Locator Root, std::shared_ptr<IStorageProvider> Reader,
                std::string Temporary, ArchiveMutationLimits Limits, EntrySnapshot Original,
                std::shared_ptr<IStorageProvider> NativeReader, std::shared_ptr<IWritableStorage> Writer, std::shared_ptr<hc::plugins::IArchiveBackend> Backend)
                : plugin_(std::move(Plugin)), root_(std::move(Root)), reader_(std::move(Reader)), temporary_(std::move(Temporary)),
                  limits_(Limits), original_(std::move(Original)), native_(std::move(NativeReader)), writer_(std::move(Writer)),
                  backend_(Backend ? std::move(Backend) : hc::plugins::MakeInProcessWcxBackend(plugin_)) {}
            ~Session() { Abort(); }
            Result<Unit> PrepareTogether(std::vector<ArchiveChange> Changes, std::unique_ptr<IArchiveMutationSession> Other,
                std::vector<ArchiveChange> OtherChanges, OperationContext Context) override
            {
                auto* Peer = dynamic_cast<Session*>(Other.get());
                if (state_ != State::New || !Peer || Peer->state_ != State::New
                    || root_.root_ != Peer->root_.root_ || SameArchiveContainer(root_, Peer->root_))
                    return Invalid("archive-joint-session-mismatch");
                state_ = State::Preparing;
                try
                {
                    if (Changes.empty())
                    {
                        parent_ = std::move(Other);
                        auto Prepared = parent_->Prepare(std::move(OtherChanges), Context);
                        if (Prepared.HasValue()) { state_ = State::Prepared; return Unit{}; }
                        const auto Failure = Prepared.Failure(); Abort(); return Failure;
                    }
                    struct Branch { Session* session_; std::unique_ptr<IArchiveMutationSession> owner_; std::vector<ArchiveChange> changes_; };
                    Branch Left{this, {}, std::move(Changes)}, Right{Peer, std::move(Other), std::move(OtherChanges)};
                    std::vector<std::shared_ptr<IMaterializedFileLease>> Leases;
                    const auto Promote = [&](Branch& Branch) -> Result<Unit> {
                        auto& Current = *Branch.session_;
                        if (Current.root_.nesting_.size() <= 1) return Invalid("archive-joint-root-mismatch");
                        auto Candidate = Current.prepareCandidate(std::move(Branch.changes_), Context);
                        if (!Candidate.HasValue()) return Candidate.Failure();
                        Leases.push_back(Candidate.Value());
                        auto Container = ResolveWcxContainer(Current.native_, Context); if (!Container.HasValue()) return Container.Failure();
                        auto ParentRoot = Current.original_.locator_; ParentRoot.nesting_.back().entry_ = "/";
                        auto Parent = BeginWcxMutation({}, ParentRoot, Current.native_, Current.temporary_, Current.limits_, Context, Current.writer_, Container.Value().backend_);
                        if (!Parent.HasValue()) return Parent.Failure();
                        auto LocalReader = MakeNativeReadProvider(LocatorKind::Local);
                        auto Local = LocalReader->Resolve({LocatorKind::Local, Candidate.Value()->LocalPath()}, Context).get(); if (!Local.HasValue()) return Local.Failure();
                        ArchiveChange Change{ArchiveChangeKind::Put, Current.original_.locator_.nesting_.back().entry_, {},
                            std::make_shared<ProviderContentSource>(LocalReader, Local.Value()), {}, {true, Current.original_.id_.revision_}};
                        auto Fresh = Current.native_->Stat(Current.original_.id_, Context).get(); if (!Fresh.HasValue()) return Fresh.Failure();
                        Branch.owner_ = std::move(Parent.Value()); Branch.session_ = static_cast<Session*>(Branch.owner_.get());
                        Branch.changes_ = {std::move(Change)}; return Unit{};
                    };
                    Result<Unit> Prepared = Unit{};
                    while (!SameArchiveContainer(Left.session_->root_, Right.session_->root_))
                    {
                        Prepared = Promote(Left.session_->root_.nesting_.size() >= Right.session_->root_.nesting_.size() ? Left : Right);
                        if (!Prepared.HasValue()) break;
                    }
                    if (Prepared.HasValue())
                    {
                        if (Left.session_->original_.locator_ != Right.session_->original_.locator_
                            || Left.session_->original_.id_.revision_ != Right.session_->original_.id_.revision_)
                            Prepared = Error{ErrorCategory::StaleRevision, "archive-joint-base-changed"};
                        else
                        {
                            // Both child candidates replace members of the same
                            // original container. A single batch checks and lifts
                            // their combined result through the remaining parents.
                            auto& Target = Left.session_ == this ? Left : Right.session_ == this ? Right : Left;
                            auto& Source = &Target == &Left ? Right : Left;
                            for (auto& Change : Source.changes_)
                            {
                                if (!Change.source_) continue;
                                auto At = Target.session_->root_; At.nesting_.back().entry_ = Change.path_;
                                auto Selected = Source.session_->reader_->Stat(*Change.source_, Context).get();
                                if (!Selected.HasValue()) return Selected.Failure();
                                auto Fresh = Target.session_->reader_->Resolve(At, Context).get();
                                if (!Fresh.HasValue()) return Fresh.Failure();
                                if (Fresh.Value().locator_ != Selected.Value().locator_ || Fresh.Value().id_.revision_ != Change.source_->revision_)
                                    return Error{ErrorCategory::StaleRevision, "archive-joint-source-changed"};
                                Change.source_ = Fresh.Value().id_;
                            }
                            Target.changes_.insert(Target.changes_.end(), std::make_move_iterator(Source.changes_.begin()), std::make_move_iterator(Source.changes_.end()));
                            if (Target.session_ == this) Prepared = prepare(std::move(Target.changes_), Context);
                            else
                            {
                                parent_ = std::move(Target.owner_);
                                Prepared = parent_->Prepare(std::move(Target.changes_), Context);
                            }
                        }
                    }
                    if (Prepared.HasValue()) { state_ = State::Prepared; return Unit{}; }
                    auto Failure = Prepared.Failure(); const auto Errors = Abort();
                    if (!Errors.empty()) Failure.userAction_ = "cleanup-pending:" + Errors.front().userAction_;
                    return Failure;
                }
                catch (...) { const auto Errors = Abort(); auto Failure = Error{ErrorCategory::Internal, "archive-joint-prepare-exception"};
                    if (!Errors.empty()) Failure.userAction_ = "cleanup-pending:" + Errors.front().userAction_; return Failure; }
            }
            Result<Unit> Prepare(std::vector<ArchiveChange> Changes, OperationContext Context) override
            {
                if (state_ != State::New) return Invalid("archive-session-not-new");
                state_ = State::Preparing;
                try
                {
                    auto Prepared = prepare(std::move(Changes), Context);
                    if (Prepared.HasValue()) { state_ = State::Prepared; return Prepared; }
                    auto Failure = Prepared.Failure(); const auto Errors = Abort();
                    if (!Errors.empty()) Failure.userAction_ = "cleanup-pending:" + Errors.front().userAction_;
                    return Failure;
                }
                catch (...) { const auto Errors = Abort(); auto Failure = Error{ErrorCategory::Internal, "archive-prepare-exception"};
                    if (!Errors.empty()) Failure.userAction_ = "cleanup-pending:" + Errors.front().userAction_; return Failure; }
            }
            Result<WriteReceipt> Commit(OperationContext Context) override
            {
                if (state_ != State::Prepared || (!output_ && !parent_)) return Invalid("archive-session-not-prepared");
                if (const auto Failure = Context.Check({"wcx"})) return *Failure;
                auto Result = parent_ ? parent_->Commit(Context) : output_->Commit(Context);
                if (Result.HasValue()) { state_ = State::Committed; output_.reset(); parent_.reset(); }
                else
                {
                    auto Failure = Result.Failure(); const auto Errors = Abort();
                    if (!Errors.empty()) Failure.userAction_ += ";cleanup-pending:" + Errors.front().userAction_;
                    return Failure;
                }
                return Result;
            }
            std::vector<Error> Abort() override
            {
                std::vector<Error> Errors;
                if (parent_) { Errors = parent_->Abort(); parent_.reset(); }
                if (output_) { const auto Result = output_->Abort(); if (!Result.HasValue()) Errors.push_back(Result.Failure()); output_.reset(); }
                if (state_ != State::Committed) state_ = State::Aborted;
                return Errors;
            }
        private:
            enum class State { New, Preparing, Prepared, Committed, Aborted };
            Result<Unit> snapshot(OperationContext Context)
            {
                std::vector<Locator> Pending{root_};
                while (!Pending.empty())
                {
                    auto At = std::move(Pending.back()); Pending.pop_back();
                    auto Cursor = reader_->Enumerate(At, Context).get(); if (!Cursor.HasValue()) return Cursor.Failure();
                    for (;;)
                    {
                        auto Page = Cursor.Value()->Next(Context.maxPageItems_, Context).get(); if (!Page.HasValue()) return Page.Failure();
                        if (Page.Value().entries_.empty() && !Page.Value().end_) return Invalid("archive-cursor-no-progress");
                        for (const auto& Entry : Page.Value().entries_)
                        {
                            if (Entry.locator_.kind_ != LocatorKind::Archive || Entry.locator_.root_ != root_.root_
                                || !SameArchiveContainer(Entry.locator_, root_)
                                || Entry.id_.providerId_ != reader_->Id()) return Invalid("archive-provider-entry-mismatch");
                            const auto Path = SafePath(Entry.locator_.nesting_.back().entry_, limits_.maxDepth_); if (!Path.HasValue()) return Path.Failure();
                            if (Parent(Path.Value()) != At.nesting_.back().entry_ || ((Entry.attributes_ & FILE_ATTRIBUTE_REPARSE_POINT) && !Entry.hints_.contains("metadataRename")))
                                return Invalid("archive-provider-entry-parent");
                            const auto Raw = Entry.hints_.find("wcxName");
                            if (!draft_.emplace(Path.Value(), DraftEntry{Entry, {}, false, false, Raw == Entry.hints_.end() ? std::string{} : Raw->second}).second) return Invalid("duplicate-archive-entry");
                            if (draft_.size() > limits_.maxEntries_) return Error{ErrorCategory::BudgetExceeded, "archive-entry-budget"};
                            if (Entry.directory_ && !(Entry.attributes_ & FILE_ATTRIBUTE_REPARSE_POINT)) Pending.push_back(Entry.locator_);
                        }
                        if (Page.Value().end_) break;
                    }
                }
                before_ = draft_; return Unit{};
            }
            Result<Unit> expected(const std::string& Path, DestinationExpectation Expected)
            {
                const auto Found = draft_.find(Path);
                if ((Found != draft_.end()) != Expected.exists_) return Error{ErrorCategory::StaleRevision, "archive-destination-changed"};
                if (Found != draft_.end() && (!Expected.revision_ || Found->second.entry_.id_.revision_ != Expected.revision_))
                    return Error{ErrorCategory::StaleRevision, "archive-destination-revision-changed"};
                return Unit{};
            }
            Result<Unit> plan(const std::vector<ArchiveChange>& Changes, OperationContext Context)
            {
                if (Changes.empty() || Changes.size() > limits_.maxEntries_) return Invalid("archive-change-count");
                for (const auto& Change : Changes)
                {
                    if (const auto Failure = Context.Check({"wcx"})) return *Failure;
                    const auto Valid = SafePath(Change.path_, limits_.maxDepth_); if (!Valid.HasValue()) return Valid.Failure();
                    if (Change.kind_ == ArchiveChangeKind::Remove || Change.kind_ == ArchiveChangeKind::Rename)
                    {
                        if (!Change.source_ || !draft_.contains(Change.path_) || draft_.at(Change.path_).changed_
                            || draft_.at(Change.path_).entry_.id_ != *Change.source_) return Error{ErrorCategory::StaleRevision, "archive-source-changed"};
                        if (Change.kind_ == ArchiveChangeKind::Rename)
                        {
                            const auto Target = SafePath(Change.target_, limits_.maxDepth_); if (!Target.HasValue()) return Target.Failure();
                            if (Inside(Change.target_, Change.path_)) return Invalid("archive-rename-inside-source");
                            if (Change.destination_.exists_) return Error{ErrorCategory::Unsupported, "archive-rename-replace-unavailable"};
                            const auto Expected = expected(Change.target_, Change.destination_); if (!Expected.HasValue()) return Expected;
                        }
                        std::vector<std::pair<std::string, DraftEntry>> Renamed;
                        std::vector<std::string> Affected{Change.path_};
                        const auto Prefix = Change.path_ + "/";
                        for (auto It = draft_.lower_bound(Prefix); It != draft_.end() && It->first.starts_with(Prefix); ++It) Affected.push_back(It->first);
                        for (const auto& Path : Affected)
                        {
                            if (Change.kind_ == ArchiveChangeKind::Rename)
                            {
                                auto Item = draft_.at(Path);
                                Item.reuseData_ = Item.entry_.hints_.contains("metadataRename");
                                if (!Item.reuseData_ && !Item.entry_.directory_ && !Item.content_) Item.content_ = std::make_shared<ProviderContentSource>(reader_, Item.entry_);
                                Item.changed_ = true;
                                Renamed.emplace_back(Change.target_ + Path.substr(Change.path_.size()), std::move(Item));
                            }
                            draft_.erase(Path);
                        }
                        for (auto& [Path, Item] : Renamed)
                            if (!draft_.emplace(Path, std::move(Item)).second) return Invalid("archive-rename-collision");
                    }
                    else if (Change.kind_ == ArchiveChangeKind::Put || Change.kind_ == ArchiveChangeKind::MakeDirectory)
                    {
                        const auto Expected = expected(Change.path_, Change.destination_); if (!Expected.HasValue()) return Expected;
                        if (draft_.contains(Change.path_) && draft_.at(Change.path_).entry_.directory_) return Invalid("archive-directory-replace");
                        DraftEntry Item; Item.changed_ = true; Item.entry_.directory_ = Change.kind_ == ArchiveChangeKind::MakeDirectory;
                        if (draft_.contains(Change.path_) && (draft_.at(Change.path_).entry_.attributes_ & FILE_ATTRIBUTE_REPARSE_POINT)) return Error{ErrorCategory::Unsupported, "archive-special-content-replace"};
                        if (draft_.contains(Change.path_)) Item.metadataOrigin_ = draft_.at(Change.path_).metadataOrigin_;
                        if (!Item.entry_.directory_)
                        {
                            if (!Change.content_) return Invalid("archive-content-required");
                            Item.content_ = Change.content_; Item.entry_ = Item.content_->Describe();
                            if (Item.entry_.directory_ || Item.entry_.attributes_ & FILE_ATTRIBUTE_REPARSE_POINT) return Invalid("archive-file-content-required");
                        }
                        if (draft_.contains(Change.path_) && draft_.at(Change.path_).entry_.hints_.contains("encrypted")) Item.entry_.hints_["encrypted"] = "1";
                        draft_[Change.path_] = std::move(Item);
                    }
                    else return Invalid("archive-change-kind");
                }
                std::set<QString> NativeNames;
                std::vector<std::string> Parents;
                std::uint64_t ExpandedBytes = 0;
                for (const auto& [Path, Item] : draft_)
                {
                    const auto Valid = SafePath(Path, limits_.maxDepth_); if (!Valid.HasValue()) return Valid.Failure();
                    if (!NativeNames.insert(QString::fromStdString(Path).toCaseFolded()).second) return Invalid("archive-native-name-collision");
                    if (!Item.entry_.directory_)
                    {
                        if (Item.entry_.size_ > limits_.maxInputBytes_ - ExpandedBytes) return Error{ErrorCategory::BudgetExceeded, "archive-expanded-byte-budget"};
                        ExpandedBytes += Item.entry_.size_;
                        if (Item.entry_.hints_.contains("packedSize"))
                        {
                            const auto Reported = std::stoull(Item.entry_.hints_.at("packedSize"));
                            const auto Packed = Reported ? Reported : original_.size_;
                            if (Context.workload_.IsAutomatic() && Item.entry_.size_ && (!Packed || (Item.entry_.size_ - 1) / Packed >= Context.workload_.automaticExpansionRatio_)) return Error{ErrorCategory::BudgetExceeded, "archive-expansion-budget"};
                        }
                    }
                    for (auto Up = Parent(Path); Up != "/"; Up = Parent(Up))
                    {
                        if (draft_.contains(Up) && !draft_.at(Up).entry_.directory_) return Invalid("archive-file-parent");
                        if (!draft_.contains(Up)) Parents.push_back(Up);
                    }
                }
                for (const auto& Path : Parents) { DraftEntry Item; Item.entry_.directory_ = true; Item.changed_ = true; draft_.try_emplace(Path, std::move(Item)); }
                // A synthesized parent has no archive record of its own. Once
                // its last descendant is removed it disappears naturally.
                std::vector<std::string> ImplicitParents;
                for (const auto& [Path, Item] : draft_)
                    if (Item.entry_.directory_ && !Item.changed_ && Item.entry_.hints_.empty()) ImplicitParents.push_back(Path);
                for (auto It = ImplicitParents.rbegin(); It != ImplicitParents.rend(); ++It)
                {
                    const auto Prefix = *It + "/";
                    const auto Child = draft_.lower_bound(Prefix);
                    if (Child == draft_.end() || !Child->first.starts_with(Prefix)) draft_.erase(*It);
                }
                if (draft_.size() > limits_.maxEntries_) return Error{ErrorCategory::BudgetExceeded, "archive-entry-budget"};
                return Unit{};
            }
            Result<Unit> copySource(const std::shared_ptr<IContentSource>& Source, const QString& To, OperationContext Context, std::uint32_t* Checksum = nullptr)
            {
                const auto Description = Source->Describe();
                const auto Verified = Source->Verify(Context).get(); if (!Verified.HasValue()) return Verified.Failure();
                auto ReadContext = Context; ReadContext.denyConcurrentMutation_ = true;
                auto Stream = Source->OpenRead(ReadContext).get(); if (!Stream.HasValue()) return Stream.Failure();
                QFile File(To); if (!File.open(QIODevice::WriteOnly | QIODevice::Truncate)) return Error{ErrorCategory::Unavailable, "archive-stage-open"};
                std::uint64_t Written = 0; std::uint32_t Check = 0xffffffff;
                for (;;)
                {
                    if (const auto Failure = Context.Check({"wcx"})) return *Failure;
                    auto Chunk = Stream.Value()->Read(std::min(Context.maxReadBytes_, std::size_t{1024 * 1024}), Context).get(); if (!Chunk.HasValue()) return Chunk.Failure();
                    if (Chunk.Value().bytes_.size() > std::min(Context.maxReadBytes_, std::size_t{1024 * 1024})
                        || Written > Description.size_ || Chunk.Value().bytes_.size() > Description.size_ - Written)
                        return Error{ErrorCategory::BudgetExceeded, "archive-input-size-changed"};
                    const auto Bytes = QByteArray::fromRawData(reinterpret_cast<const char*>(Chunk.Value().bytes_.data()), Chunk.Value().bytes_.size());
                    if (File.write(Bytes) != Bytes.size()) return Error{ErrorCategory::Unavailable, "archive-stage-write"};
                    Written += Bytes.size(); Check = Crc(Check, Bytes);
                    if (Chunk.Value().eof_) break;
                    if (Bytes.isEmpty()) return Invalid("archive-stream-no-progress");
                }
                if (Written != Description.size_) return Error{ErrorCategory::StaleRevision, "archive-input-size-changed"};
                if (!File.flush()) return Error{ErrorCategory::Unavailable, "archive-stage-flush"};
                if (Checksum) *Checksum = Check ^ 0xffffffff;
                if (!File.setFileTime(QDateTime::fromMSecsSinceEpoch(Description.modifiedUtcMs_), QFileDevice::FileModificationTime))
                    return Error{ErrorCategory::Unavailable, "archive-input-metadata"};
                File.close(); Stream.Value().reset();
                // Preserve Windows metadata for renamed/repacked members. Clear
                // readonly in the controlled input cleanup path below.
                const auto Attributes = Description.attributes_ & (FILE_ATTRIBUTE_ARCHIVE | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_READONLY);
                if (Checksum && !SetFileAttributesW(Native(To).c_str(), Attributes ? Attributes : FILE_ATTRIBUTE_NORMAL))
                    return Error{ErrorCategory::Unavailable, "archive-input-attributes"};
                return Source->Verify(Context).get();
            }
            Result<Unit> build(const QString& Output, OperationContext Context)
            {
                const auto InputRoot = QFileInfo(Output).dir().filePath("inputs");
                if (!QDir().mkdir(InputRoot)) return Error{ErrorCategory::Unavailable, "archive-input-directory"};
                QStringList Files, Directories{InputRoot};
                const auto Clean = [&]() -> std::vector<Error> {
                    std::vector<Error> Errors;
                    for (const auto& File : Files) if (QFile::exists(File)) {
                        const auto Attributes = GetFileAttributesW(Native(File).c_str());
                        if (Attributes != INVALID_FILE_ATTRIBUTES && Attributes & FILE_ATTRIBUTE_READONLY) SetFileAttributesW(Native(File).c_str(), Attributes & ~FILE_ATTRIBUTE_READONLY);
                        if (!QFile::remove(File)) Errors.push_back({ErrorCategory::Unavailable, "archive-input-cleanup", {}, {}, false, File.toStdString()}); }
                    std::sort(Directories.begin(), Directories.end(), [](const QString& A, const QString& B) { return A.size() > B.size(); });
                    Directories.removeDuplicates();
                    for (const auto& Directory : Directories) if (QDir(Directory).exists() && !QDir().rmdir(Directory)) Errors.push_back({ErrorCategory::Unavailable, "archive-input-directory-cleanup", {}, {}, false, Directory.toStdString()});
                    return Errors;
                };
                auto Cleanup = qScopeGuard([&] { Clean(); });
                const auto MakeParents = [&](const std::string& Path, bool Directory) -> Result<Unit> {
                    auto Parts = QString::fromStdString(Path).mid(1).split('/'); if (!Directory) Parts.removeLast();
                    auto At = InputRoot;
                    for (const auto& Part : Parts) { At += '/' + Part; if (!Directories.contains(At)) {
                        if (!QDir().mkdir(At)) return Error{ErrorCategory::Unavailable, "archive-input-parent"}; Directories.push_back(At); } }
                    return Unit{};
                };
                std::uint64_t Total = 0;
                // New members inherit archive protection. Replacements/renames
                // retain the destination member's protection via provenance.
                const bool Encrypt = std::ranges::any_of(before_, [](const auto& Pair) {
                    return Pair.second.entry_.hints_.contains("encrypted");
                });
                const bool Rebuild = Encrypt && std::ranges::any_of(before_, [](const auto& Pair) {
                    return Pair.second.entry_.hints_.contains("rebuildEncrypted");
                });
                std::set<std::string> EncryptedOrigins;
                for (const auto& [Path, Item] : before_) if (Item.entry_.hints_.contains("encrypted")) EncryptedOrigins.insert(Item.metadataOrigin_);
                if (Rebuild && draft_.empty()) return Error{ErrorCategory::Unsupported, "archive-encrypted-empty-rebuild-unsupported"};
                std::vector<std::string> Added, Removed, Origins;
                std::vector<bool> Reused;
                for (auto& [Path, Item] : draft_)
                {
                    if (Rebuild) {
                        if (Item.entry_.attributes_ & FILE_ATTRIBUTE_REPARSE_POINT) return Error{ErrorCategory::Unsupported, "archive-special-rebuild"};
                        Item.reuseData_ = false;
                        if (!Item.entry_.directory_ && !Item.content_) Item.content_ = std::make_shared<ProviderContentSource>(reader_, Item.entry_);
                        Item.changed_ = true;
                    }
                    if (!Item.changed_) continue;
                    if (!Item.entry_.directory_) {
                        const bool Protected = Rebuild || (Item.metadataOrigin_.empty() ? Encrypt : EncryptedOrigins.contains(Item.metadataOrigin_));
                        if (Protected) Item.entry_.hints_["encrypted"] = "1"; else Item.entry_.hints_.erase("encrypted");
                    }
                    if (!Item.reuseData_) { const auto Made = MakeParents(Path, Item.entry_.directory_); if (!Made.HasValue()) return Made; }
                    if (!Item.entry_.directory_ && !Item.reuseData_)
                    {
                        if (!Item.content_ || Item.entry_.size_ > limits_.maxInputBytes_ - Total) return Error{ErrorCategory::BudgetExceeded, "archive-input-budget"};
                        const auto Current = Item.content_->Describe();
                        if (Current.id_ != Item.entry_.id_ || Current.locator_ != Item.entry_.locator_ || Current.size_ != Item.entry_.size_ || Current.directory_)
                            return Error{ErrorCategory::StaleRevision, "archive-input-description-changed"};
                        Total += Item.entry_.size_;
                        ULARGE_INTEGER Free{};
                        if (!GetDiskFreeSpaceExW(Native(InputRoot).c_str(), &Free, nullptr, nullptr) || Free.QuadPart < Item.entry_.size_)
                            return Error{ErrorCategory::Unavailable, "archive-input-disk-space"};
                        const auto Name = InputRoot + QString::fromStdString(Path); Files.push_back(Name);
                        std::uint32_t Check = 0; const auto Copied = copySource(Item.content_, Name, Context, &Check); if (!Copied.HasValue()) return Copied;
                        Item.entry_.hints_["crc"] = std::to_string(Check);
                    }
                    auto Name = Path.substr(1); if (Item.entry_.directory_) Name += "/";
                    Added.push_back(std::move(Name));
                    Origins.push_back(Rebuild ? std::string{} : Item.metadataOrigin_); Reused.push_back(Item.reuseData_);
                }
                for (const auto& [Path, Item] : before_)
                    if (!draft_.contains(Path) || draft_.at(Path).changed_)
                    {
                        // Navigation synthesizes missing parent directories;
                        // they are not records accepted by DeleteFilesW.
                        if (Item.entry_.directory_ && Item.entry_.hints_.empty()) continue;
                        const auto Raw = Item.entry_.hints_.find("wcxName");
                        Removed.push_back(Raw == Item.entry_.hints_.end() ? Path.substr(1) : Raw->second);
                    }
                // TotalSQX's deletion callback cannot supply passwords for solid
                // encrypted data. Build a fresh candidate from bounded staging
                // instead; no deletion/recompression is attempted on the source.
                if (Rebuild) {
                    Removed.clear();
                    if (!QFile::remove(Output)) return Error{ErrorCategory::Unavailable, "archive-staging-placeholder-remove"};
                } else {
                    const auto Copied = copySource(std::make_shared<ProviderContentSource>(native_, original_), Output, Context); if (!Copied.HasValue()) return Copied;
                }
                if (std::ranges::all_of(Origins, [](const auto& Name) { return Name.empty(); })) Origins.clear();
                if (std::ranges::none_of(Reused, [](bool Flag) { return Flag; })) Reused.clear();
                auto Updated = backend_->Update({Output.toStdString(), InputRoot.toStdString(), std::move(Added), std::move(Removed), limits_.maxArchiveBytes_, Encrypt, {}, std::move(Origins), std::move(Reused)}, Context);
                if (!Updated.HasValue()) return Updated;
                auto Cursor = backend_->Enumerate(Output.toStdString(), true, Context); if (!Cursor.HasValue()) return Cursor.Failure();
                std::set<std::string> Seen;
                std::vector<std::pair<hc::plugins::ArchiveHeader, std::string>> VerifyBytes;
                for (;;)
                {
                    auto Page = Cursor.Value()->Next(Context.maxPageItems_, Context); if (!Page.HasValue()) return Page.Failure();
                    if (Page.Value().entries_.empty() && !Page.Value().end_) return Invalid("archive-cursor-no-progress");
                    for (const auto& Header : Page.Value().entries_)
                    {
                        auto Name = QString::fromStdString(Header.name_); Name.replace('\\', '/'); if (Name.endsWith('/')) Name.chop(1);
                        const auto Path = "/" + Name.toStdString();
                        if (!draft_.contains(Path) || !Seen.insert(Path).second) return Invalid("archive-output-entry-mismatch");
                        const auto& Expected = draft_.at(Path).entry_;
                        const bool KnownCrc = Expected.hints_.contains("crc")
                            && ((draft_.at(Path).changed_ && !draft_.at(Path).reuseData_) || Expected.hints_.at("crc") != "0");
                        if ((Expected.attributes_ & FILE_ATTRIBUTE_REPARSE_POINT) != (Header.attributes_ & FILE_ATTRIBUTE_REPARSE_POINT)) return Invalid("archive-output-special-kind-changed");
                        if (Expected.hints_.contains("encrypted") && !Header.encrypted_) return Invalid("archive-output-encryption-lost");
                        const auto Packed = Header.packedSize_ ? Header.packedSize_ : std::uint64_t(QFileInfo(Output).size());
                        if (Context.workload_.IsAutomatic() && !Expected.directory_ && Expected.size_ && (!Packed || (Expected.size_ - 1) / Packed >= Context.workload_.automaticExpansionRatio_))
                            return Error{ErrorCategory::BudgetExceeded, "archive-expansion-budget"};
                        if (Expected.directory_ != Header.directory_ || (!Expected.directory_ && (Expected.size_ != Header.size_
                            || (Header.crc_ && KnownCrc && Expected.hints_.at("crc") != std::to_string(Header.crc_)))))
                            return Invalid("archive-output-content-mismatch");
                        // ISO and other WCX formats may have no on-disc CRC.
                        // Zero cannot prove a payload match: validate actual
                        // extracted bytes against the staged input checksum.
                        if (!Expected.directory_ && !(Expected.attributes_ & FILE_ATTRIBUTE_REPARSE_POINT) && !Header.crc_ && KnownCrc)
                            VerifyBytes.emplace_back(Header, Expected.hints_.at("crc"));
                    }
                    if (Page.Value().end_) break;
                }
                // Release the listing before another operation on a serialized
                // WCX module. Keep extraction inside the private staging tree.
                Cursor.Value().reset();
                for (const auto& [Header, ExpectedCrc] : VerifyBytes)
                {
                    const auto VerifyPath = QFileInfo(Output).dir().filePath("verify-member");
                    Files.push_back(VerifyPath);
                    auto Extracted = backend_->Extract(Output.toStdString(), Header, VerifyPath.toStdString(), Context);
                    if (!Extracted.HasValue()) return Extracted.Failure();
                    QFile File(VerifyPath);
                    if (!File.open(QIODevice::ReadOnly) || uint64_t(File.size()) != Header.size_) return Invalid("archive-output-content-mismatch");
                    uint32_t Check = 0xffffffff;
                    while (!File.atEnd()) {
                        if (auto Error = Context.Check({"wcx"})) return *Error;
                        const auto Bytes = File.read(1024 * 1024);
                        if (Bytes.isEmpty()) return Invalid("archive-output-verify-read");
                        Check = Crc(Check, Bytes);
                    }
                    if (ExpectedCrc != std::to_string(Check ^ 0xffffffff)) return Invalid("archive-output-content-mismatch");
                    File.close();
                    const auto Attributes = GetFileAttributesW(Native(VerifyPath).c_str());
                    if (Attributes != INVALID_FILE_ATTRIBUTES && (Attributes & FILE_ATTRIBUTE_READONLY))
                        SetFileAttributesW(Native(VerifyPath).c_str(), Attributes & ~FILE_ATTRIBUTE_READONLY);
                    if (!QFile::remove(VerifyPath)) return Invalid("archive-output-verify-cleanup");
                }
                for (const auto& [Path, Item] : draft_)
                    if (!Seen.contains(Path))
                    {
                        const auto Prefix = Path + "/";
                        const auto Child = Seen.lower_bound(Prefix);
                        const bool ImplicitDirectory = Item.entry_.directory_ && Child != Seen.end() && Child->starts_with(Prefix);
                        if (!ImplicitDirectory) return Invalid("archive-output-missing-entry");
                    }
                const auto Errors = Clean(); Cleanup.dismiss();
                if (!Errors.empty()) return Errors.front();
                return Unit{};
            }
            Result<std::shared_ptr<IMaterializedFileLease>> prepareCandidate(std::vector<ArchiveChange> Changes, OperationContext Context)
            {
                if (const auto Failure = Context.Check({"wcx"})) return *Failure;
                if (!Context.maxReadBytes_ || !Context.maxPageItems_) return Invalid("archive-zero-io-budget");
                Context.maxEnumerationEntries_ = std::min(Context.maxEnumerationEntries_, limits_.maxEntries_);
                auto Initial = snapshot(Context); if (!Initial.HasValue()) return Initial.Failure();
                auto Planned = plan(Changes, Context); if (!Planned.HasValue()) return Planned.Failure();
                auto Store = MakeNativeContentStore(temporary_); if (!Store.HasValue()) return Store.Failure();
                auto Output = original_; Output.size_ = limits_.maxArchiveBytes_;
                // build() independently bounds and integrity-tests expansion.
                return Store.Value()->Produce(Output, {}, Output.size_, Context, [&](const std::string& Path, OperationContext Context) {
                    return build(QString::fromStdString(Path), Context);
                }, ProducedSize::Growing);
            }
            Result<Unit> prepare(std::vector<ArchiveChange> Changes, OperationContext Context)
            {
                auto Lease = prepareCandidate(std::move(Changes), Context);
                if (!Lease.HasValue()) return Lease.Failure();
                if (root_.nesting_.size() > 1)
                {
                    // Prepare outward, but publish only at the native root. No
                    // intermediate archive is committed to its original parent.
                    auto Container = ResolveWcxContainer(native_, Context); if (!Container.HasValue()) return Container.Failure();
                    auto ParentRoot = original_.locator_; ParentRoot.nesting_.back().entry_ = "/";
                    auto Session = BeginWcxMutation({}, ParentRoot, native_, temporary_, limits_, Context, writer_, Container.Value().backend_);
                    if (!Session.HasValue()) return Session.Failure(); parent_ = std::move(Session.Value());
                    auto LocalReader = MakeNativeReadProvider(LocatorKind::Local);
                    auto Local = LocalReader->Resolve({LocatorKind::Local, Lease.Value()->LocalPath()}, Context).get(); if (!Local.HasValue()) return Local.Failure();
                    ArchiveChange Change; Change.kind_ = ArchiveChangeKind::Put; Change.path_ = original_.locator_.nesting_.back().entry_;
                    Change.content_ = std::make_shared<ProviderContentSource>(LocalReader, Local.Value()); Change.destination_ = {true, original_.id_.revision_};
                    auto Prepared = parent_->Prepare({std::move(Change)}, Context); if (!Prepared.HasValue()) return Prepared;
                    const auto Fresh = native_->Stat(original_.id_, Context).get(); return Fresh.HasValue() ? Result<Unit>(Unit{}) : Result<Unit>(Fresh.Failure());
                }
                auto WriteContext = Context; WriteContext.expectedWriteBytes_ = uint64_t(QFileInfo(QString::fromStdString(Lease.Value()->LocalPath())).size());
                auto Written = writer_->BeginWrite(original_.locator_, {true, original_.id_.revision_}, WriteContext); if (!Written.HasValue()) return Written.Failure();
                output_ = std::move(Written.Value());
                QFile File(QString::fromStdString(Lease.Value()->LocalPath())); if (!File.open(QIODevice::ReadOnly)) return Error{ErrorCategory::Unavailable, "archive-prepared-open"};
                while (!File.atEnd())
                {
                    if (const auto Failure = Context.Check({"wcx"})) return *Failure;
                    const auto Bytes = File.read(std::min(Context.maxReadBytes_, std::size_t{1024 * 1024}));
                    if (Bytes.isEmpty()) return Error{ErrorCategory::Unavailable, "archive-prepared-read"};
                    const auto Appended = output_->Append({reinterpret_cast<const std::byte*>(Bytes.data()), static_cast<std::size_t>(Bytes.size())}, Context);
                    if (!Appended.HasValue()) return Appended;
                }
                File.close(); Lease.Value().reset();
                const auto Fresh = native_->Stat(original_.id_, Context).get(); if (!Fresh.HasValue()) return Fresh.Failure();
                return output_->SetMetadata({{}, original_.attributes_}, Context);
            }
            std::string plugin_;
            Locator root_;
            std::shared_ptr<IStorageProvider> reader_;
            std::string temporary_;
            ArchiveMutationLimits limits_;
            EntrySnapshot original_;
            std::shared_ptr<IStorageProvider> native_;
            std::shared_ptr<IWritableStorage> writer_;
            std::shared_ptr<hc::plugins::IArchiveBackend> backend_;
            std::unique_ptr<IWriteSession> output_;
            std::unique_ptr<IArchiveMutationSession> parent_;
            std::map<std::string, DraftEntry> draft_, before_;
            State state_ = State::New;
        };
    }
    Result<std::unique_ptr<IArchiveMutationSession>> BeginWcxMutation(std::string PluginPath, Locator Root,
        std::shared_ptr<IStorageProvider> Reader, std::string TemporaryBase, ArchiveMutationLimits Limits,
        OperationContext Context, std::shared_ptr<IWritableStorage> DestinationWriter, std::shared_ptr<hc::plugins::IArchiveBackend> Backend)
    {
        // Native ISO reads can feed the existing top-level WCX rewrite path.
        // Nested containers still require the WCX container resolver until P5.
        if (!Reader || (Reader->Id().value_ != "wcx" && !(Reader->Id().value_ == "iso" && Root.nesting_.size() == 1))
            || Root.kind_ != LocatorKind::Archive || Root.nesting_.empty() || Root.nesting_.size() > 8
            || !std::ranges::all_of(Root.nesting_, [](const ContainerHop& Hop) { return Hop.provider_ == "wcx"; })
            || Root.nesting_.back().entry_ != "/" || !Limits.maxEntries_
            || !Limits.maxDepth_ || Limits.maxDepth_ > 32 || !Limits.maxInputBytes_
            || !Limits.maxArchiveBytes_)
            return Invalid("invalid-archive-mutation-request");
        const auto Kind = Root.root_.starts_with("\\\\") || Root.root_.starts_with("//") ? LocatorKind::Smb : LocatorKind::Local;
        auto NativeReader = MakeNativeReadProvider(Kind);
        Result<EntrySnapshot> Original = Error{ErrorCategory::Internal, "archive-container-required"};
        if (Root.nesting_.size() > 1) {
            auto Container = ResolveWcxContainer(Reader, Context); if (!Container.HasValue()) return Container.Failure();
            if (!SameArchiveContainer(Root, Container.Value().root_)) return Invalid("archive-container-mismatch");
            NativeReader = Container.Value().reader_; Original = Container.Value().entry_;
            Backend = Container.Value().backend_;
        }
        else Original = NativeReader->Resolve({Kind, Root.root_}, Context).get();
        if (!Original.HasValue()) return Original.Failure();
        if (Original.Value().directory_ || Original.Value().attributes_ & FILE_ATTRIBUTE_REPARSE_POINT || !Original.Value().id_.revision_)
            return Invalid("archive-native-file-required");
        if (Original.Value().size_ > Limits.maxArchiveBytes_) return Error{ErrorCategory::BudgetExceeded, "archive-original-size-budget"};
        if (!DestinationWriter && Root.nesting_.size() == 1) DestinationWriter = MakeNativeWritableStorage(Kind, NativeReader);
        if (TemporaryBase.empty()) TemporaryBase = QDir::tempPath().toStdString();
        return std::unique_ptr<IArchiveMutationSession>(new Session(std::move(PluginPath), std::move(Root), std::move(Reader),
            std::move(TemporaryBase), Limits, std::move(Original.Value()), std::move(NativeReader), std::move(DestinationWriter), std::move(Backend)));
    }
}
