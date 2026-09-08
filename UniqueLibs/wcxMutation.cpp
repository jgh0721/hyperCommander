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
                if (state_ != State::Prepared || !output_) return Invalid("archive-session-not-prepared");
                if (const auto Failure = Context.Check({"wcx"})) return *Failure;
                auto Result = output_->Commit(Context);
                if (Result.HasValue()) { state_ = State::Committed; output_.reset(); }
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
                                || Entry.locator_.nesting_.size() != 1 || Entry.locator_.nesting_[0].provider_ != "wcx"
                                || Entry.id_.providerId_ != reader_->Id()) return Invalid("archive-provider-entry-mismatch");
                            const auto Path = SafePath(Entry.locator_.nesting_[0].entry_, limits_.maxDepth_); if (!Path.HasValue()) return Path.Failure();
                            if (Parent(Path.Value()) != At.nesting_[0].entry_ || Entry.attributes_ & FILE_ATTRIBUTE_REPARSE_POINT)
                                return Invalid("archive-provider-entry-parent");
                            if (!draft_.emplace(Path.Value(), DraftEntry{Entry}).second) return Invalid("duplicate-archive-entry");
                            if (draft_.size() > limits_.maxEntries_) return Error{ErrorCategory::BudgetExceeded, "archive-entry-budget"};
                            if (Entry.directory_) Pending.push_back(Entry.locator_);
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
                                if (!Item.entry_.directory_ && !Item.content_) Item.content_ = std::make_shared<ProviderContentSource>(reader_, Item.entry_);
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
                        if (!Item.entry_.directory_)
                        {
                            if (!Change.content_) return Invalid("archive-content-required");
                            Item.content_ = Change.content_; Item.entry_ = Item.content_->Describe();
                            if (Item.entry_.directory_ || Item.entry_.attributes_ & FILE_ATTRIBUTE_REPARSE_POINT) return Invalid("archive-file-content-required");
                        }
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
                            const auto Packed = std::stoull(Item.entry_.hints_.at("packedSize"));
                            if (Item.entry_.size_ && (!Packed || (Item.entry_.size_ - 1) / Packed >= 1000)) return Error{ErrorCategory::BudgetExceeded, "archive-expansion-budget"};
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
                std::vector<std::string> Added, Removed;
                for (auto& [Path, Item] : draft_)
                {
                    if (!Item.changed_) continue;
                    const auto Made = MakeParents(Path, Item.entry_.directory_); if (!Made.HasValue()) return Made;
                    if (!Item.entry_.directory_)
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
                const auto Copied = copySource(std::make_shared<ProviderContentSource>(native_, original_), Output, Context); if (!Copied.HasValue()) return Copied;
                auto Updated = backend_->Update({Output.toStdString(), InputRoot.toStdString(), std::move(Added), std::move(Removed), limits_.maxArchiveBytes_}, Context);
                if (!Updated.HasValue()) return Updated;
                auto Cursor = backend_->Enumerate(Output.toStdString(), true, Context); if (!Cursor.HasValue()) return Cursor.Failure();
                std::set<std::string> Seen;
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
                        const auto Packed = Header.packedSize_;
                        if (!Expected.directory_ && Expected.size_ && (!Packed || (Expected.size_ - 1) / Packed >= 1000))
                            return Error{ErrorCategory::BudgetExceeded, "archive-expansion-budget"};
                        if (Expected.directory_ != Header.directory_ || (!Expected.directory_ && (Expected.size_ != Header.size_
                            || (Expected.hints_.contains("crc") && Expected.hints_.at("crc") != std::to_string(Header.crc_)))))
                            return Invalid("archive-output-content-mismatch");
                    }
                    if (Page.Value().end_) break;
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
            Result<Unit> prepare(std::vector<ArchiveChange> Changes, OperationContext Context)
            {
                if (const auto Failure = Context.Check({"wcx"})) return *Failure;
                if (!Context.maxReadBytes_ || !Context.maxPageItems_) return Invalid("archive-zero-io-budget");
                Context.maxEnumerationEntries_ = std::min(Context.maxEnumerationEntries_, limits_.maxEntries_);
                auto Initial = snapshot(Context); if (!Initial.HasValue()) return Initial;
                auto Planned = plan(Changes, Context); if (!Planned.HasValue()) return Planned;
                auto Store = MakeNativeContentStore(temporary_); if (!Store.HasValue()) return Store.Failure();
                auto Output = original_; Output.size_ = limits_.maxArchiveBytes_;
                auto Lease = Store.Value()->Produce(Output, {}, {}, Context, [&](const std::string& Path, OperationContext Context) {
                    return build(QString::fromStdString(Path), Context);
                }, ProducedSize::Bounded);
                if (!Lease.HasValue()) return Lease.Failure();
                auto Written = writer_->BeginWrite(original_.locator_, {true, original_.id_.revision_}, Context); if (!Written.HasValue()) return Written.Failure();
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
            std::map<std::string, DraftEntry> draft_, before_;
            State state_ = State::New;
        };
    }
    Result<std::unique_ptr<IArchiveMutationSession>> BeginWcxMutation(std::string PluginPath, Locator Root,
        std::shared_ptr<IStorageProvider> Reader, std::string TemporaryBase, ArchiveMutationLimits Limits,
        OperationContext Context, std::shared_ptr<IWritableStorage> DestinationWriter, std::shared_ptr<hc::plugins::IArchiveBackend> Backend)
    {
        if (!Reader || Reader->Id().value_ != "wcx" || Root.kind_ != LocatorKind::Archive || Root.nesting_.size() != 1
            || Root.nesting_[0].provider_ != "wcx" || Root.nesting_[0].entry_ != "/" || !Limits.maxEntries_ || Limits.maxEntries_ > 100000
            || !Limits.maxDepth_ || Limits.maxDepth_ > 32 || !Limits.maxInputBytes_ || Limits.maxInputBytes_ > 1024ULL * 1024 * 1024
            || !Limits.maxArchiveBytes_ || Limits.maxArchiveBytes_ > 256ULL * 1024 * 1024)
            return Invalid("invalid-archive-mutation-request");
        const auto Kind = Root.root_.starts_with("\\\\") || Root.root_.starts_with("//") ? LocatorKind::Smb : LocatorKind::Local;
        auto NativeReader = MakeNativeReadProvider(Kind);
        auto Original = NativeReader->Resolve({Kind, Root.root_}, Context).get(); if (!Original.HasValue()) return Original.Failure();
        if (Original.Value().directory_ || Original.Value().attributes_ & FILE_ATTRIBUTE_REPARSE_POINT || !Original.Value().id_.revision_)
            return Invalid("archive-native-file-required");
        if (Original.Value().size_ > Limits.maxArchiveBytes_) return Error{ErrorCategory::BudgetExceeded, "archive-original-size-budget"};
        if (!DestinationWriter) DestinationWriter = MakeNativeWritableStorage(Kind, NativeReader);
        if (TemporaryBase.empty()) TemporaryBase = QDir::tempPath().toStdString();
        return std::unique_ptr<IArchiveMutationSession>(new Session(std::move(PluginPath), std::move(Root), std::move(Reader),
            std::move(TemporaryBase), Limits, std::move(Original.Value()), std::move(NativeReader), std::move(DestinationWriter), std::move(Backend)));
    }
}
