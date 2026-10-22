#include "nativeWritableStorage.hpp"
#include "operationMetrics.hpp"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <winioctl.h>
#include "nativeStorageRevision.hpp"
#include <Aclapi.h>
#include <QCryptographicHash>
#include <QScopeGuard>
#include <objbase.h>
#include <ShlObj.h>
#include <wrl/client.h>
#include <cstring>
#include <filesystem>
#include <limits>
#include <stdexcept>

#pragma comment(lib, "ole32")
#pragma comment(lib, "shell32")
#pragma comment(lib, "uuid")
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
        };
        Error Failure(DWORD Code, const ProviderId& Id, std::string NativePath = {})
        {
            Error Value{ErrorCategory::Unavailable, "win32:" + std::to_string(Code), Id};
            Value.nativePath_ = std::move(NativePath);
            if (Code == ERROR_FILE_NOT_FOUND || Code == ERROR_PATH_NOT_FOUND) Value.category_ = ErrorCategory::NotFound;
            if (Code == ERROR_ACCESS_DENIED || Code == ERROR_PRIVILEGE_NOT_HELD || Code == ERROR_LOGON_FAILURE)
            { Value.category_ = ErrorCategory::AccessDenied; Value.userAction_ = "check-permission"; }
            Value.retryable_ = Code == ERROR_SHARING_VIOLATION || Code == ERROR_LOCK_VIOLATION
                || Code == ERROR_NETWORK_UNREACHABLE || Code == ERROR_BAD_NETPATH || Code == ERROR_NETNAME_DELETED
                || Code == ERROR_SEM_TIMEOUT || Code == ERROR_NETWORK_BUSY;
            if (Code == ERROR_DISK_FULL || Code == ERROR_HANDLE_DISK_FULL) Value.userAction_ = "free-space";
            if (Code == ERROR_FILE_EXISTS || Code == ERROR_ALREADY_EXISTS)
            { Value.category_ = ErrorCategory::StaleRevision; Value.code_ = "destination-exists"; Value.userAction_ = "resolve-conflict"; }
            return Value;
        }
        std::string Utf8(const std::wstring& Text)
        {
            const int Count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, Text.data(), static_cast<int>(Text.size()), nullptr, 0, nullptr, nullptr);
            if (!Count && !Text.empty()) throw std::runtime_error("utf8-conversion");
            std::string Value(Count, '\0');
            WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, Text.data(), static_cast<int>(Text.size()), Value.data(), Count, nullptr, nullptr);
            return Value;
        }
        bool InvalidComponent(const std::wstring& Name, bool CheckReserved = true)
        {
            if (Name.empty() || Name.size() > 255 || Name == L"." || Name == L".." || Name.back() == L'.' || Name.back() == L' ') return true;
            if (Name.find_first_of(L"<>:\"|?*") != std::wstring::npos) return true;
            for (const auto Character : Name) if (Character < 32) return true;
            if (!CheckReserved) return false;
            auto Stem = Name.substr(0, Name.find(L'.'));
            for (auto& Character : Stem) if (Character >= L'a' && Character <= L'z') Character -= L'a' - L'A';
            if (Stem == L"CON" || Stem == L"PRN" || Stem == L"AUX" || Stem == L"NUL" || Stem == L"CONIN$" || Stem == L"CONOUT$") return true;
            return Stem.size() == 4 && (Stem.starts_with(L"COM") || Stem.starts_with(L"LPT"))
                && ((Stem.back() >= L'1' && Stem.back() <= L'9') || Stem.back() == L'\u00b9' || Stem.back() == L'\u00b2' || Stem.back() == L'\u00b3');
        }
        struct NativePath
        {
            std::wstring path_;
            std::wstring root_;
        };
        Result<NativePath> Normalize(const Locator& At, LocatorKind Kind, const ProviderId& Id)
        {
            if (At.kind_ != Kind || !At.nesting_.empty() || At.root_.empty() || At.root_.size() > 131072 || At.root_.find('\0') != std::string::npos)
                return Error{ErrorCategory::InvalidRequest, "native-locator", Id};
            const int Count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, At.root_.data(), static_cast<int>(At.root_.size()), nullptr, 0);
            if (Count <= 0 || Count > 32700) return Error{ErrorCategory::InvalidRequest, "invalid-path-encoding", Id};
            std::wstring Path(Count, L'\0');
            MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, At.root_.data(), static_cast<int>(At.root_.size()), Path.data(), Count);
            std::replace(Path.begin(), Path.end(), L'/', L'\\');
            if (Path.starts_with(L"\\\\?\\UNC\\")) Path = L"\\\\" + Path.substr(8);
            else if (Path.starts_with(L"\\\\?\\")) Path.erase(0, 4);
            std::size_t RootEnd = 0;
            const bool Unc = Path.starts_with(L"\\\\");
            if (Unc)
            {
                if (Kind != LocatorKind::Smb) return Error{ErrorCategory::InvalidRequest, "provider-path-mismatch", Id};
                const auto ServerEnd = Path.find(L'\\', 2);
                if (ServerEnd == std::wstring::npos || InvalidComponent(Path.substr(2, ServerEnd - 2), false)) return Error{ErrorCategory::InvalidRequest, "invalid-share", Id};
                const auto ShareEnd = Path.find(L'\\', ServerEnd + 1);
                RootEnd = ShareEnd == std::wstring::npos ? Path.size() : ShareEnd;
                if (InvalidComponent(Path.substr(ServerEnd + 1, RootEnd - ServerEnd - 1), false)) return Error{ErrorCategory::InvalidRequest, "invalid-share", Id};
            }
            else
            {
                if (Kind != LocatorKind::Local || Path.size() < 3 || Path[1] != L':' || Path[2] != L'\\'
                    || !((Path[0] >= L'A' && Path[0] <= L'Z') || (Path[0] >= L'a' && Path[0] <= L'z')))
                    return Error{ErrorCategory::InvalidRequest, "absolute-path-required", Id};
                RootEnd = 3;
            }
            while (Path.size() > RootEnd && Path.back() == L'\\') Path.pop_back();
            for (auto Offset = RootEnd + (Unc && Path.size() > RootEnd ? 1 : 0); Offset < Path.size();)
            {
                const auto End = Path.find(L'\\', Offset);
                if (InvalidComponent(Path.substr(Offset, End == std::wstring::npos ? End : End - Offset))) return Error{ErrorCategory::InvalidRequest, "invalid-file-name", Id};
                if (End == std::wstring::npos) break;
                Offset = End + 1;
            }
            const auto Prefix = Unc ? std::wstring(L"\\\\?\\UNC\\") : std::wstring(L"\\\\?\\");
            const auto Trim = Unc ? 2u : 0u;
            return NativePath{Prefix + Path.substr(Trim), Prefix + Path.substr(Trim, RootEnd - Trim)};
        }
        Result<std::vector<Handle>> PinParents(const NativePath& Path, const ProviderId& Id)
        {
            operations::OperationMetric Timing("native.parent-pin", 0, true);
            std::vector<Handle> Parents;
            const auto Parent = std::filesystem::path(Path.path_).parent_path().wstring();
            if (Path.path_ == Path.root_) return Error{ErrorCategory::InvalidRequest, "root-mutation-denied", Id};
            for (auto End = Path.root_.size();;)
            {
                const auto Part = Path.path_.substr(0, End);
                // Prevent parent rename and reparse-point edits while an output
                // is being created. Child operations do not mutate this handle.
                Handle Directory(CreateFileW(Part.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ, nullptr,
                    OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
                if (Directory.value_ == INVALID_HANDLE_VALUE) return Failure(GetLastError(), Id, Utf8(Part));
                BY_HANDLE_FILE_INFORMATION Info{};
                if (!GetFileInformationByHandle(Directory.value_, &Info)) return Failure(GetLastError(), Id);
                if (!(Info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (Info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
                    return Error{ErrorCategory::Unsupported, "reparse-parent-denied", Id};
                Parents.push_back(std::move(Directory));
                if (End >= Parent.size()) break;
                End = Path.path_.find(L'\\', End + (Path.path_[End - 1] == L'\\' ? 0 : 1));
                if (End == std::wstring::npos || End > Parent.size()) End = Parent.size();
            }
            return Parents;
        }
        std::string Identity(const BY_HANDLE_FILE_INFORMATION& Info)
        {
            // NTFS name tunneling can replace creation time on publication.
            // The volume/file ID remains stable across both staging renames.
            return std::to_string(Info.dwVolumeSerialNumber) + ":" + std::to_string(Info.nFileIndexHigh) + ":"
                + std::to_string(Info.nFileIndexLow);
        }
        Result<Unit> CheckEntry(HANDLE File, const EntrySnapshot& Expected, const ProviderId& Id)
        {
            BY_HANDLE_FILE_INFORMATION Info{};
            if (!GetFileInformationByHandle(File, &Info)) return Failure(GetLastError(), Id);
            if (Info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) return Error{ErrorCategory::Unsupported, "reparse-entry-denied", Id};
            if (Expected.id_.providerId_ != Id || !Expected.id_.revision_ || !native::MatchesRevision(*Expected.id_.revision_, Info)
                || Expected.directory_ != bool(Info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                return Error{ErrorCategory::StaleRevision, "revision-changed", Id, Expected.id_, false, "refresh"};
            return Unit{};
        }
        std::wstring TemporaryName(const std::wstring& Parent, const wchar_t* Kind)
        {
            GUID Id{};
            if (FAILED(CoCreateGuid(&Id))) throw std::runtime_error("temporary-name");
            wchar_t Text[40]{};
            StringFromGUID2(Id, Text, 40);
            return Parent + L"\\.hc-" + Kind + L"-" + Text;
        }
        Result<Unit> RenameHandle(HANDLE File, const std::wstring& To, const ProviderId& Id)
        {
            const auto NameBytes = To.size() * sizeof(wchar_t);
            std::vector<std::byte> Storage(sizeof(FILE_RENAME_INFO) + NameBytes);
            auto* Info = reinterpret_cast<FILE_RENAME_INFO*>(Storage.data());
            Info->ReplaceIfExists = FALSE;
            Info->RootDirectory = nullptr;
            Info->FileNameLength = static_cast<DWORD>(NameBytes);
            std::memcpy(Info->FileName, To.c_str(), NameBytes);
            if (!SetFileInformationByHandle(File, FileRenameInfo, Info, static_cast<DWORD>(Storage.size()))) return Failure(GetLastError(), Id);
            return Unit{};
        }
        Result<Unit> DeleteHandle(HANDLE File, const ProviderId& Id)
        {
            FILE_DISPOSITION_INFO Info{TRUE};
            if (!SetFileInformationByHandle(File, FileDispositionInfo, &Info, sizeof(Info))) return Failure(GetLastError(), Id);
            return Unit{};
        }
        class NativeWriteSession final : public IWriteSession
        {
        public:
            NativeWriteSession(ProviderId Id, Locator Destination, NativePath Path, DestinationExpectation Expected,
                std::vector<Handle> Parents, Handle File, std::wstring StagePath)
                : id_(std::move(Id)), destination_(std::move(Destination)), path_(std::move(Path)), expected_(std::move(Expected)),
                  parents_(std::move(Parents)), file_(std::move(File)), stagePath_(std::move(StagePath)) {}
            ~NativeWriteSession() override { Abort(); if (streamSearch_ != INVALID_HANDLE_VALUE) FindClose(streamSearch_); }
            void EnableVerification(bool Enabled)
            {
                if (Enabled) sourceHash_ = std::make_unique<QCryptographicHash>(QCryptographicHash::Sha256);
            }
            Result<Unit> Initialize(OperationContext Context)
            {
                expectedBytes_ = Context.expectedWriteBytes_;
                checkpoint_ = std::move(Context.writeCheckpoint_);
                record_.transaction_ = Utf8(std::filesystem::path(stagePath_).filename().wstring());
                record_.destination_ = destination_; record_.stage_ = {destination_.kind_, Utf8(stagePath_)};
                record_.backup_ = {destination_.kind_, Utf8(TemporaryName(std::filesystem::path(path_.path_).parent_path().wstring(), L"backup"))};
                BY_HANDLE_FILE_INFORMATION Info{};
                if (!GetFileInformationByHandle(file_.value_, &Info)) return Failure(GetLastError(), id_);
                const auto Time = [](FILETIME Value) { return LONGLONG((uint64_t(Value.dwHighDateTime) << 32) | Value.dwLowDateTime); };
                initialTimes_.CreationTime.QuadPart = Time(Info.ftCreationTime);
                initialTimes_.LastAccessTime.QuadPart = Time(Info.ftLastAccessTime);
                initialTimes_.LastWriteTime.QuadPart = Time(Info.ftLastWriteTime);
                record_.stageIdentity_ = Identity(Info);
                if (Context.expectedWriteBytes_) {
                    if (*Context.expectedWriteBytes_ > uint64_t(INT64_MAX)) return Error{ErrorCategory::Unsupported, "native-file-size-range", id_};
                    FILE_ALLOCATION_INFO Allocation{}; Allocation.AllocationSize.QuadPart = LONGLONG(*Context.expectedWriteBytes_);
                    if (!Context.producingContent_ && !SetFileInformationByHandle(file_.value_, FileAllocationInfo, &Allocation, sizeof(Allocation))) return Failure(GetLastError(), id_);
                }
                return checkpoint(WritePhase::Writing);
            }
            Result<Unit> Append(std::span<const std::byte> Bytes, OperationContext Context) override
            {
                operations::OperationMetric Timing("native.append", 0, true);
                if (const auto Error = Context.Check(id_)) return *Error;
                if (closed_ || failed_) return Error{ErrorCategory::InvalidRequest, "write-session-closed", id_};
                if (Bytes.empty() || Bytes.size() > (std::min)(Context.maxReadBytes_,
                    std::clamp<std::size_t>(Context.nativeCopyBufferBytes_, 1, 8 * 1024 * 1024)))
                    return Error{ErrorCategory::InvalidRequest, "write-chunk-budget", id_};
                DWORD Written = 0;
                if (!WriteFile(file_.value_, Bytes.data(), static_cast<DWORD>(Bytes.size()), &Written, nullptr))
                { failed_ = true; return Failure(GetLastError(), id_); }
                if (Written != Bytes.size()) { failed_ = true; return Error{ErrorCategory::Unavailable, "short-write", id_}; }
                Timing.SetBytes(Written);
                return Unit{};
            }
            bool CanProduceContent(OperationContext Context) const override
            {
                // Path-based producers cannot journal their creation atomically.
                return destination_.kind_ == LocatorKind::Local && expectedBytes_.has_value()
                    && !sourceHash_ && !checkpoint_ && !Context.writeCheckpoint_;
            }
            Result<Unit> ProduceContent(ContentProducer Producer, OperationContext Context) override
            {
                if (!CanProduceContent(Context) || (!Producer.produce_ && !Producer.take_) || closed_ || failed_ || productionStarted_)
                    return Error{ErrorCategory::InvalidRequest, "produced-content-state", id_};
                if (const auto Stop = Context.Check(id_)) return *Stop;
                productionStarted_ = true;
                std::wstring Output;
                std::shared_ptr<void> Lease;
                if (Producer.take_) {
                    Result<ProducedContent> Taken = Error{ErrorCategory::Unavailable, "content-producer-exception", id_};
                    try { operations::OperationMetric Timing("payload.produce", *expectedBytes_);
                        Taken = Producer.take_(Utf8(std::filesystem::path(stagePath_).parent_path().wstring()), Context); }
                    catch (...) { failed_ = true; return Taken.Failure(); }
                    if (!Taken.HasValue()) { failed_ = true; return Taken.Failure(); }
                    if (Taken.Value().path_.empty() || !Taken.Value().lifetime_) { failed_ = true; return Error{ErrorCategory::InvalidRequest, "produced-content-lease", id_}; }
                    Output = std::filesystem::u8path(Taken.Value().path_).wstring(); Lease = std::move(Taken.Value().lifetime_);
                } else {
                    productionDirectory_ = TemporaryName(std::filesystem::path(stagePath_).parent_path().wstring(), L"produce");
                    if (!CreateDirectoryW(productionDirectory_.c_str(), nullptr)) { productionDirectory_.clear(); return Failure(GetLastError(), id_); }
                    productionPin_ = Handle(CreateFileW(productionDirectory_.c_str(), FILE_READ_ATTRIBUTES | FILE_LIST_DIRECTORY,
                        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
                    BY_HANDLE_FILE_INFORMATION DirectoryInfo{};
                    if (productionPin_.value_ == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(productionPin_.value_, &DirectoryInfo)
                        || !(DirectoryInfo.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (DirectoryInfo.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
                        return Error{ErrorCategory::Unavailable, "produced-directory-pin", id_};
                    Output = productionDirectory_ + L"\\payload";
                    Result<Unit> Produced = Error{ErrorCategory::Unavailable, "content-producer-exception", id_};
                    try { operations::OperationMetric Timing("payload.produce", *expectedBytes_); Produced = Producer.produce_(Utf8(Output), Context); }
                    catch (...) { failed_ = true; return Produced; }
                    if (!Produced.HasValue()) { failed_ = true; return Produced; }
                }
                if (const auto Stop = Context.Check(id_)) { failed_ = true; return *Stop; }
                Handle File(CreateFileW(Output.c_str(), GENERIC_WRITE | FILE_READ_ATTRIBUTES | DELETE | WRITE_DAC,
                    FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
                BY_HANDLE_FILE_INFORMATION Before{}; bool Reopened = false;
                if (File.value_ == INVALID_HANDLE_VALUE && GetLastError() == ERROR_ACCESS_DENIED) {
                    // Read-only output needs a separate attribute handle. Normal
                    // output is validated while holding its final exclusive handle.
                    Handle Probe(CreateFileW(Output.c_str(), FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
                    if (Probe.value_ == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(Probe.value_, &Before)
                        || (Before.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) || Before.nNumberOfLinks != 1)
                    { failed_ = true; return Error{ErrorCategory::StaleRevision, "produced-content-type", id_}; }
                    FILE_BASIC_INFO Basic{}; Basic.FileAttributes = FILE_ATTRIBUTE_NORMAL;
                    if (!SetFileInformationByHandle(Probe.value_, FileBasicInfo, &Basic, sizeof(Basic))) { failed_ = true; return Failure(GetLastError(), id_); }
                    Probe.Close(); Reopened = true;
                    File = Handle(CreateFileW(Output.c_str(), GENERIC_WRITE | FILE_READ_ATTRIBUTES | DELETE | WRITE_DAC,
                        FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
                }
                BY_HANDLE_FILE_INFORMATION Info{};
                if (File.value_ == INVALID_HANDLE_VALUE) { failed_ = true; return Failure(GetLastError(), id_); }
                if (!GetFileInformationByHandle(File.value_, &Info)
                    || (Info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))
                    || Info.nNumberOfLinks != 1 || (Reopened && Identity(Info) != Identity(Before))
                    || ((uint64_t(Info.nFileSizeHigh) << 32) | Info.nFileSizeLow) != *expectedBytes_)
                { failed_ = true; return Error{ErrorCategory::StaleRevision, "produced-content-size-or-type", id_}; }
                auto Times = initialTimes_; Times.FileAttributes = FILE_ATTRIBUTE_NORMAL;
                if (!SetFileInformationByHandle(File.value_, FileBasicInfo, &Times, sizeof(Times))) { failed_ = true; return Failure(GetLastError(), id_); }
                PACL Dacl = nullptr; PSECURITY_DESCRIPTOR Descriptor = nullptr;
                auto Security = GetSecurityInfo(file_.value_, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &Dacl, nullptr, &Descriptor);
                if (Security != ERROR_SUCCESS) { failed_ = true; return Failure(Security, id_); }
                const auto FreeDescriptor = qScopeGuard([&] { LocalFree(Descriptor); });
                const auto Removed = DeleteHandle(file_.value_, id_);
                if (!Removed.HasValue()) { failed_ = true; return Removed; }
                file_.Close();
                const auto Renamed = RenameHandle(File.value_, stagePath_, id_);
                if (!Renamed.HasValue()) { failed_ = true; return Renamed; }
                file_ = std::move(File); record_.stageIdentity_ = Identity(Info);
                // Recompute inheritance under the actual destination parent,
                // not the producer's extra directory (NO_PROPAGATE differs).
                SECURITY_DESCRIPTOR_CONTROL Control{}; DWORD Revision = 0;
                Security = GetSecurityDescriptorControl(Descriptor, &Control, &Revision)
                    ? SetSecurityInfo(file_.value_, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION
                        | (Control & SE_DACL_PROTECTED ? PROTECTED_DACL_SECURITY_INFORMATION : UNPROTECTED_DACL_SECURITY_INFORMATION),
                        nullptr, nullptr, Dacl, nullptr) : GetLastError();
                if (Security != ERROR_SUCCESS) { failed_ = true; return Failure(Security, id_); }
                return Unit{};
            }
            Result<Unit> SetMetadata(FileMetadata Metadata, OperationContext Context) override
            {
                if (const auto Error = Context.Check(id_)) return *Error;
                if (closed_ || failed_) return Error{ErrorCategory::InvalidRequest, "write-session-closed", id_};
                if (Metadata.modifiedUtcMs_ && (*Metadata.modifiedUtcMs_ < -11644473600000LL || *Metadata.modifiedUtcMs_ > 910692730085477LL))
                    return Error{ErrorCategory::InvalidRequest, "file-time-range", id_};
                metadata_ = std::move(Metadata);
                return Unit{};
            }
            Result<Unit> ConfigureCopy(EntrySnapshot Source, CopyOptions Options, OperationContext Context) override
            {
                if (const auto Error = Context.Check(id_)) return *Error;
                options_ = Options;
                if (!Options.acl_ && !Options.streams_) return Unit{};
                if (Source.directory_) return Error{ErrorCategory::Unsupported, "copy-extras-files-only", id_};
                auto Path = Normalize(Source.locator_, Source.locator_.kind_, Source.id_.providerId_);
                if (!Path.HasValue()) return Path.Failure();
                auto Parents = PinParents(Path.Value(), Source.id_.providerId_);
                if (!Parents.HasValue()) return Parents.Failure();
                sourceParents_ = std::move(Parents.Value()); sourcePath_ = Path.Value().path_;
                sourceFile_ = Handle(CreateFileW(sourcePath_.c_str(), GENERIC_READ | (Options.acl_ ? READ_CONTROL : 0), FILE_SHARE_READ, nullptr,
                    OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
                if (sourceFile_.value_ == INVALID_HANDLE_VALUE) return extraFailure(GetLastError(), "copy-source:");
                auto Valid = CheckEntry(sourceFile_.value_, Source, Source.id_.providerId_);
                if (!Valid.HasValue()) return Valid.Failure();
                source_ = std::move(Source); return Unit{};
            }
            Result<Unit> RecordSourceBytes(std::span<const std::byte> Bytes, OperationContext Context) override
            {
                if (const auto Error = Context.Check(id_)) return *Error;
                if (!sourceHash_ || verificationStarted_) return Error{ErrorCategory::InvalidRequest, "copy-hash-state", id_};
                sourceHash_->addData(QByteArrayView(reinterpret_cast<const char*>(Bytes.data()), qsizetype(Bytes.size())));
                return Unit{};
            }
            Result<CopyExtraProgress> AdvanceCopyExtras(std::uint64_t Remaining, OperationContext Context) override
            {
                if (const auto Error = Context.Check(id_)) return *Error;
                if (extrasDone_) return CopyExtraProgress{};
                if (options_.streams_ && !streamEnumerationDone_) {
                    if (sourceStream_.value_ != INVALID_HANDLE_VALUE) {
                        std::vector<char> Buffer(std::min<std::size_t>(Context.maxReadBytes_, 1024 * 1024)); DWORD Read = 0, Written = 0;
                        if (Buffer.empty()) return Error{ErrorCategory::InvalidRequest, "copy-chunk-budget", id_};
                        if (!ReadFile(sourceStream_.value_, Buffer.data(), DWORD(Buffer.size()), &Read, nullptr)) return extraFailure(GetLastError(), "copy-ads-read:");
                        if (Read > Remaining) return Error{ErrorCategory::BudgetExceeded, "copy-ads-byte-budget", id_};
                        if (Read) {
                            if (!WriteFile(destinationStream_.value_, Buffer.data(), Read, &Written, nullptr) || Written != Read)
                                return extraFailure(Written != Read ? ERROR_WRITE_FAULT : GetLastError(), "copy-ads-write:");
                            if (streamHash_) streamHash_->addData(QByteArrayView(Buffer.data(), Read));
                            return CopyExtraProgress{false, Read};
                        }
                        if (Context.flushFileContents_ && !FlushFileBuffers(destinationStream_.value_)) return extraFailure(GetLastError(), "copy-ads-flush:");
                        if (streamHash_) streamHashes_.emplace_back(streamName_, streamHash_->result());
                        sourceStream_.Close(); destinationStream_.Close(); streamHash_.reset(); return CopyExtraProgress{false, 0};
                    }
                    WIN32_FIND_STREAM_DATA Data{}; BOOL Found = TRUE;
                    if (streamSearch_ == INVALID_HANDLE_VALUE) {
                        streamSearch_ = FindFirstStreamW(sourcePath_.c_str(), FindStreamInfoStandard, &Data, 0);
                        Found = streamSearch_ != INVALID_HANDLE_VALUE;
                    } else Found = FindNextStreamW(streamSearch_, &Data);
                    if (!Found) {
                        const auto Code = GetLastError();
                        if (Code != ERROR_HANDLE_EOF) return extraFailure(Code, "copy-ads-enumerate:");
                        streamEnumerationDone_ = true;
                    } else {
                        streamName_ = Data.cStreamName;
                        if (streamName_ == L"::$DATA") return CopyExtraProgress{false, 0};
                        if (++streamCount_ > 65536 || !streamName_.starts_with(L":") || !streamName_.ends_with(L":$DATA")
                            || streamName_.find_first_of(L"/\\") != std::wstring::npos)
                            return Error{ErrorCategory::Unsupported, "copy-ads-name-or-count", id_};
                        sourceStream_ = Handle(CreateFileW((sourcePath_ + streamName_).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
                        if (sourceStream_.value_ == INVALID_HANDLE_VALUE) return extraFailure(GetLastError(), "copy-ads-read:");
                        destinationStream_ = Handle(CreateFileW((stagePath_ + streamName_).c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
                        if (destinationStream_.value_ == INVALID_HANDLE_VALUE) return extraFailure(GetLastError(), "copy-ads-write:");
                        if (options_.verify_) streamHash_ = std::make_unique<QCryptographicHash>(QCryptographicHash::Sha256);
                        return CopyExtraProgress{false, 0};
                    }
                }
                if (options_.acl_) {
                    PACL Dacl = nullptr; PSECURITY_DESCRIPTOR Descriptor = nullptr;
                    auto Code = GetSecurityInfo(sourceFile_.value_, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &Dacl, nullptr, &Descriptor);
                    if (Code != ERROR_SUCCESS) return extraFailure(Code, "copy-acl-read:");
                    SECURITY_DESCRIPTOR_CONTROL Control{}; DWORD Revision = 0;
                    const bool HaveControl = GetSecurityDescriptorControl(Descriptor, &Control, &Revision);
                    Code = HaveControl ? SetSecurityInfo(file_.value_, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION
                        | (Control & SE_DACL_PROTECTED ? PROTECTED_DACL_SECURITY_INFORMATION : UNPROTECTED_DACL_SECURITY_INFORMATION),
                        nullptr, nullptr, Dacl, nullptr) : GetLastError();
                    LocalFree(Descriptor);
                    if (Code != ERROR_SUCCESS) return extraFailure(Code, "copy-acl-write:");
                }
                if (sourceFile_.value_ != INVALID_HANDLE_VALUE) {
                    auto Valid = CheckEntry(sourceFile_.value_, source_, source_.id_.providerId_);
                    if (!Valid.HasValue()) return Valid.Failure();
                }
                sourceFile_.Close(); sourceParents_.clear(); extrasDone_ = true; return CopyExtraProgress{};
            }
            Result<bool> VerifyContent(OperationContext Context) override
            {
                if (const auto Error = Context.Check(id_)) return *Error;
                if (!sourceHash_ || !extrasDone_ || closed_) return Error{ErrorCategory::InvalidRequest, "copy-hash-state", id_};
                if (verified_) return true;
                if (!verificationStarted_) {
                    if (Context.flushFileContents_) { operations::OperationMetric Timing("payload.flush"); if (!FlushFileBuffers(file_.value_)) return Failure(GetLastError(), id_); }
                    else { operations::OperationMetric Skipped("payload.flush.skipped"); }
                    LARGE_INTEGER Zero{};
                    if (!SetFilePointerEx(file_.value_, Zero, nullptr, FILE_BEGIN)) return Failure(GetLastError(), id_);
                    readHash_ = std::make_unique<QCryptographicHash>(QCryptographicHash::Sha256); verificationStarted_ = true;
                }
                const auto File = verifyingMain_ ? file_.value_ : verificationStream_.value_;
                std::vector<char> Buffer(std::min<std::size_t>(Context.maxReadBytes_, 1024 * 1024)); DWORD Read = 0;
                if (Buffer.empty()) return Error{ErrorCategory::InvalidRequest, "copy-chunk-budget", id_};
                if (!ReadFile(File, Buffer.data(), DWORD(Buffer.size()), &Read, nullptr)) return Failure(GetLastError(), id_);
                if (Read) { readHash_->addData(QByteArrayView(Buffer.data(), Read)); return false; }
                const auto Expected = verifyingMain_ ? sourceHash_->result() : streamHashes_[verifyStreamIndex_].second;
                if (readHash_->result() != Expected) return Error{ErrorCategory::Unavailable, verifyingMain_ ? "copy-hash-mismatch" : "copy-ads-hash-mismatch", id_, {}, false, "source-and-existing-target-preserved"};
                if (!verifyingMain_) { verificationStream_.Close(); ++verifyStreamIndex_; }
                verifyingMain_ = false;
                if (verifyStreamIndex_ == streamHashes_.size()) { verified_ = true; return true; }
                verificationStream_ = Handle(CreateFileW((stagePath_ + streamHashes_[verifyStreamIndex_].first).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
                if (verificationStream_.value_ == INVALID_HANDLE_VALUE) return Failure(GetLastError(), id_);
                readHash_->reset(); return false;
            }
            Result<WriteReceipt> Commit(OperationContext Context) override
            {
                operations::OperationMetric Timing("native.commit.total", 0, true);
                if (const auto Error = Context.Check(id_)) return *Error;
                if (closed_ || failed_) return Error{ErrorCategory::InvalidRequest, "write-session-closed", id_};
                if (sourceHash_ && !verified_) return Error{ErrorCategory::InvalidRequest, "copy-verification-required", id_};
                if (Context.flushFileContents_) { operations::OperationMetric Timing("payload.flush"); if (!FlushFileBuffers(file_.value_)) return Failure(GetLastError(), id_); }
                else { operations::OperationMetric Skipped("payload.flush.skipped"); }
                if (metadata_.modifiedUtcMs_)
                {
                    const auto Value = static_cast<std::uint64_t>(*metadata_.modifiedUtcMs_ + 11644473600000LL) * 10000;
                    FILETIME Time{static_cast<DWORD>(Value), static_cast<DWORD>(Value >> 32)};
                    if (!SetFileTime(file_.value_, nullptr, nullptr, &Time)) return Failure(GetLastError(), id_);
                }
                FILE_BASIC_INFO Basic{};
                Basic.FileAttributes = metadata_.attributes_.value_or(FILE_ATTRIBUTE_NORMAL)
                    & (FILE_ATTRIBUTE_ARCHIVE | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_READONLY);
                if (!Basic.FileAttributes) Basic.FileAttributes = FILE_ATTRIBUTE_NORMAL;
                // Apply readonly after rename so failed commits remain removable.
                const auto FinalAttributes = Basic.FileAttributes;
                Basic.FileAttributes &= ~FILE_ATTRIBUTE_READONLY;
                if (!Basic.FileAttributes) Basic.FileAttributes = FILE_ATTRIBUTE_NORMAL;
                if (!SetFileInformationByHandle(file_.value_, FileBasicInfo, &Basic, sizeof(Basic))) return Failure(GetLastError(), id_);
                if (expected_.exists_)
                {
                    if (!expected_.revision_) return Error{ErrorCategory::InvalidRequest, "expected-revision-required", id_};
                    backup_ = Handle(CreateFileW(path_.path_.c_str(), DELETE | FILE_READ_ATTRIBUTES, FILE_SHARE_READ, nullptr,
                        OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr));
                    if (backup_.value_ == INVALID_HANDLE_VALUE) return Failure(GetLastError(), id_);
                    EntrySnapshot Expected;
                    Expected.id_ = {id_, {}, IdentityScope::NavigationSession, expected_.revision_};
                    const auto Valid = CheckEntry(backup_.value_, Expected, id_);
                    if (!Valid.HasValue()) { backup_.Close(); return Valid.Failure(); }
                    BY_HANDLE_FILE_INFORMATION Original{};
                    if (!GetFileInformationByHandle(backup_.value_, &Original)) return Failure(GetLastError(), id_);
                    record_.originalIdentity_ = Identity(Original);
                    auto Saved = checkpoint(WritePhase::Prepared); if (!Saved.HasValue()) return Saved.Failure();
                    const auto BackupPath = Normalize(record_.backup_, destination_.kind_, id_).Value().path_;
                    const auto Rename = RenameHandle(backup_.value_, BackupPath, id_);
                    if (!Rename.HasValue()) { backup_.Close(); return Rename.Failure(); }
                    backupPath_ = BackupPath;
                    Saved = checkpoint(WritePhase::BackedUp);
                    if (!Saved.HasValue()) return Saved.Failure();
                }
                else { auto Saved = checkpoint(WritePhase::Prepared); if (!Saved.HasValue()) return Saved.Failure(); }
                const auto Renamed = RenameHandle(file_.value_, path_.path_, id_);
                if (!Renamed.HasValue())
                {
                    failed_ = true;
                    const auto Restore = restoreBackup();
                    return Restore.HasValue() ? Renamed.Failure() : Restore.Failure();
                }
                closed_ = true;
                committed_ = true;
                WriteReceipt Receipt{destination_};
                Receipt.transaction_ = record_.transaction_; Receipt.destinationIdentity_ = record_.stageIdentity_;
                auto Published = checkpoint(WritePhase::Published);
                if (!Published.HasValue()) {
                    Receipt.cleanupErrors_.push_back(Published.Failure());
                    if (!backupPath_.empty()) Receipt.preservedArtifacts_.push_back(record_.backup_);
                    file_.Close(); backup_.Close(); parents_.clear(); return Receipt;
                }
                Basic.FileAttributes = FinalAttributes;
                if (!SetFileInformationByHandle(file_.value_, FileBasicInfo, &Basic, sizeof(Basic))) Receipt.cleanupErrors_.push_back(Failure(GetLastError(), id_));
                else if (metadata_.attributes_)
                {
                    // SMB servers can acknowledge the update yet derive HIDDEN from
                    // the final name (for example Samba's hide dot files policy).
                    // Query through the final name while the write handle still pins
                    // the file against rename/delete. SMB handle queries may retain
                    // attributes derived from the stage name. Report a committed
                    // copy with a metadata error so moves retain their source.
                    Handle Final(CreateFileW(path_.path_.c_str(), FILE_READ_ATTRIBUTES,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
                    FILE_BASIC_INFO Actual{};
                    if (Final.value_ == INVALID_HANDLE_VALUE || !GetFileInformationByHandleEx(Final.value_, FileBasicInfo, &Actual, sizeof(Actual)))
                        Receipt.cleanupErrors_.push_back(Failure(GetLastError(), id_));
                    else
                    {
                        constexpr DWORD Mask = FILE_ATTRIBUTE_ARCHIVE | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_READONLY;
                        const auto Difference = (Actual.FileAttributes ^ FinalAttributes) & Mask;
                        if (Difference)
                        {
                            const bool AddedHidden = Difference == FILE_ATTRIBUTE_HIDDEN && (Actual.FileAttributes & FILE_ATTRIBUTE_HIDDEN);
                            Receipt.cleanupErrors_.push_back({ErrorCategory::Unsupported,
                                AddedHidden ? "destination-added-hidden-attribute" : "destination-attributes-mismatch", id_, {}, false,
                                AddedHidden ? "check-destination-hidden-file-policy" : "check-destination-attributes"});
                        }
                    }
                }
                file_.Close();
                if (backup_.value_ != INVALID_HANDLE_VALUE)
                {
                    const auto Deleted = DeleteHandle(backup_.value_, id_);
                    if (!Deleted.HasValue())
                    {
                        auto Error = Deleted.Failure();
                        Error.userAction_ = "preserved-backup:" + Utf8(backupPath_);
                        Receipt.cleanupErrors_.push_back(std::move(Error));
                        Receipt.preservedArtifacts_.push_back({destination_.kind_, Utf8(backupPath_)});
                    }
                    backup_.Close();
                    backupPath_.clear();
                }
                auto ProductionCleaned = cleanupProduction();
                if (!ProductionCleaned.HasValue()) Receipt.cleanupErrors_.push_back(ProductionCleaned.Failure());
                parents_.clear();
                auto Finished = checkpoint(WritePhase::Finished);
                if (!Finished.HasValue()) Receipt.cleanupErrors_.push_back(Finished.Failure());
                return Receipt;
            }
            Result<Unit> Abort() override
            {
                sourceStream_.Close(); destinationStream_.Close(); verificationStream_.Close(); sourceFile_.Close(); sourceParents_.clear();
                if (committed_ || closed_) return Unit{};
                const auto Restored = restoreBackup();
                std::optional<Error> Error;
                if (!Restored.HasValue()) Error = Restored.Failure();
                if (file_.value_ != INVALID_HANDLE_VALUE)
                {
                    const auto Deleted = DeleteHandle(file_.value_, id_);
                    if (!Deleted.HasValue() && !Error)
                    { Error = Deleted.Failure(); Error->userAction_ = "preserved-stage:" + Utf8(stagePath_); }
                    file_.Close();
                }
                auto ProductionCleaned = cleanupProduction();
                if (!ProductionCleaned.HasValue() && !Error) Error = ProductionCleaned.Failure();
                parents_.clear();
                closed_ = true;
                if (Error) return *Error;
                if (checkpoint_ && recoveryRow_) { auto Saved = checkpoint(WritePhase::Aborted); if (!Saved.HasValue()) return Saved; }
                return Unit{};
            }
        private:
            Result<Unit> cleanupProduction()
            {
                if (productionDirectory_.empty()) return Unit{};
                const auto Output = productionDirectory_ + L"\\payload";
                Handle File(CreateFileW(Output.c_str(), DELETE | FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
                if (File.value_ != INVALID_HANDLE_VALUE) {
                    BY_HANDLE_FILE_INFORMATION Info{};
                    if (GetFileInformationByHandle(File.value_, &Info) && Info.nNumberOfLinks == 1
                        && !(Info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
                        FILE_BASIC_INFO Basic{}; Basic.FileAttributes = FILE_ATTRIBUTE_NORMAL;
                        SetFileInformationByHandle(File.value_, FileBasicInfo, &Basic, sizeof(Basic));
                    }
                    auto Removed = DeleteHandle(File.value_, id_); if (!Removed.HasValue()) return Removed;
                    File.Close();
                } else if (GetLastError() != ERROR_FILE_NOT_FOUND) return Failure(GetLastError(), id_);
                productionPin_.Close();
                if (!RemoveDirectoryW(productionDirectory_.c_str())) {
                    auto Error = Failure(GetLastError(), id_); Error.userAction_ = "preserved-production-directory:" + Utf8(productionDirectory_); return Error;
                }
                productionDirectory_.clear(); return Unit{};
            }
            std::optional<std::uint64_t> expectedBytes_;
            FILE_BASIC_INFO initialTimes_{};
            std::wstring productionDirectory_;
            bool productionStarted_ = false;
            Handle productionPin_;
            Error extraFailure(DWORD Code, const char* Phase)
            { auto Error = Failure(Code, id_); Error.code_ = Phase + Error.code_; return Error; }
            CopyOptions options_;
            EntrySnapshot source_;
            std::wstring sourcePath_, streamName_;
            Handle sourceFile_, sourceStream_, destinationStream_, verificationStream_;
            std::vector<Handle> sourceParents_;
            HANDLE streamSearch_ = INVALID_HANDLE_VALUE;
            std::unique_ptr<QCryptographicHash> sourceHash_, streamHash_, readHash_;
            std::vector<std::pair<std::wstring, QByteArray>> streamHashes_;
            std::size_t streamCount_ = 0, verifyStreamIndex_ = 0;
            bool extrasDone_ = false, streamEnumerationDone_ = false, verificationStarted_ = false, verifyingMain_ = true, verified_ = false;
            Result<Unit> checkpoint(WritePhase Phase)
            {
                record_.phase_ = Phase;
                if (!checkpoint_) return Unit{};
                try {
                    auto Saved = checkpoint_(record_, recoveryRow_); if (!Saved.HasValue()) return Saved.Failure();
                    recoveryRow_ = Saved.Value(); return Unit{};
                } catch (...) { return Error{ErrorCategory::Unavailable, "write-checkpoint-exception", id_}; }
            }
            Result<Unit> restoreBackup()
            {
                if (backupPath_.empty()) { backup_.Close(); return Unit{}; }
                const auto Restored = RenameHandle(backup_.value_, path_.path_, id_);
                backup_.Close();
                if (!Restored.HasValue())
                {
                    auto Error = Restored.Failure();
                    Error.code_ = "backup-restore-failed";
                    Error.userAction_ = "preserved-backup:" + Utf8(backupPath_);
                    backupPath_.clear();
                    return Error;
                }
                backupPath_.clear();
                return Unit{};
            }
            ProviderId id_;
            Locator destination_;
            NativePath path_;
            DestinationExpectation expected_;
            std::vector<Handle> parents_;
            Handle file_;
            std::wstring stagePath_;
            Handle backup_;
            std::wstring backupPath_;
            FileMetadata metadata_;
            WriteCheckpoint checkpoint_;
            WriteRecoveryRecord record_;
            std::optional<uint64_t> recoveryRow_;
            bool closed_ = false;
            bool committed_ = false;
            bool failed_ = false;
        };
        std::optional<DWORD> CopyNvmeDisk(const NativePath& Path)
        {
            wchar_t Root[32768]{}, Volume[128]{}, FileSystem[32]{};
            if (!GetVolumePathNameW(Path.path_.c_str(), Root, DWORD(std::size(Root))) || GetDriveTypeW(Root) != DRIVE_FIXED
                || !GetVolumeInformationW(Root, nullptr, 0, nullptr, nullptr, nullptr, FileSystem, DWORD(std::size(FileSystem)))
                || (_wcsicmp(FileSystem, L"NTFS") && _wcsicmp(FileSystem, L"ReFS"))
                || !GetVolumeNameForVolumeMountPointW(Root, Volume, DWORD(std::size(Volume)))) return {};
            std::wstring Name(Volume); if (Name.ends_with(L'\\')) Name.pop_back();
            Handle Device(CreateFileW(Name.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr));
            if (Device.value_ == INVALID_HANDLE_VALUE) return {};
            DWORD Returned = 0; VOLUME_DISK_EXTENTS Extents{};
            if (!DeviceIoControl(Device.value_, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, nullptr, 0, &Extents, sizeof(Extents), &Returned, nullptr)
                || Returned < sizeof(Extents) || Extents.NumberOfDiskExtents != 1) return {};
            STORAGE_PROPERTY_QUERY Query{}; Query.PropertyId = StorageDeviceSeekPenaltyProperty; Query.QueryType = PropertyStandardQuery;
            DEVICE_SEEK_PENALTY_DESCRIPTOR Seek{};
            if (!DeviceIoControl(Device.value_, IOCTL_STORAGE_QUERY_PROPERTY, &Query, sizeof(Query), &Seek, sizeof(Seek), &Returned, nullptr)
                || Returned < sizeof(Seek) || Seek.Size < sizeof(Seek) || Seek.Version < sizeof(Seek) || Seek.IncursSeekPenalty) return {};
            Query.PropertyId = StorageDeviceProperty;
            alignas(STORAGE_DEVICE_DESCRIPTOR) std::byte Storage[4096]{};
            if (!DeviceIoControl(Device.value_, IOCTL_STORAGE_QUERY_PROPERTY, &Query, sizeof(Query), Storage, sizeof(Storage), &Returned, nullptr)
                || Returned < sizeof(STORAGE_DEVICE_DESCRIPTOR)) return {};
            const auto Descriptor = reinterpret_cast<const STORAGE_DEVICE_DESCRIPTOR*>(Storage);
            if (Descriptor->BusType != BusTypeNvme) return {};
            return Extents.Extents[0].DiskNumber;
        }
        class NativeWritableStorage final : public IWritableStorage
        {
        public:
            NativeWritableStorage(LocatorKind Kind, std::shared_ptr<IStorageProvider> Reader)
                : kind_(Kind), id_{Kind == LocatorKind::Smb ? "smb" : "local"}, reader_(std::move(Reader)) {}
            ProviderId Id() const override { return id_; }
            Result<SupportDecision> Supports(Mutation Operation, Locator At, OperationContext Context) override
            {
                if (const auto Error = Context.Check(id_)) return *Error;
                const auto Path = Normalize(At, kind_, id_);
                if (!Path.HasValue()) return Path.Failure();
                if (Operation == Mutation::Recycle) {
                    if (kind_ != LocatorKind::Local || GetDriveTypeW(Path.Value().root_.c_str()) != DRIVE_FIXED)
                        return SupportDecision{Availability::Unsupported, "recycle-requires-fixed-local-volume"};
                    return SupportDecision{Availability::Conditional, "native-shell-recycle-only"};
                }
                return SupportDecision{Availability::Conditional, "native-staged-write", false, true,
                    {"access-checked-at-execution", "reparse-denied", "same-volume-rename", "replacement-backup-then-rename", "metadata-revision"}};
            }
            Result<std::optional<EntrySnapshot>> Inspect(Locator At, OperationContext Context) override
            {
                operations::OperationMetric Timing("native.inspect", 0, true);
                if (const auto Error = Context.Check(id_)) return *Error;
                auto Path = Normalize(At, kind_, id_);
                if (!Path.HasValue()) return Path.Failure();
                At.root_ = Utf8(Path.Value().path_);
                auto Entry = reader_->Resolve(At, Context).get();
                if (!Entry.HasValue())
                {
                    if (Entry.Failure().category_ == ErrorCategory::NotFound) return std::optional<EntrySnapshot>{};
                    return Entry.Failure();
                }
                if (Entry.Value().attributes_ & FILE_ATTRIBUTE_REPARSE_POINT) return Error{ErrorCategory::Unsupported, "reparse-entry-denied", id_};
                return std::optional<EntrySnapshot>{std::move(Entry.Value())};
            }
            CopyCapabilities CopySupport() const override { return {true, true, true, true}; }
            bool SupportsParallelCopies() const override { return kind_ == LocatorKind::Local; }
            std::size_t PreferredCopyWorkers(Locator Source, Locator Destination) const override
            {
                if (kind_ != LocatorKind::Local || Source.kind_ != LocatorKind::Local || Destination.kind_ != LocatorKind::Local) return 1;
                const auto From = Normalize(Source, kind_, id_), To = Normalize(Destination, kind_, id_);
                if (!From.HasValue() || !To.HasValue()) return 1;
                const auto SourceDisk = CopyNvmeDisk(From.Value()), DestinationDisk = CopyNvmeDisk(To.Value());
                return SourceDisk && DestinationDisk && *SourceDisk != *DestinationDisk ? 4 : 1;
            }
            bool CanProduceContent(OperationContext Context) const override
            { return kind_ == LocatorKind::Local && !Context.writeCheckpoint_; }
            Result<Unit> PatchMetadata(EntrySnapshot Entry, MetadataPatch Patch, OperationContext Context) override
            {
                if (!Patch.Valid()) return Error{ErrorCategory::InvalidRequest, "invalid-metadata-patch", id_};
                if (const auto Error = Context.Check(id_)) return *Error;
                if (Patch.Empty()) return Unit{};
                if (!Entry.id_.revision_ || (!Entry.id_.revision_->token_.starts_with("directory:")
                    && Entry.id_.revision_->token_.find(":identity:") == std::string::npos))
                    return Error{ErrorCategory::StaleRevision, "metadata-identity-required", id_};
                auto Path = Normalize(Entry.locator_, kind_, id_);
                if (!Path.HasValue()) return Path.Failure();
                auto Parents = PinParents(Path.Value(), id_);
                if (!Parents.HasValue()) return Parents.Failure();
                Handle File(CreateFileW(Path.Value().path_.c_str(), FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES,
                    FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr));
                if (File.value_ == INVALID_HANDLE_VALUE) return Failure(GetLastError(), id_);
                const auto Valid = CheckEntry(File.value_, Entry, id_);
                if (!Valid.HasValue()) return Valid.Failure();
                FILE_BASIC_INFO Before{};
                if (!GetFileInformationByHandleEx(File.value_, FileBasicInfo, &Before, sizeof(Before))) return Failure(GetLastError(), id_);
                // Revision tokens deliberately omit attributes. Check these separately
                // so a concurrent attribute editor cannot silently lose its changes.
                if (Before.FileAttributes != Entry.attributes_) return Error{ErrorCategory::StaleRevision, "metadata-attributes-changed", id_};
                FILE_BASIC_INFO Update{};
                Update.FileAttributes = ((Before.FileAttributes | Patch.setMask_) & ~Patch.clearMask_) & ~FILE_ATTRIBUTE_NORMAL;
                if (!Update.FileAttributes) Update.FileAttributes = FILE_ATTRIBUTE_NORMAL;
                if (Patch.modifiedUtcMs_) Update.LastWriteTime.QuadPart = (*Patch.modifiedUtcMs_ + 11644473600000LL) * 10000LL;
                if (const auto Error = Context.Check(id_)) return *Error;
                // One handle operation changes attributes and mtime together. Zero
                // timestamps preserve creation/access times, including on directories.
                if (!SetFileInformationByHandle(File.value_, FileBasicInfo, &Update, sizeof(Update))) return Failure(GetLastError(), id_);
                FILE_BASIC_INFO After{};
                if (!GetFileInformationByHandleEx(File.value_, FileBasicInfo, &After, sizeof(After)))
                    return Error{ErrorCategory::Unavailable, "metadata-outcome-unknown", id_};
                if (((After.FileAttributes ^ Update.FileAttributes) & 0x27u)
                    || (Patch.modifiedUtcMs_ && After.LastWriteTime.QuadPart != Update.LastWriteTime.QuadPart))
                    return Error{ErrorCategory::Unavailable, "metadata-applied-readback-differs", id_};
                return Unit{};
            }
            Result<std::unique_ptr<IWriteSession>> BeginWrite(Locator At, DestinationExpectation Expected, OperationContext Context) override
            { return BeginCopy(std::move(At), std::move(Expected), {}, Context); }
            Result<std::unique_ptr<IWriteSession>> BeginCopy(Locator At, DestinationExpectation Expected, CopyOptions Options, OperationContext Context) override
            {
                operations::OperationMetric Timing("native.begin-copy.total", 0, true);
                if (const auto Error = Context.Check(id_)) return *Error;
                if (Expected.exists_ != Expected.revision_.has_value()) return Error{ErrorCategory::InvalidRequest, "destination-expectation", id_};
                auto Path = Normalize(At, kind_, id_);
                if (!Path.HasValue()) return Path.Failure();
                auto Parents = PinParents(Path.Value(), id_);
                if (!Parents.HasValue()) return Parents.Failure();
                const auto Temp = TemporaryName(std::filesystem::path(Path.Value().path_).parent_path().wstring(), L"stage");
                Handle File(CreateFileW(Temp.c_str(), GENERIC_WRITE | FILE_READ_ATTRIBUTES | READ_CONTROL | DELETE | (Options.verify_ ? GENERIC_READ : 0) | (Options.acl_ ? WRITE_DAC : 0), FILE_SHARE_READ, nullptr,
                    CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
                if (File.value_ == INVALID_HANDLE_VALUE) return Failure(GetLastError(), id_);
                auto Session = std::make_unique<NativeWriteSession>(id_, At, std::move(Path.Value()), std::move(Expected),
                    std::move(Parents.Value()), std::move(File), Temp);
                Session->EnableVerification(Options.verify_);
                auto Initialized = Session->Initialize(Context); if (!Initialized.HasValue()) return Initialized.Failure();
                return std::unique_ptr<IWriteSession>(std::move(Session));
            }
            Result<Unit> MakeDirectory(Locator At, OperationContext Context) override
            {
                if (const auto Error = Context.Check(id_)) return *Error;
                const auto Path = Normalize(At, kind_, id_);
                if (!Path.HasValue()) return Path.Failure();
                auto Parents = PinParents(Path.Value(), id_);
                if (!Parents.HasValue()) return Parents.Failure();
                if (!CreateDirectoryW(Path.Value().path_.c_str(), nullptr)) return Failure(GetLastError(), id_);
                return Unit{};
            }
            Result<Unit> Remove(EntrySnapshot Entry, OperationContext Context) override
            {
                if (const auto Error = Context.Check(id_)) return *Error;
                auto Path = Normalize(Entry.locator_, kind_, id_);
                if (!Path.HasValue()) return Path.Failure();
                auto Parents = PinParents(Path.Value(), id_);
                if (!Parents.HasValue()) return Parents.Failure();
                Handle File(CreateFileW(Path.Value().path_.c_str(), DELETE | FILE_READ_ATTRIBUTES | (Context.allowReadOnlyRemoval_ ? FILE_WRITE_ATTRIBUTES : 0), FILE_SHARE_READ, nullptr,
                    OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr));
                if (File.value_ == INVALID_HANDLE_VALUE) return Failure(GetLastError(), id_);
                const auto Valid = CheckEntry(File.value_, Entry, id_);
                if (!Valid.HasValue()) return Valid.Failure();
                FILE_BASIC_INFO Basic{}; if (!GetFileInformationByHandleEx(File.value_, FileBasicInfo, &Basic, sizeof(Basic))) return Failure(GetLastError(), id_);
                const bool ReadOnly = (Basic.FileAttributes & FILE_ATTRIBUTE_READONLY) != 0;
                if (ReadOnly && !Context.allowReadOnlyRemoval_) return Error{ErrorCategory::AccessDenied, "readonly-removal-not-authorized", id_};
                const auto Original = Basic;
                if (ReadOnly) { Basic.FileAttributes &= ~FILE_ATTRIBUTE_READONLY; if (!Basic.FileAttributes) Basic.FileAttributes = FILE_ATTRIBUTE_NORMAL;
                    if (!SetFileInformationByHandle(File.value_, FileBasicInfo, &Basic, sizeof(Basic))) return Failure(GetLastError(), id_); }
                const auto Removed = DeleteHandle(File.value_, id_);
                // Restore the same verified object after a failed deletion, never a
                // newly resolved path which might now designate another file.
                if (!Removed.HasValue() && ReadOnly) { auto Restore = Original; SetFileInformationByHandle(File.value_, FileBasicInfo, &Restore, sizeof(Restore)); }
                return Removed;
            }
            Result<WriteReceipt> Recycle(EntrySnapshot Entry, OperationContext Context) override
            {
                using Microsoft::WRL::ComPtr;
                if (const auto Error = Context.Check(id_)) return *Error;
                const auto Supported = Supports(Mutation::Recycle, Entry.locator_, Context);
                if (!Supported.HasValue()) return Supported.Failure();
                if (Supported.Value().availability_ == Availability::Unsupported) return Error{ErrorCategory::Unsupported, Supported.Value().reason_, id_};
                auto Path = Normalize(Entry.locator_, kind_, id_); if (!Path.HasValue()) return Path.Failure();
                auto Parents = PinParents(Path.Value(), id_); if (!Parents.HasValue()) return Parents.Failure();
                {
                    Handle File(CreateFileW(Path.Value().path_.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                        OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr));
                    if (File.value_ == INVALID_HANDLE_VALUE) return Failure(GetLastError(), id_);
                    const auto Valid = CheckEntry(File.value_, Entry, id_); if (!Valid.HasValue()) return Valid.Failure();
                    BY_HANDLE_FILE_INFORMATION Info{}; if (!GetFileInformationByHandle(File.value_, &Info)) return Failure(GetLastError(), id_);
                    if ((Info.dwFileAttributes & FILE_ATTRIBUTE_READONLY) && !Context.allowReadOnlyRemoval_) return Error{ErrorCategory::AccessDenied, "readonly-removal-not-authorized", id_};
                }
                const auto Initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
                if (FAILED(Initialized)) return Error{ErrorCategory::Unavailable, "recycle-com-apartment", id_};
                struct Apartment { ~Apartment() { CoUninitialize(); } } ApartmentScope;
                ComPtr<IShellItem> Item, Parent, Bin, Recycled;
                ComPtr<ITransferSource> Transfer;
                const auto ShellPath = Path.Value().path_.substr(4); // Native normalization uses the extended prefix; Shell parsing expects a DOS path.
                std::string Stage = "source"; auto Hr = SHCreateItemFromParsingName(ShellPath.c_str(), nullptr, IID_PPV_ARGS(&Item));
                if (SUCCEEDED(Hr)) { Stage = "parent"; Hr = Item->GetParent(&Parent); }
                if (SUCCEEDED(Hr)) { Stage = "transfer"; Hr = Parent->BindToHandler(nullptr, BHID_Transfer, IID_PPV_ARGS(&Transfer)); }
                if (SUCCEEDED(Hr)) { Stage = "bin"; Hr = SHGetKnownFolderItem(FOLDERID_RecycleBinFolder, KF_FLAG_DEFAULT, nullptr, IID_PPV_ARGS(&Bin)); }
                if (const auto Error = Context.Check(id_)) return *Error;
                // RecycleItem has a recycle-only contract. Do not fall back to
                // Remove/DeleteItem/SHFileOperation, even when quota or policy fails.
                // The Shell still resolves a path; unlike handle deletion this
                // cannot make the leaf identity check and mutation one atomic step.
                if (SUCCEEDED(Hr)) { Stage = "recycle"; Hr = Transfer->RecycleItem(Item.Get(), Bin.Get(), TSF_NORMAL, &Recycled); }
                if (FAILED(Hr) || !Recycled) {
                    if (HRESULT_FACILITY(Hr) == FACILITY_WIN32 && (HRESULT_CODE(Hr) == ERROR_SHARING_VIOLATION || HRESULT_CODE(Hr) == ERROR_LOCK_VIOLATION))
                        return Failure(HRESULT_CODE(Hr), id_, Entry.locator_.root_);
                    return Error{ErrorCategory::Unavailable, "native-recycle-" + Stage + "-failed-" + std::to_string(static_cast<unsigned long>(Hr)), id_};
                }
                PWSTR Name = nullptr; Hr = Recycled->GetDisplayName(SIGDN_FILESYSPATH, &Name);
                if (FAILED(Hr)) return Error{ErrorCategory::Internal, "recycle-location-unavailable", id_};
                const auto RecyclePath = Utf8(Name); CoTaskMemFree(Name);
                return WriteReceipt{Locator{LocatorKind::Local, RecyclePath}};
            }
            Result<Unit> Rename(EntrySnapshot Entry, Locator To, OperationContext Context) override
            {
                if (const auto Error = Context.Check(id_)) return *Error;
                const auto FromPath = Normalize(Entry.locator_, kind_, id_);
                if (!FromPath.HasValue()) return FromPath.Failure();
                const auto ToPath = Normalize(To, kind_, id_);
                if (!ToPath.HasValue()) return ToPath.Failure();
                auto FromParents = PinParents(FromPath.Value(), id_);
                if (!FromParents.HasValue()) return FromParents.Failure();
                auto ToParents = PinParents(ToPath.Value(), id_);
                if (!ToParents.HasValue()) return ToParents.Failure();
                Handle File(CreateFileW(FromPath.Value().path_.c_str(), DELETE | FILE_READ_ATTRIBUTES, FILE_SHARE_READ, nullptr,
                    OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr));
                if (File.value_ == INVALID_HANDLE_VALUE) return Failure(GetLastError(), id_);
                const auto Valid = CheckEntry(File.value_, Entry, id_);
                if (!Valid.HasValue()) return Valid.Failure();
                return RenameHandle(File.value_, ToPath.Value().path_, id_);
            }
        private:
            LocatorKind kind_;
            ProviderId id_;
            std::shared_ptr<IStorageProvider> reader_;
        };
    }
    Result<WriteReceipt> RecoverNativeWrite(const WriteRecoveryRecord& Record)
    {
        const ProviderId Id{"write-recovery"};
        if (Record.transaction_.empty() || Record.stageIdentity_.empty()) return Error{ErrorCategory::InvalidRequest, "write-recovery-identity"};
        const auto Kind = Record.destination_.kind_;
        auto Destination = Normalize(Record.destination_, Kind, Id), Stage = Normalize(Record.stage_, Kind, Id), Backup = Normalize(Record.backup_, Kind, Id);
        if (!Destination.HasValue() || !Stage.HasValue() || !Backup.HasValue()) return Error{ErrorCategory::InvalidRequest, "write-recovery-path"};
        const auto Parent = std::filesystem::path(Destination.Value().path_).parent_path();
        if (std::filesystem::path(Stage.Value().path_).parent_path() != Parent || std::filesystem::path(Backup.Value().path_).parent_path() != Parent
            || !std::filesystem::path(Stage.Value().path_).filename().wstring().starts_with(L".hc-stage-")
            || !std::filesystem::path(Backup.Value().path_).filename().wstring().starts_with(L".hc-backup-"))
            return Error{ErrorCategory::InvalidRequest, "write-recovery-container"};
        auto Parents = PinParents(Destination.Value(), Id); if (!Parents.HasValue()) return Parents.Failure();
        struct Observed { Handle handle_; std::string identity_; };
        const auto Observe = [&](const NativePath& Path) -> Result<Observed> {
            Handle File(CreateFileW(Path.path_.c_str(), DELETE | FILE_READ_ATTRIBUTES, FILE_SHARE_READ, nullptr,
                OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
            if (File.value_ == INVALID_HANDLE_VALUE) {
                const auto Code = GetLastError();
                if (Code == ERROR_FILE_NOT_FOUND) return Observed{};
                return Failure(Code, Id);
            }
            BY_HANDLE_FILE_INFORMATION Info{};
            if (!GetFileInformationByHandle(File.value_, &Info)) return Failure(GetLastError(), Id);
            if ((Info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) || Info.nNumberOfLinks != 1)
                return Error{ErrorCategory::Unsupported, "write-recovery-file-type"};
            return Observed{std::move(File), Identity(Info)};
        };
        auto Current = Observe(Destination.Value()), Pending = Observe(Stage.Value()), Original = Observe(Backup.Value());
        if (!Current.HasValue()) return Current.Failure();
        if (!Pending.HasValue()) return Pending.Failure();
        if (!Original.HasValue()) return Original.Failure();
        if ((!Pending.Value().identity_.empty() && Pending.Value().identity_ != Record.stageIdentity_)
            || (!Original.Value().identity_.empty() && Original.Value().identity_ != Record.originalIdentity_))
            return Error{ErrorCategory::StaleRevision, "write-recovery-artifact-changed", Id, {}, false, "inspect-preserved-files"};
        WriteReceipt Receipt{Record.destination_}; Receipt.transaction_ = Record.transaction_;
        Receipt.destinationCommitted_ = Current.Value().identity_ == Record.stageIdentity_;
        Receipt.destinationIdentity_ = Current.Value().identity_;
        if (!Current.Value().identity_.empty() && !Receipt.destinationCommitted_
            && Current.Value().identity_ != Record.originalIdentity_ && Record.phase_ != WritePhase::Writing && Record.phase_ != WritePhase::Aborted)
            return Error{ErrorCategory::StaleRevision, "write-recovery-destination-changed", Id, {}, false, "inspect-destination-and-backup"};
        if (Current.Value().identity_.empty() && !Original.Value().identity_.empty()) {
            // Non-replacing rename through the verified handle also refuses a
            // destination that appears after inspection. No source is deleted.
            auto Restored = RenameHandle(Original.Value().handle_.value_, Destination.Value().path_, Id);
            if (!Restored.HasValue()) return Restored.Failure();
            Receipt.destinationIdentity_ = Original.Value().identity_; Original.Value().identity_.clear();
        }
        if (!Pending.Value().identity_.empty()) Receipt.preservedArtifacts_.push_back(Record.stage_);
        if (!Original.Value().identity_.empty()) Receipt.preservedArtifacts_.push_back(Record.backup_);
        if (Receipt.destinationIdentity_.empty() && !Record.originalIdentity_.empty())
            Receipt.cleanupErrors_.push_back({ErrorCategory::Unavailable, "write-recovery-original-missing", Id, {}, false, "inspect-preserved-files"});
        return Receipt;
    }
    std::shared_ptr<IWritableStorage> MakeNativeWritableStorage(LocatorKind Kind, std::shared_ptr<IStorageProvider> Reader)
    {
        if ((Kind != LocatorKind::Local && Kind != LocatorKind::Smb) || !Reader
            || Reader->Id().value_ != (Kind == LocatorKind::Smb ? "smb" : "local")) throw std::invalid_argument("native-writer-provider");
        return std::make_shared<NativeWritableStorage>(Kind, std::move(Reader));
    }
}
