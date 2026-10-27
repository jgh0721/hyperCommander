#include "isoProvider.hpp"
#include "nativeIsoRuntime.hpp"
#include <QString>
#include <QUuid>
#include <QScopeGuard>
#include <charconv>

namespace hc::iso {
using namespace storage;
namespace {
constexpr uint64_t RootIndex = UINT64_MAX;
Error Invalid(const char* Code) { return {ErrorCategory::InvalidRequest, Code, {"iso"}}; }
Error Stale() { return {ErrorCategory::StaleRevision, "iso-image-changed", {"iso"}}; }
std::string Fingerprint(const BY_HANDLE_FILE_INFORMATION& Info)
{
    return std::to_string(Info.dwVolumeSerialNumber) + ":" + std::to_string(Info.nFileIndexHigh) + ":"
        + std::to_string(Info.nFileIndexLow) + ":" + std::to_string(Info.ftCreationTime.dwHighDateTime) + ":"
        + std::to_string(Info.ftCreationTime.dwLowDateTime) + ":" + std::to_string(Info.ftLastWriteTime.dwHighDateTime)
        + ":" + std::to_string(Info.ftLastWriteTime.dwLowDateTime) + ":" + std::to_string(Info.nFileSizeHigh)
        + ":" + std::to_string(Info.nFileSizeLow);
}
struct Session {
    Locator root_;
    uint32_t namespace_;
    std::string epoch_ = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
    std::mutex mutex_;
    std::shared_ptr<Runtime> runtime_;
    HANDLE file_ = INVALID_HANDLE_VALUE;
    HcIsoImage* image_ = nullptr;
    HcIsoInfo info_{sizeof(HcIsoInfo)};
    ResourceRevision revision_;
    const OperationContext* context_ = nullptr;
    std::optional<Error> inputError_;
    Session(Locator Root, uint32_t Namespace) : root_(std::move(Root)), namespace_(Namespace) {}
    ~Session() { if (image_) runtime_->close_(image_); if (file_ != INVALID_HANDLE_VALUE) CloseHandle(file_); }
    static int ReadAt(void* Opaque, uint64_t Offset, void* Buffer, uint32_t Capacity, uint32_t* Read)
    {
        auto& Value = *static_cast<Session*>(Opaque);
        *Read = 0;
        if (auto Failure = Value.context_->Check({"iso"})) {
            Value.inputError_ = *Failure; return HC_ISO_CANCELLED;
        }
        LARGE_INTEGER Position; Position.QuadPart = static_cast<LONGLONG>(Offset);
        DWORD Count = 0;
        if (!SetFilePointerEx(Value.file_, Position, nullptr, FILE_BEGIN)
            || !ReadFile(Value.file_, Buffer, Capacity, &Count, nullptr)) return HC_ISO_IO;
        *Read = Count;
        return HC_ISO_OK;
    }
    Result<Unit> Verify(const OperationContext& Context)
    {
        if (auto Error = Context.Check({"iso"})) return *Error;
        const auto Path = QString::fromStdString(root_.root_);
        const auto Current = CreateFileW(reinterpret_cast<LPCWSTR>(Path.utf16()), FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (Current == INVALID_HANDLE_VALUE) return Stale();
        const auto Close = qScopeGuard([&] { CloseHandle(Current); });
        BY_HANDLE_FILE_INFORMATION Held{}, Named{};
        if (!GetFileInformationByHandle(file_, &Held) || !GetFileInformationByHandle(Current, &Named)
            || Fingerprint(Held) != revision_.token_ || Fingerprint(Named) != revision_.token_) return Stale();
        return Unit{};
    }
    Result<Unit> Mount(const OperationContext& Context)
    {
        if (image_) return Verify(Context);
        if (auto Error = Context.Check({"iso"})) return *Error;
        if (root_.kind_ != LocatorKind::Archive || root_.nesting_.size() != 1
            || root_.nesting_[0].provider_ != "wcx" || root_.nesting_[0].entry_ != "/"
            || root_.root_.find('\0') != std::string::npos) return Invalid("iso-root-required");
        auto Module = LoadRuntime(Context); if (!Module.HasValue()) return Module.Failure();
        runtime_ = Module.Value();
        const auto Path = QString::fromStdString(root_.root_);
        // Deny in-place writes so a read cannot mix old/new extents before the
        // writer closes (Windows may defer mtime). Allow atomic replacement;
        // Verify detects the new file identity without blocking publication.
        file_ = CreateFileW(reinterpret_cast<LPCWSTR>(Path.utf16()), GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file_ == INVALID_HANDLE_VALUE) return Error{ErrorCategory::Unavailable, "iso-source-open", {"iso"}};
        bool Success = false;
        const auto Cleanup = qScopeGuard([&] {
            context_ = nullptr;
            if (!Success) { if (image_) runtime_->close_(image_); image_ = nullptr; CloseHandle(file_); file_ = INVALID_HANDLE_VALUE; }
        });
        BY_HANDLE_FILE_INFORMATION Info{};
        if (!GetFileInformationByHandle(file_, &Info) || (Info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return Invalid("iso-file-required");
        revision_.token_ = Fingerprint(Info);
        const HcIsoInput Input{sizeof(HcIsoInput), (uint64_t(Info.nFileSizeHigh) << 32) | Info.nFileSizeLow, ReadAt, this};
        context_ = &Context; inputError_.reset();
        const auto Open = runtime_->openInput_(&Input, namespace_, &image_);
        if (Open != HC_ISO_OK) return inputError_.value_or(EngineError(Open));
        const auto Get = runtime_->info_(image_, &info_); if (Get != HC_ISO_OK) return EngineError(Get);
        auto Valid = Verify(Context); if (!Valid.HasValue()) return Valid.Failure();
        Success = true; return Unit{};
    }
    Result<uint64_t> Index(const Locator& At)
    {
        if (!SameArchiveContainer(root_, At)) return Invalid("iso-container-mismatch");
        auto Path = At.nesting_.back().entry_;
        if (Path == "/" || Path.empty()) return RootIndex;
        if (Path.front() == '/') Path.erase(0, 1);
        if (!Path.empty() && Path.back() == '/') Path.pop_back();
        if (Path.find('\0') != std::string::npos) return Invalid("iso-path-invalid");
        uint64_t Index = RootIndex;
        const auto Status = runtime_->find_(image_, Path.c_str(), &Index);
        if (Status != HC_ISO_OK) return EngineError(Status);
        return Index;
    }
    std::string Prefix() const { return epoch_ + ":" + std::to_string(info_.selectedNamespace_) + ":"; }
    Result<uint64_t> Index(const ResourceId& Id)
    {
        if (Id.providerId_.value_ != "iso" || Id.scope_ != IdentityScope::NavigationSession
            || Id.revision_ != revision_ || !Id.key_.starts_with(Prefix())) return Stale();
        const auto Token = std::string_view(Id.key_).substr(Prefix().size());
        uint64_t Index = 0;
        const auto Parsed = std::from_chars(Token.data(), Token.data() + Token.size(), Index);
        if (Parsed.ec != std::errc{} || Parsed.ptr != Token.data() + Token.size()
            || (Index != RootIndex && Index >= info_.entryCount_)) return Invalid("iso-entry-token");
        return Index;
    }
    Result<EntrySnapshot> Entry(uint64_t Index)
    {
        EntrySnapshot Value;
        Value.id_ = {{"iso"}, Prefix() + std::to_string(Index), IdentityScope::NavigationSession, revision_};
        Value.locator_ = root_;
        if (Index == RootIndex) {
            Value.name_ = "/"; Value.directory_ = true; Value.attributes_ = FILE_ATTRIBUTE_DIRECTORY;
            Value.hints_ = {{"containerKind", "diskImage"}, {"containerBytes", std::to_string(info_.imageBytes_)}};
            return Value;
        }
        HcIsoEntry Raw{sizeof(HcIsoEntry)};
        const auto Status = runtime_->entry_(image_, Index, &Raw); if (Status != HC_ISO_OK) return EngineError(Status);
        const std::string Path = Raw.path_;
        Value.locator_.nesting_.back().entry_ = "/" + Path;
        Value.name_ = Path.substr(Path.find_last_of('/') == std::string::npos ? 0 : Path.find_last_of('/') + 1);
        Value.directory_ = Raw.directory_ != 0; Value.size_ = Raw.size_;
        Value.attributes_ = Value.directory_ ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
        Value.hints_ = {{"wcxName", Path}, {"packedSize", std::to_string(Value.size_)},
            {"isoNamespace", std::to_string(info_.selectedNamespace_)}};
        return Value;
    }
};
class Cursor final : public IEntryCursor {
public:
    Cursor(std::shared_ptr<Session> Value, uint64_t Directory, size_t Limit)
        : session_(std::move(Value)), directory_(Directory), limit_(Limit) {}
    Task<EntryPage> Next(size_t MaxItems, OperationContext Context) override
    {
        return std::async(std::launch::async, [this, MaxItems, Context = std::move(Context)]() -> Result<EntryPage> {
            std::lock_guard Lock(session_->mutex_);
            auto Valid = session_->Verify(Context); if (!Valid.HasValue()) return Valid.Failure();
            if (!MaxItems || !Context.maxPageItems_) return Invalid("iso-page-size");
            EntryPage Page; Page.revision_ = session_->revision_;
            auto Next = position_;
            while (Page.entries_.size() < (std::min)(MaxItems, Context.maxPageItems_)) {
                if (auto Error = Context.Check({"iso"})) return *Error;
                uint64_t Index = RootIndex;
                const auto Status = session_->runtime_->child_(session_->image_, directory_, Next, &Index);
                if (Status == HC_ISO_END) { Page.end_ = true; break; }
                if (Status != HC_ISO_OK) return EngineError(Status);
                if (Next >= (std::min)(limit_, Context.EnumerationLimit()))
                    return Error{ErrorCategory::BudgetExceeded, "iso-enumeration-policy", {"iso"}};
                auto Entry = session_->Entry(Index); if (!Entry.HasValue()) return Entry.Failure();
                Page.entries_.push_back(std::move(Entry.Value())); ++Next;
            }
            Valid = session_->Verify(Context); if (!Valid.HasValue()) return Valid.Failure();
            position_ = Next; return Page;
        });
    }
private:
    std::shared_ptr<Session> session_;
    uint64_t directory_, position_ = 0;
    size_t limit_;
};
class Stream final : public IReadStream {
public:
    Stream(std::shared_ptr<Session> Value, uint64_t Index, uint64_t Size)
        : session_(std::move(Value)), index_(Index), size_(Size) {}
    bool CanSeek() const override { return true; }
    ReadAccess Access() const override { return ReadAccess::RandomAccess; }
    Task<ReadChunk> Read(size_t MaxBytes, OperationContext Context) override
    {
        return std::async(std::launch::async, [this, MaxBytes, Context = std::move(Context)]() -> Result<ReadChunk> {
            std::lock_guard Lock(session_->mutex_);
            auto Valid = session_->Verify(Context); if (!Valid.HasValue()) return Valid.Failure();
            if (!MaxBytes || !Context.maxReadBytes_) return Invalid("iso-read-size");
            ReadChunk Chunk;
            // A transport chunk bounds allocation, not the size of a file or transfer.
            const auto Count = (std::min)({uint64_t(MaxBytes), uint64_t(Context.maxReadBytes_), uint64_t(1024 * 1024), size_ - offset_});
            Chunk.bytes_.resize(static_cast<size_t>(Count));
            session_->context_ = &Context; session_->inputError_.reset();
            const auto Cleanup = qScopeGuard([&] { session_->context_ = nullptr; });
            uint32_t Read = 0;
            const auto Status = session_->runtime_->read_(session_->image_, index_, offset_, Chunk.bytes_.data(), uint32_t(Count), &Read);
            if (Status != HC_ISO_OK) return session_->inputError_.value_or(EngineError(Status));
            if (Read > Count || (Count && !Read)) return Invalid("iso-read-no-progress");
            Valid = session_->Verify(Context); if (!Valid.HasValue()) return Valid.Failure();
            offset_ += Read; Chunk.bytes_.resize(Read); Chunk.eof_ = offset_ == size_; return Chunk;
        });
    }
    Task<Unit> Seek(uint64_t Offset, OperationContext Context) override
    {
        return std::async(std::launch::async, [this, Offset, Context = std::move(Context)]() -> Result<Unit> {
            std::lock_guard Lock(session_->mutex_);
            auto Valid = session_->Verify(Context); if (!Valid.HasValue()) return Valid.Failure();
            if (Offset > size_) return Invalid("iso-seek-range");
            offset_ = Offset; return Unit{};
        });
    }
private:
    std::shared_ptr<Session> session_;
    uint64_t index_, size_, offset_ = 0;
};
class Provider final : public IStorageProvider {
public:
    Provider(Locator Root, uint32_t Namespace) : session_(std::make_shared<Session>(std::move(Root), Namespace)) {}
    ProviderId Id() const override { return {"iso"}; }
    ProviderCapabilities Capabilities() const override
    {
        ProviderCapabilities Value; Value.read_ = ReadAccess::RandomAccess;
        Value.concurrency_ = ProviderConcurrency::SerializedPerSession;
        Value.constraints_.maxEntries_ = 100000;
        Value.constraints_.reason_ = "iso-engine: 100000 entries, 256 MiB metadata, 128 directory levels";
        return Value;
    }
    Task<SupportDecision> Supports(Operation Request, OperationContext Context) override
    {
        if (auto Error = Context.Check(Id())) return Ready<SupportDecision>(*Error);
        return Ready<SupportDecision>(SupportDecision{Request == Operation::Write ? Availability::Unsupported : Availability::Supported,
            Request == Operation::Write ? "iso-prepared-rewrite-required" : ""});
    }
    Task<std::unique_ptr<IEntryCursor>> Enumerate(Locator At, OperationContext Context) override
    {
        return std::async(std::launch::async, [Value = session_, At = std::move(At), Context = std::move(Context)]() -> Result<std::unique_ptr<IEntryCursor>> {
            std::lock_guard Lock(Value->mutex_);
            auto Mounted = Value->Mount(Context); if (!Mounted.HasValue()) return Mounted.Failure();
            auto Index = Value->Index(At); if (!Index.HasValue()) return Index.Failure();
            auto Entry = Value->Entry(Index.Value()); if (!Entry.HasValue()) return Entry.Failure();
            if (!Entry.Value().directory_) return Invalid("iso-directory-required");
            return std::unique_ptr<IEntryCursor>(new Cursor(Value, Index.Value(), Context.EnumerationLimit()));
        });
    }
    Task<EntrySnapshot> Resolve(Locator At, OperationContext Context) override
    {
        return std::async(std::launch::async, [Value = session_, At = std::move(At), Context = std::move(Context)]() -> Result<EntrySnapshot> {
            std::lock_guard Lock(Value->mutex_);
            auto Mounted = Value->Mount(Context); if (!Mounted.HasValue()) return Mounted.Failure();
            auto Index = Value->Index(At); if (!Index.HasValue()) return Index.Failure();
            auto Entry = Value->Entry(Index.Value());
            auto Valid = Value->Verify(Context); if (!Valid.HasValue()) return Valid.Failure();
            return Entry;
        });
    }
    Task<EntrySnapshot> Stat(ResourceId Resource, OperationContext Context) override
    {
        return std::async(std::launch::async, [Value = session_, Resource = std::move(Resource), Context = std::move(Context)]() -> Result<EntrySnapshot> {
            std::lock_guard Lock(Value->mutex_);
            auto Mounted = Value->Mount(Context); if (!Mounted.HasValue()) return Mounted.Failure();
            auto Index = Value->Index(Resource); if (!Index.HasValue()) return Index.Failure();
            auto Entry = Value->Entry(Index.Value());
            auto Valid = Value->Verify(Context); if (!Valid.HasValue()) return Valid.Failure();
            return Entry;
        });
    }
    Task<std::shared_ptr<IReadStream>> OpenRead(ResourceId Resource, OperationContext Context) override
    {
        return std::async(std::launch::async, [Value = session_, Resource = std::move(Resource), Context = std::move(Context)]() -> Result<std::shared_ptr<IReadStream>> {
            std::lock_guard Lock(Value->mutex_);
            auto Mounted = Value->Mount(Context); if (!Mounted.HasValue()) return Mounted.Failure();
            auto Index = Value->Index(Resource); if (!Index.HasValue()) return Index.Failure();
            auto Entry = Value->Entry(Index.Value()); if (!Entry.HasValue()) return Entry.Failure();
            if (Entry.Value().directory_) return Invalid("iso-file-required");
            return std::shared_ptr<IReadStream>(new Stream(Value, Index.Value(), Entry.Value().size_));
        });
    }
private:
    std::shared_ptr<Session> session_;
};
}
std::shared_ptr<IStorageProvider> MakeReadProvider(Locator Root, uint32_t Namespace)
{ return std::make_shared<Provider>(std::move(Root), Namespace); }
}
