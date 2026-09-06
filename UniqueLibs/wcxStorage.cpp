#include "wcxStorage.hpp"
#include "storageProviders.hpp"
#include "nativeMaterializer.hpp"
#include <QtCore>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include "externalLibs/TotalCommander/wcxhead.h"
#include <set>

std::recursive_timed_mutex& WcxCallMutex() { static std::recursive_timed_mutex Mutex; return Mutex; }
namespace hc::storage
{
    namespace
    {
        Error WcxError(int Code)
        { return {Code == E_EABORTED ? ErrorCategory::Cancelled : Code == E_NOT_SUPPORTED ? ErrorCategory::Unsupported : ErrorCategory::Unavailable,
            "wcx:" + std::to_string(Code), {"wcx"}}; }
        Result<std::string> EntryPath(const wchar_t* Name)
        {
            const auto Size = wcsnlen_s(Name, 1024);
            if (!Size || Size == 1024) return Error{ErrorCategory::InvalidRequest, "invalid-archive-name", {"wcx"}};
            auto Path = QString::fromWCharArray(Name, static_cast<int>(Size)); Path.replace('\\', '/');
            if (Path.startsWith('/') || Path.contains(':') || Path.contains(QChar::Null)) return Error{ErrorCategory::InvalidRequest, "archive-traversal", {"wcx"}};
            if (Path.endsWith('/')) Path.chop(1);
            const auto Parts = Path.split('/');
            if (Parts.size() > 32) return Error{ErrorCategory::BudgetExceeded, "archive-depth-budget", {"wcx"}};
            for (const auto& Part : Parts)
                if (Part.isEmpty() || Part == "." || Part == "..") return Error{ErrorCategory::InvalidRequest, "archive-traversal", {"wcx"}};
            return ("/" + Path).toStdString();
        }
        std::string ParentOf(const std::string& Path) { const auto Slash = Path.find_last_of('/'); return Slash == 0 ? "/" : Path.substr(0, Slash); }
        struct CallbackState
        {
            OperationContext context_;
            QString output_;
            std::uint64_t maxBytes_ = 0;
            bool volume_ = false, exceeded_ = false;
        };
        thread_local CallbackState* Current = nullptr;
        int __stdcall Progress(wchar_t*, int)
        {
            if (!Current) return 0;
            if (Current->context_.Check({"wcx"})) return 0;
            if (!Current->output_.isEmpty() && QFileInfo(Current->output_).size() > static_cast<qint64>(Current->maxBytes_))
            { Current->exceeded_ = true; return 0; }
            return 1;
        }
        int __stdcall Volume(wchar_t*, int) { if (Current) Current->volume_ = true; return 0; }
        struct Module
        {
            HMODULE module_ = nullptr;
            HANDLE (__stdcall* open_)(tOpenArchiveDataW*) = nullptr;
            int (__stdcall* header_)(HANDLE, tHeaderDataExW*) = nullptr;
            int (__stdcall* process_)(HANDLE, int, wchar_t*, wchar_t*) = nullptr;
            int (__stdcall* close_)(HANDLE) = nullptr;
            void (__stdcall* progress_)(HANDLE, tProcessDataProcW) = nullptr;
            void (__stdcall* volume_)(HANDLE, tChangeVolProcW) = nullptr;
            ~Module() { if (module_) { std::lock_guard Lock(WcxCallMutex()); FreeLibrary(module_); } }
            Result<Unit> Load(const std::string& Path)
            {
                if (module_) return Unit{};
                const auto Name = QString::fromStdString(Path);
                if (!QFileInfo(Name).isAbsolute() || Name.contains(QChar::Null)) return Error{ErrorCategory::InvalidRequest, "absolute-plugin-path-required"};
                module_ = LoadLibraryExW(QDir::toNativeSeparators(Name).toStdWString().c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
                if (!module_) return Error{ErrorCategory::Unavailable, "wcx-load:" + std::to_string(GetLastError()), {"wcx"}};
#define HC_WCX_BIND(Member, Export) Member = reinterpret_cast<decltype(Member)>(GetProcAddress(module_, Export))
                HC_WCX_BIND(open_, "OpenArchiveW"); HC_WCX_BIND(header_, "ReadHeaderExW"); HC_WCX_BIND(process_, "ProcessFileW");
                HC_WCX_BIND(close_, "CloseArchive"); HC_WCX_BIND(progress_, "SetProcessDataProcW"); HC_WCX_BIND(volume_, "SetChangeVolProcW");
#undef HC_WCX_BIND
                const auto Background = reinterpret_cast<int (__stdcall*)()>(GetProcAddress(module_, "GetBackgroundFlags"));
                if (!open_ || !header_ || !process_ || !close_ || !progress_ || !volume_ || !Background || !(Background() & BACKGROUND_UNPACK))
                { FreeLibrary(module_); module_ = nullptr; return Error{ErrorCategory::Unsupported, "wcx-background-unicode-read-required", {"wcx"}}; }
                return Unit{};
            }
        };
        class Cursor final : public IEntryCursor
        {
        public:
            explicit Cursor(std::vector<EntrySnapshot> Entries) : entries_(std::move(Entries)) {}
            Task<EntryPage> Next(std::size_t Count, OperationContext Context) override
            {
                if (const auto Failure = Context.Check({"wcx"})) return Ready<EntryPage>(*Failure);
                Count = std::min({Count, Context.maxPageItems_, entries_.size() - offset_});
                if (!Count && offset_ != entries_.size()) return Ready<EntryPage>(Error{ErrorCategory::InvalidRequest, "zero-page-size"});
                EntryPage Page; Page.entries_.assign(entries_.begin() + offset_, entries_.begin() + offset_ + Count);
                offset_ += Count; Page.end_ = offset_ == entries_.size(); return Ready<EntryPage>(std::move(Page));
            }
        private:
            std::vector<EntrySnapshot> entries_;
            std::size_t offset_ = 0;
        };
        class LeasedStream final : public IReadStream
        {
        public:
            LeasedStream(std::shared_ptr<IReadStream> Stream, std::shared_ptr<IMaterializedFileLease> Lease)
                : lease_(std::move(Lease)), stream_(std::move(Stream)) {}
            bool CanSeek() const override { return stream_->CanSeek(); }
            Task<ReadChunk> Read(std::size_t Count, OperationContext Context) override { return stream_->Read(Count, std::move(Context)); }
            Task<Unit> Seek(std::uint64_t At, OperationContext Context) override { return stream_->Seek(At, std::move(Context)); }
        private:
            // Stream closes before the last lease can schedule deletion.
            std::shared_ptr<IMaterializedFileLease> lease_;
            std::shared_ptr<IReadStream> stream_;
        };
        class WcxProvider final : public IStorageProvider, public std::enable_shared_from_this<WcxProvider>
        {
        public:
            WcxProvider(std::string Plugin, Locator Root, std::string Temporary)
                : plugin_(std::move(Plugin)), root_(std::move(Root)), temporary_(std::move(Temporary)),
                session_(QUuid::createUuid().toString(QUuid::Id128).toStdString() + ":"),
                native_(MakeNativeReadProvider(root_.root_.starts_with("\\\\") || root_.root_.starts_with("//") ? LocatorKind::Smb : LocatorKind::Local)) {}
            ProviderId Id() const override { return {"wcx"}; }
            Task<SupportDecision> Supports(Operation Op, OperationContext Context) override
            {
                if (const auto Failure = Context.Check(Id())) return Ready<SupportDecision>(*Failure);
                return Ready<SupportDecision>(SupportDecision{Op == Operation::Write ? Availability::Unsupported : Availability::Conditional,
                    Op == Operation::Write ? "archive-write-unavailable" : "wcx-background-unicode-read-required", Op == Operation::OpenRead});
            }
            Task<std::unique_ptr<IEntryCursor>> Enumerate(Locator At, OperationContext Context) override
            {
                return run<std::unique_ptr<IEntryCursor>>(Context, [At = std::move(At)](WcxProvider& Self, OperationContext Context) -> Result<std::unique_ptr<IEntryCursor>> {
                    auto Valid = Self.mount(Context); if (!Valid.HasValue()) return Valid.Failure();
                    if (!Self.validLocation(At)) return Error{ErrorCategory::InvalidRequest, "archive-location"};
                    const auto& Path = At.nesting_.back().entry_;
                    if (Path != "/" && (!Self.entries_.contains(Path) || !Self.entries_.at(Path).directory_)) return Error{ErrorCategory::NotFound, "archive-directory-not-found"};
                    std::vector<EntrySnapshot> Items;
                    for (const auto& [Key, Entry] : Self.entries_) if (ParentOf(Key) == Path) Items.push_back(Entry);
                    return std::unique_ptr<IEntryCursor>(new Cursor(std::move(Items)));
                });
            }
            Task<EntrySnapshot> Stat(ResourceId Id, OperationContext Context) override
            {
                return run<EntrySnapshot>(Context, [Id = std::move(Id)](WcxProvider& Self, OperationContext Context) { return Self.stat(Id, Context); });
            }
            Task<EntrySnapshot> Resolve(Locator At, OperationContext Context) override
            {
                return run<EntrySnapshot>(Context, [At = std::move(At)](WcxProvider& Self, OperationContext Context) -> Result<EntrySnapshot> {
                    auto Valid = Self.mount(Context); if (!Valid.HasValue()) return Valid.Failure();
                    if (!Self.validLocation(At) || !Self.entries_.contains(At.nesting_.back().entry_)) return Error{ErrorCategory::NotFound, "archive-entry-not-found"};
                    return Self.entries_.at(At.nesting_.back().entry_);
                });
            }
            Task<std::shared_ptr<IReadStream>> OpenRead(ResourceId Id, OperationContext Context) override
            {
                return run<std::shared_ptr<IReadStream>>(Context, [Id = std::move(Id)](WcxProvider& Self, OperationContext Context) -> Result<std::shared_ptr<IReadStream>> {
                    auto Entry = Self.stat(Id, Context); if (!Entry.HasValue()) return Entry.Failure();
                    if (Entry.Value().directory_) return Error{ErrorCategory::Unsupported, "file-required"};
                    if (!Self.store_)
                    {
                        auto Store = MakeNativeContentStore(Self.temporary_.empty() ? QDir::tempPath().toStdString() : Self.temporary_);
                        if (!Store.HasValue()) return Store.Failure(); Self.store_ = std::move(Store.Value());
                    }
                    const auto Packed = std::stoull(Entry.Value().hints_.at("packedSize"));
                    auto Lease = Self.store_->Produce(Entry.Value(), {}, Packed, Context, [&](const std::string& Output, OperationContext Context) {
                        return Self.extract(Entry.Value(), Output, Context);
                    });
                    if (!Lease.HasValue()) return Lease.Failure();
                    auto Reader = MakeNativeReadProvider(LocatorKind::Local);
                    auto Local = Reader->Resolve({LocatorKind::Local, Lease.Value()->LocalPath()}, Context).get(); if (!Local.HasValue()) return Local.Failure();
                    auto Stream = Reader->OpenRead(Local.Value().id_, Context).get(); if (!Stream.HasValue()) return Stream.Failure();
                    return std::shared_ptr<IReadStream>(new LeasedStream(std::move(Stream.Value()), std::move(Lease.Value())));
                });
            }
        private:
            template<class T, class F> Task<T> run(OperationContext Context, F Work)
            {
                return std::async(std::launch::async, [Self = shared_from_this(), Context, Work = std::move(Work)]() mutable -> Result<T> {
                    try
                    {
                        std::unique_lock Lock(WcxCallMutex(), std::defer_lock);
                        while (!Lock.try_lock_for(std::chrono::milliseconds(25))) if (const auto Failure = Context.Check(Self->Id())) return *Failure;
                        if (const auto Failure = Context.Check(Self->Id())) return *Failure;
                        return Work(*Self, Context);
                    }
                    catch (...) { return Error{ErrorCategory::Internal, "wcx-provider-exception", {"wcx"}}; }
                });
            }
            bool validLocation(const Locator& At) const
            { return At.kind_ == LocatorKind::Archive && At.root_ == root_.root_ && At.nesting_.size() == 1 && At.nesting_.front().provider_ == "wcx"; }
            EntrySnapshot makeEntry(const std::string& Path, bool Directory) const
            {
                EntrySnapshot Entry; Entry.locator_ = root_; Entry.locator_.nesting_.back().entry_ = Path;
                Entry.name_ = Path.substr(Path.find_last_of('/') + 1); Entry.directory_ = Directory;
                Entry.attributes_ = Directory ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
                Entry.id_ = {Id(), session_ + Path, IdentityScope::NavigationSession, ResourceRevision{archive_.id_.revision_->token_ + ":" + Path}};
                return Entry;
            }
            Result<Unit> mount(OperationContext Context)
            {
                if (mounted_) { const auto Fresh = native_->Stat(archive_.id_, Context).get(); return Fresh.HasValue() ? Result<Unit>(Unit{}) : Result<Unit>(Fresh.Failure()); }
                if (!validLocation(root_)) return Error{ErrorCategory::InvalidRequest, "archive-location"};
                const auto Loaded = module_.Load(plugin_); if (!Loaded.HasValue()) return Loaded.Failure();
                auto Archive = native_->Resolve({native_->Id().value_ == "smb" ? LocatorKind::Smb : LocatorKind::Local, root_.root_}, Context).get();
                if (!Archive.HasValue()) return Archive.Failure();
                if (Archive.Value().directory_ || Archive.Value().attributes_ & FILE_ATTRIBUTE_REPARSE_POINT || !Archive.Value().id_.revision_)
                    return Error{ErrorCategory::Unsupported, "archive-native-file-required"};
                archive_ = std::move(Archive.Value());
                Context.denyConcurrentMutation_ = true;
                auto Pin = native_->OpenRead(archive_.id_, Context).get(); if (!Pin.HasValue()) return Pin.Failure();
                entries_.clear();
                const auto Scanned = scan(PK_OM_LIST, Context, [&](HANDLE Arc, const tHeaderDataExW& Header, const std::string& Path) -> Result<bool> {
                    if (entries_.size() >= Context.maxEnumerationEntries_) return Error{ErrorCategory::BudgetExceeded, "archive-entry-budget"};
                    const bool Directory = Header.FileAttr & FILE_ATTRIBUTE_DIRECTORY;
                    if (entries_.contains(Path) && !(Directory && entries_.at(Path).directory_ && entries_.at(Path).hints_.empty()))
                        return Error{ErrorCategory::InvalidRequest, "duplicate-archive-entry"};
                    auto Entry = makeEntry(Path, Directory);
                    Entry.attributes_ = Header.FileAttr; Entry.size_ = (static_cast<std::uint64_t>(Header.UnpSizeHigh) << 32) | Header.UnpSize;
                    const QDate Date(1980 + ((Header.FileTime >> 25) & 127), (Header.FileTime >> 21) & 15, (Header.FileTime >> 16) & 31);
                    const QTime Time((Header.FileTime >> 11) & 31, (Header.FileTime >> 5) & 63, (Header.FileTime & 31) * 2);
                    if (Date.isValid() && Time.isValid()) Entry.modifiedUtcMs_ = QDateTime(Date, Time).toMSecsSinceEpoch();
                    if (Entry.size_ > 64ULL * 1024 * 1024 * 1024) return Error{ErrorCategory::BudgetExceeded, "archive-size-budget"};
                    Entry.hints_["packedSize"] = std::to_string((static_cast<std::uint64_t>(Header.PackSizeHigh) << 32) | Header.PackSize);
                    Entry.hints_["crc"] = std::to_string(static_cast<std::uint32_t>(Header.FileCRC));
                    Entry.id_.revision_->token_ += ":" + Entry.hints_["crc"] + ":" + std::to_string(Entry.size_);
                    entries_[Path] = std::move(Entry);
                    for (auto Parent = ParentOf(Path); Parent != "/"; Parent = ParentOf(Parent))
                    {
                        if (entries_.contains(Parent) && !entries_.at(Parent).directory_) return Error{ErrorCategory::InvalidRequest, "archive-file-parent"};
                        if (!entries_.contains(Parent)) entries_[Parent] = makeEntry(Parent, true);
                        if (entries_.size() > Context.maxEnumerationEntries_) return Error{ErrorCategory::BudgetExceeded, "archive-entry-budget"};
                    }
                    const auto Skipped = module_.process_(Arc, PK_SKIP, nullptr, nullptr); if (Skipped) return WcxError(Skipped);
                    return false;
                });
                if (!Scanned.HasValue()) { entries_.clear(); return Scanned.Failure(); }
                mounted_ = true; return Unit{};
            }
            Result<EntrySnapshot> stat(const ResourceId& Resource, OperationContext Context)
            {
                if (Resource.providerId_ != Id() || !Resource.key_.starts_with(session_)) return Error{ErrorCategory::NotFound, "foreign-session"};
                const auto Valid = mount(Context); if (!Valid.HasValue()) return Valid.Failure();
                const auto It = entries_.find(Resource.key_.substr(session_.size()));
                if (It == entries_.end()) return Error{ErrorCategory::NotFound, "archive-entry-not-found"};
                if (It->second.id_ != Resource) return Error{ErrorCategory::StaleRevision, "revision-changed"};
                return It->second;
            }
            template<class Visitor> Result<Unit> scan(int Mode, OperationContext Context, Visitor Visit, QString Output = {}, std::uint64_t MaxBytes = 0)
            {
                CallbackState Call{Context, std::move(Output), MaxBytes};
                const auto Previous = Current; Current = &Call; const auto Restore = qScopeGuard([&] { Current = Previous; });
                auto Path = QDir::toNativeSeparators(QString::fromStdString(root_.root_)).toStdWString();
                tOpenArchiveDataW Open{}; Open.ArcName = Path.data(); Open.OpenMode = Mode;
                const auto Arc = module_.open_(&Open); if (!Arc) return WcxError(Open.OpenResult ? Open.OpenResult : E_EOPEN);
                auto Close = qScopeGuard([&] { module_.close_(Arc); });
                module_.progress_(Arc, &Progress); module_.volume_(Arc, &Volume);
                for (std::size_t Count = 0;; ++Count)
                {
                    if (const auto Failure = Context.Check(Id())) return *Failure;
                    tHeaderDataExW Header{}; const auto Code = module_.header_(Arc, &Header);
                    if (Call.volume_) return Error{ErrorCategory::Unsupported, "archive-volume-required"};
                    if (Code == E_END_ARCHIVE) break;
                    if (Code) return WcxError(Code);
                    if (Count >= Context.maxEnumerationEntries_) return Error{ErrorCategory::BudgetExceeded, "archive-entry-budget"};
                    auto Name = EntryPath(Header.FileName); if (!Name.HasValue()) return Name.Failure();
                    auto Done = Visit(Arc, Header, Name.Value());
                    if (Call.volume_) return Error{ErrorCategory::Unsupported, "archive-volume-required"};
                    if (Call.exceeded_) return Error{ErrorCategory::BudgetExceeded, "archive-extraction-budget"};
                    if (const auto Failure = Context.Check(Id())) return *Failure;
                    if (!Done.HasValue()) return Done.Failure();
                    if (Done.Value()) break;
                }
                const auto Closed = module_.close_(Arc); Close.dismiss();
                if (Closed) return WcxError(Closed);
                return Unit{};
            }
            Result<Unit> extract(const EntrySnapshot& Entry, const std::string& Output, OperationContext Context)
            {
                Context.denyConcurrentMutation_ = true;
                auto Pin = native_->OpenRead(archive_.id_, Context).get(); if (!Pin.HasValue()) return Pin.Failure();
                bool Found = false;
                auto Destination = QDir::toNativeSeparators(QString::fromStdString(Output)).toStdWString();
                auto Result = scan(PK_OM_EXTRACT, Context, [&](HANDLE Arc, const tHeaderDataExW& Header, const std::string& Path) -> hc::storage::Result<bool> {
                    if (Path != Entry.locator_.nesting_.back().entry_)
                    { const auto Code = module_.process_(Arc, PK_SKIP, nullptr, nullptr); if (Code) return WcxError(Code); return false; }
                    if (((static_cast<std::uint64_t>(Header.UnpSizeHigh) << 32) | Header.UnpSize) != Entry.size_
                        || std::to_string(static_cast<std::uint32_t>(Header.FileCRC)) != Entry.hints_.at("crc")) return Error{ErrorCategory::StaleRevision, "archive-entry-changed"};
                    const auto Code = module_.process_(Arc, PK_EXTRACT, nullptr, Destination.data()); if (Code) return WcxError(Code);
                    Found = true; return true;
                }, QString::fromStdString(Output), Entry.size_);
                if (!Result.HasValue()) return Result;
                if (!Found) return Error{ErrorCategory::NotFound, "archive-entry-not-found"};
                return Unit{};
            }
            std::string plugin_;
            Locator root_;
            std::string temporary_, session_;
            std::shared_ptr<IStorageProvider> native_;
            std::shared_ptr<INativeContentStore> store_;
            Module module_;
            EntrySnapshot archive_;
            std::map<std::string, EntrySnapshot> entries_;
            bool mounted_ = false;
        };
    }
    std::shared_ptr<IStorageProvider> MakeWcxReadProvider(std::string PluginPath, Locator Root, std::string TemporaryBase)
    { return std::make_shared<WcxProvider>(std::move(PluginPath), std::move(Root), std::move(TemporaryBase)); }
}
