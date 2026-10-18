#include "stdafx.h"
#include "storageProviders.hpp"
#include "nativeStorageRevision.hpp"
#include <lm.h>
#include <shlobj.h>
#include <QUuid>
#include <QFileInfo>
#include <shlwapi.h>
#pragma comment(lib, "shlwapi")

#pragma comment(lib, "netapi32")

namespace hc::storage
{
    namespace
    {
        std::wstring Wide(const std::string& Value) { return QString::fromUtf8(Value).toStdWString(); }
        std::string Utf8(const wchar_t* Value) { return QString::fromWCharArray(Value).toStdString(); }
        std::string Session() { return QUuid::createUuid().toString(QUuid::Id128).toStdString() + ":"; }
        Error NativeError(DWORD Code, const ProviderId& Provider)
        {
            if ((Code & 0xffff0000UL) == 0x80070000UL) Code &= 0xffffUL;
            auto Category = ErrorCategory::Unavailable;
            if (Code == ERROR_FILE_NOT_FOUND || Code == ERROR_PATH_NOT_FOUND || Code == ERROR_BAD_NET_NAME)
                Category = ErrorCategory::NotFound;
            else if (Code == ERROR_ACCESS_DENIED || Code == ERROR_LOGON_FAILURE)
                Category = ErrorCategory::AccessDenied;
            return {Category, "win32:" + std::to_string(Code), Provider, {}, Category == ErrorCategory::Unavailable, "refresh-or-reconnect"};
        }
        template<class T, class F> Task<T> Run(F Work)
        {
            return std::async(std::launch::async, [Work = std::move(Work)]() mutable -> Result<T> {
                try { return Work(); }
                catch (const std::exception&) { return Error{ErrorCategory::Internal, "provider-exception"}; }
            });
        }
        std::uint64_t Ticks(FILETIME Time)
        {
            return (static_cast<std::uint64_t>(Time.dwHighDateTime) << 32) | Time.dwLowDateTime;
        }
        std::int64_t Milliseconds(FILETIME Time)
        {
            return static_cast<std::int64_t>(Ticks(Time) / 10000) - 11644473600000LL;
        }
        ResourceRevision Revision(const WIN32_FILE_ATTRIBUTE_DATA& Data)
        {
            return native::ListingRevision(Data);
        }
        bool ReadAttributes(const std::wstring& Path, WIN32_FILE_ATTRIBUTE_DATA& Data, ResourceRevision* ActualRevision = nullptr, BY_HANDLE_FILE_INFORMATION* ActualInfo = nullptr)
        {
            // Query the opened object. SMB/NTFS pathname metadata caches can lag
            // behind a child's deletion and falsely invalidate a directory job.
            const auto Handle = CreateFileW(Path.c_str(), FILE_READ_ATTRIBUTES,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
            if (Handle == INVALID_HANDLE_VALUE) return false;
            BY_HANDLE_FILE_INFORMATION Info{};
            const bool Success = GetFileInformationByHandle(Handle, &Info);
            const auto Code = GetLastError();
            CloseHandle(Handle);
            if (!Success) { SetLastError(Code); return false; }
            Data = {Info.dwFileAttributes, Info.ftCreationTime, Info.ftLastAccessTime, Info.ftLastWriteTime, Info.nFileSizeHigh, Info.nFileSizeLow};
            if (ActualRevision) *ActualRevision = native::HandleRevision(Info);
            if (ActualInfo) *ActualInfo = Info;
            return true;
        }
        EntrySnapshot Entry(const ProviderId& Provider, const std::string& Prefix, const Locator& At,
            std::string Name, const WIN32_FILE_ATTRIBUTE_DATA& Data)
        {
            EntrySnapshot Value;
            Value.id_ = {Provider, Prefix + Serialize(At), IdentityScope::NavigationSession, Revision(Data)};
            Value.locator_ = At;
            Value.name_ = std::move(Name);
            Value.attributes_ = Data.dwFileAttributes;
            Value.directory_ = (Data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            Value.size_ = (static_cast<std::uint64_t>(Data.nFileSizeHigh) << 32) | Data.nFileSizeLow;
            Value.createdUtcMs_ = Milliseconds(Data.ftCreationTime);
            Value.modifiedUtcMs_ = Milliseconds(Data.ftLastWriteTime);
            if (Ticks(Data.ftLastAccessTime)) Value.hints_["accessedUtcMs"] = std::to_string(Milliseconds(Data.ftLastAccessTime));
            // Called on the provider worker. Cache by extension for this thread;
            // no file access or shell association lookup occurs during painting.
            if (!Value.directory_) {
                const auto Extension = QFileInfo(QString::fromStdString(Value.name_)).suffix().toLower();
                if (!Extension.isEmpty()) {
                    static thread_local QMap<QString, QString> Descriptions;
                    if (!Descriptions.contains(Extension)) {
                        wchar_t Buffer[1024]{}; DWORD Length = DWORD(std::size(Buffer));
                        const auto Result = AssocQueryStringW(ASSOCF_NOTRUNCATE, ASSOCSTR_FRIENDLYDOCNAME,
                            (L"." + Extension.toStdWString()).c_str(), nullptr, Buffer, &Length);
                        Descriptions[Extension] = SUCCEEDED(Result) ? QString::fromWCharArray(Buffer) : QString{};
                    }
                    if (!Descriptions[Extension].isEmpty()) Value.hints_["typeDescription"] = Descriptions[Extension].toStdString();
                }
            }
            return Value;
        }
        class SnapshotCursor final : public IEntryCursor
        {
        public:
            SnapshotCursor(std::vector<EntrySnapshot> Entries, ProviderId Provider)
                : entries_(std::move(Entries)), provider_(std::move(Provider)) {}
            Task<EntryPage> Next(std::size_t MaxItems, OperationContext Context) override
            {
                if (const auto Failure = Context.Check(provider_)) return Ready<EntryPage>(*Failure);
                if (!MaxItems || !Context.maxPageItems_) return Ready<EntryPage>(Error{ErrorCategory::InvalidRequest, "zero-page-size", provider_});
                const auto Count = (std::min)({MaxItems, Context.maxPageItems_, entries_.size() - offset_});
                EntryPage Page;
                Page.entries_.assign(entries_.begin() + offset_, entries_.begin() + offset_ + Count);
                offset_ += Count;
                Page.end_ = offset_ == entries_.size();
                return Ready<EntryPage>(std::move(Page));
            }
        private:
            std::vector<EntrySnapshot> entries_;
            ProviderId provider_;
            std::size_t offset_ = 0;
        };
        struct FileState
        {
            HANDLE handle_ = INVALID_HANDLE_VALUE;
            std::mutex mutex_;
            ProviderId provider_;
            ~FileState() { if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_); }
        };
        class FileStream final : public IReadStream
        {
        public:
            explicit FileStream(std::shared_ptr<FileState> State) : state_(std::move(State)) {}
            bool CanSeek() const override { return true; }
            Task<ReadChunk> Read(std::size_t MaxBytes, OperationContext Context) override
            {
                return Run<ReadChunk>([State = state_, MaxBytes, Context]() -> Result<ReadChunk> {
                    std::lock_guard Lock(State->mutex_);
                    if (const auto Failure = Context.Check(State->provider_)) return *Failure;
                    if (!MaxBytes || !Context.maxReadBytes_) return Error{ErrorCategory::InvalidRequest, "zero-read-size", State->provider_};
                    const auto Count = (std::min)({MaxBytes, Context.maxReadBytes_, std::size_t{1024 * 1024}});
                    ReadChunk Chunk;
                    Chunk.bytes_.resize(Count);
                    DWORD Read = 0;
                    if (!ReadFile(State->handle_, Chunk.bytes_.data(), static_cast<DWORD>(Count), &Read, nullptr))
                        return NativeError(GetLastError(), State->provider_);
                    Chunk.bytes_.resize(Read);
                    Chunk.eof_ = Read == 0;
                    return Chunk;
                });
            }
            Task<Unit> Seek(std::uint64_t Offset, OperationContext Context) override
            {
                return Run<Unit>([State = state_, Offset, Context]() -> Result<Unit> {
                    std::lock_guard Lock(State->mutex_);
                    if (const auto Failure = Context.Check(State->provider_)) return *Failure;
                    LARGE_INTEGER Size, Position;
                    if (!GetFileSizeEx(State->handle_, &Size)) return NativeError(GetLastError(), State->provider_);
                    if (Offset > static_cast<std::uint64_t>(Size.QuadPart)) return Error{ErrorCategory::InvalidRequest, "seek-out-of-range", State->provider_};
                    Position.QuadPart = static_cast<LONGLONG>(Offset);
                    if (!SetFilePointerEx(State->handle_, Position, nullptr, FILE_BEGIN)) return NativeError(GetLastError(), State->provider_);
                    return Unit{};
                });
            }
        private:
            std::shared_ptr<FileState> state_;
        };
        Result<std::vector<EntrySnapshot>> ReadShell(const Locator& At, const OperationContext& Context);
        class NativeProvider final : public IStorageProvider
        {
        public:
            explicit NativeProvider(LocatorKind Kind) : kind_(Kind), id_{Kind == LocatorKind::Smb ? "smb" : "local"}, session_(Session()) {}
            ProviderId Id() const override { return id_; }
            Result<EntrySnapshot> BindSnapshot(EntrySnapshot Entry) const {
                if (Entry.locator_.kind_ != kind_ || !Entry.locator_.nesting_.empty()) return Error{ErrorCategory::InvalidRequest, "native-snapshot-kind"};
                Entry.id_.providerId_ = id_; Entry.id_.key_ = session_ + Serialize(Entry.locator_);
                Entry.id_.scope_ = IdentityScope::NavigationSession;
                return Entry;
            }
            Task<EntrySnapshot> Resolve(Locator At, OperationContext Context) override
            {
                return Run<EntrySnapshot>([At = std::move(At), Context, Id = id_, Prefix = session_, Kind = kind_]() -> Result<EntrySnapshot> {
                    if (const auto Failure = Context.Check(Id)) return *Failure;
                    if (At.kind_ != Kind || At.root_.empty() || !At.nesting_.empty()) return Error{ErrorCategory::InvalidRequest, "locator-kind", Id};
                    WIN32_FILE_ATTRIBUTE_DATA Data{};
                    ResourceRevision ActualRevision;
                    if (!ReadAttributes(Wide(At.root_), Data, &ActualRevision)) return NativeError(GetLastError(), Id);
                    const auto Separator = At.root_.find_last_of("\\/");
                    auto Value = Entry(Id, Prefix, At, At.root_.substr(Separator == std::string::npos ? 0 : Separator + 1), Data);
                    Value.id_.revision_ = std::move(ActualRevision);
                    return Value;
                });
            }
            Task<SupportDecision> Supports(Operation Request, OperationContext Context) override
            {
                if (const auto Failure = Context.Check(id_)) return Ready<SupportDecision>(*Failure);
                return Ready<SupportDecision>(Request == Operation::Write
                    ? SupportDecision{Availability::Unsupported, "read-only-contract"}
                    : SupportDecision{Availability::Supported, "native-read", false, false, {"access-checked-at-execution", "metadata-revision"}});
            }
            Task<std::unique_ptr<IEntryCursor>> Enumerate(Locator At, OperationContext Context) override
            {
                return Run<std::unique_ptr<IEntryCursor>>([At = std::move(At), Context, Id = id_, Prefix = session_, Kind = kind_]() -> Result<std::unique_ptr<IEntryCursor>> {
                    if (const auto Failure = Context.Check(Id)) return *Failure;
                    if (At.kind_ != Kind || At.root_.empty() || !At.nesting_.empty()) return Error{ErrorCategory::InvalidRequest, "locator-kind", Id};
                    if (Kind == LocatorKind::Smb && At.root_ == "\\")
                    {
                        auto Items = ReadShell({LocatorKind::Shell, "::{F02C1A0D-BE21-4350-88B0-7367FC96EF3C}"}, Context);
                        if (!Items.HasValue()) return Items.Failure();
                        return std::unique_ptr<IEntryCursor>(new SnapshotCursor(std::move(Items.Value()), Id));
                    }
                    auto Path = Wide(At.root_);
                    std::vector<EntrySnapshot> Entries;
                    if (Kind == LocatorKind::Smb && At.root_.starts_with("\\\\") && std::count(At.root_.begin(), At.root_.end(), '\\') == 2)
                    {
                        DWORD Resume = 0;
                        NET_API_STATUS Status;
                        do
                        {
                            if (const auto Failure = Context.Check(Id)) return *Failure;
                            BYTE* Buffer = nullptr;
                            DWORD Read = 0, Total = 0;
                            Status = NetShareEnum(Path.data(), 1, &Buffer, 64 * 1024, &Read, &Total, &Resume);
                            auto Free = qScopeGuard([&] { if (Buffer) NetApiBufferFree(Buffer); });
                            if (Status != NERR_Success && Status != ERROR_MORE_DATA) return NativeError(Status, Id);
                            const auto Shares = reinterpret_cast<SHARE_INFO_1*>(Buffer);
                            for (DWORD Index = 0; Index < Read; ++Index)
                            {
                                if (Shares[Index].shi1_type != STYPE_DISKTREE) continue;
                                if (Entries.size() >= Context.EnumerationLimit()) return Error{ErrorCategory::BudgetExceeded, "entry-limit", Id};
                                WIN32_FILE_ATTRIBUTE_DATA Data{};
                                Data.dwFileAttributes = FILE_ATTRIBUTE_DIRECTORY;
                                const auto Name = Utf8(Shares[Index].shi1_netname);
                                Entries.push_back(Entry(Id, Prefix, {Kind, At.root_ + "\\" + Name}, Name, Data));
                                Entries.back().id_.revision_.reset();
                            }
                        } while (Status == ERROR_MORE_DATA);
                    }
                    else
                    {
                        const auto Attributes = GetFileAttributesW(Path.c_str());
                        if (Attributes == INVALID_FILE_ATTRIBUTES) return NativeError(GetLastError(), Id);
                        if (!(Attributes & FILE_ATTRIBUTE_DIRECTORY)) return Error{ErrorCategory::InvalidRequest, "not-directory", Id};
                        WIN32_FIND_DATAW Found{};
                        const auto Handle = FindFirstFileExW((Path + L"\\*").c_str(), FindExInfoBasic, &Found, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
                        if (Handle == INVALID_HANDLE_VALUE)
                        {
                            const auto Code = GetLastError();
                            if (Code != ERROR_FILE_NOT_FOUND) return NativeError(Code, Id);
                        }
                        else
                        {
                            auto Close = qScopeGuard([&] { FindClose(Handle); });
                            do
                            {
                                if (const auto Failure = Context.Check(Id)) return *Failure;
                                if (wcscmp(Found.cFileName, L".") == 0 || wcscmp(Found.cFileName, L"..") == 0) continue;
                                if (Entries.size() >= Context.EnumerationLimit()) return Error{ErrorCategory::BudgetExceeded, "entry-limit", Id};
                                const auto Name = Utf8(Found.cFileName);
                                WIN32_FILE_ATTRIBUTE_DATA Data{Found.dwFileAttributes, Found.ftCreationTime, Found.ftLastAccessTime, Found.ftLastWriteTime, Found.nFileSizeHigh, Found.nFileSizeLow};
                                const auto ChildPath = At.root_ + (At.root_.ends_with('\\') ? "" : "\\") + Name;
                                auto ActualRevision = Revision(Data);
                                bool HasRevision = true;
                                // NTFS directory enumeration can retain a cached
                                // mtime after children change. Use the same metadata
                                // source as Stat so a fresh snapshot is revalidatable.
                                if (((Data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || Context.freshEnumerationMetadata_)
                                    && !ReadAttributes(Wide(ChildPath), Data, &ActualRevision)) {
                                    const auto Code = GetLastError();
                                    // Listing a readable parent must not disappear because
                                    // one child (e.g. System Volume Information) is protected.
                                    // Browsing may use FindFirstFile metadata, without claiming
                                    // a verified revision. Mutation planning still requires it.
                                    if ((Context.freshEnumerationMetadata_ && !Context.allowUnavailableChildMetadata_)
                                        || (Code != ERROR_ACCESS_DENIED && Code != ERROR_SHARING_VIOLATION))
                                        return NativeError(Code, Id);
                                    HasRevision = false;
                                }
                                auto Item = Entry(Id, Prefix, {Kind, ChildPath}, Name, Data);
                                if (HasRevision) Item.id_.revision_ = std::move(ActualRevision);
                                else Item.id_.revision_.reset();
                                Item.hints_["reparseTag"] = std::to_string(Found.dwReserved0);
                                Entries.push_back(std::move(Item));
                            } while (FindNextFileW(Handle, &Found));
                            const auto Code = GetLastError();
                            if (Code != ERROR_NO_MORE_FILES) return NativeError(Code, Id);
                        }
                    }
                    if (const auto Failure = Context.Check(Id)) return *Failure;
                    return std::unique_ptr<IEntryCursor>(new SnapshotCursor(std::move(Entries), Id));
                });
            }
            Task<EntrySnapshot> Stat(ResourceId Resource, OperationContext Context) override
            {
                return Run<EntrySnapshot>([Resource = std::move(Resource), Context, Id = id_, Prefix = session_]() -> Result<EntrySnapshot> {
                    if (const auto Failure = Context.Check(Id)) return *Failure;
                    if (!(Resource.providerId_ == Id) || !Resource.key_.starts_with(Prefix)) return Error{ErrorCategory::NotFound, "foreign-session", Id, Resource};
                    const auto At = ParseLocator(std::string_view(Resource.key_).substr(Prefix.size()));
                    if (!At.HasValue()) return At.Failure();
                    WIN32_FILE_ATTRIBUTE_DATA Data{};
                    ResourceRevision ActualRevision;
                    BY_HANDLE_FILE_INFORMATION Info{};
                    if (!ReadAttributes(Wide(At.Value().root_), Data, &ActualRevision, &Info)) return NativeError(GetLastError(), Id);
                    auto Value = Entry(Id, Prefix, At.Value(), QFileInfo(QString::fromUtf8(At.Value().root_)).fileName().toStdString(), Data);
                    Value.id_.revision_ = std::move(ActualRevision);
                    if (Resource.revision_ && !native::MatchesRevision(*Resource.revision_, Info)) return Error{ErrorCategory::StaleRevision, "revision-changed", Id, Resource, false, "refresh"};
                    return Value;
                });
            }
            Task<std::shared_ptr<IReadStream>> OpenRead(ResourceId Resource, OperationContext Context) override
            {
                return Run<std::shared_ptr<IReadStream>>([Resource = std::move(Resource), Context, Id = id_, Prefix = session_]() -> Result<std::shared_ptr<IReadStream>> {
                    if (const auto Failure = Context.Check(Id)) return *Failure;
                    if (!(Resource.providerId_ == Id) || !Resource.key_.starts_with(Prefix)) return Error{ErrorCategory::NotFound, "foreign-session", Id, Resource};
                    const auto At = ParseLocator(std::string_view(Resource.key_).substr(Prefix.size()));
                    if (!At.HasValue()) return At.Failure();
                    auto State = std::make_shared<FileState>();
                    State->provider_ = Id;
                    const DWORD Share = Context.denyConcurrentMutation_ ? FILE_SHARE_READ : FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
                    State->handle_ = CreateFileW(Wide(At.Value().root_).c_str(), GENERIC_READ, Share,
                        nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
                    if (State->handle_ == INVALID_HANDLE_VALUE) return NativeError(GetLastError(), Id);
                    BY_HANDLE_FILE_INFORMATION Info{};
                    if (!GetFileInformationByHandle(State->handle_, &Info)) return NativeError(GetLastError(), Id);
                    if (Resource.revision_ && !native::MatchesRevision(*Resource.revision_, Info)) return Error{ErrorCategory::StaleRevision, "revision-changed", Id, Resource};
                    if (const auto Failure = Context.Check(Id)) return *Failure;
                    return std::shared_ptr<IReadStream>(new FileStream(std::move(State)));
                });
            }
        private:
            LocatorKind kind_;
            ProviderId id_;
            std::string session_;
        };

        using CatalogReader = std::function<Result<std::vector<EntrySnapshot>>(const Locator&, const OperationContext&)>;
        class CatalogProvider final : public IStorageProvider
        {
        public:
            CatalogProvider(ProviderId Id, CatalogReader Reader) : id_(std::move(Id)), reader_(std::move(Reader)), session_(Session()) {}
            ProviderId Id() const override { return id_; }
            Task<SupportDecision> Supports(Operation Request, OperationContext Context) override
            {
                if (const auto Failure = Context.Check(id_)) return Ready<SupportDecision>(*Failure);
                if (Request == Operation::OpenRead)
                    return Ready<SupportDecision>(SupportDecision{Availability::Conditional, "materialization-required", true, false, {"content-materializer"}});
                if (Request == Operation::Stat && id_.value_ == "shell")
                    return Ready<SupportDecision>(SupportDecision{Availability::Unsupported, "shell-stat-requires-binding"});
                return Ready<SupportDecision>(Request == Operation::Write
                    ? SupportDecision{Availability::Unsupported, "read-only-contract"}
                    : SupportDecision{Availability::Supported, "catalog-read"});
            }
            Task<std::unique_ptr<IEntryCursor>> Enumerate(Locator At, OperationContext Context) override
            {
                return Run<std::unique_ptr<IEntryCursor>>([Reader = reader_, At = std::move(At), Context, Id = id_, Prefix = session_]() -> Result<std::unique_ptr<IEntryCursor>> {
                    if (const auto Failure = Context.Check(Id)) return *Failure;
                    auto Items = Reader(At, Context);
                    if (!Items.HasValue()) return Items.Failure();
                    if (Items.Value().size() > Context.EnumerationLimit()) return Error{ErrorCategory::BudgetExceeded, "entry-limit", Id};
                    for (auto& Item : Items.Value())
                    {
                        Item.id_.providerId_ = Id;
                        Item.id_.key_ = Prefix + Serialize(Item.locator_);
                        Item.id_.scope_ = IdentityScope::NavigationSession;
                    }
                    if (const auto Failure = Context.Check(Id)) return *Failure;
                    return std::unique_ptr<IEntryCursor>(new SnapshotCursor(std::move(Items.Value()), Id));
                });
            }
            Task<EntrySnapshot> Stat(ResourceId Resource, OperationContext Context) override
            {
                return Run<EntrySnapshot>([Reader = reader_, Resource = std::move(Resource), Context, Id = id_, Prefix = session_]() -> Result<EntrySnapshot> {
                    if (const auto Failure = Context.Check(Id)) return *Failure;
                    if (!(Resource.providerId_ == Id) || !Resource.key_.starts_with(Prefix)) return Error{ErrorCategory::NotFound, "foreign-session", Id, Resource};
                    const auto Parsed = ParseLocator(std::string_view(Resource.key_).substr(Prefix.size()));
                    if (!Parsed.HasValue()) return Parsed.Failure();
                    auto Parent = Parsed.Value();
                    if (Parent.kind_ != LocatorKind::Archive)
                        return Error{ErrorCategory::Unsupported, "shell-stat-requires-binding", Id, Resource};
                    auto& Path = Parent.nesting_.back().entry_;
                    const auto Slash = Path.find_last_of('/');
                    Path = Slash == 0 ? "/" : Path.substr(0, Slash);
                    auto Items = Reader(Parent, Context);
                    if (!Items.HasValue()) return Items.Failure();
                    for (auto& Item : Items.Value())
                    {
                        if (!(Item.locator_ == Parsed.Value())) continue;
                        if (Resource.revision_ && Resource.revision_ != Item.id_.revision_) return Error{ErrorCategory::StaleRevision, "revision-changed", Id, Resource};
                        Item.id_ = Resource;
                        return Item;
                    }
                    return Error{ErrorCategory::NotFound, "entry-not-found", Id, Resource};
                });
            }
            Task<std::shared_ptr<IReadStream>> OpenRead(ResourceId Resource, OperationContext Context) override
            {
                if (const auto Failure = Context.Check(id_)) return Ready<std::shared_ptr<IReadStream>>(*Failure);
                return Ready<std::shared_ptr<IReadStream>>(Error{ErrorCategory::Unsupported, "materialization-required", id_, Resource, false, "use-legacy-viewer"});
            }
        private:
            ProviderId id_;
            CatalogReader reader_;
            std::string session_;
        };

        Result<std::vector<EntrySnapshot>> ReadShell(const Locator& At, const OperationContext& Context)
        {
            const ProviderId Id{"shell"};
            if (At.kind_ != LocatorKind::Shell || At.root_.empty()) return Error{ErrorCategory::InvalidRequest, "shell-location", Id};
            const auto Initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            if (FAILED(Initialized)) return NativeError(Initialized, Id);
            auto Uninitialize = qScopeGuard([] { CoUninitialize(); });
            PIDLIST_ABSOLUTE Root = nullptr;
            auto Status = SHParseDisplayName(Wide(At.root_).c_str(), nullptr, &Root, 0, nullptr);
            if (FAILED(Status)) return NativeError(Status, Id);
            auto FreeRoot = qScopeGuard([&] { CoTaskMemFree(Root); });
            IShellFolder* Desktop = nullptr;
            Status = SHGetDesktopFolder(&Desktop);
            if (FAILED(Status)) return NativeError(Status, Id);
            auto FreeDesktop = qScopeGuard([&] { Desktop->Release(); });
            IShellFolder* Folder = nullptr;
            Status = Desktop->BindToObject(Root, nullptr, IID_PPV_ARGS(&Folder));
            if (FAILED(Status)) return NativeError(Status, Id);
            auto FreeFolder = qScopeGuard([&] { Folder->Release(); });
            IEnumIDList* Enumerator = nullptr;
            // Network discovery may wait for slow/offline devices. Ask the
            // shell for its quickly enumerable snapshot; refresh can discover
            // later arrivals without holding the previous panel for a timeout.
            // QSHChangeNotify subscribes to the network namespace and invalidates
            // active network panels when asynchronous discovery adds entries.
            const bool Network = At.root_ == "::{F02C1A0D-BE21-4350-88B0-7367FC96EF3C}";
            const SHCONTF Flags = Network ? SHCONTF_FOLDERS | SHCONTF_FASTITEMS | SHCONTF_ENABLE_ASYNC
                : SHCONTF_FOLDERS | SHCONTF_NONFOLDERS | SHCONTF_INCLUDEHIDDEN;
            Status = Folder->EnumObjects(nullptr, Flags, &Enumerator);
            if (FAILED(Status)) return NativeError(Status, Id);
            std::vector<EntrySnapshot> Entries;
            if (!Enumerator) return Entries;
            auto FreeEnumerator = qScopeGuard([&] { Enumerator->Release(); });
            for (;;)
            {
                if (const auto Failure = Context.Check(Id)) return *Failure;
                PITEMID_CHILD Child = nullptr;
                Status = Enumerator->Next(1, &Child, nullptr);
                if (Status == S_FALSE) break;
                if (FAILED(Status)) return NativeError(Status, Id);
                auto FreeChild = qScopeGuard([&] { CoTaskMemFree(Child); });
                if (Entries.size() >= Context.EnumerationLimit()) return Error{ErrorCategory::BudgetExceeded, "entry-limit", Id};
                const auto Absolute = ILCombine(Root, Child);
                if (!Absolute) return Error{ErrorCategory::Internal, "shell-id-allocation", Id};
                auto FreeAbsolute = qScopeGuard([&] { CoTaskMemFree(Absolute); });
                PWSTR Name = nullptr, Parsing = nullptr;
                Status = SHGetNameFromIDList(Absolute, SIGDN_NORMALDISPLAY, &Name);
                if (FAILED(Status)) return NativeError(Status, Id);
                auto FreeName = qScopeGuard([&] { CoTaskMemFree(Name); });
                Status = SHGetNameFromIDList(Absolute, SIGDN_DESKTOPABSOLUTEPARSING, &Parsing);
                if (FAILED(Status)) return NativeError(Status, Id);
                auto FreeParsing = qScopeGuard([&] { CoTaskMemFree(Parsing); });
                SFGAOF Attributes = SFGAO_FOLDER | SFGAO_HIDDEN | SFGAO_SYSTEM;
                PCUITEMID_CHILD ChildArray[] = {Child};
                Status = Folder->GetAttributesOf(1, ChildArray, &Attributes);
                if (FAILED(Status)) return NativeError(Status, Id);
                EntrySnapshot Value;
                Value.name_ = Utf8(Name);
                Value.locator_ = {LocatorKind::Shell, Utf8(Parsing)};
                Value.directory_ = (Attributes & SFGAO_FOLDER) != 0;
                Value.attributes_ = (Value.directory_ ? FILE_ATTRIBUTE_DIRECTORY : 0)
                    | ((Attributes & SFGAO_HIDDEN) ? FILE_ATTRIBUTE_HIDDEN : 0)
                    | ((Attributes & SFGAO_SYSTEM) ? FILE_ATTRIBUTE_SYSTEM : 0);
                Value.hints_["shellPidl"] = QByteArray(reinterpret_cast<const char*>(Absolute), ILGetSize(Absolute)).toBase64().toStdString();
                PWSTR FilePath = nullptr;
                if (SUCCEEDED(SHGetNameFromIDList(Absolute, SIGDN_FILESYSPATH, &FilePath)))
                {
                    Value.hints_["filesystemPath"] = Utf8(FilePath);
                    CoTaskMemFree(FilePath);
                }
                Entries.push_back(std::move(Value));
            }
            return Entries;
        }
    }

    std::shared_ptr<IStorageProvider> MakeNativeReadProvider(LocatorKind Kind)
    {
        return std::make_shared<NativeProvider>(Kind);
    }
    Result<EntrySnapshot> BindNativeSnapshot(IStorageProvider& Provider, EntrySnapshot Entry)
    {
        if (auto* Native = dynamic_cast<NativeProvider*>(&Provider)) return Native->BindSnapshot(std::move(Entry));
        return Error{ErrorCategory::Unsupported, "native-snapshot-provider"};
    }

    std::shared_ptr<IStorageProvider> MakeShellReadProvider(std::string RootParsingName)
    {
        return std::make_shared<CatalogProvider>(ProviderId{"shell"}, [Root = std::move(RootParsingName)](const Locator& At, const OperationContext& Context) {
            return ReadShell(At.root_.empty() ? Locator{LocatorKind::Shell, Root} : At, Context);
        });
    }

    std::shared_ptr<IStorageProvider> MakeArchiveReadProvider(Locator Root, std::vector<SnapshotDirectory> Directories)
    {
        return std::make_shared<CatalogProvider>(ProviderId{"wcx"}, [Root = std::move(Root), Directories = std::move(Directories)](const Locator& At, const OperationContext&) -> Result<std::vector<EntrySnapshot>> {
            if (At.kind_ != LocatorKind::Archive || At.root_ != Root.root_) return Error{ErrorCategory::InvalidRequest, "archive-location", {"wcx"}};
            for (const auto& [Location, Entries] : Directories)
                if (Location == At) return Entries;
            return Error{ErrorCategory::NotFound, "archive-directory-not-found", {"wcx"}};
        });
    }
}
