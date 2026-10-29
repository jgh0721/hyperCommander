#include "sevenZipBackend.hpp"
#include "Common/MyInitGuid.h"
#include "commonLibs/plugins/archiveSelectionDigest.hpp"
#include <thread>
#include "commonLibs/plugins/archiveVolumeNames.hpp"
#include "7zip/Archive/IArchive.h"
#include "7zip/IPassword.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStorageInfo>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QCryptographicHash>
#include <QRegularExpression>
#include <QtEndian>
#include <map>
#include <set>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <msi.h>
#include <msiquery.h>
#include <thread>

namespace hc::plugins {
using namespace storage;
namespace {
Error Failure(std::string Code, ErrorCategory Category = ErrorCategory::Unavailable)
{ return {Category, std::move(Code), {"7zip"}}; }
bool IsZipx(const QString& Path) { return Path.endsWith(".zipx", Qt::CaseInsensitive); }
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
    std::optional<Error> volumeError_;
    QString selectedVolume_;
    bool selectedSeen_ = false, volumeSet_ = false;
    unsigned attempts_ = 0;
    std::optional<storage::Password> password_;
    HRESULT Check(uint64_t Done = 0) noexcept {
        try {
            while (context_.pauseRequested_ && context_.pauseRequested_()) {
                if (context_.Check({"7zip"})) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
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
    bool Open(const QString& Path, bool Split = false, unsigned RequiredPart = 1) {
        auto Name = Path; unsigned Number = 1;
        const auto Width = QFileInfo(Path).suffix().size();
        do {
            if (state_->Check() != S_OK) return false;
            const auto Handle = CreateFileW(reinterpret_cast<const wchar_t*>(QDir::toNativeSeparators(Name).utf16()), GENERIC_READ, FILE_SHARE_READ,
                nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
            if (Handle == INVALID_HANDLE_VALUE) return parts_.size() >= RequiredPart && GetLastError() == ERROR_FILE_NOT_FOUND;
            BY_HANDLE_FILE_INFORMATION Info{};
            if (!GetFileInformationByHandle(Handle, &Info) || (Info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) { CloseHandle(Handle); return false; }
            const auto Size = (uint64_t(Info.nFileSizeHigh) << 32) | Info.nFileSizeLow;
            if (Size > INT64_MAX - size_) { CloseHandle(Handle); return false; }
            parts_.push_back({Handle, size_, Size}); size_ += Size;
            if (!Split) return true;
            Name = Path.left(Path.size() - Width) + QString::number(++Number).rightJustified(Width, '0');
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
    uint64_t PartOffset(unsigned Number) const { return parts_.at(Number - 1).offset_; }
private:
    struct Part { HANDLE handle_; uint64_t offset_, size_; };
    std::vector<Part> parts_;
    uint64_t size_ = 0, position_ = 0;
    std::shared_ptr<State> state_;
};
class OpenCallback final : public IArchiveOpenCallback, public IArchiveOpenVolumeCallback, public ICryptoGetTextPassword {
public:
    explicit OpenCallback(std::shared_ptr<State> State) : state_(std::move(State)) {}
    void SetFormat(const QString& Format, bool Zipx) { zip_ = Format == "zip"; zipx_ = Zipx; }
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
        if (Id == kpidName) {
            auto Name = QFileInfo(QString::fromStdString(state_->archive_)).fileName();
            // The ZIP reader understands .zip/.zNN. Keep the real ZIPX paths
            // outside the SDK and translate only this archive's volume names.
            if (zipx_) Name.chop(1);
            Value->vt = VT_BSTR; Value->bstrVal = SysAllocStringLen(reinterpret_cast<const wchar_t*>(Name.utf16()), UINT(Name.size()));
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetStream(const wchar_t* Name, IInStream** Stream) noexcept override {
        *Stream = nullptr;
        try {
            if (state_->Check() != S_OK) return E_ABORT;
            auto Leaf = QString::fromWCharArray(Name);
            if (Leaf.isEmpty() || Leaf == "." || Leaf == ".." || Leaf.contains('/') || Leaf.contains('\\') || Leaf.contains(':'))
                return state_->Fail(Failure("7zip-volume-path", ErrorCategory::InvalidRequest));
            const auto Archive = QFileInfo(QString::fromStdString(state_->archive_));
            bool RequiredVolume = false;
            if (zipx_) {
                const auto Stem = Archive.completeBaseName();
                if (Leaf.left(Stem.size() + 1).compare(Stem + '.', Qt::CaseInsensitive)) return S_FALSE;
                const auto Extension = Leaf.mid(Stem.size() + 1);
                if (Extension.compare("zip", Qt::CaseInsensitive) == 0) Leaf = Archive.fileName();
                else {
                    static const QRegularExpression Volume("^[zZ]([0-9]{2,})$");
                    const auto Match = Volume.match(Extension);
                    if (!Match.hasMatch()) return S_FALSE;
                    Leaf = Stem + ".zx" + Match.captured(1); RequiredVolume = true;
                }
            }
            if (zip_ && !zipx_) {
                const auto Volume = ParseArchiveVolumeName(Leaf);
                RequiredVolume = Volume.kind_ == ArchiveVolumeKind::Zip
                    && Volume.firstName_.compare(Archive.fileName(), Qt::CaseInsensitive) == 0;
            }
            Com<Input> File{new Input(state_)};
            const auto Path = Archive.dir().filePath(Leaf);
            if (!File->Open(Path)) {
                if (RequiredVolume && !state_->volumeError_) state_->volumeError_ = Failure("7zip-volume-unavailable:" + Leaf.toStdString(),
                    QFileInfo::exists(Path) ? ErrorCategory::Unavailable : ErrorCategory::NotFound);
                return S_FALSE;
            }
            if (Leaf.compare(state_->selectedVolume_, Qt::CaseInsensitive) == 0) state_->selectedSeen_ = true;
            *Stream = File.value_; File.value_ = nullptr; return S_OK;
        } catch (...) { return E_ABORT; }
    }
private:
    std::shared_ptr<State> state_;
    bool zip_ = false, zipx_ = false;
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
        Header.metadataRename_ = (format_ == "7z" || format_ == "zip") && !state_->volumeSet_ && !IsZipx(QString::fromStdString(state_->archive_));
        Header.encrypted_ = Encrypted.Boolean(); Header.attributes_ = uint32_t(Attr.Number()); Header.crc_ = uint32_t(Crc.Number());
        Property Posix; if (!Read(kpidPosixAttrib, Posix)) return Failure("7zip-entry-properties");
        const auto Mode = Posix.Number() ? Posix.Number() : (Attr.Number() & 0x8000) ? Attr.Number() >> 16 : 0;
        if (!Link.Text().isEmpty() || !Hard.Text().isEmpty() || ((Mode & 0170000) && (Mode & 0170000) != (Header.directory_ ? 0040000 : 0100000))) Header.attributes_ |= FILE_ATTRIBUTE_REPARSE_POINT;
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
    Output(QFile* File, uint64_t Limit, std::shared_ptr<State> State, bool Hash = false, std::function<Result<Unit>(std::span<const std::byte>)> Sink = {}) : file_(File), limit_(Limit), state_(std::move(State)), hashEnabled_(Hash), sink_(std::move(Sink)) {}
    HC_COM_REFS;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID Id, void** Value) noexcept override {
        *Value = nullptr; if (Id == IID_IUnknown || Id == IID_ISequentialOutStream) *Value = static_cast<ISequentialOutStream*>(this);
        if (!*Value) return E_NOINTERFACE; AddRef(); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Write(const void* Data, UInt32 Size, UInt32* Done) noexcept override {
        if (Done) *Done = 0; if (state_->Check(written_) != S_OK) return E_ABORT;
        if (Size > limit_ - written_) return state_->Fail(Failure("7zip-output-size", ErrorCategory::BudgetExceeded));
        if (file_ && file_->write(static_cast<const char*>(Data), Size) != Size) return state_->Fail(Failure("7zip-output-write"));
        if (sink_) { auto Written = sink_({static_cast<const std::byte*>(Data),Size}); if (!Written.HasValue()) return state_->Fail(Written.Failure()); }
        if (hashEnabled_) hash_.addData(QByteArrayView(static_cast<const char*>(Data), Size));
        written_ += Size; if (Done) *Done = Size; return state_->Check(written_);
    }
    uint64_t written_ = 0;
    QCryptographicHash hash_{QCryptographicHash::Sha256};
private:
    QFile* file_; uint64_t limit_; std::shared_ptr<State> state_; bool hashEnabled_; std::function<Result<Unit>(std::span<const std::byte>)> sink_;
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
class BulkExtractCallback final : public IArchiveExtractCallback, public ICryptoGetTextPassword {
public:
    BulkExtractCallback(std::shared_ptr<State> State, const std::vector<ArchiveHeader>& Headers, QString Directory)
        : state_(std::move(State)), headers_(Headers), directory_(std::move(Directory)) {}
    ~BulkExtractCallback() { close(); }
    HC_COM_REFS;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID Id, void** Value) noexcept override {
        *Value = nullptr;
        if (Id == IID_IUnknown || Id == IID_IArchiveExtractCallback || Id == IID_IProgress) *Value = static_cast<IArchiveExtractCallback*>(this);
        else if (Id == IID_ICryptoGetTextPassword) *Value = static_cast<ICryptoGetTextPassword*>(this);
        if (!*Value) return E_NOINTERFACE; AddRef(); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetTotal(UInt64) noexcept override { return state_->Check(); }
    HRESULT STDMETHODCALLTYPE SetCompleted(const UInt64* Done) noexcept override { return state_->Check(Done ? *Done : 0); }
    HRESULT STDMETHODCALLTYPE CryptoGetTextPassword(BSTR* Value) noexcept override { return state_->Password(Value); }
    HRESULT STDMETHODCALLTYPE GetStream(UInt32 Index, ISequentialOutStream** Stream, Int32 Mode) noexcept override {
        *Stream = nullptr;
        try {
            close();
            if (Index >= headers_.size()) return state_->Fail(Failure("7zip-batch-index"));
            if (Mode != NArchive::NExtract::NAskMode::kExtract || headers_[Index].directory_) return state_->Check();
            file_ = std::make_unique<QFile>(QDir(directory_).filePath(QString::number(Index)+".bin"));
            if (!file_->open(QIODevice::WriteOnly | QIODevice::NewOnly)) return state_->Fail(Failure("7zip-batch-output-open"));
            index_ = Index; output_ = new Output(file_.get(), headers_[Index].size_, state_);
            output_->AddRef(); *Stream = output_; return state_->Check();
        } catch (...) { return state_->Fail(Failure("7zip-batch-output-exception")); }
    }
    HRESULT STDMETHODCALLTYPE PrepareOperation(Int32) noexcept override { return state_->Check(); }
    HRESULT STDMETHODCALLTYPE SetOperationResult(Int32 Code) noexcept override {
        if (Code) return state_->Fail(Failure("7zip-extract-result:"+std::to_string(Code), Code==NArchive::NExtract::NOperationResult::kWrongPassword
            ? ErrorCategory::AccessDenied : Code==NArchive::NExtract::NOperationResult::kUnsupportedMethod ? ErrorCategory::Unsupported : ErrorCategory::Unavailable));
        try {
            if (output_ && (output_->written_ != headers_[index_].size_ || !file_->flush())) return state_->Fail(Failure("7zip-batch-output-size"));
            return state_->Check();
        } catch (...) { return state_->Fail(Failure("7zip-batch-flush-exception")); }
    }
private:
    void close() { if (output_) { output_->Release(); output_ = nullptr; } file_.reset(); }
    std::shared_ptr<State> state_;
    const std::vector<ArchiveHeader>& headers_;
    QString directory_;
    std::unique_ptr<QFile> file_;
    Output* output_ = nullptr;
    UInt32 index_ = 0;
};
Result<Unit> Extract(Archive& Archive, UInt32 Index, const ArchiveHeader& Header, QFile* File, bool Test) {
    Com<Output> Output{new class Output(File, Header.size_, Archive.state_)};
    Com<ExtractCallback> Callback{new ExtractCallback(Archive.state_, Output.value_, Index)};
    const auto Code = Archive.archive_->Extract(&Index, 1, Test ? 1 : 0, Callback.value_);
    if (Code != S_OK || Archive.state_->error_) return Archive.state_->Result(Code);
    if (!Test && Output->written_ != Header.size_) return Failure("7zip-extracted-size");
    return Unit{};
}
Result<std::unique_ptr<Archive>> Open(HMODULE Module, std::string Path, OperationContext Context, unsigned FirstAttempt = 0) {
    if (const auto Stop = Context.Check({"7zip"})) return *Stop;
    const auto SelectedPath = QString::fromStdString(Path);
    auto FilePath = SelectedPath;
    if (!QFileInfo(FilePath).isAbsolute() || FilePath.contains(QChar::Null)) return Failure("7zip-absolute-path", ErrorCategory::InvalidRequest);
    auto Volume = ParseArchiveVolumeName(QFileInfo(FilePath).fileName());
    if (Volume.kind_ != ArchiveVolumeKind::None) {
        const auto Selected = std::make_shared<State>(); Selected->context_ = Context; Selected->archive_ = Path;
        Com<Input> Probe{new Input(Selected)};
        if (!Probe->Open(FilePath)) return Selected->error_.value_or(Failure("7zip-input-open", ErrorCategory::NotFound));
        const auto Directory = QFileInfo(FilePath).dir();
        FilePath = Directory.filePath(Volume.firstName_);
        if (!Volume.alternateName_.isEmpty()) {
            const auto Alternate = Directory.filePath(Volume.alternateName_);
            const bool HasMain = QFileInfo::exists(FilePath), HasAlternate = QFileInfo::exists(Alternate);
            if (HasMain && HasAlternate) return Failure("7zip-ambiguous-volume-set", ErrorCategory::InvalidRequest);
            if (HasAlternate) { FilePath = Alternate; if (Volume.kind_ == ArchiveVolumeKind::Zip) Volume.kind_ = ArchiveVolumeKind::Rar; }
        }
        Path = FilePath.toStdString();
    }
    // Every candidate is a sibling; native and Qt path separators may differ.
    const bool Redirected = QFileInfo(FilePath).fileName().compare(QFileInfo(SelectedPath).fileName(), Qt::CaseInsensitive) != 0;
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
        if (!QStringList{"7z", "zip", "Rar", "Rar5", "Compound", "Cab"}.contains(Name.Text(), Qt::CaseInsensitive)) continue;
        if (Id.vt != VT_BSTR || SysStringByteLen(Id.bstrVal) != sizeof(GUID)) return Failure("7zip-format-guid");
        auto Value = std::make_unique<Archive>(); Value->format_ = Name.Text().toLower(); Value->state_->context_ = Context; Value->state_->archive_ = Path;
        Value->state_->volumeSet_ = Volume.kind_ != ArchiveVolumeKind::None; Value->state_->attempts_ = FirstAttempt;
        if (Redirected && Volume.kind_ != ArchiveVolumeKind::Numbered) Value->state_->selectedVolume_ = QFileInfo(SelectedPath).fileName();
        Value->callback_->SetFormat(Value->format_, Value->format_ == "zip" && IsZipx(FilePath));
        if (!Value->input_->Open(FilePath, Volume.kind_ == ArchiveVolumeKind::Numbered, Volume.number_))
            return Value->state_->error_.value_or(Failure("7zip-volume-unavailable:" + QFileInfo(FilePath).fileName().toStdString(), ErrorCategory::NotFound));
        GUID Class{}; memcpy(&Class, Id.bstrVal, sizeof(Class));
        if (Create(&Class, &IID_IInArchive, reinterpret_cast<void**>(&Value->archive_.value_)) != S_OK) continue;
        const auto Code = Value->archive_->Open(Value->input_.value_, Scan ? nullptr : &StartOnly, Value->callback_.value_);
        if (Code != S_OK && Value->state_->attempts_ > FirstAttempt && Value->state_->attempts_ < 104
            && (!Value->state_->error_ || Value->state_->error_->category_ == ErrorCategory::AccessDenied)) {
            const auto Attempt = Value->state_->attempts_; Value.reset();
            return Open(Module, SelectedPath.toStdString(), Context, Attempt);
        }
        if (Value->state_->error_) return Value->state_->Result(Code);
        if (Code != S_OK && Value->state_->attempts_ > FirstAttempt) return Failure("7zip-password-or-header-error", ErrorCategory::AccessDenied);
        if (Code == S_FALSE) continue;
        if (Code != S_OK) return Value->state_->volumeError_.value_or(Value->state_->Result(Code));
        // The ZIP handler can return S_OK and a central-directory listing even
        // when a data volume is missing. Do not expose such a partial ZIPX set.
        if (Value->state_->volumeError_) return *Value->state_->volumeError_;
        if (!Value->state_->selectedVolume_.isEmpty() && !Value->state_->selectedSeen_)
            return Failure("7zip-volume-not-in-set:" + Value->state_->selectedVolume_.toStdString(), ErrorCategory::InvalidRequest);
        Property Errors; if (Value->archive_->GetArchiveProperty(kpidErrorFlags, &Errors) != S_OK || Errors.Number())
            return Value->state_->volumeError_.value_or(Failure("7zip-archive-errors:" + std::to_string(Errors.Number())));
        if (Value->archive_->GetNumberOfItems(&Value->count_) != S_OK) return Failure("7zip-item-count");
        Property IsVolume; if (Value->archive_->GetArchiveProperty(kpidIsVolume, &IsVolume) == S_OK && IsVolume.Boolean()) Value->state_->volumeSet_ = true;
        if (Value->state_->volumeSet_) {
            Property Detail;
            if (Value->archive_->GetArchiveProperty(kpidError, &Detail) == S_OK && !Detail.Text().isEmpty())
                return Failure("7zip-archive-error:" + Detail.Text().toStdString());
        }
        if (Volume.kind_ == ArchiveVolumeKind::Numbered && Volume.number_ > 1) {
            Property Offset, Size;
            if (Value->archive_->GetArchiveProperty(kpidOffset, &Offset) != S_OK || Value->archive_->GetArchiveProperty(kpidPhySize, &Size) != S_OK
                || Offset.Number() > UINT64_MAX - Size.Number() || Offset.Number() + Size.Number() <= Value->input_->PartOffset(Volume.number_))
                return Failure("7zip-volume-not-in-set", ErrorCategory::InvalidRequest);
        }
        if (Value->count_ > Context.EnumerationLimit()) return Failure("7zip-enumeration-budget", ErrorCategory::BudgetExceeded);
        return Value;
    }
    return Failure("7zip-format-not-recognized", ErrorCategory::Unsupported);
}
bool RetryPassword(const Error& Error, unsigned Attempt, bool Encrypted) {
    if (!Encrypted || !Attempt || Attempt >= 104) return false;
    return Error.code_ == "7zip-extract-result:" + std::to_string(NArchive::NExtract::NOperationResult::kWrongPassword)
        || Error.code_ == "7zip-extract-result:" + std::to_string(NArchive::NExtract::NOperationResult::kCRCError)
        || Error.code_ == "7zip-extract-result:" + std::to_string(NArchive::NExtract::NOperationResult::kDataError);
}
#include "sevenZipStream.inl"
class Listing final : public IArchiveListing {
public:
    Listing(HMODULE Module, std::unique_ptr<Archive> Archive, bool Test) : module_(Module), archive_(std::move(Archive)), test_(Test) {}
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
            bool Encrypted = false;
            for (UInt32 Index = 0; Index < archive_->count_; ++Index) { auto H = archive_->Header(Index); if (!H.HasValue()) return H.Failure(); Encrypted |= H.Value().encrypted_; }
            for (;;) {
                HRESULT Code;
                { Com<ExtractCallback> Callback{new ExtractCallback(archive_->state_, nullptr, UINT32_MAX)};
                  Code = archive_->archive_->Extract(nullptr, UINT32_MAX, 1, Callback.value_); }
                if (Code == S_OK && !archive_->state_->error_) break;
                const auto Error = archive_->state_->Result(Code);
                if (!RetryPassword(Error, archive_->state_->attempts_, Encrypted)) return Error;
                const auto Attempt = archive_->state_->attempts_; const auto Path = archive_->state_->archive_; archive_.reset();
                auto Reopened = Open(module_, Path, Context, Attempt); if (!Reopened.HasValue()) return Reopened.Failure(); archive_ = std::move(Reopened.Value());
            }
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
    HMODULE module_; std::unique_ptr<Archive> archive_; UInt32 index_ = 0; bool test_;
};
#include "sevenZipUpdate.inl"
class Backend final : public IArchiveBackend {
public:
    explicit Backend(HMODULE Module) : module_(Module) {}
    Result<std::unique_ptr<IArchiveListing>> Enumerate(std::string Path, bool Test, OperationContext Context) override {
        auto Value = Open(module_, std::move(Path), Context); if (!Value.HasValue()) return Value.Failure();
        return std::unique_ptr<IArchiveListing>(new Listing(module_, std::move(Value.Value()), Test));
    }
    Result<Unit> Extract(std::string Path, ArchiveHeader Expected, std::string OutputPath, OperationContext Context) override {
        return extractOne(std::move(Path), std::move(Expected), std::move(OutputPath), std::move(Context), 0);
    }
    Result<Unit> extractOne(std::string Path, ArchiveHeader Expected, std::string OutputPath, OperationContext Context, unsigned FirstAttempt) {
        auto Value = Open(module_, Path, Context, FirstAttempt); if (!Value.HasValue()) return Value.Failure(); auto& Archive = *Value.Value();
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
        auto Done = hc::plugins::Extract(Archive, *Selected, Expected, &File, false);
        if (!Done.HasValue()) {
            if (!RetryPassword(Done.Failure(), Archive.state_->attempts_, Expected.encrypted_)) return Done;
            const auto Attempt = Archive.state_->attempts_; File.close();
            if (!File.remove()) return Done;
            Cleanup.dismiss(); Value.Value().reset();
            return extractOne(std::move(Path), std::move(Expected), std::move(OutputPath), std::move(Context), Attempt);
        }
        if (!File.flush()) return Failure("7zip-output-flush"); Cleanup.dismiss(); return Unit{};
    }
    Result<Unit> ExtractAll(std::string Path, std::string Directory, OperationContext Context) override {
        return extractSelection(std::move(Path), std::move(Directory), {}, std::move(Context));
    }
    Result<Unit> ExtractSelected(std::string Path, std::string Directory, std::vector<std::uint32_t> Indices, OperationContext Context) override {
        if (Indices.empty() || Indices.size() > Context.EnumerationLimit() || !std::is_sorted(Indices.begin(), Indices.end())
            || std::adjacent_find(Indices.begin(), Indices.end()) != Indices.end()) return Failure("7zip-selected-indices", ErrorCategory::InvalidRequest);
        return extractSelection(std::move(Path), std::move(Directory), std::move(Indices), std::move(Context));
    }
    Result<Unit> extractSelection(std::string Path, std::string Directory, std::optional<std::vector<std::uint32_t>> Indices, OperationContext Context, unsigned FirstAttempt = 0) {
        if (Context.workload_.IsAutomatic()) return Failure("archive-batch-extract-unsupported", ErrorCategory::Unsupported);
        const auto To = QString::fromStdString(Directory); const QFileInfo Info(To);
        if (!Info.isAbsolute() || !Info.isDir() || Info.isSymLink() || !QDir(To).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System).isEmpty())
            return Failure("7zip-batch-private-directory", ErrorCategory::InvalidRequest);
        const auto NativeDirectory=QDir::toNativeSeparators(To);
        const auto DirectoryPin=CreateFileW(reinterpret_cast<LPCWSTR>(NativeDirectory.utf16()),FILE_READ_ATTRIBUTES|FILE_LIST_DIRECTORY,
            FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
        if(DirectoryPin==INVALID_HANDLE_VALUE)return Failure("7zip-batch-directory-pin");
        auto CloseDirectory=qScopeGuard([&]{CloseHandle(DirectoryPin);});
        BY_HANDLE_FILE_INFORMATION DirectoryInfo{};
        if(!GetFileInformationByHandle(DirectoryPin,&DirectoryInfo) || !(DirectoryInfo.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)
            || (DirectoryInfo.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT))return Failure("7zip-batch-private-directory",ErrorCategory::InvalidRequest);
        auto Value = Open(module_, Path, Context, FirstAttempt); if (!Value.HasValue()) return Value.Failure(); auto& Archive = *Value.Value();
        if (Indices && Indices->back() >= Archive.count_) return Failure("7zip-selected-index-range", ErrorCategory::InvalidRequest);
        std::vector<ArchiveHeader> Headers; Headers.reserve(Archive.count_); std::uint64_t Required=0;
        ArchiveSelectionDigest Digest;
        for (UInt32 Index=0; Index<Archive.count_; ++Index) {
            if (const auto Stop=Context.Check({"7zip"})) return *Stop;
            auto Header=Archive.Header(Index); if (!Header.HasValue()) return Header.Failure();
            if (Header.Value().attributes_ & FILE_ATTRIBUTE_REPARSE_POINT) return Failure("7zip-link-extraction", ErrorCategory::Unsupported);
            if (!Header.Value().directory_ && (!Indices || std::binary_search(Indices->begin(), Indices->end(), Index))) {
                if (Indices) Digest.Add(Index, Header.Value());
                if(Header.Value().size_>UINT64_MAX-Required)return Failure("7zip-batch-size-budget",ErrorCategory::BudgetExceeded);
                Required+=Header.Value().size_;
            }
            Headers.push_back(std::move(Header.Value()));
        }
        const QStorageInfo Volume(To);
        if(Volume.isValid() && Volume.bytesAvailable()>=0 && std::uint64_t(Volume.bytesAvailable())<Required)
            return Failure("7zip-batch-staging-space",ErrorCategory::BudgetExceeded);
        // One SDK call preserves solid-block decoder state across all members.
        HRESULT Code;
        { Com<BulkExtractCallback> Callback{new BulkExtractCallback(Archive.state_, Headers, To)};
          Code=Archive.archive_->Extract(Indices ? Indices->data() : nullptr, Indices ? UInt32(Indices->size()) : UINT32_MAX, 0, Callback.value_); }
        if (Code!=S_OK || Archive.state_->error_) {
            const auto Error = Archive.state_->Result(Code);
            const bool Encrypted = std::any_of(Headers.begin(), Headers.end(), [](const ArchiveHeader& H) { return H.encrypted_; });
            if (!RetryPassword(Error, Archive.state_->attempts_, Encrypted)) return Error;
            const auto Attempt = Archive.state_->attempts_;
            for (size_t Index = 0; Index < Headers.size(); ++Index) {
                const auto File = QDir(To).filePath(QString::number(Index) + ".bin");
                if (QFileInfo::exists(File) && !QFile::remove(File)) return Error;
            }
            std::vector<ArchiveHeader>().swap(Headers);
            Value.Value().reset(); CloseDirectory.dismiss(); CloseHandle(DirectoryPin);
            return extractSelection(std::move(Path), std::move(Directory), std::move(Indices), std::move(Context), Attempt);
        }
        return Indices ? Digest.Save(To) : Result<Unit>(Unit{});
    }
    Result<std::unique_ptr<IArchiveReadSession>> OpenReadSession(std::string Path, std::vector<std::uint32_t> Ordinals, OperationContext Context) override {
        if (Ordinals.empty() || Ordinals.size()>Context.EnumerationLimit() || !std::is_sorted(Ordinals.begin(),Ordinals.end())
            || std::adjacent_find(Ordinals.begin(),Ordinals.end())!=Ordinals.end()) return Failure("7zip-stream-indices",ErrorCategory::InvalidRequest);
        return std::unique_ptr<IArchiveReadSession>(new StreamingRead(module_,std::move(Path),std::move(Ordinals),std::move(Context)));
    }
    Result<Unit> Update(ArchiveUpdate Request, OperationContext Context) override {
        if (!Context.passwordPrompt_ || Request.createNew_ || QFileInfo(QString::fromStdString(Request.archive_)).size() <= 0)
            return UpdateArchive(module_, std::move(Request), Context);
        unsigned Attempt = 0; const auto Prompt = Context.passwordPrompt_;
        Context.passwordPrompt_ = [&Attempt, Prompt](const std::string& Archive, unsigned, const OperationContext& Operation) {
            return Prompt(Archive, ++Attempt, Operation);
        };
        for (;;) {
            auto Done = UpdateArchive(module_, Request, Context);
            if (Done.HasValue() || !RetryPassword(Done.Failure(), Attempt, true)) return Done;
        }
    }
private:
    HMODULE module_;
};
#include "sevenZipMsi.inl"
#undef HC_COM_REFS
}
std::shared_ptr<IArchiveBackend> MakeSevenZipBackend(HMODULE Module) { return std::make_shared<msiBrowse::Adapter>(Module); }
}
