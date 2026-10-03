#include "sevenZipBackend.hpp"
#include "Common/MyInitGuid.h"
#include "7zip/Archive/IArchive.h"
#include "7zip/IPassword.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QCryptographicHash>
#include <QRegularExpression>
#include <QtEndian>
#include <map>
#include <set>
#include <atomic>
#include <mutex>

namespace hc::plugins {
using namespace storage;
namespace {
Error Failure(std::string Code, ErrorCategory Category = ErrorCategory::Unavailable)
{ return {Category, std::move(Code), {"7zip"}}; }
template<class T> struct Com {
    T* value_ = nullptr;
    ~Com() { if (value_) value_->Release(); }
    T* operator->() const { return value_; }
};
#define HC_COM_REFS \
    ULONG STDMETHODCALLTYPE AddRef() noexcept override { return ++refs_; } \
    ULONG STDMETHODCALLTYPE Release() noexcept override { const auto Left = --refs_; if (!Left) delete this; return Left; } \
    std::atomic<ULONG> refs_{1}

struct State {
    OperationContext context_;
    std::string archive_;
    std::mutex mutex_;
    std::optional<Error> error_;
    unsigned attempts_ = 0;
    std::optional<storage::Password> password_;
    HRESULT Check(uint64_t Done = 0) noexcept {
        try {
            std::lock_guard Lock(mutex_);
            if (!error_) error_ = context_.Check({"7zip"});
            if (error_) return E_ABORT;
            if (context_.progress_) context_.progress_(Done);
            if (const auto Stop = context_.Check({"7zip"})) { error_ = *Stop; return E_ABORT; }
            return S_OK;
        } catch (...) { return E_ABORT; }
    }
    HRESULT Fail(Error Value) noexcept { std::lock_guard Lock(mutex_); if (!error_) error_ = std::move(Value); return E_ABORT; }
    Error Result(HRESULT Code) { std::lock_guard Lock(mutex_); return error_.value_or(Failure("7zip-hresult:" + std::to_string(uint32_t(Code)))); }
    HRESULT Password(BSTR* Output) noexcept {
        *Output = nullptr;
        try {
            if (Check() != S_OK) return E_ABORT;
            if (!password_) {
                if (!context_.passwordPrompt_) return Fail(Failure("archive-password-required", ErrorCategory::AccessDenied));
                auto Reply = context_.passwordPrompt_(archive_, ++attempts_, context_);
                if (!Reply.HasValue()) return Fail(Reply.Failure());
                password_.emplace(std::move(Reply.Value()));
            }
            auto Text = QString::fromStdString(password_->Value()); auto Clear = qScopeGuard([&] { Text.fill(QChar::Null); });
            if (Text.size() > 4096 || Text.contains(QChar::Null)) return Fail(Failure("invalid-password", ErrorCategory::InvalidRequest));
            if (Check() != S_OK) return E_ABORT;
            *Output = SysAllocStringLen(reinterpret_cast<const wchar_t*>(Text.utf16()), UINT(Text.size()));
            return *Output ? S_OK : E_OUTOFMEMORY;
        } catch (...) { return E_ABORT; }
    }
};
class Input final : public IInStream {
public:
    explicit Input(std::shared_ptr<State> State) : state_(std::move(State)) {}
    ~Input() { for (const auto& Part : parts_) CloseHandle(Part.handle_); }
    HC_COM_REFS;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID Id, void** Value) noexcept override {
        *Value = nullptr;
        if (Id == IID_IUnknown || Id == IID_IInStream || Id == IID_ISequentialInStream) *Value = static_cast<IInStream*>(this);
        if (!*Value) return E_NOINTERFACE; AddRef(); return S_OK;
    }
    bool Open(const QString& Path, bool Split = false) {
        auto Name = Path; unsigned Number = 1;
        do {
            if (state_->Check() != S_OK) return false;
            const auto Handle = CreateFileW(reinterpret_cast<const wchar_t*>(QDir::toNativeSeparators(Name).utf16()), GENERIC_READ, FILE_SHARE_READ,
                nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
            if (Handle == INVALID_HANDLE_VALUE) return !parts_.empty() && GetLastError() == ERROR_FILE_NOT_FOUND;
            BY_HANDLE_FILE_INFORMATION Info{};
            if (!GetFileInformationByHandle(Handle, &Info) || (Info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) { CloseHandle(Handle); return false; }
            const auto Size = (uint64_t(Info.nFileSizeHigh) << 32) | Info.nFileSizeLow;
            if (Size > INT64_MAX - size_) { CloseHandle(Handle); return false; }
            parts_.push_back({Handle, size_, Size}); size_ += Size;
            if (!Split) return true;
            Name = Path.left(Path.size() - 3) + QString::number(++Number).rightJustified(3, '0');
        } while (Number);
        return false;
    }
    HRESULT STDMETHODCALLTYPE Read(void* Data, UInt32 Size, UInt32* Done) noexcept override {
        if (Done) *Done = 0; if (state_->Check(position_) != S_OK) return E_ABORT;
        if (!Size || position_ >= size_) return S_OK;
        for (const auto& Part : parts_) if (position_ >= Part.offset_ && position_ - Part.offset_ < Part.size_) {
            LARGE_INTEGER At{}; At.QuadPart = position_ - Part.offset_;
            DWORD Count = 0;
            if (!SetFilePointerEx(Part.handle_, At, nullptr, FILE_BEGIN) || !ReadFile(Part.handle_, Data, DWORD(std::min<uint64_t>(Size, Part.size_ - At.QuadPart)), &Count, nullptr))
                return HRESULT_FROM_WIN32(GetLastError());
            position_ += Count; if (Done) *Done = Count; return S_OK;
        }
        return E_FAIL;
    }
    HRESULT STDMETHODCALLTYPE Seek(Int64 Offset, UInt32 Origin, UInt64* Position) noexcept override {
        const auto Base = Origin == 0 ? uint64_t(0) : Origin == 1 ? position_ : size_;
        if (Origin > 2 || (Offset < 0 ? uint64_t(-(Offset + 1)) + 1 > Base : uint64_t(Offset) > INT64_MAX - Base)) return E_INVALIDARG;
        position_ = Base + Offset; if (Position) *Position = position_; return S_OK;
    }
    uint64_t Size() const { return size_; }
private:
    struct Part { HANDLE handle_; uint64_t offset_, size_; };
    std::vector<Part> parts_;
    uint64_t size_ = 0, position_ = 0;
    std::shared_ptr<State> state_;
};
class OpenCallback final : public IArchiveOpenCallback, public IArchiveOpenVolumeCallback, public ICryptoGetTextPassword {
public:
    explicit OpenCallback(std::shared_ptr<State> State) : state_(std::move(State)) {}
    HC_COM_REFS;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID Id, void** Value) noexcept override {
        *Value = nullptr;
        if (Id == IID_IUnknown || Id == IID_IArchiveOpenCallback) *Value = static_cast<IArchiveOpenCallback*>(this);
        else if (Id == IID_IArchiveOpenVolumeCallback) *Value = static_cast<IArchiveOpenVolumeCallback*>(this);
        else if (Id == IID_ICryptoGetTextPassword) *Value = static_cast<ICryptoGetTextPassword*>(this);
        if (!*Value) return E_NOINTERFACE; AddRef(); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetTotal(const UInt64*, const UInt64*) noexcept override { return state_->Check(); }
    HRESULT STDMETHODCALLTYPE SetCompleted(const UInt64*, const UInt64* Bytes) noexcept override { return state_->Check(Bytes ? *Bytes : 0); }
    HRESULT STDMETHODCALLTYPE CryptoGetTextPassword(BSTR* Value) noexcept override { return state_->Password(Value); }
    HRESULT STDMETHODCALLTYPE GetProperty(PROPID Id, PROPVARIANT* Value) noexcept override {
        PropVariantInit(Value);
        if (Id == kpidName) { const auto Name = QFileInfo(QString::fromStdString(state_->archive_)).fileName(); Value->vt = VT_BSTR; Value->bstrVal = SysAllocStringLen(reinterpret_cast<const wchar_t*>(Name.utf16()), UINT(Name.size())); }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetStream(const wchar_t* Name, IInStream** Stream) noexcept override {
        *Stream = nullptr;
        try {
            if (state_->Check() != S_OK) return E_ABORT;
            const auto Leaf = QString::fromWCharArray(Name);
            if (Leaf.isEmpty() || Leaf == "." || Leaf == ".." || Leaf.contains('/') || Leaf.contains('\\') || Leaf.contains(':'))
                return state_->Fail(Failure("7zip-volume-path", ErrorCategory::InvalidRequest));
            Com<Input> File{new Input(state_)};
            if (!File->Open(QFileInfo(QString::fromStdString(state_->archive_)).dir().filePath(Leaf))) return S_FALSE;
            *Stream = File.value_; File.value_ = nullptr; return S_OK;
        } catch (...) { return E_ABORT; }
    }
private:
    std::shared_ptr<State> state_;
};
struct Property : PROPVARIANT {
    Property() { PropVariantInit(this); }
    ~Property() { PropVariantClear(this); }
    uint64_t Number() const { return vt == VT_UI8 ? uhVal.QuadPart : vt == VT_UI4 ? ulVal : 0; }
    bool Boolean() const { return vt == VT_BOOL && boolVal != VARIANT_FALSE; }
    QString Text() const { return vt == VT_BSTR ? QString::fromWCharArray(bstrVal, SysStringLen(bstrVal)) : QString{}; }
};
struct Archive {
    QString format_;
    std::shared_ptr<State> state_ = std::make_shared<State>();
    Com<OpenCallback> callback_{new OpenCallback(state_)};
    Com<Input> input_{new Input(state_)};
    Com<IInArchive> archive_;
    UInt32 count_ = 0;
    ~Archive() { if (archive_.value_) archive_->Close(); }
    Result<ArchiveHeader> Header(UInt32 Index) {
        ArchiveHeader Header;
        const auto Read = [&](PROPID Id, Property& Value) { return archive_->GetProperty(Index, Id, &Value) == S_OK; };
        Property Path, Size, Packed, Dir, Encrypted, Attr, Crc, Time, Link, Hard, Anti;
        if (!Read(kpidPath, Path) || !Read(kpidSize, Size) || !Read(kpidPackSize, Packed) || !Read(kpidIsDir, Dir)
            || !Read(kpidEncrypted, Encrypted) || !Read(kpidAttrib, Attr) || !Read(kpidCRC, Crc) || !Read(kpidMTime, Time)
            || !Read(kpidSymLink, Link) || !Read(kpidHardLink, Hard) || !Read(kpidIsAnti, Anti)) return Failure("7zip-entry-properties");
        auto Name = Path.Text(); Name.replace('\\', '/');
        if (Name.isEmpty() || Name.startsWith('/') || Name.contains(':') || Name.contains(QChar::Null)) return Failure("7zip-entry-path", ErrorCategory::InvalidRequest);
        for (const auto& Part : Name.split('/')) if (Part == ".." || Part == ".") return Failure("7zip-entry-path", ErrorCategory::InvalidRequest);
        if (Anti.Boolean())
            return Failure("7zip-special-entry", ErrorCategory::Unsupported);
        Header.name_ = Name.toStdString(); Header.size_ = Size.Number(); Header.packedSize_ = Packed.Number(); Header.directory_ = Dir.Boolean();
        Header.encrypted_ = Encrypted.Boolean(); Header.attributes_ = uint32_t(Attr.Number()); Header.crc_ = uint32_t(Crc.Number());
        Property Posix; if (!Read(kpidPosixAttrib, Posix)) return Failure("7zip-entry-properties");
        const auto Mode = Posix.Number() ? Posix.Number() : (Attr.Number() & 0x8000) ? Attr.Number() >> 16 : 0;
        if (!Link.Text().isEmpty() || !Hard.Text().isEmpty() || (Mode & 0170000) == 0120000) Header.attributes_ |= FILE_ATTRIBUTE_REPARSE_POINT;
        if (Time.vt == VT_FILETIME) { const auto Ticks = (uint64_t(Time.filetime.dwHighDateTime) << 32) | Time.filetime.dwLowDateTime; Header.modifiedUtcMs_ = int64_t(Ticks / 10000) - 11644473600000LL; }
        if (state_->context_.workload_.IsAutomatic() && Header.size_ > state_->context_.workload_.automaticFileBytes_)
            return Failure("automatic-content-budget", ErrorCategory::BudgetExceeded);
        const auto PackedBytes = Header.packedSize_ ? Header.packedSize_ : input_->Size();
        if (state_->context_.workload_.IsAutomatic() && Header.size_ && (!PackedBytes
            || (Header.size_ - 1) / PackedBytes >= state_->context_.workload_.automaticExpansionRatio_))
            return Failure("automatic-expansion-budget", ErrorCategory::BudgetExceeded);
        return Header;
    }
};
class Output final : public ISequentialOutStream {
public:
    Output(QFile* File, uint64_t Limit, std::shared_ptr<State> State, bool Hash = false) : file_(File), limit_(Limit), state_(std::move(State)), hashEnabled_(Hash) {}
    HC_COM_REFS;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID Id, void** Value) noexcept override {
        *Value = nullptr; if (Id == IID_IUnknown || Id == IID_ISequentialOutStream) *Value = static_cast<ISequentialOutStream*>(this);
        if (!*Value) return E_NOINTERFACE; AddRef(); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Write(const void* Data, UInt32 Size, UInt32* Done) noexcept override {
        if (Done) *Done = 0; if (state_->Check(written_) != S_OK) return E_ABORT;
        if (Size > limit_ - written_) return state_->Fail(Failure("7zip-output-size", ErrorCategory::BudgetExceeded));
        if (file_ && file_->write(static_cast<const char*>(Data), Size) != Size) return state_->Fail(Failure("7zip-output-write"));
        if (hashEnabled_) hash_.addData(QByteArrayView(static_cast<const char*>(Data), Size));
        written_ += Size; if (Done) *Done = Size; return state_->Check(written_);
    }
    uint64_t written_ = 0;
    QCryptographicHash hash_{QCryptographicHash::Sha256};
private:
    QFile* file_; uint64_t limit_; std::shared_ptr<State> state_; bool hashEnabled_;
};
class ExtractCallback final : public IArchiveExtractCallback, public ICryptoGetTextPassword {
public:
    ExtractCallback(std::shared_ptr<State> State, Output* Output, UInt32 Index) : state_(std::move(State)), output_(Output), index_(Index) {}
    HC_COM_REFS;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID Id, void** Value) noexcept override {
        *Value = nullptr; if (Id == IID_IUnknown || Id == IID_IArchiveExtractCallback || Id == IID_IProgress) *Value = static_cast<IArchiveExtractCallback*>(this);
        else if (Id == IID_ICryptoGetTextPassword) *Value = static_cast<ICryptoGetTextPassword*>(this);
        if (!*Value) return E_NOINTERFACE; AddRef(); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetTotal(UInt64) noexcept override { return state_->Check(); }
    HRESULT STDMETHODCALLTYPE SetCompleted(const UInt64* Done) noexcept override { return state_->Check(Done ? *Done : 0); }
    HRESULT STDMETHODCALLTYPE CryptoGetTextPassword(BSTR* Value) noexcept override { return state_->Password(Value); }
    HRESULT STDMETHODCALLTYPE GetStream(UInt32 Index, ISequentialOutStream** Stream, Int32 Mode) noexcept override {
        *Stream = nullptr;
        if (Index == index_ && Mode == NArchive::NExtract::NAskMode::kExtract && output_) { output_->AddRef(); *Stream = output_; }
        return state_->Check();
    }
    HRESULT STDMETHODCALLTYPE PrepareOperation(Int32) noexcept override { return state_->Check(); }
    HRESULT STDMETHODCALLTYPE SetOperationResult(Int32 Code) noexcept override {
        if (!Code) return state_->Check();
        return state_->Fail(Failure("7zip-extract-result:" + std::to_string(Code), Code == NArchive::NExtract::NOperationResult::kWrongPassword
            ? ErrorCategory::AccessDenied : Code == NArchive::NExtract::NOperationResult::kUnsupportedMethod ? ErrorCategory::Unsupported : ErrorCategory::Unavailable));
    }
private:
    std::shared_ptr<State> state_; Output* output_; UInt32 index_;
};
Result<Unit> Extract(Archive& Archive, UInt32 Index, const ArchiveHeader& Header, QFile* File, bool Test) {
    Com<Output> Output{new class Output(File, Header.size_, Archive.state_)};
    Com<ExtractCallback> Callback{new ExtractCallback(Archive.state_, Output.value_, Index)};
    const auto Code = Archive.archive_->Extract(&Index, 1, Test ? 1 : 0, Callback.value_);
    if (Code != S_OK || Archive.state_->error_) return Archive.state_->Result(Code);
    if (!Test && Output->written_ != Header.size_) return Failure("7zip-extracted-size");
    return Unit{};
}
Result<std::unique_ptr<Archive>> Open(HMODULE Module, std::string Path, OperationContext Context) {
    if (const auto Stop = Context.Check({"7zip"})) return *Stop;
    const auto FilePath = QString::fromStdString(Path);
    if (!QFileInfo(FilePath).isAbsolute() || FilePath.contains(QChar::Null)) return Failure("7zip-absolute-path", ErrorCategory::InvalidRequest);
    const auto Create = reinterpret_cast<HRESULT(WINAPI*)(const GUID*, const GUID*, void**)>(GetProcAddress(Module, "CreateObject"));
    const auto Formats = reinterpret_cast<HRESULT(WINAPI*)(UInt32*)>(GetProcAddress(Module, "GetNumberOfFormats"));
    const auto Get = reinterpret_cast<HRESULT(WINAPI*)(UInt32, PROPID, PROPVARIANT*)>(GetProcAddress(Module, "GetHandlerProperty2"));
    if (!Create || !Formats || !Get) return Failure("7zip-exports", ErrorCategory::Unsupported);
    UInt32 Count = 0; if (Formats(&Count) != S_OK) return Failure("7zip-formats");
    // Probe signatures at offset zero before allowing SFX scanning. Otherwise a
    // stored RAR/7z member inside a ZIP can be mistaken for the outer archive.
    const UInt64 StartOnly = 0;
    for (unsigned Scan = 0; Scan < 2; ++Scan) for (UInt32 Index = 0; Index < Count; ++Index) {
        Property Name, Id; if (Get(Index, NArchive::NHandlerPropID::kName, &Name) != S_OK || Get(Index, NArchive::NHandlerPropID::kClassID, &Id) != S_OK) return Failure("7zip-format-property");
        if (!QStringList{"7z", "zip", "Rar", "Rar5"}.contains(Name.Text(), Qt::CaseInsensitive)) continue;
        if (Id.vt != VT_BSTR || SysStringByteLen(Id.bstrVal) != sizeof(GUID)) return Failure("7zip-format-guid");
        auto Value = std::make_unique<Archive>(); Value->format_ = Name.Text().toLower(); Value->state_->context_ = Context; Value->state_->archive_ = Path;
        if (!Value->input_->Open(FilePath, FilePath.endsWith(".001"))) return Failure("7zip-input-open", ErrorCategory::NotFound);
        GUID Class{}; memcpy(&Class, Id.bstrVal, sizeof(Class));
        if (Create(&Class, &IID_IInArchive, reinterpret_cast<void**>(&Value->archive_.value_)) != S_OK) continue;
        const auto Code = Value->archive_->Open(Value->input_.value_, Scan ? nullptr : &StartOnly, Value->callback_.value_);
        if (Value->state_->error_) return Value->state_->Result(Code);
        if (Code != S_OK && Value->state_->attempts_) return Failure("7zip-password-or-header-error", ErrorCategory::AccessDenied);
        if (Code == S_FALSE) continue;
        if (Code != S_OK) return Value->state_->Result(Code);
        Property Errors; if (Value->archive_->GetArchiveProperty(kpidErrorFlags, &Errors) != S_OK || Errors.Number()) return Failure("7zip-archive-errors:" + std::to_string(Errors.Number()));
        if (Value->archive_->GetNumberOfItems(&Value->count_) != S_OK) return Failure("7zip-item-count");
        if (Value->count_ > Context.EnumerationLimit()) return Failure("7zip-enumeration-budget", ErrorCategory::BudgetExceeded);
        return Value;
    }
    return Failure("7zip-format-not-recognized", ErrorCategory::Unsupported);
}
class Listing final : public IArchiveListing {
public:
    Listing(std::unique_ptr<Archive> Archive, bool Test) : archive_(std::move(Archive)), test_(Test) {}
    Result<ArchivePage> Next(size_t Count, OperationContext Context) override {
        if (!Count || !Context.maxPageItems_) return Failure("zero-page-size", ErrorCategory::InvalidRequest);
        archive_->state_->context_ = Context; ArchivePage Page;
        if (test_) {
            // Testing one member at a time repeatedly decodes a solid block.
            // Validate metadata/policy first, then test the archive in one pass.
            for (UInt32 Index = 0; Index < archive_->count_; ++Index) {
                if (const auto Stop = Context.Check({"7zip"})) return *Stop;
                auto Header = archive_->Header(Index); if (!Header.HasValue()) return Header.Failure();
            }
            Com<ExtractCallback> Callback{new ExtractCallback(archive_->state_, nullptr, UINT32_MAX)};
            const auto Code = archive_->archive_->Extract(nullptr, UINT32_MAX, 1, Callback.value_);
            if (Code != S_OK || archive_->state_->error_) return archive_->state_->Result(Code);
            test_ = false;
        }
        while (index_ < archive_->count_ && Page.entries_.size() < std::min(Count, Context.maxPageItems_)) {
            if (const auto Stop = Context.Check({"7zip"})) return *Stop;
            auto Header = archive_->Header(index_); if (!Header.HasValue()) return Header.Failure();
            Page.entries_.push_back(std::move(Header.Value())); ++index_;
        }
        Page.end_ = index_ == archive_->count_; return Page;
    }
private:
    std::unique_ptr<Archive> archive_; UInt32 index_ = 0; bool test_;
};
#include "sevenZipUpdate.inl"
class Backend final : public IArchiveBackend {
public:
    explicit Backend(HMODULE Module) : module_(Module) {}
    Result<std::unique_ptr<IArchiveListing>> Enumerate(std::string Path, bool Test, OperationContext Context) override {
        auto Value = Open(module_, std::move(Path), Context); if (!Value.HasValue()) return Value.Failure();
        return std::unique_ptr<IArchiveListing>(new Listing(std::move(Value.Value()), Test));
    }
    Result<Unit> Extract(std::string Path, ArchiveHeader Expected, std::string OutputPath, OperationContext Context) override {
        auto Value = Open(module_, std::move(Path), Context); if (!Value.HasValue()) return Value.Failure(); auto& Archive = *Value.Value();
        std::optional<UInt32> Selected;
        for (UInt32 Index = 0; Index < Archive.count_; ++Index) {
            if (const auto Stop = Context.Check({"7zip"})) return *Stop;
            auto Header = Archive.Header(Index); if (!Header.HasValue()) return Header.Failure();
            if (Header.Value().name_ != Expected.name_) continue;
            if (Header.Value().attributes_ & FILE_ATTRIBUTE_REPARSE_POINT) return Failure("7zip-link-extraction", ErrorCategory::Unsupported);
            if (Selected) return Failure("7zip-ambiguous-entry", ErrorCategory::InvalidRequest);
            if (Header.Value().size_ != Expected.size_ || Header.Value().crc_ != Expected.crc_ || Header.Value().directory_)
                return Failure("7zip-entry-changed", ErrorCategory::StaleRevision);
            Selected = Index;
        }
        if (!Selected) return Failure("7zip-entry-missing", ErrorCategory::NotFound);
        const auto PathTo = QString::fromStdString(OutputPath);
        if (!QFileInfo(PathTo).isAbsolute() || PathTo.contains(QChar::Null)) return Failure("7zip-output-path", ErrorCategory::InvalidRequest);
        QFile File(PathTo); if (!File.open(QIODevice::WriteOnly | QIODevice::Truncate)) return Failure("7zip-output-open");
        auto Cleanup = qScopeGuard([&] { File.close(); File.remove(); });
        auto Done = hc::plugins::Extract(Archive, *Selected, Expected, &File, false); if (!Done.HasValue()) return Done;
        if (!File.flush()) return Failure("7zip-output-flush"); Cleanup.dismiss(); return Unit{};
    }
    Result<Unit> Update(ArchiveUpdate Request, OperationContext Context) override { return UpdateArchive(module_, std::move(Request), Context); }
private:
    HMODULE module_;
};
#undef HC_COM_REFS
}
std::shared_ptr<IArchiveBackend> MakeSevenZipBackend(HMODULE Module) { return std::make_shared<Backend>(Module); }
}
