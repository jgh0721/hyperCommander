#include "stdafx.h"
#include "nativeWritableStorage.hpp"
#include "operationMetrics.hpp"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <winioctl.h>
#include <winternl.h>
#include "nativeStorageRevision.hpp"
#include "nativeCopyCapabilities.hpp"
#include <Aclapi.h>
#include <QCryptographicHash>
#include <QScopeGuard>
#include <objbase.h>
#include <ShlObj.h>
#include <wrl/client.h>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstring>
#include <deque>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <stop_token>
#include <thread>

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
            Value.retryable_ = Code == ERROR_SHARING_VIOLATION || Code == ERROR_LOCK_VIOLATION || Code == ERROR_USER_MAPPED_FILE
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
        using ParentPins = std::shared_ptr<std::vector<Handle>>;
        Result<ParentPins> OpenParents(const NativePath& Path, const ProviderId& Id, bool Retain = false)
        {
            operations::OperationMetric Timing("native.parent-pin", 0, true);
            auto Parents = std::make_shared<std::vector<Handle>>();
            const auto Parent = std::filesystem::path(Path.path_).parent_path().wstring();
            if (Path.path_ == Path.root_) return Error{ErrorCategory::InvalidRequest, "root-mutation-denied", Id};
            for (auto End = Path.root_.size();;)
            {
                const auto Part = Path.path_.substr(0, End);
                // Recheck every component for each file. Some NAS servers allow
                // directory rename despite these share restrictions, so retaining
                // the handles alone cannot authorize reuse for later paths.
                Handle Directory(CreateFileW(Part.c_str(), FILE_READ_ATTRIBUTES | (Retain ? FILE_LIST_DIRECTORY : 0), FILE_SHARE_READ, nullptr,
                    OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
                if (Directory.value_ == INVALID_HANDLE_VALUE) return Failure(GetLastError(), Id, Utf8(Part));
                BY_HANDLE_FILE_INFORMATION Info{};
                if (!GetFileInformationByHandle(Directory.value_, &Info)) return Failure(GetLastError(), Id);
                if (!(Info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (Info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
                    return Error{ErrorCategory::Unsupported, "reparse-parent-denied", Id};
                Parents->push_back(std::move(Directory));
                if (End >= Parent.size()) break;
                End = Path.path_.find(L'\\', End + (Path.path_[End - 1] == L'\\' ? 0 : 1));
                if (End == std::wstring::npos || End > Parent.size()) End = Parent.size();
            }
            return Parents;
        }
        HANDLE OpenRelativeRemoval(const NativePath& Path, const ParentPins& Parents)
        {
            // A single component relative to the already verified, pinned parent
            // avoids reparsing the whole path in the object manager. NtCreateFile
            // runs in user mode with the same desired access/share/backup policy;
            // the opened object is still inspected before it can be deleted.
            using Open = NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK,
                PLARGE_INTEGER, ULONG, ULONG, ULONG, ULONG, PVOID, ULONG);
            static const auto Create = reinterpret_cast<Open>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtCreateFile"));
            if (!Create || !Parents || Parents->empty()) return INVALID_HANDLE_VALUE;
            const auto Begin = Path.path_.find_last_of(L'\\') + 1;
            UNICODE_STRING Name{}; Name.Length = USHORT((Path.path_.size() - Begin) * sizeof(wchar_t));
            Name.MaximumLength = Name.Length; Name.Buffer = const_cast<wchar_t*>(Path.path_.data() + Begin);
            OBJECT_ATTRIBUTES Attributes{}; Attributes.Length = sizeof(Attributes);
            Attributes.RootDirectory = Parents->back().value_; Attributes.ObjectName = &Name;
            Attributes.Attributes = 0x40; // OBJ_CASE_INSENSITIVE
            IO_STATUS_BLOCK Status{}; HANDLE File = INVALID_HANDLE_VALUE;
            constexpr ULONG OpenExisting = 1, Synchronous = 0x20, NonDirectory = 0x40, BackupIntent = 0x4000, OpenReparsePoint = 0x200000;
            const auto Result = Create(&File, DELETE | FILE_READ_ATTRIBUTES | SYNCHRONIZE, &Attributes, &Status,
                nullptr, 0, FILE_SHARE_READ, OpenExisting, Synchronous | NonDirectory | BackupIntent | OpenReparsePoint, nullptr, 0);
            if (Result == 0 && File != INVALID_HANDLE_VALUE) {
                operations::OperationMetric Relative("native.prepare.relative-open", 0, true); return File;
            }
            if (File != INVALID_HANDLE_VALUE && File) CloseHandle(File);
            return INVALID_HANDLE_VALUE;
        }
        class NativeMutationScope final : public MutationScope, public IRemovalPreparation
        {
        public:
            void Clear() override
            {
                std::lock_guard Lock(mutex_); prepared_.clear(); parents_.clear();
            }
            void ReleaseDirectory(const Locator& At)
            {
                const auto Path = Normalize(At, LocatorKind::Local, {"local"});
                if (!Path.HasValue()) { Clear(); return; }
                const auto& Directory = Path.Value().path_;
                const auto Within = [&](const std::wstring& Candidate) {
                    return Candidate.size() >= Directory.size()
                        && !_wcsnicmp(Candidate.c_str(), Directory.c_str(), Directory.size())
                        && (Candidate.size() == Directory.size() || Directory.back() == L'\\' || Candidate[Directory.size()] == L'\\');
                };
                std::lock_guard Lock(mutex_);
                for (auto It = prepared_.begin(); It != prepared_.end();) {
                    const auto Child = Normalize(It->second->locator_, LocatorKind::Local, {"local"});
                    if (!Child.HasValue() || Within(Child.Value().path_)) It = prepared_.erase(It); else ++It;
                }
                // Keep common ancestors and unrelated folders. Clearing every
                // pin for each empty leaf directory repeated the root walk once
                // per directory during large delete/move cleanup.
                for (auto It = parents_.begin(); It != parents_.end();)
                    if (Within(It->first)) It = parents_.erase(It); else ++It;
            }
            struct PreparedRemoval {
                Handle file_;
                ParentPins parents_;
                ResourceRevision revision_;
                BY_HANDLE_FILE_INFORMATION info_{};
                Locator locator_;
            };
            Result<EntrySnapshot> Prepare(EntrySnapshot Entry, const OperationContext& Context) override
            {
                if (const auto Error = Context.Check(Entry.id_.providerId_)) return *Error;
                const auto Path = Normalize(Entry.locator_, LocatorKind::Local, Entry.id_.providerId_);
                if (!Path.HasValue()) return Path.Failure();
                { std::lock_guard Lock(mutex_);
                  if (!reusableRoot(Path.Value())) return Error{ErrorCategory::Unsupported, "delete-preparation-share-policy"};
                  if (prepared_.size() >= 512) return Error{ErrorCategory::Unavailable, "delete-preparation-full"}; }
                auto Parents = Pin(Path.Value(), Entry.id_.providerId_); if (!Parents.HasValue()) return Parents.Failure();
                auto Prepared = std::make_unique<PreparedRemoval>(); Prepared->parents_ = std::move(Parents.Value());
                Prepared->file_ = Handle(OpenRelativeRemoval(Path.Value(), Prepared->parents_));
                if (Prepared->file_.value_ == INVALID_HANDLE_VALUE)
                    Prepared->file_ = Handle(CreateFileW(Path.Value().path_.c_str(), DELETE | FILE_READ_ATTRIBUTES, FILE_SHARE_READ,
                        nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr));
                if (Prepared->file_.value_ == INVALID_HANDLE_VALUE) return Failure(GetLastError(), Entry.id_.providerId_);
                BY_HANDLE_FILE_INFORMATION Info{};
                if (!GetFileInformationByHandle(Prepared->file_.value_, &Info)) return Failure(GetLastError(), Entry.id_.providerId_);
                if (Info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_READONLY))
                    return Error{ErrorCategory::Unsupported, "delete-preparation-special-entry"};
                Entry.attributes_ = Info.dwFileAttributes; Entry.directory_ = false;
                Entry.size_ = (uint64_t(Info.nFileSizeHigh) << 32) | Info.nFileSizeLow;
                const auto Milliseconds = [](FILETIME Time) { return int64_t(((uint64_t(Time.dwHighDateTime) << 32) | Time.dwLowDateTime) / 10000) - 11644473600000LL; };
                Entry.createdUtcMs_ = Milliseconds(Info.ftCreationTime); Entry.modifiedUtcMs_ = Milliseconds(Info.ftLastWriteTime);
                bool Ntfs = false;
                { std::lock_guard Lock(mutex_); Ntfs = ntfs_[Path.Value().root_]; }
                // NTFS exposes its complete 64-bit file reference in this query;
                // ReFS keeps the 128-bit FileIdInfo path. The live handle remains
                // pinned through deletion in both cases.
                Entry.id_.revision_ = Ntfs ? native::HandleRevision(Info)
                    : native::HandleRevision(Prepared->file_.value_, Info, Path.Value().path_);
                Prepared->locator_ = Entry.locator_;
                Prepared->revision_ = *Entry.id_.revision_; Prepared->info_ = Info;
                // Deny write/delete sharing from capture through deletion. The
                // pinned ancestor chain also prevents pathname replacement.
                std::lock_guard Lock(mutex_);
                if (prepared_.size() >= 512) return Error{ErrorCategory::Unavailable, "delete-preparation-full"};
                prepared_[Entry.locator_.root_] = std::move(Prepared);
                return Entry;
            }
            std::unique_ptr<PreparedRemoval> Take(const EntrySnapshot& Entry)
            {
                std::lock_guard Lock(mutex_);
                const auto Found = prepared_.find(Entry.locator_.root_);
                if (Found == prepared_.end()) return {};
                auto Prepared = std::move(Found->second); prepared_.erase(Found);
                if (Entry.id_.providerId_.value_ != "local" || Entry.locator_ != Prepared->locator_
                    || !Entry.id_.revision_ || Prepared->revision_ != *Entry.id_.revision_) return {};
                operations::OperationMetric Reused("native.remove.prepared", 0, true);
                return Prepared;
            }
            Result<ParentPins> Pin(const NativePath& Path, const ProviderId& Id)
            {
                std::lock_guard Lock(mutex_);
                const bool Reusable = reusableRoot(Path);
                // Local NTFS/ReFS honors the no-delete/no-write sharing on each
                // pinned ancestor. NAS and redirected drives must keep rechecking
                // every component; their rename/share semantics can differ.
                if (!Reusable) return OpenParents(Path, Id);
                if (Path.path_ == Path.root_) return Error{ErrorCategory::InvalidRequest, "root-mutation-denied", Id};
                const auto Parent = std::filesystem::path(Path.path_).parent_path().wstring();
                if (const auto Existing = parents_.find(Parent); Existing != parents_.end()) {
                    operations::OperationMetric Hit("native.parent-pin.reused", 0, true);
                    return Existing->second;
                }
                // Attribute-only handles do not participate in Windows share
                // checks. Retained directory handles need LIST_DIRECTORY so
                // no-delete sharing actually prevents ancestor replacement.
                // A traverse-only user retains the uncached verification path.
                auto Pins = OpenParents(Path, Id, true);
                if (!Pins.HasValue()) return OpenParents(Path, Id);
                // Bound retained handles even when a batch visits many folders.
                // Write sessions retain their own shared pins through publication.
                if (parents_.size() == 8) parents_.erase(parents_.begin());
                parents_.emplace(Parent, Pins.Value()); return Pins;
            }
        private:
            std::mutex mutex_;
            std::map<std::wstring, ParentPins> parents_;
            std::map<std::string, std::unique_ptr<PreparedRemoval>> prepared_;
            std::map<std::wstring, bool> roots_, ntfs_;
            bool reusableRoot(const NativePath& Path)
            {
                if (const auto Found = roots_.find(Path.root_); Found != roots_.end()) return Found->second;
                wchar_t FileSystem[32]{}; const auto Type = GetDriveTypeW(Path.root_.c_str());
                const bool Reusable = (Type == DRIVE_FIXED || Type == DRIVE_REMOVABLE || Type == DRIVE_RAMDISK)
                    && GetVolumeInformationW(Path.root_.c_str(), nullptr, 0, nullptr, nullptr, nullptr,
                        FileSystem, DWORD(std::size(FileSystem)))
                    && (!_wcsicmp(FileSystem, L"NTFS") || !_wcsicmp(FileSystem, L"ReFS"));
                roots_.emplace(Path.root_, Reusable); ntfs_.emplace(Path.root_, Reusable && !_wcsicmp(FileSystem, L"NTFS"));
                return Reusable;
            }
        };
        Result<ParentPins> PinParents(const NativePath& Path, const ProviderId& Id, const OperationContext& Context = {}, bool Removal = false)
        {
            // Strong directory pins are retained only for removals. A rename
            // into a pinned directory needs write sharing on that parent.
            if (Removal && Id.value_ == "local") if (const auto Scope = std::dynamic_pointer_cast<NativeMutationScope>(Context.mutationScope_))
                return Scope->Pin(Path, Id);
            return OpenParents(Path, Id);
        }
        void ReleaseDirectoryPins(const EntrySnapshot& Entry, const OperationContext& Context)
        {
            // A previously visited child pins this directory. Release cached
            // ancestors before deleting/renaming the directory itself; active
            // write sessions keep their independent references until commit.
            if (Entry.directory_ && Context.mutationScope_) {
                if (const auto Scope = std::dynamic_pointer_cast<NativeMutationScope>(Context.mutationScope_)) {
                    if (Entry.locator_.kind_ == LocatorKind::Local) Scope->ReleaseDirectory(Entry.locator_);
                } else Context.mutationScope_->Clear();
            }
        }
        std::string Identity(const BY_HANDLE_FILE_INFORMATION& Info)
        {
            // NTFS name tunneling can replace creation time on publication.
            // The volume/file ID remains stable across both staging renames.
            return std::to_string(Info.dwVolumeSerialNumber) + ":" + std::to_string(Info.nFileIndexHigh) + ":"
                + std::to_string(Info.nFileIndexLow);
        }
        Result<Unit> CheckEntry(HANDLE File, const EntrySnapshot& Expected, const ProviderId& Id, BY_HANDLE_FILE_INFORMATION* Verified = nullptr)
        {
            BY_HANDLE_FILE_INFORMATION Info{};
            if (!GetFileInformationByHandle(File, &Info)) return Failure(GetLastError(), Id);
            if (Info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) return Error{ErrorCategory::Unsupported, "reparse-entry-denied", Id};
            if (Expected.id_.providerId_ != Id || !Expected.id_.revision_
                || !native::MatchesRevision(*Expected.id_.revision_, Info, File, std::filesystem::u8path(Expected.locator_.root_).wstring())
                || Expected.directory_ != bool(Info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                return Error{ErrorCategory::StaleRevision, "revision-changed", Id, Expected.id_, false, "refresh"};
            if (Verified) *Verified = Info;
            return Unit{};
        }
        Result<std::string> RecoveryIdentity(HANDLE File, const std::wstring& Path, bool Content,
            const BY_HANDLE_FILE_INFORMATION* KnownInfo = nullptr)
        {
            BY_HANDLE_FILE_INFORMATION Info{};
            if (KnownInfo) Info = *KnownInfo;
            else if (!GetFileInformationByHandle(File, &Info)) return Failure(GetLastError(), {"write-recovery"}, Utf8(Path));
            if (!Content) return native::PersistentIdentity(File, Info);
            // The caller pins the object against writes/deletion. Content binds
            // recovery across reopen/rename when the transport has no stable ID.
            // Read through the already pinned handle. A second RDP read handle
            // can keep a cached file object alive and block subsequent cleanup.
            LARGE_INTEGER Position{};
            if (!SetFilePointerEx(File, {}, &Position, FILE_CURRENT)) return Failure(GetLastError(), {"write-recovery"}, Utf8(Path));
            const auto Restore = qScopeGuard([&] { SetFilePointerEx(File, Position, nullptr, FILE_BEGIN); });
            if (!SetFilePointerEx(File, {}, nullptr, FILE_BEGIN)) return Failure(GetLastError(), {"write-recovery"}, Utf8(Path));
            QCryptographicHash Hash(QCryptographicHash::Sha256);
            std::vector<char> Buffer(1024 * 1024);
            std::uint64_t Bytes = 0;
            for (;;) {
                DWORD Count = 0;
                if (!ReadFile(File, Buffer.data(), DWORD(Buffer.size()), &Count, nullptr)) return Failure(GetLastError(), {"write-recovery"}, Utf8(Path));
                if (!Count) break;
                Hash.addData(QByteArrayView(Buffer.data(), Count)); Bytes += Count;
            }
            if (Bytes != ((uint64_t(Info.nFileSizeHigh) << 32) | Info.nFileSizeLow))
                return Error{ErrorCategory::StaleRevision, "write-recovery-content-changed"};
            return "sha256:" + std::to_string(Bytes) + ":" + Hash.result().toHex().toStdString();
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
        // FileDispositionInfoEx; product headers need not expose the Ex ABI.
        constexpr auto DispositionExClass = static_cast<FILE_INFO_BY_HANDLE_CLASS>(21);
        constexpr DWORD DispositionDelete = 0x1, DispositionPosix = 0x2, DispositionIgnoreReadOnly = 0x10;
        bool LegacyDispositionOnly(DWORD Code)
        { return Code == ERROR_INVALID_PARAMETER || Code == ERROR_NOT_SUPPORTED || Code == ERROR_INVALID_FUNCTION; }
        // POSIX semantics unlink the name when this handle closes, even while an
        // indexer or scanner keeps its own handle open; delete-pending names
        // otherwise block removing the parent (ERROR_DIR_NOT_EMPTY). Measured on
        // NTFS and ReFS. Filesystems without the Ex class keep legacy semantics.
        bool DisposeHandle(HANDLE File, bool IgnoreReadOnly = false)
        {
            DWORD Flags = DispositionDelete | DispositionPosix | (IgnoreReadOnly ? DispositionIgnoreReadOnly : 0);
            if (SetFileInformationByHandle(File, DispositionExClass, &Flags, sizeof(Flags))) return true;
            if (IgnoreReadOnly || !LegacyDispositionOnly(GetLastError())) return false;
            FILE_DISPOSITION_INFO Info{TRUE};
            return SetFileInformationByHandle(File, FileDispositionInfo, &Info, sizeof(Info)) != FALSE;
        }
        Result<Unit> DeleteHandle(HANDLE File, const ProviderId& Id)
        {
            if (!DisposeHandle(File)) return Failure(GetLastError(), Id);
            return Unit{};
        }
        // NtCreateFile dispositions and options used by the tree engines.
        constexpr ULONG RelativeOpen = 1, RelativeCreate = 2;
        constexpr ULONG RelativeDirectory = 0x1, RelativeSynchronous = 0x20, RelativeNonDirectory = 0x40,
            RelativeBackupIntent = 0x4000, RelativeOpenReparsePoint = 0x200000;
        // One component relative to an open directory handle: no path parsing
        // from the volume root and no dependence on the directory's current name.
        NTSTATUS CreateRelative(HANDLE Parent, std::wstring_view Name, ACCESS_MASK Access, ULONG Share, ULONG Disposition, ULONG Options,
            Handle& Opened, ULONG FileAttributes = 0)
        {
            using Open = NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK,
                PLARGE_INTEGER, ULONG, ULONG, ULONG, ULONG, PVOID, ULONG);
            static const auto Create = reinterpret_cast<Open>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtCreateFile"));
            constexpr NTSTATUS NotImplemented = static_cast<NTSTATUS>(0xC0000002L);
            if (!Create || Name.empty() || Name.size() > 255) return NotImplemented;
            UNICODE_STRING Text{}; Text.Length = Text.MaximumLength = USHORT(Name.size() * sizeof(wchar_t));
            Text.Buffer = const_cast<wchar_t*>(Name.data());
            OBJECT_ATTRIBUTES Attributes{}; Attributes.Length = sizeof(Attributes);
            Attributes.RootDirectory = Parent; Attributes.ObjectName = &Text;
            Attributes.Attributes = 0x40; // OBJ_CASE_INSENSITIVE
            IO_STATUS_BLOCK Status{}; HANDLE File = nullptr;
            const auto Result = Create(&File, Access, &Attributes, &Status, nullptr, FileAttributes, Share, Disposition, Options, nullptr, 0);
            if (Result >= 0 && File) Opened = Handle(File);
            return Result;
        }
        NTSTATUS OpenRelative(HANDLE Parent, std::wstring_view Name, ACCESS_MASK Access, ULONG Options, Handle& Opened)
        {
            return CreateRelative(Parent, Name, Access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, RelativeOpen, Options, Opened);
        }
        DWORD Win32Code(NTSTATUS Status)
        {
            using Convert = ULONG(NTAPI*)(NTSTATUS);
            static const auto ToDos = reinterpret_cast<Convert>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlNtStatusToDosError"));
            return ToDos ? DWORD(ToDos(Status)) : DWORD(ERROR_GEN_FAILURE);
        }
        // FILE_ID_EXTD_DIR_INFO (Windows 8 ABI), one 64 KiB listing per call.
        // Product headers keep an older declaration guard for these classes.
        struct DirectoryListing
        {
            ULONG nextEntryOffset_, fileIndex_;
            LARGE_INTEGER creationTime_, lastAccessTime_, lastWriteTime_, changeTime_, endOfFile_, allocationSize_;
            ULONG fileAttributes_, fileNameLength_, eaSize_, reparsePointTag_;
            BYTE fileId_[16];
            WCHAR fileName_[1];
        };
        static_assert(offsetof(DirectoryListing, fileName_) == 88);
        constexpr auto DirectoryListingClass = static_cast<FILE_INFO_BY_HANDLE_CLASS>(19);
        constexpr auto DirectoryRestartClass = static_cast<FILE_INFO_BY_HANDLE_CLASS>(20);
        constexpr DWORD DirectoryListAccess = FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | SYNCHRONIZE;
        constexpr DWORD ListingBytes = 64 * 1024;
        // Visits every child except dot entries. False when Visit stops it or the
        // listing fails; a complete listing returns true.
        template<class F> bool ListDirectory(HANDLE Directory, LONGLONG* Buffer, F&& Visit)
        {
            for (bool First = true;; First = false) {
                if (!GetFileInformationByHandleEx(Directory, First ? DirectoryRestartClass : DirectoryListingClass, Buffer, ListingBytes))
                    return GetLastError() == ERROR_NO_MORE_FILES;
                for (auto* Info = reinterpret_cast<const DirectoryListing*>(Buffer);;
                    Info = reinterpret_cast<const DirectoryListing*>(reinterpret_cast<const BYTE*>(Info) + Info->nextEntryOffset_)) {
                    const std::wstring_view Name(Info->fileName_, Info->fileNameLength_ / sizeof(wchar_t));
                    if (Name != L"." && Name != L".." && !Visit(Name, *Info)) return false;
                    if (!Info->nextEntryOffset_) break;
                }
            }
        }
        std::int64_t UtcMilliseconds(LARGE_INTEGER Time) { return std::int64_t(std::uint64_t(Time.QuadPart) / 10000) - 11644473600000LL; }
        // A child entry as the reader's own session would report it, without a
        // revision: later per-item handling queries the entry again.
        std::optional<EntrySnapshot> ListedEntry(const ProviderId& Id, const std::string& Prefix, const std::string& Parent,
            std::wstring_view Name, const DirectoryListing& Info)
        {
            const int Count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, Name.data(), int(Name.size()), nullptr, 0, nullptr, nullptr);
            if (Count <= 0) return std::nullopt;
            EntrySnapshot Entry; Entry.name_.resize(std::size_t(Count));
            WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, Name.data(), int(Name.size()), Entry.name_.data(), Count, nullptr, nullptr);
            Entry.locator_ = {LocatorKind::Local, Parent + (Parent.ends_with('\\') ? "" : "\\") + Entry.name_};
            Entry.id_ = {Id, Prefix + Serialize(Entry.locator_), IdentityScope::NavigationSession, std::nullopt};
            Entry.attributes_ = Info.fileAttributes_; Entry.directory_ = (Info.fileAttributes_ & FILE_ATTRIBUTE_DIRECTORY) != 0;
            Entry.size_ = Entry.directory_ ? 0 : std::uint64_t(Info.endOfFile_.QuadPart);
            Entry.createdUtcMs_ = UtcMilliseconds(Info.creationTime_); Entry.modifiedUtcMs_ = UtcMilliseconds(Info.lastWriteTime_);
            return Entry;
        }
        // Removes one directory tree for a coordinator that already verified the
        // selected root. Descendants are deleted as listed, without per-file
        // revision checks, and never through links: every open names one
        // component relative to its parent handle with FILE_OPEN_REPARSE_POINT,
        // and reparse entries are returned untouched. One worker owns a directory
        // at a time because the filesystem serializes one directory's updates;
        // separate directories proceed in parallel. Delete measurements, 30,000
        // files on NVMe/ReFS: four workers splitting one folder 18,000 files/s,
        // one worker per folder 26,000, four folders at once 64,000.
        class NativeTreeRemoval final : public ITreeRemoval
        {
        public:
            NativeTreeRemoval(ProviderId Id, std::string Prefix, TreeRemovalOptions Options, const OperationContext& Context)
                : id_(std::move(Id)), prefix_(std::move(Prefix)), options_(Options), pause_(Context.pauseRequested_), external_(Context.stopToken_) {}
            ~NativeTreeRemoval() override { Release(); }
            Result<Unit> Start(const EntrySnapshot& Root, const NativePath& Path)
            {
                // The root is resolved by path once, below pinned ancestors.
                // Everything else is opened relative to a handle of this tree.
                auto Parents = OpenParents(Path, id_, true);
                if (!Parents.HasValue()) Parents = OpenParents(Path, id_);
                if (!Parents.HasValue()) return Parents.Failure();
                parents_ = std::move(Parents.Value());
                auto Top = std::make_shared<Node>(); Top->entry_ = Root;
                const auto Open = [&](DWORD Access) {
                    return Handle(CreateFileW(Path.path_.c_str(), Access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
                };
                Top->handle_ = Open(DirectoryAccess | DELETE);
                if (Top->handle_.value_ == INVALID_HANDLE_VALUE) { Top->handle_ = Open(DirectoryAccess); Top->deletable_ = false; }
                if (Top->handle_.value_ == INVALID_HANDLE_VALUE) return Failure(GetLastError(), id_);
                BY_HANDLE_FILE_INFORMATION Info{};
                if (!GetFileInformationByHandle(Top->handle_.value_, &Info)) return Failure(GetLastError(), id_);
                if (!(Info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (Info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
                    || !Root.id_.revision_ || !native::MatchesRevision(*Root.id_.revision_, Info, Top->handle_.value_, Path.path_))
                    return Error{ErrorCategory::StaleRevision, "revision-changed", id_, Root.id_, false, "refresh"};
                Top->entry_.attributes_ = Info.dwFileAttributes;
                root_ = Top; queue_.push_back(std::move(Top));
                if (external_.stop_possible()) stopLink_.emplace(external_, [this] {
                    stop_.request_stop(); { std::lock_guard Lock(mutex_); } changed_.notify_all();
                });
                for (std::size_t Index = 0; Index < std::clamp<std::size_t>(options_.workers_, 1, 8); ++Index)
                    workers_.emplace_back([this] { work(); });
                return Unit{};
            }
            Result<TreeRemovalBatch> Collect(std::size_t MaxItems, std::chrono::milliseconds Wait) override
            {
                TreeRemovalBatch Batch;
                {
                    std::unique_lock Lock(mutex_);
                    changed_.wait_for(Lock, Wait, [&] { return removed_.size() >= MaxItems || finished_ || stop_.stop_requested(); });
                    const auto Count = std::min(MaxItems, removed_.size());
                    Batch.removed_.assign(std::make_move_iterator(removed_.begin()), std::make_move_iterator(removed_.begin() + Count));
                    removed_.erase(removed_.begin(), removed_.begin() + Count);
                    if (finished_ && removed_.empty() && !delivered_) {
                        delivered_ = true; Batch.finished_ = true;
                        Batch.deferred_ = std::move(deferred_); Batch.remaining_ = std::move(remaining_);
                    }
                    if (error_ && Batch.removed_.empty() && removed_.empty()) return *error_;
                }
                changed_.notify_all();
                if (Batch.finished_) settle(Batch);
                return Batch;
            }
            void Release() override
            {
                stop_.request_stop();
                { std::lock_guard Lock(mutex_); }
                changed_.notify_all();
                for (auto& Worker : workers_) if (Worker.joinable()) Worker.join();
                workers_.clear(); stopLink_.reset();
                { std::lock_guard Lock(mutex_); queue_.clear(); }
                root_.reset(); parents_.reset();
            }
        private:
            static constexpr DWORD DirectoryAccess = DirectoryListAccess;
            static constexpr auto ListingClass = DirectoryListingClass;
            static constexpr auto ListingRestartClass = DirectoryRestartClass;
            static constexpr ULONG Synchronous = RelativeSynchronous, DirectoryOnly = RelativeDirectory, NonDirectory = RelativeNonDirectory,
                BackupIntent = RelativeBackupIntent, OpenReparsePoint = RelativeOpenReparsePoint;
            // Unreported removals bound how far deletion runs ahead of the ledger.
            static constexpr std::size_t Capacity = 512, Credit = 16;
            using Listed = DirectoryListing;
            struct Node
            {
                std::shared_ptr<Node> parent_;
                Handle handle_;
                std::wstring name_;           // one component relative to the parent
                EntrySnapshot entry_;
                std::size_t depth_ = 0;
                std::size_t pending_ = 1;     // the listing itself plus unfinished children
                std::atomic<bool> keep_{false};
                bool deferred_ = false, deletable_ = true;
            };
            struct Worker
            {
                std::unique_ptr<LONGLONG[]> buffer_ = std::make_unique<LONGLONG[]>(64 * 1024 / sizeof(LONGLONG));
                std::vector<EntrySnapshot> removed_;
                std::size_t credits_ = 0;
            };
            bool stopped() const { return stop_.stop_requested(); }
            void work()
            {
                Worker State;
                try {
                    for (;;) {
                        std::shared_ptr<Node> Next;
                        {
                            std::unique_lock Lock(mutex_);
                            changed_.wait(Lock, [&] { return stopped() || finished_ || !queue_.empty(); });
                            if (stopped() || queue_.empty()) break;
                            // Depth first keeps the number of open directory handles small.
                            Next = std::move(queue_.back()); queue_.pop_back();
                        }
                        list(Next, State);
                    }
                } catch (...) {
                    { std::lock_guard Lock(mutex_); if (!error_) error_ = Error{ErrorCategory::Internal, "tree-removal-exception", id_}; }
                    stop_.request_stop();
                }
                // Completed removals are reported even when the work stops early.
                try { flush(State, true); } catch (...) {}
            }
            bool waitWhilePaused()
            {
                // A paused job stops collecting; stop starting removals as well.
                while (pause_ && pause_()) {
                    std::unique_lock Lock(mutex_);
                    if (changed_.wait_for(Lock, std::chrono::milliseconds(25), [&] { return stopped(); })) return false;
                }
                return !stopped();
            }
            void flush(Worker& State, bool ReturnCredits)
            {
                if (State.removed_.empty() && !(ReturnCredits && State.credits_)) return;
                {
                    std::lock_guard Lock(mutex_);
                    reserved_ -= State.removed_.size();
                    for (auto& Entry : State.removed_) removed_.push_back(std::move(Entry));
                    if (ReturnCredits) { reserved_ -= State.credits_; State.credits_ = 0; }
                }
                State.removed_.clear(); changed_.notify_all();
            }
            bool acquireCredit(Worker& State)
            {
                if (State.credits_) return true;
                flush(State, false);
                std::unique_lock Lock(mutex_);
                changed_.wait(Lock, [&] { return stopped() || removed_.size() + reserved_ < Capacity; });
                if (stopped()) return false;
                State.credits_ = std::min(Credit, Capacity - removed_.size() - reserved_); reserved_ += State.credits_;
                return true;
            }
            std::optional<EntrySnapshot> entryFor(const Node& Parent, std::wstring_view Name, const Listed& Info) const
            {
                return ListedEntry(id_, prefix_, Parent.entry_.locator_.root_, Name, Info);
            }
            void defer(EntrySnapshot Entry, std::size_t Depth)
            {
                std::lock_guard Lock(mutex_); deferred_.push_back({std::move(Entry), Depth});
            }
            bool openDirectory(Node& Current)
            {
                const auto Parent = Current.parent_->handle_.value_;
                const auto Options = DirectoryOnly | Synchronous | BackupIntent | OpenReparsePoint;
                if (OpenRelative(Parent, Current.name_, DirectoryAccess | DELETE, Options, Current.handle_) < 0) {
                    // List and empty it anyway; the coordinator retries removal.
                    if (OpenRelative(Parent, Current.name_, DirectoryAccess, Options, Current.handle_) < 0) return false;
                    Current.deletable_ = false;
                }
                // The name may now designate a junction or symbolic link. Never
                // enumerate through it; the per-item path reports such entries.
                FILE_ATTRIBUTE_TAG_INFO Tag{};
                if (!GetFileInformationByHandleEx(Current.handle_.value_, FileAttributeTagInfo, &Tag, sizeof(Tag))
                    || !(Tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (Tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
                    Current.handle_.Close(); return false;
                }
                Current.entry_.attributes_ = Tag.FileAttributes;
                return true;
            }
            bool removeFile(HANDLE Directory, std::wstring_view Name, bool ReadOnly)
            {
                Handle File;
                if (OpenRelative(Directory, Name, DELETE | SYNCHRONIZE, NonDirectory | Synchronous | BackupIntent | OpenReparsePoint, File) < 0) return false;
                return DisposeHandle(File.value_, ReadOnly);
            }
            void list(const std::shared_ptr<Node>& Current, Worker& State)
            {
                if (!waitWhilePaused()) return;
                if (Current->handle_.value_ == INVALID_HANDLE_VALUE && !openDirectory(*Current)) {
                    // Open/identity errors, prompts and elevation stay with the
                    // per-item path; it reopens and reports this directory.
                    Current->deferred_ = true; defer(Current->entry_, Current->depth_);
                    finish(Current, State); return;
                }
                bool First = true, Failed = false;
                std::size_t Seen = 0;
                const auto Bytes = DWORD(64 * 1024);
                while (!Failed) {
                    if (!GetFileInformationByHandleEx(Current->handle_.value_, First ? ListingRestartClass : ListingClass, State.buffer_.get(), Bytes)) {
                        Failed = GetLastError() != ERROR_NO_MORE_FILES;
                        break;
                    }
                    First = false;
                    for (auto* Info = reinterpret_cast<const Listed*>(State.buffer_.get());;
                        Info = reinterpret_cast<const Listed*>(reinterpret_cast<const BYTE*>(Info) + Info->nextEntryOffset_)) {
                        if (stopped()) { flush(State, false); return; }
                        if (++Seen % 64 == 0 && !waitWhilePaused()) { flush(State, false); return; }
                        const std::wstring_view Name(Info->fileName_, Info->fileNameLength_ / sizeof(wchar_t));
                        if (Name != L"." && Name != L"..") {
                            auto Entry = entryFor(*Current, Name, *Info);
                            // A name UTF-8 cannot carry fails the same way in the
                            // per-item listing, which reports it for this folder.
                            if (!Entry) { Failed = true; break; }
                            const auto Depth = Current->depth_ + 1;
                            const auto Attributes = Info->fileAttributes_;
                            if (Depth > options_.maxDepth_ || (Attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
                                Current->keep_ = true; defer(std::move(*Entry), Depth);
                            } else if (Attributes & FILE_ATTRIBUTE_DIRECTORY) {
                                auto Child = std::make_shared<Node>();
                                Child->parent_ = Current; Child->name_.assign(Name); Child->entry_ = std::move(*Entry); Child->depth_ = Depth;
                                { std::lock_guard Lock(mutex_); ++Current->pending_; queue_.push_back(std::move(Child)); }
                                changed_.notify_one();
                            } else if ((Attributes & FILE_ATTRIBUTE_READONLY) && !options_.allowReadOnly_) {
                                Current->keep_ = true; defer(std::move(*Entry), Depth);
                            } else {
                                if (!acquireCredit(State)) return;
                                if (removeFile(Current->handle_.value_, Name, (Attributes & FILE_ATTRIBUTE_READONLY) != 0)) {
                                    --State.credits_; State.removed_.push_back(std::move(*Entry));
                                    if (State.removed_.size() >= Credit) flush(State, false);
                                } else {
                                    // Locks, access and races get the per-item
                                    // path's verification, prompts and retries.
                                    Current->keep_ = true; defer(std::move(*Entry), Depth);
                                }
                            }
                        }
                        if (!Info->nextEntryOffset_) break;
                    }
                }
                flush(State, false);
                if (Failed) { Current->deferred_ = true; defer(Current->entry_, Current->depth_); }
                finish(Current, State);
            }
            void finish(std::shared_ptr<Node> Current, Worker& State)
            {
                // The listing worker and then the last child to finish complete a
                // directory, so removal is post-order without a separate pass.
                for (;;) {
                    { std::lock_guard Lock(mutex_); if (--Current->pending_) return; }
                    if (stopped()) return;
                    const bool Kept = Current->keep_ || Current->deferred_;
                    const bool ReadOnly = (Current->entry_.attributes_ & FILE_ATTRIBUTE_READONLY) != 0;
                    bool Removed = false;
                    if (!Kept && Current->deletable_ && (!ReadOnly || options_.allowReadOnly_)) {
                        // A name added meanwhile keeps the folder; its handle then
                        // still supplies the revision for the verified removal.
                        Removed = DisposeHandle(Current->handle_.value_, ReadOnly);
                        if (Removed) Current->handle_.Close();
                    }
                    if (Removed) {
                        flush(State, false);
                        std::lock_guard Lock(mutex_); removed_.push_back(Current->entry_);
                    } else if (!Current->deferred_) {
                        // Kept for the coordinator's verified removal after the
                        // per-item entries below it have been resolved.
                        auto Entry = Current->entry_;
                        Entry.id_.revision_.reset();
                        BY_HANDLE_FILE_INFORMATION Info{};
                        if (Current->handle_.value_ != INVALID_HANDLE_VALUE && GetFileInformationByHandle(Current->handle_.value_, &Info)) {
                            Entry.attributes_ = Info.dwFileAttributes;
                            Entry.id_.revision_ = native::HandleRevision(Current->handle_.value_, Info, std::filesystem::u8path(Entry.locator_.root_).wstring());
                        }
                        std::lock_guard Lock(mutex_); remaining_.push_back({std::move(Entry), Current->depth_});
                    }
                    Current->handle_.Close();
                    auto Parent = Current->parent_;
                    if (!Parent) {
                        { std::lock_guard Lock(mutex_); finished_ = true; }
                        changed_.notify_all(); return;
                    }
                    if (Removed == false) Parent->keep_ = true;
                    Current = std::move(Parent);
                }
            }
            void settle(TreeRemovalBatch& Batch) const
            {
                // A deferred directory is listed again by the per-item path; drop
                // what this engine reported beneath it to avoid duplicates.
                std::vector<std::string> Folders;
                for (const auto& Item : Batch.deferred_) if (Item.entry_.directory_) Folders.push_back(Item.entry_.locator_.root_ + "\\");
                const auto Beneath = [&](const TreeRemovalEntry& Item) {
                    return std::ranges::any_of(Folders, [&](const std::string& Folder) {
                        return Item.entry_.locator_.root_.size() > Folder.size()
                            && !_strnicmp(Item.entry_.locator_.root_.c_str(), Folder.c_str(), Folder.size());
                    });
                };
                std::erase_if(Batch.deferred_, Beneath); std::erase_if(Batch.remaining_, Beneath);
                // Ancestors first: the coordinator's final pass runs in reverse.
                std::ranges::stable_sort(Batch.remaining_, {}, &TreeRemovalEntry::depth_);
            }
            ProviderId id_;
            std::string prefix_;
            TreeRemovalOptions options_;
            std::function<bool()> pause_;
            std::stop_token external_;
            std::stop_source stop_;
            std::optional<std::stop_callback<std::function<void()>>> stopLink_;
            ParentPins parents_;
            std::shared_ptr<Node> root_;
            std::mutex mutex_;
            std::condition_variable changed_;
            std::vector<std::shared_ptr<Node>> queue_;
            std::deque<EntrySnapshot> removed_;
            std::vector<TreeRemovalEntry> deferred_, remaining_;
            std::size_t reserved_ = 0;
            bool finished_ = false, delivered_ = false;
            std::optional<Error> error_;
            std::vector<std::thread> workers_;
        };
        // Copies, moves or merges one directory tree for a coordinator that
        // already verified the selected root (see ITreeTransfer). Folders are
        // opened, listed and created one component relative to a folder handle
        // of this tree, with FILE_OPEN_REPARSE_POINT; listed links are returned
        // to the per-item path. A folder is listed by one worker; its files are
        // shared by all workers in groups, so one large folder still copies in
        // parallel the way robocopy /MT does. Files are copied with CopyFile2
        // and renamed with MoveFileExW, neither of which follows a final link.
        // A move removes each source file only after its copy is complete and
        // removes a folder once every child left it.
        class NativeTreeTransfer final : public ITreeTransfer
        {
        public:
            NativeTreeTransfer(ProviderId Id, std::string Prefix, TreeTransferOptions Options, const OperationContext& Context)
                : id_(std::move(Id)), prefix_(std::move(Prefix)), options_(Options), pause_(Context.pauseRequested_), external_(Context.stopToken_) {}
            ~NativeTreeTransfer() override { Release(); }
            Result<Unit> Start(const EntrySnapshot& Root, const NativePath& Source, const NativePath& Destination, const Locator& DestinationAt)
            {
                // The source root is resolved by path once, below pinned ancestors.
                auto Parents = OpenParents(Source, id_, true);
                if (!Parents.HasValue()) Parents = OpenParents(Source, id_);
                if (!Parents.HasValue()) return Parents.Failure();
                sourceParents_ = std::move(Parents.Value());
                // Destination ancestors are checked for links, not retained: a
                // retained directory pin refuses renames into that folder.
                const auto Above = OpenParents(Destination, id_);
                if (!Above.HasValue()) return Above.Failure();
                auto Top = std::make_shared<Node>();
                Top->entry_ = Root; Top->destination_ = DestinationAt;
                Top->sourcePath_ = Source.path_; Top->destinationPath_ = Destination.path_;
                const auto Open = [](const std::wstring& Path, DWORD Access) {
                    return Handle(CreateFileW(Path.c_str(), Access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
                };
                Top->source_ = Open(Source.path_, DirectoryListAccess | (moving() ? DELETE : 0));
                if (Top->source_.value_ == INVALID_HANDLE_VALUE && moving()) { Top->source_ = Open(Source.path_, DirectoryListAccess); Top->deletable_ = false; }
                if (Top->source_.value_ == INVALID_HANDLE_VALUE) return Failure(GetLastError(), id_);
                BY_HANDLE_FILE_INFORMATION Info{};
                if (!GetFileInformationByHandle(Top->source_.value_, &Info)) return Failure(GetLastError(), id_);
                if (!(Info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (Info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
                    || !Root.id_.revision_ || !native::MatchesRevision(*Root.id_.revision_, Info, Top->source_.value_, Source.path_))
                    return Error{ErrorCategory::StaleRevision, "revision-changed", id_, Root.id_, false, "refresh"};
                Top->entry_.attributes_ = Info.dwFileAttributes;
                if (options_.mode_ == TreeTransferMode::Merge) {
                    Top->target_ = Open(Destination.path_, TargetAccess);
                    if (Top->target_.value_ == INVALID_HANDLE_VALUE) return Failure(GetLastError(), id_, Utf8(Destination.path_));
                    if (!plainDirectory(Top->target_.value_)) return Error{ErrorCategory::StaleRevision, "destination-not-directory", id_};
                } else {
                    // A new destination root: everything below it is then written
                    // by this engine alone and an interrupted file can be removed.
                    const std::filesystem::path Target(Destination.path_);
                    Handle Parent = Open(Target.parent_path().wstring(), TargetAccess);
                    if (Parent.value_ == INVALID_HANDLE_VALUE) return Failure(GetLastError(), id_, Utf8(Target.parent_path().wstring()));
                    const auto Name = Target.filename().wstring();
                    const auto Created = CreateRelative(Parent.value_, Name, TargetAccess, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        RelativeCreate, RelativeDirectory | RelativeSynchronous | RelativeBackupIntent | RelativeOpenReparsePoint, Top->target_);
                    if (Created < 0) return Failure(Win32Code(Created), id_, Utf8(Destination.path_));
                }
                root_ = Top;
                if (external_.stop_possible()) stopLink_.emplace(external_, [this] {
                    stop_.request_stop(); { std::lock_guard Lock(mutex_); } changed_.notify_all();
                });
                directories_.push_back(Top);
                for (std::size_t Index = 0; Index < std::clamp<std::size_t>(options_.workers_, 1, 16); ++Index)
                    workers_.emplace_back([this] { work(); });
                // Totals for progress come from one listing thread beside the
                // transfer, which starts at once as robocopy does.
                // Not for a merge: renames move no bytes, and listing the same
                // folders beside them slowed qt-repo from 2.7 s to 3.7 s.
                if (options_.countFirst_ && options_.mode_ != TreeTransferMode::Merge)
                    counter_ = std::thread([this, Path = Source.path_] { count(Path); });
                return Unit{};
            }
            Result<TreeTransferBatch> Collect(std::size_t MaxItems, std::chrono::milliseconds Wait) override
            {
                TreeTransferBatch Batch;
                {
                    std::unique_lock Lock(mutex_);
                    changed_.wait_for(Lock, Wait, [&] { return done_.size() >= MaxItems || finished_ || stop_.stop_requested(); });
                    const auto Count = std::min(MaxItems, done_.size());
                    Batch.done_.assign(std::make_move_iterator(done_.begin()), std::make_move_iterator(done_.begin() + Count));
                    done_.erase(done_.begin(), done_.begin() + Count);
                    if (counted_) { Batch.countedBytes_ = countedBytes_; Batch.countedItems_ = countedItems_; }
                    if (finished_ && done_.empty() && !delivered_) {
                        delivered_ = true; Batch.finished_ = true;
                        Batch.deferred_ = std::move(deferred_); Batch.remaining_ = std::move(remaining_);
                    }
                    if (error_ && Batch.done_.empty() && done_.empty()) return *error_;
                }
                Batch.bytes_ = bytes_.load();
                changed_.notify_all();
                if (Batch.finished_) settle(Batch);
                return Batch;
            }
            void Release() override
            {
                stop_.request_stop();
                { std::lock_guard Lock(mutex_); }
                changed_.notify_all();
                for (auto& Worker : workers_) if (Worker.joinable()) Worker.join();
                if (counter_.joinable()) counter_.join();
                workers_.clear(); stopLink_.reset();
                { std::lock_guard Lock(mutex_); directories_.clear(); chunks_.clear(); }
                root_.reset(); sourceParents_.reset();
            }
        private:
            static constexpr DWORD TargetAccess = DirectoryListAccess | FILE_ADD_FILE | FILE_ADD_SUBDIRECTORY | FILE_TRAVERSE;
            static constexpr ULONG FolderOptions = RelativeDirectory | RelativeSynchronous | RelativeBackupIntent | RelativeOpenReparsePoint;
            // Unreported results bound how far the engine runs ahead of the ledger.
            static constexpr std::size_t Capacity = 512, Credit = 16;
            // Files are shared between workers in groups of this size or bytes.
            static constexpr std::size_t GroupFiles = 64;
            static constexpr std::uint64_t GroupBytes = 32ull * 1024 * 1024;
            // Unbuffered from this file size, once the transfer is known to be at
            // least UnbufferedTransfer: then write-back throttles. NVMe to NVMe,
            // 2-6 GiB files: 2.2 s against 3.6 s buffered (robocopy 3.3 s), and
            // the data is durable on return. From 32 MiB instead, 9.3 GiB of
            // 100-500 MiB files took 1.52 s against 1.25 s. A 1.4 GiB transfer
            // fits the cache: 156 ms buffered against 232 ms unbuffered.
            static constexpr std::uint64_t UnbufferedFile = 256ull * 1024 * 1024;
            static constexpr std::uint64_t UnbufferedTransfer = 2ull * 1024 * 1024 * 1024;
            struct Node
            {
                std::shared_ptr<Node> parent_;
                Handle source_, target_;
                std::wstring name_;            // one component, the same on both sides
                std::wstring sourcePath_, destinationPath_;
                EntrySnapshot entry_;
                Locator destination_;
                std::size_t depth_ = 0;
                std::size_t pending_ = 1;      // the listing plus unfinished children and file groups
                std::atomic<bool> keep_{false};
                bool deferred_ = false, deletable_ = true;
            };
            struct File
            {
                std::wstring name_;
                EntrySnapshot entry_;
                // A move deletes the source only while it is still this file.
                BYTE fileId_[16]{};
                LONGLONG lastWriteTime_ = 0;
            };
            struct Group { std::shared_ptr<Node> node_; std::vector<File> files_; };
            struct Worker
            {
                std::unique_ptr<LONGLONG[]> listing_ = std::make_unique<LONGLONG[]>(ListingBytes / sizeof(LONGLONG));
                std::vector<TreeTransferEntry> done_;
                std::size_t credits_ = 0;
            };
            enum class Copied { Done, Deferred, Stopped };
            bool moving() const { return options_.mode_ != TreeTransferMode::Copy; }
            bool stopped() const { return stop_.stop_requested(); }
            static bool plainDirectory(HANDLE Directory)
            {
                FILE_ATTRIBUTE_TAG_INFO Tag{};
                return GetFileInformationByHandleEx(Directory, FileAttributeTagInfo, &Tag, sizeof(Tag))
                    && (Tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(Tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT);
            }
            static Locator child(Locator Parent, const std::string& Name)
            {
                Parent.root_ += (Parent.root_.ends_with('\\') ? "" : "\\") + Name; return Parent;
            }
            void work()
            {
                Worker State;
                try {
                    for (;;) {
                        std::shared_ptr<Node> Folder; Group Files;
                        {
                            operations::OperationMetric Idle("transfer.idle", 0, true);
                            std::unique_lock Lock(mutex_);
                            changed_.wait(Lock, [&] { return stopped() || finished_ || !chunks_.empty() || !directories_.empty(); });
                            if (stopped() || (chunks_.empty() && directories_.empty())) break;
                            // File groups first: they finish listed folders and
                            // release handles. Folders depth first.
                            if (!chunks_.empty()) { Files = std::move(chunks_.front()); chunks_.pop_front(); }
                            else { Folder = std::move(directories_.back()); directories_.pop_back(); }
                        }
                        if (Folder) options_.mode_ == TreeTransferMode::Merge ? merge(Folder, State) : list(Folder, State);
                        else copy(Files, State);
                    }
                } catch (...) {
                    { std::lock_guard Lock(mutex_); if (!error_) error_ = Error{ErrorCategory::Internal, "tree-transfer-exception", id_}; }
                    stop_.request_stop(); changed_.notify_all();
                }
                // Completed results are reported even when the work stops early.
                try { flush(State, true); } catch (...) {}
            }
            void count(const std::wstring& Root)
            {
                // Totals for progress only; the transfer lists again and decides.
                try {
                    const auto Buffer = std::make_unique<LONGLONG[]>(ListingBytes / sizeof(LONGLONG));
                    std::vector<std::pair<std::wstring, std::size_t>> Pending{{Root, 0}};
                    std::uint64_t Bytes = 0, Items = 1;
                    while (!Pending.empty() && !stopped() && !complete_) {
                        auto [Path, Depth] = std::move(Pending.back()); Pending.pop_back();
                        Handle Directory(CreateFileW(Path.c_str(), DirectoryListAccess, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                            OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
                        if (Directory.value_ == INVALID_HANDLE_VALUE || !plainDirectory(Directory.value_)) continue;
                        ListDirectory(Directory.value_, Buffer.get(), [&](std::wstring_view Name, const DirectoryListing& Info) {
                            ++Items;
                            if (!(Info.fileAttributes_ & FILE_ATTRIBUTE_DIRECTORY)) Bytes += std::uint64_t(Info.endOfFile_.QuadPart);
                            else if (!(Info.fileAttributes_ & FILE_ATTRIBUTE_REPARSE_POINT) && Depth + 1 < options_.maxDepth_)
                                Pending.emplace_back(Path + L"\\" + std::wstring(Name), Depth + 1);
                            return !stopped() && !complete_;
                        });
                    }
                    if (stopped() || complete_) return;
                    std::lock_guard Lock(mutex_);
                    countedBytes_ = Bytes; countedItems_ = Items; counted_ = true;
                    for (auto Known = transferBytes_.load(); Bytes > Known && !transferBytes_.compare_exchange_weak(Known, Bytes);) {}
                } catch (...) {}
            }
            bool waitWhilePaused()
            {
                // A paused job stops collecting; stop starting files as well.
                // The query takes the scheduler's lock: ask at most every 5 ms
                // across all workers, not once per file or buffer.
                if (stopped()) return false;
                if (!pause_) return true;
                const auto Now = std::chrono::steady_clock::now().time_since_epoch().count();
                auto Due = pauseDue_.load(std::memory_order_relaxed);
                if (Now < Due || !pauseDue_.compare_exchange_strong(Due, Now + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                        std::chrono::milliseconds(5)).count(), std::memory_order_relaxed)) return true;
                while (pause_()) {
                    std::unique_lock Lock(mutex_);
                    if (changed_.wait_for(Lock, std::chrono::milliseconds(25), [&] { return stopped(); })) return false;
                }
                return !stopped();
            }
            void flush(Worker& State, bool ReturnCredits)
            {
                if (State.done_.empty() && !(ReturnCredits && State.credits_)) return;
                {
                    std::lock_guard Lock(mutex_);
                    reserved_ -= State.done_.size();
                    for (auto& Entry : State.done_) done_.push_back(std::move(Entry));
                    if (ReturnCredits) { reserved_ -= State.credits_; State.credits_ = 0; }
                }
                State.done_.clear(); changed_.notify_all();
            }
            bool acquireCredit(Worker& State)
            {
                if (State.credits_) return true;
                flush(State, false);
                operations::OperationMetric Waiting("transfer.credit", 0, true);
                std::unique_lock Lock(mutex_);
                changed_.wait(Lock, [&] { return stopped() || done_.size() + reserved_ < Capacity; });
                if (stopped()) return false;
                State.credits_ = std::min(Credit, Capacity - done_.size() - reserved_); reserved_ += State.credits_;
                return true;
            }
            void report(Worker& State, TreeTransferEntry Entry)
            {
                // Callers hold a credit for every reported entry.
                --State.credits_; State.done_.push_back(std::move(Entry));
                if (State.done_.size() >= Credit) flush(State, false);
            }
            void defer(TreeTransferEntry Entry)
            {
                std::lock_guard Lock(mutex_); deferred_.push_back(std::move(Entry));
            }
            void deferFolder(const std::shared_ptr<Node>& Current, Worker& State)
            {
                // Open, listing and creation errors, prompts and elevation stay with
                // the per-item path; it lists this folder again by itself.
                Current->deferred_ = true;
                defer({Current->entry_, Current->destination_, Current->depth_});
                finish(Current, State);
            }
            bool openSource(Node& Current)
            {
                const auto Parent = Current.parent_->source_.value_;
                if (OpenRelative(Parent, Current.name_, DirectoryListAccess | (moving() ? DELETE : 0), FolderOptions, Current.source_) < 0) {
                    // List and empty it anyway; the coordinator retries its removal.
                    if (!moving() || OpenRelative(Parent, Current.name_, DirectoryListAccess, FolderOptions, Current.source_) < 0) return false;
                    Current.deletable_ = false;
                }
                // The name may now designate a junction or symbolic link. Never
                // enumerate through it; the per-item path reports such entries.
                if (!plainDirectory(Current.source_.value_)) { Current.source_.Close(); return false; }
                Current.entry_.attributes_ = [&] { FILE_BASIC_INFO Basic{};
                    return GetFileInformationByHandleEx(Current.source_.value_, FileBasicInfo, &Basic, sizeof(Basic)) ? Basic.FileAttributes : Current.entry_.attributes_; }();
                return true;
            }
            std::shared_ptr<Node> childNode(const std::shared_ptr<Node>& Current, std::wstring_view Name, EntrySnapshot Entry, std::size_t Depth) const
            {
                auto Child = std::make_shared<Node>();
                Child->parent_ = Current; Child->name_.assign(Name); Child->depth_ = Depth;
                Child->destination_ = child(Current->destination_, Entry.name_); Child->entry_ = std::move(Entry);
                Child->sourcePath_ = Current->sourcePath_ + L"\\" + Child->name_;
                Child->destinationPath_ = Current->destinationPath_ + L"\\" + Child->name_;
                return Child;
            }
            // Children that this engine never decides go to the per-item path.
            bool deferredChild(DWORD Attributes, std::size_t Depth) const
            {
                return Depth > options_.maxDepth_ || (Attributes & FILE_ATTRIBUTE_REPARSE_POINT)
                    || (moving() && !(Attributes & FILE_ATTRIBUTE_DIRECTORY) && (Attributes & FILE_ATTRIBUTE_READONLY) && !options_.allowReadOnly_);
            }
            void list(const std::shared_ptr<Node>& Current, Worker& State)
            {
                operations::OperationMetric Timing("transfer.folder", 0, true);
                if (!waitWhilePaused()) return;
                if (Current != root_ && !openSource(*Current)) { deferFolder(Current, State); return; }
                // List the whole folder before creating anything: a listing error
                // then leaves nothing behind for the per-item path to collide with.
                std::vector<File> Files; std::vector<std::shared_ptr<Node>> Folders; std::vector<TreeTransferEntry> Deferred;
                bool Unnamed = false;
                const auto Listed = ListDirectory(Current->source_.value_, State.listing_.get(), [&](std::wstring_view Name, const DirectoryListing& Info) {
                    auto Entry = ListedEntry(id_, prefix_, Current->entry_.locator_.root_, Name, Info);
                    if (!Entry) { Unnamed = true; return false; }
                    const auto Depth = Current->depth_ + 1;
                    if (deferredChild(Info.fileAttributes_, Depth)) {
                        auto Target = child(Current->destination_, Entry->name_);
                        Deferred.push_back({std::move(*Entry), std::move(Target), Depth});
                    } else if (Info.fileAttributes_ & FILE_ATTRIBUTE_DIRECTORY) Folders.push_back(childNode(Current, Name, std::move(*Entry), Depth));
                    else {
                        File Item{std::wstring(Name), std::move(*Entry)};
                        std::memcpy(Item.fileId_, Info.fileId_, sizeof(Item.fileId_)); Item.lastWriteTime_ = Info.lastWriteTime_.QuadPart;
                        Files.push_back(std::move(Item));
                    }
                    return !stopped();
                });
                if (stopped()) return;
                if (!Listed || Unnamed) { deferFolder(Current, State); return; }
                if (Current != root_ && CreateRelative(Current->parent_->target_.value_, Current->name_, TargetAccess,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, RelativeCreate, FolderOptions, Current->target_) < 0) {
                    deferFolder(Current, State); return;
                }
                if (!Deferred.empty()) {
                    Current->keep_ = true;
                    std::lock_guard Lock(mutex_); for (auto& Entry : Deferred) deferred_.push_back(std::move(Entry));
                }
                if (!moving()) {
                    // A copied folder is complete once created; a moved one once removed.
                    if (!acquireCredit(State)) return;
                    report(State, {Current->entry_, Current->destination_, Current->depth_});
                }
                std::uint64_t FolderBytes = 0;
                for (const auto& Item : Files) FolderBytes += Item.entry_.size_;
                for (auto Known = transferBytes_.load(); FolderBytes > Known && !transferBytes_.compare_exchange_weak(Known, FolderBytes);) {}
                {
                    std::lock_guard Lock(mutex_);
                    for (auto& Folder : Folders) { ++Current->pending_; directories_.push_back(std::move(Folder)); }
                    Group Next{Current, {}}; std::uint64_t Bytes = 0;
                    for (auto& Item : Files) {
                        if (!Next.files_.empty() && (Next.files_.size() >= GroupFiles || Bytes + Item.entry_.size_ > GroupBytes)) {
                            ++Current->pending_; chunks_.push_back(std::move(Next)); Next = Group{Current, {}}; Bytes = 0;
                        }
                        Bytes += Item.entry_.size_; Next.files_.push_back(std::move(Item));
                    }
                    if (!Next.files_.empty()) { ++Current->pending_; chunks_.push_back(std::move(Next)); }
                }
                changed_.notify_all();
                finish(Current, State);
            }
            void copy(Group& Files, Worker& State)
            {
                auto& Folder = *Files.node_;
                for (auto& Item : Files.files_) {
                    if (!waitWhilePaused() || !acquireCredit(State)) break;
                    const auto Depth = Folder.depth_ + 1;
                    const auto Outcome = transfer(Folder, Item, State);
                    if (Outcome == Copied::Stopped) break;
                    auto Target = child(Folder.destination_, Item.entry_.name_);
                    if (Outcome == Copied::Done) report(State, {std::move(Item.entry_), std::move(Target), Depth});
                    else { Folder.keep_ = true; defer({std::move(Item.entry_), std::move(Target), Depth}); }
                }
                flush(State, false);
                finish(Files.node_, State);
            }
            Copied targetFailure(DWORD Code, const std::wstring& Path)
            {
                // A full destination stops the whole transfer, as Explorer does;
                // other failures get the per-item path's prompts and retries.
                if (Code != ERROR_DISK_FULL && Code != ERROR_HANDLE_DISK_FULL) return Copied::Deferred;
                { std::lock_guard Lock(mutex_); if (!error_) error_ = Failure(Code, id_, Utf8(Path)); }
                stop_.request_stop(); changed_.notify_all();
                return Copied::Stopped;
            }
            static void takeBack(const std::wstring& Path)
            {
                // Only a copy this engine just created reaches here; CopyFile2 may
                // have made it readonly. Filesystems without the Ex disposition
                // take the legacy one.
                Handle Target(CreateFileW(Path.c_str(), DELETE | SYNCHRONIZE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                    OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
                if (Target.value_ == INVALID_HANDLE_VALUE) return;
                if (!DisposeHandle(Target.value_, true)) DisposeHandle(Target.value_);
            }
            Copied transfer(Node& Folder, File& Item, Worker&)
            {
                // CopyFile2 for every file: it block-clones on ReFS, pipelines
                // large files, and real-time scanning recognises a copy of an
                // already scanned file. Writing the data through our own handles
                // instead made closing each new file wait for a scan: 0.5 ms per
                // file with two workers and 2.5 ms with eight on qt-repo, slower
                // than a single CopyFile2 thread.
                operations::OperationMetric Timing("transfer.file", Item.entry_.size_, true);
                const auto SourcePath = Folder.sourcePath_ + L"\\" + Item.name_, TargetPath = Folder.destinationPath_ + L"\\" + Item.name_;
                struct Progress { NativeTreeTransfer* owner_; std::uint64_t reported_ = 0; } Callback{this};
                COPYFILE2_EXTENDED_PARAMETERS Parameters{}; Parameters.dwSize = sizeof(Parameters);
                constexpr DWORD SkipAlternateStreams = 0x00008000;
                Parameters.dwCopyFlags = COPY_FILE_FAIL_IF_EXISTS | SkipAlternateStreams
                    | (Item.entry_.size_ >= UnbufferedFile && transferBytes_.load() >= UnbufferedTransfer ? COPY_FILE_NO_BUFFERING : 0);
                Parameters.pvCallbackContext = &Callback;
                Parameters.pProgressRoutine = [](const COPYFILE2_MESSAGE* Message, void* Opaque) -> COPYFILE2_MESSAGE_ACTION {
                    auto& Callback = *static_cast<Progress*>(Opaque);
                    try {
                        if (!Callback.owner_->waitWhilePaused()) return COPYFILE2_PROGRESS_CANCEL;
                        if (Message->Type == COPYFILE2_CALLBACK_CHUNK_FINISHED) {
                            const auto Total = std::uint64_t(Message->Info.ChunkFinished.uliTotalBytesTransferred.QuadPart);
                            if (Total > Callback.reported_) { Callback.owner_->bytes_ += Total - Callback.reported_; Callback.reported_ = Total; }
                        }
                        return COPYFILE2_PROGRESS_CONTINUE;
                    } catch (...) { return COPYFILE2_PROGRESS_CANCEL; }
                };
                const auto Code = CopyFile2(SourcePath.c_str(), TargetPath.c_str(), &Parameters);
                if (FAILED(Code)) {
                    // CopyFile2 removes its own partial copy when it fails or is
                    // cancelled; an existing name is left alone.
                    bytes_ -= Callback.reported_;
                    if (stopped()) return Copied::Stopped;
                    return targetFailure(HRESULT_FACILITY(Code) == FACILITY_WIN32 ? DWORD(HRESULT_CODE(Code)) : DWORD(ERROR_GEN_FAILURE), TargetPath);
                }
                if (Item.entry_.size_ > Callback.reported_) bytes_ += Item.entry_.size_ - Callback.reported_;
                if (!moving()) return Copied::Done;
                // Remove the source only while it is still the listed, unchanged
                // file. Otherwise the copy goes too and the per-item path decides.
                Handle Removal(CreateFileW(SourcePath.c_str(), DELETE | FILE_READ_ATTRIBUTES | SYNCHRONIZE, FILE_SHARE_READ, nullptr,
                    OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
                // FileIdInfo (Windows 8 ABI); product PCHs keep an older NTDDI guard.
                struct FileIdentityInfo { ULONGLONG volumeSerial_; BYTE fileId_[16]; } Identity{};
                static_assert(sizeof(FileIdentityInfo) == 24);
                constexpr auto IdentityClass = static_cast<FILE_INFO_BY_HANDLE_CLASS>(18);
                FILE_BASIC_INFO Basic{}; FILE_STANDARD_INFO Length{};
                if (Removal.value_ == INVALID_HANDLE_VALUE
                    || !GetFileInformationByHandleEx(Removal.value_, IdentityClass, &Identity, sizeof(Identity))
                    || !GetFileInformationByHandleEx(Removal.value_, FileBasicInfo, &Basic, sizeof(Basic))
                    || !GetFileInformationByHandleEx(Removal.value_, FileStandardInfo, &Length, sizeof(Length))
                    || std::memcmp(Identity.fileId_, Item.fileId_, sizeof(Item.fileId_)) || Basic.LastWriteTime.QuadPart != Item.lastWriteTime_
                    || std::uint64_t(Length.EndOfFile.QuadPart) != Item.entry_.size_ || deferredChild(Basic.FileAttributes, 0)
                    || !DisposeHandle(Removal.value_, (Basic.FileAttributes & FILE_ATTRIBUTE_READONLY) != 0)) {
                    Removal.Close(); takeBack(TargetPath); bytes_ -= Item.entry_.size_;
                    return Copied::Deferred;
                }
                return Copied::Done;
            }
            void merge(const std::shared_ptr<Node>& Current, Worker& State)
            {
                operations::OperationMetric Timing("transfer.merge-folder", 0, true);
                if (!waitWhilePaused()) return;
                // No folder handle stays open while its children are renamed. With
                // both folders held, renames cost 0.6 ms each with two workers; with
                // only the source held, qt-repo took 3.7 s against 3.3 s without.
                // The source is checked for links here and reopened by path for its
                // removal once empty.
                const auto Plain = [](const std::wstring& Path) {
                    Handle Folder(CreateFileW(Path.c_str(), DirectoryListAccess, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
                    if (Folder.value_ != INVALID_HANDLE_VALUE && !plainDirectory(Folder.value_)) Folder.Close();
                    return Folder;
                };
                Current->source_.Close(); Current->target_.Close();
                auto Source = Plain(Current->sourcePath_);
                // The colliding destination folder, never a link: one attribute
                // query rather than an open per merged folder.
                const auto Target = Current == root_ ? DWORD(FILE_ATTRIBUTE_DIRECTORY) : GetFileAttributesW(Current->destinationPath_.c_str());
                if (Source.value_ == INVALID_HANDLE_VALUE || Target == INVALID_FILE_ATTRIBUTES
                    || !(Target & FILE_ATTRIBUTE_DIRECTORY) || (Target & FILE_ATTRIBUTE_REPARSE_POINT)) {
                    deferFolder(Current, State); return;
                }
                struct Child { std::wstring name_; EntrySnapshot entry_; };
                std::vector<Child> Children; bool Unnamed = false;
                const auto Listed = ListDirectory(Source.value_, State.listing_.get(), [&](std::wstring_view Name, const DirectoryListing& Info) {
                    auto Entry = ListedEntry(id_, prefix_, Current->entry_.locator_.root_, Name, Info);
                    if (!Entry) { Unnamed = true; return false; }
                    Children.push_back({std::wstring(Name), std::move(*Entry)});
                    return !stopped();
                });
                Source.Close();
                if (stopped()) return;
                if (!Listed || Unnamed) { deferFolder(Current, State); return; }
                for (auto& Item : Children) {
                    if (!waitWhilePaused() || !acquireCredit(State)) break;
                    const auto Depth = Current->depth_ + 1;
                    auto Target = child(Current->destination_, Item.entry_.name_);
                    const bool Folder = Item.entry_.directory_;
                    if (Folder && (Item.entry_.attributes_ & FILE_ATTRIBUTE_READONLY) && !options_.allowReadOnly_ && Depth <= options_.maxDepth_
                        && !(Item.entry_.attributes_ & FILE_ATTRIBUTE_REPARSE_POINT)) {
                        // Moving a readonly folder whole needs the user's answer;
                        // merging its children into an existing folder does not.
                        Handle Existing(CreateFileW((Current->destinationPath_ + L"\\" + Item.name_).c_str(), FILE_READ_ATTRIBUTES | SYNCHRONIZE,
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
                        if (Existing.value_ != INVALID_HANDLE_VALUE && plainDirectory(Existing.value_)) {
                            auto Node = childNode(Current, Item.name_, std::move(Item.entry_), Depth);
                            { std::lock_guard Lock(mutex_); ++Current->pending_; directories_.push_back(std::move(Node)); }
                            changed_.notify_one();
                        } else { Current->keep_ = true; defer({std::move(Item.entry_), std::move(Target), Depth}); }
                        continue;
                    }
                    if (deferredChild(Item.entry_.attributes_, Depth)) {
                        Current->keep_ = true; defer({std::move(Item.entry_), std::move(Target), Depth}); continue;
                    }
                    // MoveFileExW renames a link itself and never replaces an
                    // existing name. Renaming through our own handle measured
                    // 0.26 ms per entry at its close with two workers (scanning),
                    // against 0.05 ms for MoveFileExW.
                    const auto Code = MoveFileExW((Current->sourcePath_ + L"\\" + Item.name_).c_str(),
                        (Current->destinationPath_ + L"\\" + Item.name_).c_str(), 0) ? DWORD(ERROR_SUCCESS) : GetLastError();
                    if (Code == ERROR_SUCCESS) {
                        if (!Folder) bytes_ += Item.entry_.size_;
                        report(State, {std::move(Item.entry_), std::move(Target), Depth});
                        continue;
                    }
                    if (Folder && (Code == ERROR_ALREADY_EXISTS || Code == ERROR_FILE_EXISTS)) {
                        // An existing folder of that name: merge into it as well.
                        auto Node = childNode(Current, Item.name_, std::move(Item.entry_), Depth);
                        { std::lock_guard Lock(mutex_); ++Current->pending_; directories_.push_back(std::move(Node)); }
                        changed_.notify_one(); continue;
                    }
                    // Conflicts, locks and access get the per-item path's prompts.
                    Current->keep_ = true; defer({std::move(Item.entry_), std::move(Target), Depth});
                }
                flush(State, false);
                finish(Current, State);
            }
            void finish(std::shared_ptr<Node> Current, Worker& State)
            {
                // The listing worker and then the last child to finish complete a
                // folder, so source folders are removed post-order. Report this
                // worker's results first: once the root completes, the coordinator
                // takes the final batch and must find every result in it.
                flush(State, false);
                for (;;) {
                    { std::lock_guard Lock(mutex_); if (--Current->pending_) return; }
                    if (stopped()) return;
                    bool Removed = false;
                    Current->target_.Close();
                    if (moving() && Current->source_.value_ == INVALID_HANDLE_VALUE && !Current->deferred_) {
                        // Merged folders hold no handle while their children move;
                        // reopen by path, never through a link, to remove them.
                        const auto Open = [&](DWORD Access) {
                            return Handle(CreateFileW(Current->sourcePath_.c_str(), Access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
                        };
                        Current->source_ = Open(DirectoryListAccess | DELETE);
                        if (Current->source_.value_ == INVALID_HANDLE_VALUE) { Current->source_ = Open(DirectoryListAccess); Current->deletable_ = false; }
                        if (Current->source_.value_ != INVALID_HANDLE_VALUE && !plainDirectory(Current->source_.value_)) {
                            Current->source_.Close(); Current->deletable_ = false;
                        }
                    }
                    if (moving()) {
                        const bool ReadOnly = (Current->entry_.attributes_ & FILE_ATTRIBUTE_READONLY) != 0;
                        if (!Current->keep_ && !Current->deferred_ && Current->deletable_ && (!ReadOnly || options_.allowReadOnly_)) {
                            operations::OperationMetric Removing("transfer.folder-remove", 0, true);
                            Removed = DisposeHandle(Current->source_.value_, ReadOnly);
                            if (Removed) Current->source_.Close();
                        }
                        if (Removed) {
                            flush(State, false);
                            std::lock_guard Lock(mutex_); done_.push_back({Current->entry_, Current->destination_, Current->depth_});
                        } else if (!Current->deferred_) {
                            // Kept for the coordinator's verified removal after the
                            // per-item entries below it have been resolved.
                            auto Entry = Current->entry_; Entry.id_.revision_.reset();
                            BY_HANDLE_FILE_INFORMATION Info{};
                            if (Current->source_.value_ != INVALID_HANDLE_VALUE && GetFileInformationByHandle(Current->source_.value_, &Info)) {
                                Entry.attributes_ = Info.dwFileAttributes;
                                Entry.id_.revision_ = native::HandleRevision(Current->source_.value_, Info, std::filesystem::u8path(Entry.locator_.root_).wstring());
                            }
                            std::lock_guard Lock(mutex_); remaining_.push_back({std::move(Entry), Current->destination_, Current->depth_});
                        }
                    }
                    Current->source_.Close();
                    auto Parent = Current->parent_;
                    if (!Parent) {
                        complete_ = true;
                        { std::lock_guard Lock(mutex_); finished_ = true; }
                        changed_.notify_all(); return;
                    }
                    if (moving() && !Removed) Parent->keep_ = true;
                    Current = std::move(Parent);
                }
            }
            void settle(TreeTransferBatch& Batch) const
            {
                // A deferred folder is listed again by the per-item path; drop what
                // this engine deferred beneath it to avoid duplicates.
                std::vector<std::string> Folders;
                for (const auto& Item : Batch.deferred_) if (Item.entry_.directory_) Folders.push_back(Item.entry_.locator_.root_ + "\\");
                const auto Beneath = [&](const TreeTransferEntry& Item) {
                    return std::ranges::any_of(Folders, [&](const std::string& Folder) {
                        return Item.entry_.locator_.root_.size() > Folder.size()
                            && !_strnicmp(Item.entry_.locator_.root_.c_str(), Folder.c_str(), Folder.size());
                    });
                };
                std::erase_if(Batch.deferred_, Beneath); std::erase_if(Batch.remaining_, Beneath);
                // Ancestors first: the coordinator's final pass runs in reverse.
                std::ranges::stable_sort(Batch.remaining_, {}, &TreeTransferEntry::depth_);
            }
            ProviderId id_;
            std::string prefix_;
            TreeTransferOptions options_;
            std::function<bool()> pause_;
            std::stop_token external_;
            std::stop_source stop_;
            std::optional<std::stop_callback<std::function<void()>>> stopLink_;
            ParentPins sourceParents_;
            std::shared_ptr<Node> root_;
            std::mutex mutex_;
            std::condition_variable changed_;
            std::vector<std::shared_ptr<Node>> directories_;
            std::deque<Group> chunks_;
            std::thread counter_;
            std::atomic<bool> complete_{false};
            bool counted_ = false;
            std::uint64_t countedBytes_ = 0, countedItems_ = 0;
            std::atomic<std::uint64_t> bytes_{0};
            // Largest known bytes of the transfer: one folder's files, then the count.
            std::atomic<std::uint64_t> transferBytes_{0};
            std::atomic<std::chrono::steady_clock::rep> pauseDue_{0};
            std::deque<TreeTransferEntry> done_;
            std::vector<TreeTransferEntry> deferred_, remaining_;
            std::size_t reserved_ = 0;
            bool finished_ = false, delivered_ = false;
            std::optional<Error> error_;
            std::vector<std::thread> workers_;
        };
        class NativeWriteSession final : public IWriteSession
        {
        public:
            NativeWriteSession(ProviderId Id, Locator Destination, NativePath Path, DestinationExpectation Expected,
                ParentPins Parents, Handle File, std::wstring StagePath)
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
                // Ordinary SMB streaming needs neither producer timestamps nor
                // an initial journal identity. Commit queries the completed file.
                if (destination_.kind_ != LocatorKind::Smb || checkpoint_ || Context.producingContent_ || !Context.expectedWriteBytes_) {
                    BY_HANDLE_FILE_INFORMATION Info{};
                    if (!GetFileInformationByHandle(file_.value_, &Info)) return Failure(GetLastError(), id_);
                    const auto Time = [](FILETIME Value) { return LONGLONG((uint64_t(Value.dwHighDateTime) << 32) | Value.dwLowDateTime); };
                    initialTimes_.CreationTime.QuadPart = Time(Info.ftCreationTime);
                    initialTimes_.LastAccessTime.QuadPart = Time(Info.ftLastAccessTime);
                    initialTimes_.LastWriteTime.QuadPart = Time(Info.ftLastWriteTime);
                    record_.stageIdentity_ = native::PersistentIdentity(file_.value_, Info);
                    if (checkpoint_ && native::NeedsContentIdentity(file_.value_, path_.path_)) record_.stageIdentity_ = "content:writing";
                }
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
                return destination_.kind_ == LocatorKind::Local && !native::IsRedirectedPath(path_.path_) && expectedBytes_.has_value()
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
                file_ = std::move(File); record_.stageIdentity_ = native::PersistentIdentity(file_.value_, Info);
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
            Result<Unit> AdoptContent(ProducedContent Content, OperationContext Context) override
            {
                if (closed_ || failed_ || productionStarted_ || !expectedBytes_ || !Content.lifetime_ || sourceHash_)
                    return Error{ErrorCategory::InvalidRequest, "adopt-content-state", id_};
                if (const auto Stop = Context.Check(id_)) return *Stop;
                const auto Kind = Content.path_.starts_with("//") || Content.path_.starts_with("\\\\") ? LocatorKind::Smb : LocatorKind::Local;
                auto Candidate = Normalize({Kind, Content.path_}, destination_.kind_, id_);
                if (!Candidate.HasValue()) return Candidate.Failure();
                auto Parents = PinParents(Candidate.Value(), id_); if (!Parents.HasValue()) return Parents.Failure();
                const auto Parent = std::filesystem::path(stagePath_).parent_path().wstring() + L"\\";
                if (Candidate.Value().path_.size() <= Parent.size()
                    || _wcsnicmp(Candidate.Value().path_.c_str(), Parent.c_str(), Parent.size()))
                    return Error{ErrorCategory::InvalidRequest, "adopt-content-outside-destination", id_};
                const bool Redirected = native::IsRedirectedPath(path_.path_);
                DWORD VolumeFlags=0;
                const bool HaveVolumeFlags=GetVolumeInformationByHandleW(Parents.Value()->back().value_,nullptr,0,nullptr,nullptr,&VolumeFlags,nullptr,0);
                const bool PreserveAcl=!Redirected&&(!HaveVolumeFlags||(VolumeFlags&FILE_PERSISTENT_ACLS));
                const bool RecoveryRead = Context.writeCheckpoint_ && native::NeedsContentIdentity(Parents.Value()->back().value_, path_.path_);
                const DWORD Access = GENERIC_WRITE | FILE_READ_ATTRIBUTES | DELETE | (PreserveAcl ? WRITE_DAC : 0) | (RecoveryRead ? GENERIC_READ : 0);
                Handle File(CreateFileW(Candidate.Value().path_.c_str(), Access,
                    FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
                BY_HANDLE_FILE_INFORMATION Info{};
                if (File.value_ == INVALID_HANDLE_VALUE) return Failure(GetLastError(), id_);
                if (!GetFileInformationByHandle(File.value_, &Info) || (Info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))
                    || Info.nNumberOfLinks > 1 || ((uint64_t(Info.nFileSizeHigh) << 32) | Info.nFileSizeLow) != *expectedBytes_)
                    return Error{ErrorCategory::StaleRevision, "adopt-content-size-or-type", id_};
                if (PreserveAcl) {
                    PACL Dacl = nullptr; PSECURITY_DESCRIPTOR Descriptor = nullptr;
                    auto Security = GetSecurityInfo(file_.value_, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &Dacl, nullptr, &Descriptor);
                    if (Security != ERROR_SUCCESS) return Failure(Security, id_);
                    const auto FreeDescriptor = qScopeGuard([&] { LocalFree(Descriptor); });
                    SECURITY_DESCRIPTOR_CONTROL Control{}; DWORD Revision = 0;
                    Security = GetSecurityDescriptorControl(Descriptor, &Control, &Revision)
                        ? SetSecurityInfo(File.value_, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION
                            | (Control & SE_DACL_PROTECTED ? PROTECTED_DACL_SECURITY_INFORMATION : UNPROTECTED_DACL_SECURITY_INFORMATION),
                            nullptr, nullptr, Dacl, nullptr) : GetLastError();
                    if (Security != ERROR_SUCCESS) return Failure(Security, id_);
                }
                auto Removed = DeleteHandle(file_.value_, id_); if (!Removed.HasValue()) return Removed;
                file_.Close();
                auto Renamed = RenameHandle(File.value_, stagePath_, id_); if (!Renamed.HasValue()) { failed_ = true; return Renamed; }
                const auto Refreshed = native::RefreshRenamedHandle(File.value_, stagePath_, Access, Info);
                if (Refreshed) {
                    failed_ = true; auto Error = Failure(Refreshed, id_, Utf8(stagePath_));
                    Error.userAction_ = "preserved-stage:" + Utf8(stagePath_); return Error;
                }
                file_ = std::move(File); productionStarted_ = true;
                record_.stageIdentity_ = checkpoint_ && native::NeedsContentIdentity(file_.value_, path_.path_)
                    ? "content:writing" : native::PersistentIdentity(file_.value_, Info);
                return checkpoint(WritePhase::Writing);
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
                // Guard direct sessions and administrator/recovery callers too.
                const auto Input = native::CopyExtrasAt(Path.Value().path_, Context);
                const auto Output = native::CopyExtrasAt(path_.root_, Context);
                Options.acl_ = Options.acl_ && Input.acl_ && Output.acl_;
                Options.streams_ = Options.streams_ && Input.streams_ && Output.streams_;
                options_ = Options;
                if (!Options.acl_ && !Options.streams_) return Unit{};
                auto Parents = PinParents(Path.Value(), Source.id_.providerId_, Context);
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
                sourceFile_.Close(); sourceParents_.reset(); extrasDone_ = true; return CopyExtraProgress{};
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
                // Keep the existing pre-publication metadata order; only
                // redundant information queries are elided on the SMB path.
                if (!SetFileInformationByHandle(file_.value_, FileBasicInfo, &Basic, sizeof(Basic))) return Failure(GetLastError(), id_);
                BY_HANDLE_FILE_INFORMATION StageInfo{};
                if (!GetFileInformationByHandle(file_.value_, &StageInfo)) return Failure(GetLastError(), id_, Utf8(stagePath_));
                const bool ContentIdentity = checkpoint_ && native::NeedsContentIdentity(file_.value_, path_.path_);
                auto StageIdentity = RecoveryIdentity(file_.value_, stagePath_, ContentIdentity, expectedBytes_ ? &StageInfo : nullptr);
                if (!StageIdentity.HasValue()) return StageIdentity.Failure();
                record_.stageIdentity_ = std::move(StageIdentity.Value());
                if (expected_.exists_)
                {
                    if (!expected_.revision_) return Error{ErrorCategory::InvalidRequest, "expected-revision-required", id_};
                    backup_ = Handle(CreateFileW(path_.path_.c_str(), DELETE | FILE_READ_ATTRIBUTES | (ContentIdentity ? GENERIC_READ : 0), FILE_SHARE_READ, nullptr,
                        OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr));
                    if (backup_.value_ == INVALID_HANDLE_VALUE) return Failure(GetLastError(), id_);
                    EntrySnapshot Expected;
                    Expected.id_ = {id_, {}, IdentityScope::NavigationSession, expected_.revision_};
                    Expected.locator_ = destination_;
                    const auto Valid = CheckEntry(backup_.value_, Expected, id_);
                    if (!Valid.HasValue()) { backup_.Close(); return Valid.Failure(); }
                    BY_HANDLE_FILE_INFORMATION Original{};
                    if (!GetFileInformationByHandle(backup_.value_, &Original)) return Failure(GetLastError(), id_);
                    auto OriginalIdentity = RecoveryIdentity(backup_.value_, path_.path_, ContentIdentity, expectedBytes_ ? &Original : nullptr);
                    if (!OriginalIdentity.HasValue()) return OriginalIdentity.Failure();
                    record_.originalIdentity_ = std::move(OriginalIdentity.Value());
                    auto Saved = checkpoint(WritePhase::Prepared); if (!Saved.HasValue()) return Saved.Failure();
                    const auto BackupPath = Normalize(record_.backup_, destination_.kind_, id_).Value().path_;
                    const auto Rename = RenameHandle(backup_.value_, BackupPath, id_);
                    if (!Rename.HasValue()) { backup_.Close(); return Rename.Failure(); }
                    backupPath_ = BackupPath;
                    const auto Reopened = native::RefreshRenamedHandle(backup_.value_, BackupPath, DELETE | FILE_READ_ATTRIBUTES, Original);
                    if (Reopened) return Failure(Reopened, id_, Utf8(BackupPath));
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
                const auto Reopened = native::RefreshRenamedHandle(file_.value_, path_.path_,
                    GENERIC_WRITE | FILE_READ_ATTRIBUTES | DELETE, StageInfo);
                if (Reopened) {
                    auto Error = Failure(Reopened, id_, destination_.root_); Error.userAction_ = "published-file-reopen";
                    Receipt.cleanupErrors_.push_back(std::move(Error));
                    if (!backupPath_.empty()) Receipt.preservedArtifacts_.push_back(record_.backup_);
                    backup_.Close(); parents_.reset(); return Receipt;
                }
                auto Published = checkpoint(WritePhase::Published);
                if (!Published.HasValue()) {
                    Receipt.cleanupErrors_.push_back(Published.Failure());
                    if (!backupPath_.empty()) Receipt.preservedArtifacts_.push_back(record_.backup_);
                    file_.Close(); backup_.Close(); parents_.reset(); return Receipt;
                }
                Basic.FileAttributes = FinalAttributes;
                if (!SetFileInformationByHandle(file_.value_, FileBasicInfo, &Basic, sizeof(Basic))) {
                    auto Error = Failure(GetLastError(), id_, destination_.root_); Error.userAction_ = "published-file-metadata";
                    Receipt.cleanupErrors_.push_back(std::move(Error));
                }
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
                        Receipt.cleanupErrors_.push_back(Failure(GetLastError(), id_, destination_.root_));
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
                parents_.reset();
                auto Finished = checkpoint(WritePhase::Finished);
                if (!Finished.HasValue()) Receipt.cleanupErrors_.push_back(Finished.Failure());
                return Receipt;
            }
            Result<Unit> Abort() override
            {
                sourceStream_.Close(); destinationStream_.Close(); verificationStream_.Close(); sourceFile_.Close(); sourceParents_.reset();
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
                parents_.reset();
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
            ParentPins sourceParents_;
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
            ParentPins parents_;
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
        std::optional<DWORD> SolidStateDisk(const NativePath& Path, bool RequireNvme = true)
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
            if (!RequireNvme) return Extents.Extents[0].DiskNumber;
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
            std::shared_ptr<MutationScope> CreateMutationScope() const override
            { return kind_ == LocatorKind::Local ? std::make_shared<NativeMutationScope>() : nullptr; }
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
            Result<CopyCapabilities> CopySupportAt(Locator At, OperationContext Context) override {
                if (const auto Error = Context.Check(id_)) return *Error;
                const auto Path = Normalize(At, kind_, id_); if (!Path.HasValue()) return Path.Failure();
                const auto Extras = native::CopyExtrasAt(Path.Value().root_, Context);
                return CopyCapabilities{true, true, Extras.acl_, Extras.streams_};
            }
            bool SupportsParallelCopies() const override { return kind_ == LocatorKind::Local; }
            bool SupportsParallelRemovals() const override { return kind_ == LocatorKind::Local; }
            bool SerializesDirectoryRemovals() const override { return kind_ == LocatorKind::Local; }
            std::size_t PreferredDeleteWorkers(Locator At) const override
            {
                // Workers take whole directories. Separate folders on NVMe/ReFS
                // scaled to 64,000 (4) and 74,000 (8) files/s from 27,000 (1).
                if (kind_ != LocatorKind::Local || At.kind_ != LocatorKind::Local) return 1;
                const auto Path = Normalize(At, kind_, id_);
                if (!Path.HasValue() || !SolidStateDisk(Path.Value(), false)) return 1;
                return SolidStateDisk(Path.Value()) ? std::clamp<std::size_t>(std::thread::hardware_concurrency(), 4, 8) : 4;
            }
            bool SupportsTreeRemoval(const IStorageProvider& Reader, const Locator& At) const override
            {
                // The engine replaces only this writer's own reader listing.
                if (kind_ != LocatorKind::Local || &Reader != reader_.get() || At.kind_ != LocatorKind::Local) return false;
                const auto Path = Normalize(At, kind_, id_);
                if (!Path.HasValue() || Path.Value().path_ == Path.Value().root_ || native::IsRedirectedPath(Path.Value().path_)) return false;
                // Same volumes as the retained deletion pins: local NTFS and ReFS.
                wchar_t FileSystem[32]{}; const auto Type = GetDriveTypeW(Path.Value().root_.c_str());
                return (Type == DRIVE_FIXED || Type == DRIVE_REMOVABLE || Type == DRIVE_RAMDISK)
                    && GetVolumeInformationW(Path.Value().root_.c_str(), nullptr, 0, nullptr, nullptr, nullptr, FileSystem, DWORD(std::size(FileSystem)))
                    && (!_wcsicmp(FileSystem, L"NTFS") || !_wcsicmp(FileSystem, L"ReFS"));
            }
            Result<std::unique_ptr<ITreeRemoval>> BeginTreeRemoval(EntrySnapshot Root, TreeRemovalOptions Options, OperationContext Context) override
            {
                if (const auto Error = Context.Check(id_)) return *Error;
                if (!Root.directory_ || Root.id_.providerId_ != id_ || !Root.id_.revision_) return Error{ErrorCategory::InvalidRequest, "tree-removal-root", id_};
                const auto Path = Normalize(Root.locator_, kind_, id_);
                if (!Path.HasValue()) return Path.Failure();
                if (Path.Value().path_ == Path.Value().root_) return Error{ErrorCategory::InvalidRequest, "root-mutation-denied", id_};
                // Child identities extend the reader's own session key prefix.
                const auto Suffix = Serialize(Root.locator_);
                if (!Root.id_.key_.ends_with(Suffix)) return Error{ErrorCategory::Unsupported, "tree-removal-identity", id_};
                auto Removal = std::make_unique<NativeTreeRemoval>(id_, Root.id_.key_.substr(0, Root.id_.key_.size() - Suffix.size()), Options, Context);
                auto Started = Removal->Start(Root, Path.Value());
                if (!Started.HasValue()) return Started.Failure();
                return std::unique_ptr<ITreeRemoval>(std::move(Removal));
            }
            std::size_t PreferredCopyWorkers(Locator Source, Locator Destination) const override
            {
                if (kind_ != LocatorKind::Local || Source.kind_ != LocatorKind::Local || Destination.kind_ != LocatorKind::Local) return 1;
                const auto From = Normalize(Source, kind_, id_), To = Normalize(Destination, kind_, id_);
                if (!From.HasValue() || !To.HasValue()) return 1;
                const auto SourceDisk = SolidStateDisk(From.Value()), DestinationDisk = SolidStateDisk(To.Value());
                return SourceDisk && DestinationDisk && *SourceDisk != *DestinationDisk ? 4 : 1;
            }
            bool SupportsTreeTransfer(const IStorageProvider& Reader, const Locator& Source, const IWritableStorage& DestinationWriter, const Locator& Destination) const override
            {
                // The engine replaces only this writer's own reader listing, and
                // writes only where another native local writer would.
                if (kind_ != LocatorKind::Local || &Reader != reader_.get() || Source.kind_ != LocatorKind::Local || Destination.kind_ != LocatorKind::Local) return false;
                const auto* Target = dynamic_cast<const NativeWritableStorage*>(&DestinationWriter);
                if (!Target || Target->kind_ != LocatorKind::Local) return false;
                const auto From = Normalize(Source, kind_, id_), To = Normalize(Destination, kind_, id_);
                if (!From.HasValue() || !To.HasValue() || From.Value().path_ == From.Value().root_ || To.Value().path_ == To.Value().root_
                    || native::IsRedirectedPath(From.Value().path_) || native::IsRedirectedPath(To.Value().path_)) return false;
                // Listing needs FileIdExtdDirectoryInfo (NTFS, ReFS). Destinations
                // may also be FAT-family removable or fixed volumes.
                const auto Local = [](const std::wstring& Root, bool Fat) {
                    wchar_t FileSystem[32]{}; const auto Type = GetDriveTypeW(Root.c_str());
                    return (Type == DRIVE_FIXED || Type == DRIVE_REMOVABLE || Type == DRIVE_RAMDISK)
                        && GetVolumeInformationW(Root.c_str(), nullptr, 0, nullptr, nullptr, nullptr, FileSystem, DWORD(std::size(FileSystem)))
                        && (!_wcsicmp(FileSystem, L"NTFS") || !_wcsicmp(FileSystem, L"ReFS")
                            || (Fat && (!_wcsicmp(FileSystem, L"exFAT") || !_wcsicmp(FileSystem, L"FAT32") || !_wcsicmp(FileSystem, L"FAT"))));
                };
                return Local(From.Value().root_, false) && Local(To.Value().root_, true);
            }
            std::size_t PreferredTreeTransferWorkers(Locator Source, Locator Destination) const override
            {
                // Workers take folders and groups of files, like robocopy's default
                // eight threads; spinning disks keep two to limit seeking.
                if (kind_ != LocatorKind::Local || Source.kind_ != LocatorKind::Local || Destination.kind_ != LocatorKind::Local) return 1;
                const auto From = Normalize(Source, kind_, id_), To = Normalize(Destination, kind_, id_);
                if (!From.HasValue() || !To.HasValue()) return 1;
                return SolidStateDisk(From.Value(), false) && SolidStateDisk(To.Value(), false)
                    ? std::clamp<std::size_t>(std::thread::hardware_concurrency(), 4, 8) : 2;
            }
            Result<std::unique_ptr<ITreeTransfer>> BeginTreeTransfer(EntrySnapshot Root, Locator Destination, TreeTransferOptions Options, OperationContext Context) override
            {
                if (const auto Error = Context.Check(id_)) return *Error;
                if (!Root.directory_ || Root.id_.providerId_ != id_ || !Root.id_.revision_) return Error{ErrorCategory::InvalidRequest, "tree-transfer-root", id_};
                const auto Source = Normalize(Root.locator_, kind_, id_);
                if (!Source.HasValue()) return Source.Failure();
                const auto Target = Normalize(Destination, kind_, id_);
                if (!Target.HasValue()) return Target.Failure();
                if (Source.Value().path_ == Source.Value().root_ || Target.Value().path_ == Target.Value().root_)
                    return Error{ErrorCategory::InvalidRequest, "root-mutation-denied", id_};
                const auto& From = Source.Value().path_; const auto& To = Target.Value().path_;
                if (To.size() >= From.size() && !_wcsnicmp(To.c_str(), From.c_str(), From.size()) && (To.size() == From.size() || To[From.size()] == L'\\'))
                    return Error{ErrorCategory::InvalidRequest, "destination-inside-source", id_};
                // Child identities extend the reader's own session key prefix.
                const auto Suffix = Serialize(Root.locator_);
                if (!Root.id_.key_.ends_with(Suffix)) return Error{ErrorCategory::Unsupported, "tree-transfer-identity", id_};
                auto Transfer = std::make_unique<NativeTreeTransfer>(id_, Root.id_.key_.substr(0, Root.id_.key_.size() - Suffix.size()), Options, Context);
                auto Started = Transfer->Start(Root, Source.Value(), Target.Value(), Destination);
                if (!Started.HasValue()) return Started.Failure();
                return std::unique_ptr<ITreeTransfer>(std::move(Transfer));
            }
            bool CanProduceContent(OperationContext Context) const override
            { return kind_ == LocatorKind::Local && !Context.writeCheckpoint_; }
            Result<Unit> PatchMetadata(EntrySnapshot Entry, MetadataPatch Patch, OperationContext Context) override
            {
                if (Context.mutationScope_) Context.mutationScope_->Clear();
                if (!Patch.Valid()) return Error{ErrorCategory::InvalidRequest, "invalid-metadata-patch", id_};
                if (const auto Error = Context.Check(id_)) return *Error;
                ReleaseDirectoryPins(Entry, Context);
                if (Patch.Empty()) return Unit{};
                if (!Entry.id_.revision_ || (!Entry.id_.revision_->token_.starts_with("directory:")
                    && !Entry.id_.revision_->token_.starts_with("rdp:")
                    && Entry.id_.revision_->token_.find(":identity:") == std::string::npos))
                    return Error{ErrorCategory::StaleRevision, "metadata-identity-required", id_};
                auto Path = Normalize(Entry.locator_, kind_, id_);
                if (!Path.HasValue()) return Path.Failure();
                auto Parents = PinParents(Path.Value(), id_, Context);
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
                if (Context.mutationScope_) Context.mutationScope_->Clear();
                operations::OperationMetric Timing("native.begin-copy.total", 0, true);
                if (const auto Error = Context.Check(id_)) return *Error;
                if (Expected.exists_ != Expected.revision_.has_value()) return Error{ErrorCategory::InvalidRequest, "destination-expectation", id_};
                auto Path = Normalize(At, kind_, id_);
                if (!Path.HasValue()) return Path.Failure();
                auto Parents = PinParents(Path.Value(), id_, Context);
                if (!Parents.HasValue()) return Parents.Failure();
                const auto Temp = TemporaryName(std::filesystem::path(Path.Value().path_).parent_path().wstring(), L"stage");
                if (Options.acl_ || Options.streams_) {
                    const auto Extras = native::CopyExtrasAt(Path.Value().root_, Context);
                    Options.acl_ = Options.acl_ && Extras.acl_; Options.streams_ = Options.streams_ && Extras.streams_;
                }
                const bool RecoveryRead = Context.writeCheckpoint_ && native::NeedsContentIdentity(Parents.Value()->back().value_, Path.Value().path_);
                Handle File(CreateFileW(Temp.c_str(), GENERIC_WRITE | FILE_READ_ATTRIBUTES | READ_CONTROL | DELETE | ((Options.verify_ || RecoveryRead) ? GENERIC_READ : 0) | (Options.acl_ ? WRITE_DAC : 0), FILE_SHARE_READ, nullptr,
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
                if (Context.mutationScope_) Context.mutationScope_->Clear();
                if (const auto Error = Context.Check(id_)) return *Error;
                const auto Path = Normalize(At, kind_, id_);
                if (!Path.HasValue()) return Path.Failure();
                auto Parents = PinParents(Path.Value(), id_, Context);
                if (!Parents.HasValue()) return Parents.Failure();
                if (!CreateDirectoryW(Path.Value().path_.c_str(), nullptr)) return Failure(GetLastError(), id_);
                return Unit{};
            }
            Result<Unit> Remove(EntrySnapshot Entry, OperationContext Context) override
            {
                operations::OperationMetric Timing("native.remove", 0, true);
                if (const auto Error = Context.Check(id_)) return *Error;
                ReleaseDirectoryPins(Entry, Context);
                if (!Entry.directory_ && id_.value_ == "local") {
                    if (const auto Scope = std::dynamic_pointer_cast<NativeMutationScope>(Context.mutationScope_)) {
                        if (auto Prepared = Scope->Take(Entry)) {
                            if (const auto Error = Context.Check(id_)) return *Error;
                            // The handle itself cannot designate a replacement.
                            // Attribute-only writers can still change timestamps,
                            // so retain the original revision/readonly checks.
                            FILE_BASIC_INFO Current{};
                            if (!GetFileInformationByHandleEx(Prepared->file_.value_, FileBasicInfo, &Current, sizeof(Current))) return Failure(GetLastError(), id_);
                            const auto Ticks = [](FILETIME Time) { return (uint64_t(Time.dwHighDateTime) << 32) | Time.dwLowDateTime; };
                            const auto& Before = Prepared->info_;
                            // The exact handle and denied write sharing protect
                            // identity/size, including writable mappings. Only
                            // attribute writers can still change times/readonly;
                            // do not requery volume IDs and file IDs here.
                            if (Current.FileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)
                                || uint64_t(Current.CreationTime.QuadPart) != Ticks(Before.ftCreationTime)
                                || uint64_t(Current.LastWriteTime.QuadPart) != Ticks(Before.ftLastWriteTime))
                                return Error{ErrorCategory::StaleRevision, "revision-changed", id_, Entry.id_, false, "refresh"};
                            if (Current.FileAttributes & FILE_ATTRIBUTE_READONLY) {
                                if (!Context.allowReadOnlyRemoval_) return Error{ErrorCategory::AccessDenied, "readonly-removal-not-authorized", id_};
                                // Captured ordinary handles have no WRITE_ATTRIBUTES.
                                // Reopen through the normal verified/restore path.
                                Prepared->file_.Close(); return Remove(std::move(Entry), Context);
                            }
                            auto Removed = DeleteHandle(Prepared->file_.value_, id_);
                            Prepared->file_.Close(); // disposition takes effect before reporting success
                            return Removed;
                        }
                    }
                }
                auto Path = Normalize(Entry.locator_, kind_, id_);
                if (!Path.HasValue()) return Path.Failure();
                auto Parents = PinParents(Path.Value(), id_, Context, true);
                if (!Parents.HasValue()) return Parents.Failure();
                Handle File(CreateFileW(Path.Value().path_.c_str(), DELETE | FILE_READ_ATTRIBUTES | (Context.allowReadOnlyRemoval_ ? FILE_WRITE_ATTRIBUTES : 0), FILE_SHARE_READ, nullptr,
                    OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr));
                if (File.value_ == INVALID_HANDLE_VALUE) return Failure(GetLastError(), id_);
                BY_HANDLE_FILE_INFORMATION Info{};
                const auto Valid = CheckEntry(File.value_, Entry, id_, &Info);
                if (!Valid.HasValue()) return Valid.Failure();
                // The verified handle already supplied attributes. Zero times
                // preserve timestamps when clearing/restoring readonly.
                FILE_BASIC_INFO Basic{}; Basic.FileAttributes = Info.dwFileAttributes;
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
                if (Context.mutationScope_) Context.mutationScope_->Clear();
                using Microsoft::WRL::ComPtr;
                if (const auto Error = Context.Check(id_)) return *Error;
                ReleaseDirectoryPins(Entry, Context);
                const auto Supported = Supports(Mutation::Recycle, Entry.locator_, Context);
                if (!Supported.HasValue()) return Supported.Failure();
                if (Supported.Value().availability_ == Availability::Unsupported) return Error{ErrorCategory::Unsupported, Supported.Value().reason_, id_};
                auto Path = Normalize(Entry.locator_, kind_, id_); if (!Path.HasValue()) return Path.Failure();
                auto Parents = PinParents(Path.Value(), id_, Context); if (!Parents.HasValue()) return Parents.Failure();
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
            Result<bool> CanRenameTo(Locator From, Locator To, OperationContext Context) override
            {
                if (const auto Error = Context.Check(id_)) return *Error;
                if (kind_ != LocatorKind::Local || From.kind_ != kind_ || To.kind_ != kind_) return false;
                const auto Source = Normalize(From, kind_, id_), Destination = Normalize(To, kind_, id_);
                if (!Source.HasValue() || !Destination.HasValue()) return false;
                const auto Volume = [](const std::wstring& Path) {
                    std::vector<wchar_t> Mount(32768), Name(32768);
                    if (!GetVolumePathNameW(Path.c_str(), Mount.data(), DWORD(Mount.size()))
                        || !GetVolumeNameForVolumeMountPointW(Mount.data(), Name.data(), DWORD(Name.size()))) return std::wstring{};
                    return std::wstring(Name.data());
                };
                const auto SourceVolume = Volume(Source.Value().path_);
                const auto Parent = std::filesystem::path(Destination.Value().path_).parent_path().wstring();
                return !SourceVolume.empty() && SourceVolume == Volume(Parent);
            }
            Result<Unit> Rename(EntrySnapshot Entry, Locator To, OperationContext Context) override
            {
                if (Context.mutationScope_) Context.mutationScope_->Clear();
                if (const auto Error = Context.Check(id_)) return *Error;
                ReleaseDirectoryPins(Entry, Context);
                const auto FromPath = Normalize(Entry.locator_, kind_, id_);
                if (!FromPath.HasValue()) return FromPath.Failure();
                const auto ToPath = Normalize(To, kind_, id_);
                if (!ToPath.HasValue()) return ToPath.Failure();
                auto FromParents = PinParents(FromPath.Value(), id_, Context);
                if (!FromParents.HasValue()) return FromParents.Failure();
                auto ToParents = PinParents(ToPath.Value(), id_, Context);
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
            const bool Content = Record.stageIdentity_.starts_with("sha256:") || Record.stageIdentity_ == "content:writing";
            Handle File(CreateFileW(Path.path_.c_str(), DELETE | FILE_READ_ATTRIBUTES | (Content ? GENERIC_READ : 0), FILE_SHARE_READ, nullptr,
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
            if (!Content && native::NeedsContentIdentity(File.value_, Path.path_))
                return Error{ErrorCategory::StaleRevision, "write-recovery-unstable-legacy-identity", Id, {}, false, "inspect-preserved-files"};
            auto Token = RecoveryIdentity(File.value_, Path.path_, Content);
            if (!Token.HasValue()) return Token.Failure();
            // Keep pre-128-bit journals readable without weakening new tokens.
            if (!Content && !Record.stageIdentity_.starts_with("id128:")) Token = Identity(Info);
            return Observed{std::move(File), std::move(Token.Value())};
        };
        auto Current = Observe(Destination.Value()), Pending = Observe(Stage.Value()), Original = Observe(Backup.Value());
        if (!Current.HasValue()) return Current.Failure();
        if (!Pending.HasValue()) return Pending.Failure();
        if (!Original.HasValue()) return Original.Failure();
        if (Record.stageIdentity_ == "content:writing") {
            // No destructive operation has started. Incomplete bytes cannot be
            // authenticated; preserve them rather than guessing their identity.
            WriteReceipt Receipt{Record.destination_}; Receipt.transaction_ = Record.transaction_;
            Receipt.destinationCommitted_ = false;
            if (!Pending.Value().identity_.empty()) Receipt.preservedArtifacts_.push_back(Record.stage_);
            if (!Original.Value().identity_.empty()) Receipt.preservedArtifacts_.push_back(Record.backup_);
            return Receipt;
        }
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
