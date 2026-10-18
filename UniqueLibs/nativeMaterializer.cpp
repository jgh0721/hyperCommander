#include "nativeMaterializer.hpp"
#include "commonLibs/operations/jobScheduler.hpp"
#include <QtCore>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <sddl.h>
#include <array>
#include <condition_variable>
#include <deque>
#include <thread>

#pragma comment(lib, "advapi32")

namespace hc::storage
{
    namespace
    {
        struct Handle
        {
            HANDLE value_ = INVALID_HANDLE_VALUE;
            Handle() = default;
            explicit Handle(HANDLE Value) : value_(Value) {}
            Handle(const Handle&) = delete;
            Handle& operator=(const Handle&) = delete;
            Handle(Handle&& Other) noexcept : value_(std::exchange(Other.value_, INVALID_HANDLE_VALUE)) {}
            Handle& operator=(Handle&& Other) noexcept
            { if (this != &Other) { Close(); value_ = std::exchange(Other.value_, INVALID_HANDLE_VALUE); } return *this; }
            ~Handle() { Close(); }
            void Close() { if (value_ != INVALID_HANDLE_VALUE) CloseHandle(std::exchange(value_, INVALID_HANDLE_VALUE)); }
            bool Valid() const { return value_ != INVALID_HANDLE_VALUE; }
        };
        std::wstring Native(const QString& Path) { return ("\\\\?\\" + QDir::toNativeSeparators(Path)).toStdWString(); }
        Error WinError(const QString& Path, DWORD Code = GetLastError())
        {
            return {Code == ERROR_ACCESS_DENIED ? ErrorCategory::AccessDenied : ErrorCategory::Unavailable,
                "materialization-win32:" + std::to_string(Code), {"materializer"}, {},
                Code == ERROR_SHARING_VIOLATION, Path.toStdString()};
        }
        Result<std::vector<Handle>> Pin(const QString& Path)
        {
            const auto Clean = QDir::toNativeSeparators(Path);
            if (!QRegularExpression("^[A-Za-z]:\\\\").match(Clean).hasMatch() || Clean.contains(QChar::Null))
                return Error{ErrorCategory::InvalidRequest, "local-materialization-root-required"};
            std::vector<Handle> Pins;
            int End = 3;
            while (true)
            {
                const auto Part = Clean.left(End);
                Handle Directory(CreateFileW(Native(Part).c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ, nullptr,
                    OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
                if (!Directory.Valid()) return WinError(Part);
                BY_HANDLE_FILE_INFORMATION Info{};
                if (!GetFileInformationByHandle(Directory.value_, &Info)) return WinError(Part);
                if (!(Info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (Info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
                    return Error{ErrorCategory::Unsupported, "materialization-reparse-root-denied"};
                Pins.push_back(std::move(Directory));
                if (End >= Clean.size()) return Pins;
                const auto Start = End == 3 ? End : End + 1;
                End = Clean.indexOf('\\', Start);
                if (End < 0) End = Clean.size();
                const auto Component = Clean.mid(Start, End - Start);
                if (Component.isEmpty() || Component == "." || Component == ".." || Component.contains(':'))
                    return Error{ErrorCategory::InvalidRequest, "invalid-materialization-root"};
            }
        }
        Result<Unit> PrivateDirectory(const QString& Path, bool AllowExisting = false)
        {
            Handle Token;
            if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &Token.value_)) return WinError(Path);
            DWORD Size = 0; GetTokenInformation(Token.value_, TokenUser, nullptr, 0, &Size);
            std::vector<std::byte> Buffer(Size);
            if (!GetTokenInformation(Token.value_, TokenUser, Buffer.data(), Size, &Size)) return WinError(Path);
            LPWSTR Sid = nullptr;
            if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(Buffer.data())->User.Sid, &Sid)) return WinError(Path);
            const auto Sddl = (QStringLiteral("D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;") + QString::fromWCharArray(Sid) + ')').toStdWString();
            LocalFree(Sid);
            PSECURITY_DESCRIPTOR Descriptor = nullptr;
            if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(Sddl.c_str(), SDDL_REVISION_1, &Descriptor, nullptr)) return WinError(Path);
            SECURITY_ATTRIBUTES Attributes{sizeof(SECURITY_ATTRIBUTES), Descriptor, FALSE};
            const auto Made = CreateDirectoryW(Native(Path).c_str(), &Attributes);
            const auto Code = GetLastError(); LocalFree(Descriptor);
            if (!Made && !(AllowExisting && Code == ERROR_ALREADY_EXISTS)) return WinError(Path, Code);
            return Unit{};
        }
        Result<Handle> Exclusive(const QString& Path, DWORD Creation)
        {
            Handle File(CreateFileW(Native(Path).c_str(), GENERIC_READ | GENERIC_WRITE | DELETE, 0, nullptr,
                Creation, FILE_ATTRIBUTE_HIDDEN | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
            if (!File.Valid()) return WinError(Path);
            BY_HANDLE_FILE_INFORMATION Info{};
            if (!GetFileInformationByHandle(File.value_, &Info)) return WinError(Path);
            if (Info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT) || Info.nNumberOfLinks != 1)
                return Error{ErrorCategory::InvalidRequest, "invalid-lease-manifest"};
            return File;
        }
        Result<Unit> Dispose(HANDLE File, const QString& Path)
        {
            FILE_DISPOSITION_INFO Removed{TRUE};
            if (!SetFileInformationByHandle(File, FileDispositionInfo, &Removed, sizeof(Removed))) return WinError(Path);
            return Unit{};
        }
        using Identity = std::array<DWORD, 3>;
        Identity FileIdentity(const BY_HANDLE_FILE_INFORMATION& Info) { return {Info.dwVolumeSerialNumber, Info.nFileIndexHigh, Info.nFileIndexLow}; }
        struct Record
        {
            QString directory_, content_;
            Handle owner_, file_;
            std::vector<Handle> pins_;
            Identity identity_{};
            std::uint64_t bytes_ = 0;
            bool accounted_ = false;
            bool created_ = false;
        };
        Result<Unit> Cleanup(Record& Item)
        {
            Item.file_.Close();
            // A producer can leave a private workspace after a crash. Preserve
            // its ownership manifest until all unexpected entries are resolved;
            // never recursively remove a tree based only on its location.
            WIN32_FIND_DATAW Found{};
            const auto Search = FindFirstFileW(Native(QDir(Item.directory_).filePath("*")).c_str(), &Found);
            if (Search != INVALID_HANDLE_VALUE)
            {
                const auto Close = qScopeGuard([&] { FindClose(Search); });
                for (;;)
                {
                    const auto Name = QString::fromWCharArray(Found.cFileName);
                    if (Name != "." && Name != ".." && Name != "owner.lock" && Name != QFileInfo(Item.content_).fileName())
                        return Error{ErrorCategory::InvalidRequest, "unexpected-lease-entry", {}, {}, false, "preserved-workspace:" + Item.directory_.toStdString()};
                    if (!FindNextFileW(Search, &Found)) { if (GetLastError() != ERROR_NO_MORE_FILES) return WinError(Item.directory_); break; }
                }
            }
            else if (GetLastError() != ERROR_FILE_NOT_FOUND && GetLastError() != ERROR_PATH_NOT_FOUND) return WinError(Item.directory_);
            if (!Item.content_.isEmpty())
            {
                Handle Data(CreateFileW(Native(Item.content_).c_str(), DELETE | FILE_READ_ATTRIBUTES, FILE_SHARE_READ,
                    nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr));
                if (Data.Valid())
                {
                    BY_HANDLE_FILE_INFORMATION Info{};
                    if (!GetFileInformationByHandle(Data.value_, &Info)) return WinError(Item.content_);
                    if (Info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) return Error{ErrorCategory::InvalidRequest, "unexpected-lease-directory"};
                    const auto Deleted = Dispose(Data.value_, Item.content_); if (!Deleted.HasValue()) return Deleted;
                }
                else if (GetLastError() != ERROR_FILE_NOT_FOUND) return WinError(Item.content_);
            }
            if (Item.owner_.Valid())
            {
                const auto Deleted = Dispose(Item.owner_.value_, Item.directory_); if (!Deleted.HasValue()) return Deleted;
                Item.owner_.Close();
            }
            Item.pins_.clear();
            Handle Directory(CreateFileW(Native(Item.directory_).c_str(), DELETE | FILE_READ_ATTRIBUTES, FILE_SHARE_READ,
                nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
            if (!Directory.Valid())
            {
                if (GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND) return Unit{};
                return WinError(Item.directory_);
            }
            BY_HANDLE_FILE_INFORMATION Info{};
            if (!GetFileInformationByHandle(Directory.value_, &Info)) return WinError(Item.directory_);
            if (FileIdentity(Info) != Item.identity_ || (Info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
                return Error{ErrorCategory::StaleRevision, "lease-directory-replaced"};
            return Dispose(Directory.value_, Item.directory_);
        }
        class Store : public INativeContentStore, public std::enable_shared_from_this<Store>
        {
        public:
            Store(QString Root, std::vector<Handle> Pins, MaterializationLimits Limits)
                : root_(std::move(Root)), pins_(std::move(Pins)), limits_(Limits), reclaimer_([this] { reclaim(); }) {}
            ~Store()
            {
                { std::lock_guard Lock(mutex_); stopping_ = true; }
                changed_.notify_all(); reclaimer_.join();
            }
            Result<std::shared_ptr<IMaterializedFileLease>> Produce(EntrySnapshot Entry,
                std::vector<std::shared_ptr<IMaterializedFileLease>> Parents, std::optional<std::uint64_t> CompressedBytes,
                OperationContext Context, std::function<Result<Unit>(const std::string&, OperationContext)> Producer, ProducedSize Size) override;
            Result<std::unique_ptr<Record>> Create(const EntrySnapshot& Entry)
            {
                std::lock_guard Maintenance(maintenance_);
                {
                    std::lock_guard Lock(mutex_);
                    if (active_ >= limits_.maxActiveFiles_ || Entry.size_ > limits_.maxTotalBytes_ - bytes_)
                        return Error{ErrorCategory::BudgetExceeded, "materialization-total-budget"};
                    ++active_; bytes_ += Entry.size_;
                }
                auto Item = std::make_unique<Record>(); Item->bytes_ = Entry.size_; Item->accounted_ = true;
                const auto Release = qScopeGuard([&] {
                    if (!Item) return;
                    if (!Item->created_ || Cleanup(*Item).HasValue()) release(*Item);
                    else Schedule(std::move(Item));
                });
                auto Gate = Exclusive(root_ + "/root.lock", OPEN_ALWAYS);
                if (!Gate.HasValue()) return Gate.Failure();
                ULARGE_INTEGER Available{};
                if (!GetDiskFreeSpaceExW(Native(root_).c_str(), &Available, nullptr, nullptr)) return WinError(root_);
                if (Entry.size_ > Available.QuadPart) return Error{ErrorCategory::BudgetExceeded, "materialization-disk-space"};
                Item->directory_ = root_ + "/hc-lease-v1-" + QUuid::createUuid().toString(QUuid::Id128);
                auto Made = PrivateDirectory(Item->directory_); if (!Made.HasValue()) return Made.Failure();
                Item->created_ = true;
                auto Pins = Pin(Item->directory_); if (!Pins.HasValue()) return Pins.Failure();
                Item->pins_ = std::move(Pins.Value());
                BY_HANDLE_FILE_INFORMATION Info{};
                if (!GetFileInformationByHandle(Item->pins_.back().value_, &Info)) return WinError(Item->directory_);
                Item->identity_ = FileIdentity(Info);
                auto Owner = Exclusive(Item->directory_ + "/owner.lock", CREATE_NEW); if (!Owner.HasValue()) return Owner.Failure();
                Item->owner_ = std::move(Owner.Value());
                const auto Extension = QRegularExpression("\\.[A-Za-z0-9]{1,16}$").match(QString::fromStdString(Entry.name_)).captured();
                const auto Leaf = "content" + Extension;
                Item->content_ = Item->directory_ + '/' + Leaf;
                const auto Manifest = QJsonDocument(QJsonObject{{"schema", 1}, {"directory", QFileInfo(Item->directory_).fileName()}, {"file", Leaf}}).toJson(QJsonDocument::Compact);
                DWORD Written = 0;
                if (!WriteFile(Item->owner_.value_, Manifest.data(), static_cast<DWORD>(Manifest.size()), &Written, nullptr)
                    || Written != Manifest.size() || !FlushFileBuffers(Item->owner_.value_)) return WinError(Item->directory_);
                Item->file_ = Handle(CreateFileW(Native(Item->content_).c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ,
                    nullptr, CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
                if (!Item->file_.Valid()) return WinError(Item->content_);
                return std::move(Item);
            }
            void Schedule(std::unique_ptr<Record> Item)
            { { std::lock_guard Lock(mutex_); queue_.push_back(std::move(Item)); } changed_.notify_all(); }
            MaterializationCleanup Drain()
            {
                std::unique_lock Lock(mutex_);
                for (auto& Item : failed_) queue_.push_back(std::move(Item));
                failed_.clear(); changed_.notify_all();
                changed_.wait(Lock, [this] { return queue_.empty() && !working_; });
                auto Result = report_; report_ = {}; return Result;
            }
            MaterializationCleanup Recover()
            {
                std::lock_guard Maintenance(maintenance_);
                MaterializationCleanup Report;
                auto Gate = Exclusive(root_ + "/root.lock", OPEN_ALWAYS);
                if (!Gate.HasValue()) { Report.errors_.push_back(Gate.Failure()); return Report; }
                const auto Names = QDir(root_).entryList({"hc-lease-v1-*"}, QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
                if (Names.size() > 4096) { Report.errors_.push_back({ErrorCategory::BudgetExceeded, "lease-recovery-entry-budget"}); return Report; }
                for (const auto& Name : Names)
                {
                    if (!QRegularExpression("^hc-lease-v1-[0-9a-f]{32}$").match(Name).hasMatch()) continue;
                    Record Item; Item.directory_ = root_ + '/' + Name;
                    auto Pins = Pin(Item.directory_);
                    if (!Pins.HasValue()) { Report.errors_.push_back(Pins.Failure()); continue; }
                    Item.pins_ = std::move(Pins.Value());
                    BY_HANDLE_FILE_INFORMATION Info{};
                    if (!GetFileInformationByHandle(Item.pins_.back().value_, &Info)) { Report.errors_.push_back(WinError(Item.directory_)); continue; }
                    Item.identity_ = FileIdentity(Info);
                    auto Owner = Exclusive(Item.directory_ + "/owner.lock", OPEN_EXISTING);
                    if (!Owner.HasValue())
                    {
                        if (Owner.Failure().retryable_) { ++Report.busy_; continue; }
                        // A crash between directory creation and manifest creation
                        // can leave only an empty directory. Disposition refuses
                        // nonempty directories; never recursively delete unknown data.
                        if (Owner.Failure().code_ != "materialization-win32:2") { Report.errors_.push_back(Owner.Failure()); continue; }
                    }
                    else
                    {
                        Item.owner_ = std::move(Owner.Value());
                        std::array<char, 513> Bytes{}; DWORD Read = 0;
                        if (!ReadFile(Item.owner_.value_, Bytes.data(), 513, &Read, nullptr) || Read > 512)
                        { Report.errors_.push_back({ErrorCategory::InvalidRequest, "invalid-lease-manifest"}); continue; }
                        const auto Manifest = QJsonDocument::fromJson(QByteArray(Bytes.data(), Read)).object();
                        const auto Leaf = Manifest.value("file").toString();
                        if (Manifest.value("schema").toInt() != 1 || Manifest.value("directory").toString() != Name
                            || !QRegularExpression("^content(?:\\.[A-Za-z0-9]{1,16})?$").match(Leaf).hasMatch())
                        { Report.errors_.push_back({ErrorCategory::InvalidRequest, "invalid-lease-manifest"}); continue; }
                        Item.content_ = Item.directory_ + '/' + Leaf;
                    }
                    const auto Removed = Cleanup(Item);
                    if (Removed.HasValue()) ++Report.removed_; else Report.errors_.push_back(Removed.Failure());
                }
                return Report;
            }
            const MaterializationLimits limits_;
        private:
            void release(Record& Item)
            {
                if (!Item.accounted_) return;
                std::lock_guard Lock(mutex_); bytes_ -= Item.bytes_; --active_; Item.accounted_ = false;
            }
            void reclaim()
            {
                for (;;)
                {
                    std::unique_ptr<Record> Item;
                    {
                        std::unique_lock Lock(mutex_); changed_.wait(Lock, [this] { return stopping_ || !queue_.empty(); });
                        if (queue_.empty() && stopping_) return;
                        Item = std::move(queue_.front()); queue_.pop_front(); working_ = true;
                    }
                    const auto Removed = Cleanup(*Item);
                    if (Removed.HasValue()) release(*Item);
                    {
                        std::lock_guard Lock(mutex_);
                        if (Removed.HasValue()) ++report_.removed_;
                        else { report_.errors_.push_back(Removed.Failure()); failed_.push_back(std::move(Item)); }
                        working_ = false;
                    }
                    changed_.notify_all();
                }
            }
            QString root_;
            std::vector<Handle> pins_;
            std::mutex mutex_;
            std::mutex maintenance_;
            std::condition_variable changed_;
            std::deque<std::unique_ptr<Record>> queue_;
            std::vector<std::unique_ptr<Record>> failed_;
            MaterializationCleanup report_;
            std::uint64_t bytes_ = 0;
            std::size_t active_ = 0;
            bool stopping_ = false, working_ = false;
            std::jthread reclaimer_;
        };
        class Lease final : public IMaterializedFileLease
        {
        public:
            Lease(std::shared_ptr<Store> Store, std::unique_ptr<Record> Item, EntrySnapshot Entry,
                std::vector<std::shared_ptr<IMaterializedFileLease>> Parents, std::size_t Depth)
                : store_(std::move(Store)), item_(std::move(Item)), entry_(std::move(Entry)), parents_(std::move(Parents)), depth_(Depth) {}
            ~Lease() { store_->Schedule(std::move(item_)); }
            std::string LocalPath() const override { return item_->content_.toStdString(); }
            EntrySnapshot Describe() const override { return entry_; }
            std::size_t Depth() const override { return depth_; }
        private:
            std::shared_ptr<Store> store_;
            std::unique_ptr<Record> item_;
            EntrySnapshot entry_;
            std::vector<std::shared_ptr<IMaterializedFileLease>> parents_;
            std::size_t depth_;
        };
        Result<std::shared_ptr<IMaterializedFileLease>> Store::Produce(EntrySnapshot Entry,
            std::vector<std::shared_ptr<IMaterializedFileLease>> Parents, std::optional<std::uint64_t> CompressedBytes,
            OperationContext Context, std::function<Result<Unit>(const std::string&, OperationContext)> Producer, ProducedSize Size)
        {
            if (const auto Failure = Context.Check({"materializer"})) return *Failure;
            if (!Producer || Entry.directory_ || (Entry.attributes_ & 0x400)
                || (Size != ProducedSize::Exact && Size != ProducedSize::Bounded && Size != ProducedSize::Growing))
                return Error{ErrorCategory::InvalidRequest, "invalid-content-producer"};
            auto Depth = Entry.locator_.nesting_.size();
            for (const auto& Parent : Parents)
            {
                if (!Parent || Parent->Depth() >= limits_.maxDepth_) return Error{ErrorCategory::BudgetExceeded, "materialization-file-budget"};
                Depth = std::max(Depth, Parent->Depth() + 1);
            }
            auto Reservation = Entry; if (Size == ProducedSize::Growing) Reservation.size_ = 0;
            if (const auto Policy = CheckMaterialization(Reservation, Depth, CompressedBytes, limits_, Context)) return *Policy;
            auto Created = Create(Reservation); if (!Created.HasValue()) return Created.Failure();
            auto Item = std::move(Created.Value());
            auto Guard = qScopeGuard([&] { if (Item) Schedule(std::move(Item)); });
            const auto Fail = [&](Error Failure) -> Result<std::shared_ptr<IMaterializedFileLease>> {
                Schedule(std::move(Item));
                const auto Report = Drain();
                if (!Report.errors_.empty()) Failure.userAction_ = "cleanup-pending:" + Report.errors_.front().userAction_;
                return Failure;
            };
            // WCX opens the destination by path. Keep its parent and ownership
            // manifest pinned while releasing the initial writable file handle.
            Item->file_.Close();
            std::optional<Error> GrowthFailure;
            std::stop_source Stop;
            std::stop_callback Cancel(Context.stopToken_, [&] { Stop.request_stop(); });
            const auto Account = [&](uint64_t Bytes) -> Result<Unit> {
                auto Current = Entry; Current.size_ = Bytes;
                if (Bytes > Entry.size_) return Error{ErrorCategory::BudgetExceeded, "produced-size-budget"};
                if (const auto Policy = CheckMaterialization(Current, Depth, CompressedBytes, limits_, Context)) return *Policy;
                std::lock_guard Lock(mutex_);
                if (Bytes > Item->bytes_ && Bytes - Item->bytes_ > limits_.maxTotalBytes_ - bytes_)
                    return Error{ErrorCategory::BudgetExceeded, "materialization-total-budget"};
                bytes_ = bytes_ - Item->bytes_ + Bytes; Item->bytes_ = Bytes; return Unit{};
            };
            if (Size == ProducedSize::Growing) {
                Context.stopToken_ = Stop.get_token();
                Context.progress_ = [&, Progress = Context.progress_](uint64_t Done) {
                    const auto Observed = QFileInfo(Item->content_).size();
                    auto Charged = Account(Observed < 0 ? 0 : uint64_t(Observed));
                    if (!Charged.HasValue()) { GrowthFailure = Charged.Failure(); Stop.request_stop(); }
                    if (Progress) Progress(Done);
                };
            }
            try
            {
                const auto Produced = Producer(Item->content_.toStdString(), Context);
                if (GrowthFailure) return Fail(*GrowthFailure);
                if (!Produced.HasValue()) return Fail(Produced.Failure());
            }
            catch (...) { return Fail({ErrorCategory::Internal, "content-producer-exception"}); }
            if (const auto Failure = Context.Check({"materializer"})) return Fail(*Failure);
            Item->file_ = Handle(CreateFileW(Native(Item->content_).c_str(), GENERIC_READ, FILE_SHARE_READ,
                nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
            if (!Item->file_.Valid()) return Fail(WinError(Item->content_));
            BY_HANDLE_FILE_INFORMATION Info{};
            if (!GetFileInformationByHandle(Item->file_.value_, &Info)) return Fail(WinError(Item->content_));
            const auto Bytes = (static_cast<std::uint64_t>(Info.nFileSizeHigh) << 32) | Info.nFileSizeLow;
            if (Info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT) || Info.nNumberOfLinks != 1
                || (Size == ProducedSize::Exact ? Bytes != Entry.size_ : Bytes > Entry.size_))
                return Fail({ErrorCategory::InvalidRequest, "invalid-produced-content"});
            if (Size == ProducedSize::Growing) { auto Charged = Account(Bytes); if (!Charged.HasValue()) return Fail(Charged.Failure()); }
            // The immutable read handle now denies write/delete. Release unused
            // reservation so a chain of small candidates does not consume one
            // full archive ceiling per level; actual live bytes remain charged.
            if (Size == ProducedSize::Bounded)
            {
                std::lock_guard Lock(mutex_); bytes_ -= Item->bytes_ - Bytes; Item->bytes_ = Bytes;
            }
            Entry.size_ = Bytes;
            return std::shared_ptr<IMaterializedFileLease>(new Lease(shared_from_this(), std::move(Item), std::move(Entry), std::move(Parents), Depth));
        }

        class MaterializeTask final : public operations::IJobTask
        {
        public:
            MaterializeTask(std::shared_ptr<Store> Store, MaterializationRequest Request,
                std::shared_ptr<std::promise<Result<std::shared_ptr<IMaterializedFileLease>>>> Promise, OperationContext Original)
                : store_(std::move(Store)), request_(std::move(Request)), promise_(std::move(Promise)), original_(std::move(Original)) {}
            operations::JobStep Advance(operations::JobStepContext Context) override
            {
                using operations::JobState;
                if (const auto Failure = Context.operation_.Check({"materializer"})) return fail(*Failure);
                if (!item_)
                {
                    entry_ = request_.source_->Describe();
                    depth_ = entry_.locator_.nesting_.size();
                    for (const auto& Parent : request_.parents_)
                    {
                        if (!Parent) return fail({ErrorCategory::InvalidRequest, "null-parent-lease"});
                        if (Parent->Depth() >= store_->limits_.maxDepth_) return fail({ErrorCategory::BudgetExceeded, "materialization-file-budget"});
                        depth_ = std::max(depth_, Parent->Depth() + 1);
                    }
                    const auto& Limits = store_->limits_;
                    if (entry_.directory_ || (entry_.attributes_ & 0x400)) return fail({ErrorCategory::Unsupported, "materialization-file-required"});
                    if (const auto Policy = CheckMaterialization(entry_, depth_, request_.compressedBytes_, Limits, Context.operation_)) return fail(*Policy);
                    const auto Verified = request_.source_->Verify(Context.operation_).get(); if (!Verified.HasValue()) return fail(Verified.Failure());
                    auto Created = store_->Create(entry_);
                    if (!Created.HasValue())
                    {
                        if (Created.Failure().retryable_ && Context.retryAttempt_ < 3)
                        { operations::JobStep Step{JobState::Preparing}; Step.error_ = Created.Failure(); Step.retry_ = true; return Step; }
                        return fail(Created.Failure());
                    }
                    item_ = std::move(Created.Value());
                    Context.operation_.denyConcurrentMutation_ = true;
                    auto Opened = request_.source_->OpenRead(Context.operation_).get(); if (!Opened.HasValue()) return fail(Opened.Failure());
                    stream_ = std::move(Opened.Value());
                    if (!stream_) return fail({ErrorCategory::Internal, "null-content-stream"});
                    return {JobState::Running};
                }
                auto Chunk = stream_->Read(std::min<std::size_t>(Context.operation_.maxReadBytes_, 1024 * 1024), Context.operation_).get();
                if (!Chunk.HasValue()) return fail(Chunk.Failure());
                if (Chunk.Value().bytes_.size() > entry_.size_ - written_
                    || Chunk.Value().bytes_.size() > std::min<std::size_t>(Context.operation_.maxReadBytes_, 1024 * 1024)
                    || (Chunk.Value().bytes_.empty() && !Chunk.Value().eof_))
                    return fail({ErrorCategory::BudgetExceeded, "materialization-content-size"});
                DWORD Written = 0;
                if (!Chunk.Value().bytes_.empty() && (!WriteFile(item_->file_.value_, Chunk.Value().bytes_.data(),
                    static_cast<DWORD>(Chunk.Value().bytes_.size()), &Written, nullptr) || Written != Chunk.Value().bytes_.size()))
                    return fail(WinError(item_->content_));
                written_ += Written;
                operations::JobStep Step{JobState::Running}; Step.progress_ = operations::JobProgress{written_, entry_.size_, entry_.name_};
                if (!Chunk.Value().eof_) return Step;
                if (written_ != entry_.size_) return fail({ErrorCategory::StaleRevision, "materialization-content-size"});
                const auto Verified = request_.source_->Verify(Context.operation_).get(); if (!Verified.HasValue()) return fail(Verified.Failure());
                if (!FlushFileBuffers(item_->file_.value_)) return fail(WinError(item_->content_));
                stream_.reset(); item_->file_.Close();
                item_->file_ = Handle(CreateFileW(Native(item_->content_).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                    OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
                if (!item_->file_.Valid()) return fail(WinError(item_->content_));
                ready_ = true; Step.next_ = JobState::Completed; return Step;
            }
            std::vector<Error> Finalize(operations::JobState State) override
            {
                stream_.reset();
                if (State == operations::JobState::Completed && ready_)
                {
                    promise_->set_value(std::shared_ptr<IMaterializedFileLease>(new Lease(store_, std::move(item_), entry_, std::move(request_.parents_), depth_)));
                    return {};
                }
                if (item_) store_->Schedule(std::move(item_));
                request_.parents_.clear();
                const auto Cleanup = store_->Drain();
                if (!error_) error_ = original_.Check({"materializer"});
                auto Failure = error_.value_or(Error{State == operations::JobState::Cancelled ? ErrorCategory::Cancelled : ErrorCategory::Internal,
                    State == operations::JobState::Cancelled ? "cancelled" : "materialization-failed"});
                if (!Cleanup.errors_.empty()) Failure.userAction_ = "cleanup-pending:" + Cleanup.errors_.front().userAction_;
                promise_->set_value(std::move(Failure));
                return Cleanup.errors_;
            }
        private:
            operations::JobStep fail(Error Failure)
            { error_ = Failure; operations::JobStep Step{operations::JobState::Failed}; Step.error_ = std::move(Failure); return Step; }
            std::shared_ptr<Store> store_;
            MaterializationRequest request_;
            std::shared_ptr<std::promise<Result<std::shared_ptr<IMaterializedFileLease>>>> promise_;
            OperationContext original_;
            EntrySnapshot entry_;
            std::unique_ptr<Record> item_;
            std::shared_ptr<IReadStream> stream_;
            std::optional<Error> error_;
            std::uint64_t written_ = 0;
            std::size_t depth_ = 0;
            bool ready_ = false;
        };
        class Materializer final : public IMaterializer
        {
        public:
            explicit Materializer(std::shared_ptr<Store> Store) : store_(std::move(Store))
            {
                // Shared providers may release their last reference on another
                // Qt pool thread. Keep scheduler construction and shutdown on
                // one owner without weakening JobScheduler's self-join guard.
                std::promise<void> Ready;
                auto Started = Ready.get_future();
                owner_ = std::jthread([this, &Ready](std::stop_token Stop) {
                    try
                    {
                        scheduler_ = std::make_unique<operations::JobScheduler>(std::make_shared<commands::EventHub>(), 2, 128);
                        Ready.set_value();
                    }
                    catch (...) { Ready.set_exception(std::current_exception()); return; }
                    std::unique_lock Lock(ownerMutex_);
                    ownerChanged_.wait(Lock, Stop, [] { return false; });
                    Lock.unlock();
                    scheduler_.reset();
                });
                Started.get();
            }
            ~Materializer()
            {
                owner_.request_stop();
                owner_.join();
                store_->Drain();
            }
            Task<std::shared_ptr<IMaterializedFileLease>> Materialize(MaterializationRequest Request, OperationContext Context) override
            {
                if (!Request.source_ || !Context.maxReadBytes_)
                    return Ready<std::shared_ptr<IMaterializedFileLease>>(Error{ErrorCategory::InvalidRequest, "invalid-materialization-request"});
                std::lock_guard Lock(mutex_);
                for (auto It = jobs_.begin(); It != jobs_.end();)
                {
                    const auto Status = scheduler_->Snapshot(*It);
                    if (Status.HasValue() && operations::IsTerminal(Status.Value().state_) && scheduler_->Forget(*It).HasValue()) It = jobs_.erase(It);
                    else ++It;
                }
                auto Promise = std::make_shared<std::promise<Result<std::shared_ptr<IMaterializedFileLease>>>>();
                auto Future = Promise->get_future();
                operations::JobRequest Job; Job.operation_ = Context;
                auto Started = scheduler_->Submit(std::move(Job), std::make_unique<MaterializeTask>(store_, std::move(Request), Promise, std::move(Context)));
                if (!Started.HasValue()) Promise->set_value(Started.Failure()); else jobs_.push_back(Started.Value().id_);
                return Future;
            }
            MaterializationCleanup Recover() override { return store_->Recover(); }
            MaterializationCleanup DrainCleanup() override { return store_->Drain(); }
        private:
            std::shared_ptr<Store> store_;
            std::unique_ptr<operations::JobScheduler> scheduler_;
            std::mutex mutex_;
            std::vector<std::string> jobs_;
            std::mutex ownerMutex_;
            std::condition_variable_any ownerChanged_;
            std::jthread owner_;
        };
    }
    static Result<std::shared_ptr<Store>> OpenStore(const std::string& BaseDirectory, MaterializationLimits Limits)
    {
        if (!Limits.maxFileBytes_ || !Limits.maxTotalBytes_ || !Limits.maxActiveFiles_
            || !Limits.maxDepth_ || !Limits.maxExpansionRatio_)
            return Error{ErrorCategory::InvalidRequest, "invalid-materialization-limits"};
        const auto Base = QString::fromUtf8(BaseDirectory.data(), static_cast<qsizetype>(BaseDirectory.size()));
        if (Base.toUtf8().toStdString() != BaseDirectory) return Error{ErrorCategory::InvalidRequest, "invalid-path-encoding"};
        auto Pins = Pin(Base); if (!Pins.HasValue()) return Pins.Failure();
        const auto Root = QDir(Base).filePath("hc-materialized-v1");
        const auto Created = PrivateDirectory(Root, true); if (!Created.HasValue()) return Created.Failure();
        auto RootPins = Pin(Root); if (!RootPins.HasValue()) return RootPins.Failure();
        BY_HANDLE_FILE_INFORMATION Info{};
        if (!GetFileInformationByHandle(RootPins.Value().back().value_, &Info)) return WinError(Root);
        static std::mutex RegistryMutex;
        static std::map<Identity, std::weak_ptr<Store>> Registry;
        std::lock_guard Lock(RegistryMutex);
        for (auto It = Registry.begin(); It != Registry.end();) { if (It->second.expired()) It = Registry.erase(It); else ++It; }
        auto& Slot = Registry[FileIdentity(Info)];
        if (auto Existing = Slot.lock())
        {
            if (Existing->limits_ != Limits) return Error{ErrorCategory::InvalidRequest, "materialization-root-limits-conflict"};
            return Existing;
        }
        auto CreatedStore = std::make_shared<Store>(Root, std::move(RootPins.Value()), Limits);
        Slot = CreatedStore;
        return CreatedStore;
    }
    Result<std::shared_ptr<INativeContentStore>> MakeNativeContentStore(const std::string& BaseDirectory, MaterializationLimits Limits)
    {
        auto Store = OpenStore(BaseDirectory, Limits);
        if (!Store.HasValue()) return Store.Failure();
        return std::shared_ptr<INativeContentStore>(std::move(Store.Value()));
    }
    Result<std::shared_ptr<IMaterializer>> MakeNativeMaterializer(const std::string& BaseDirectory, MaterializationLimits Limits)
    {
        auto Store = OpenStore(BaseDirectory, Limits);
        if (!Store.HasValue()) return Store.Failure();
        return std::shared_ptr<IMaterializer>(new Materializer(std::move(Store.Value())));
    }
}
