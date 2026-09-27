#include "nativeWritableStorage.hpp"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <objbase.h>
#include <cstring>
#include <filesystem>
#include <limits>
#include <stdexcept>

#pragma comment(lib, "ole32")

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
        Error Failure(DWORD Code, const ProviderId& Id)
        {
            Error Value{ErrorCategory::Unavailable, "win32:" + std::to_string(Code), Id};
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
                if (Directory.value_ == INVALID_HANDLE_VALUE) return Failure(GetLastError(), Id);
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
        std::uint64_t Ticks(FILETIME Time) { return (static_cast<std::uint64_t>(Time.dwHighDateTime) << 32) | Time.dwLowDateTime; }
        ResourceRevision Revision(const BY_HANDLE_FILE_INFORMATION& Info)
        {
            if (Info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                return {"directory:" + std::to_string(Ticks(Info.ftCreationTime)) + ":" + std::to_string(Info.dwVolumeSerialNumber)
                    + ":" + std::to_string(Info.nFileIndexHigh) + ":" + std::to_string(Info.nFileIndexLow)};
            return {std::to_string(Ticks(Info.ftCreationTime)) + ":" + std::to_string(Ticks(Info.ftLastWriteTime))
                + ":" + std::to_string(Info.nFileSizeHigh) + ":" + std::to_string(Info.nFileSizeLow)};
        }
        Result<Unit> CheckEntry(HANDLE File, const EntrySnapshot& Expected, const ProviderId& Id)
        {
            BY_HANDLE_FILE_INFORMATION Info{};
            if (!GetFileInformationByHandle(File, &Info)) return Failure(GetLastError(), Id);
            if (Info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) return Error{ErrorCategory::Unsupported, "reparse-entry-denied", Id};
            if (Expected.id_.providerId_ != Id || !Expected.id_.revision_ || Revision(Info) != *Expected.id_.revision_
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
            ~NativeWriteSession() override { Abort(); }
            Result<Unit> Append(std::span<const std::byte> Bytes, OperationContext Context) override
            {
                if (const auto Error = Context.Check(id_)) return *Error;
                if (closed_ || failed_) return Error{ErrorCategory::InvalidRequest, "write-session-closed", id_};
                if (Bytes.empty() || Bytes.size() > (std::min)(Context.maxReadBytes_, std::size_t{1024 * 1024}))
                    return Error{ErrorCategory::InvalidRequest, "write-chunk-budget", id_};
                DWORD Written = 0;
                if (!WriteFile(file_.value_, Bytes.data(), static_cast<DWORD>(Bytes.size()), &Written, nullptr))
                { failed_ = true; return Failure(GetLastError(), id_); }
                if (Written != Bytes.size()) { failed_ = true; return Error{ErrorCategory::Unavailable, "short-write", id_}; }
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
            Result<WriteReceipt> Commit(OperationContext Context) override
            {
                if (const auto Error = Context.Check(id_)) return *Error;
                if (closed_ || failed_) return Error{ErrorCategory::InvalidRequest, "write-session-closed", id_};
                if (!FlushFileBuffers(file_.value_)) return Failure(GetLastError(), id_);
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
                    const auto BackupPath = TemporaryName(std::filesystem::path(path_.path_).parent_path().wstring(), L"backup");
                    const auto Rename = RenameHandle(backup_.value_, BackupPath, id_);
                    if (!Rename.HasValue()) { backup_.Close(); return Rename.Failure(); }
                    backupPath_ = BackupPath;
                }
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
                parents_.clear();
                return Receipt;
            }
            Result<Unit> Abort() override
            {
                if (committed_) return Unit{};
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
                parents_.clear();
                closed_ = true;
                if (Error) return *Error;
                return Unit{};
            }
        private:
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
            bool closed_ = false;
            bool committed_ = false;
            bool failed_ = false;
        };
        class NativeWritableStorage final : public IWritableStorage
        {
        public:
            NativeWritableStorage(LocatorKind Kind, std::shared_ptr<IStorageProvider> Reader)
                : kind_(Kind), id_{Kind == LocatorKind::Smb ? "smb" : "local"}, reader_(std::move(Reader)) {}
            ProviderId Id() const override { return id_; }
            Result<SupportDecision> Supports(Mutation, Locator At, OperationContext Context) override
            {
                if (const auto Error = Context.Check(id_)) return *Error;
                const auto Path = Normalize(At, kind_, id_);
                if (!Path.HasValue()) return Path.Failure();
                return SupportDecision{Availability::Conditional, "native-staged-write", false, true,
                    {"access-checked-at-execution", "reparse-denied", "same-volume-rename", "replacement-backup-then-rename", "metadata-revision"}};
            }
            Result<std::optional<EntrySnapshot>> Inspect(Locator At, OperationContext Context) override
            {
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
            Result<std::unique_ptr<IWriteSession>> BeginWrite(Locator At, DestinationExpectation Expected, OperationContext Context) override
            {
                if (const auto Error = Context.Check(id_)) return *Error;
                if (Expected.exists_ != Expected.revision_.has_value()) return Error{ErrorCategory::InvalidRequest, "destination-expectation", id_};
                auto Path = Normalize(At, kind_, id_);
                if (!Path.HasValue()) return Path.Failure();
                auto Parents = PinParents(Path.Value(), id_);
                if (!Parents.HasValue()) return Parents.Failure();
                const auto Temp = TemporaryName(std::filesystem::path(Path.Value().path_).parent_path().wstring(), L"stage");
                Handle File(CreateFileW(Temp.c_str(), GENERIC_WRITE | FILE_READ_ATTRIBUTES | DELETE, FILE_SHARE_READ, nullptr,
                    CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
                if (File.value_ == INVALID_HANDLE_VALUE) return Failure(GetLastError(), id_);
                return std::unique_ptr<IWriteSession>(new NativeWriteSession(id_, At, std::move(Path.Value()), std::move(Expected),
                    std::move(Parents.Value()), std::move(File), Temp));
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
                Handle File(CreateFileW(Path.Value().path_.c_str(), DELETE | FILE_READ_ATTRIBUTES, FILE_SHARE_READ, nullptr,
                    OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr));
                if (File.value_ == INVALID_HANDLE_VALUE) return Failure(GetLastError(), id_);
                const auto Valid = CheckEntry(File.value_, Entry, id_);
                if (!Valid.HasValue()) return Valid.Failure();
                return DeleteHandle(File.value_, id_);
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
    std::shared_ptr<IWritableStorage> MakeNativeWritableStorage(LocatorKind Kind, std::shared_ptr<IStorageProvider> Reader)
    {
        if ((Kind != LocatorKind::Local && Kind != LocatorKind::Smb) || !Reader
            || Reader->Id().value_ != (Kind == LocatorKind::Smb ? "smb" : "local")) throw std::invalid_argument("native-writer-provider");
        return std::make_shared<NativeWritableStorage>(Kind, std::move(Reader));
    }
}
