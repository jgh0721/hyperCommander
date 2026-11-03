#pragma once
#include "commonLibs/storage/storage.hpp"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <algorithm>
#include <cwctype>
#include <iterator>
#include <string_view>
#include <utility>

namespace hc::storage::native
{
    inline std::uint64_t FileTimeTicks(FILETIME Time)
    {
        return (static_cast<std::uint64_t>(Time.dwHighDateTime) << 32) | Time.dwLowDateTime;
    }
    inline ResourceRevision ListingRevision(const WIN32_FILE_ATTRIBUTE_DATA& Data)
    {
        // A protected directory can have only enumeration metadata. Match both
        // birth and modification times once, then upgrade it to a file-ID token.
        if (Data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            return {"listing-directory:" + std::to_string(FileTimeTicks(Data.ftCreationTime))
                + ":" + std::to_string(FileTimeTicks(Data.ftLastWriteTime))};
        // SMB may synthesize different creation times for attribute-only and
        // read handles. Creation time is display metadata, not a content version.
        return {"file:" + std::to_string(FileTimeTicks(Data.ftLastWriteTime))
            + ":" + std::to_string(Data.nFileSizeHigh) + ":" + std::to_string(Data.nFileSizeLow)};
    }
    inline ResourceRevision HandleRevision(const BY_HANDLE_FILE_INFORMATION& Info)
    {
        const auto Identity = std::to_string(Info.dwVolumeSerialNumber) + ":"
            + std::to_string(Info.nFileIndexHigh) + ":" + std::to_string(Info.nFileIndexLow);
        // SMB servers may synthesize creation time differently between handles.
        // A nonzero file ID detects replacement without rejecting child deletion.
        if (Info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (Info.nFileIndexHigh || Info.nFileIndexLow) return {"directory:identity:" + Identity};
            // Retain the conservative fallback when the server supplies no ID.
            return {"directory:" + std::to_string(FileTimeTicks(Info.ftCreationTime)) + ":" + Identity};
        }
        const WIN32_FILE_ATTRIBUTE_DATA Data{Info.dwFileAttributes, Info.ftCreationTime,
            Info.ftLastAccessTime, Info.ftLastWriteTime, Info.nFileSizeHigh, Info.nFileSizeLow};
        // Fresh snapshots additionally detect replacement by a different file
        // even when its size and modification time were deliberately preserved.
        return {ListingRevision(Data).token_ + ":identity:" + Identity};
    }
    inline ResourceRevision MetadataRevision(ResourceRevision Revision)
    {
        // RDP redirected files can report a synthetic file ID per opened handle.
        // Callers which bind document bytes by hash may retain the exact mtime
        // and size token without treating that synthetic ID as persistent.
        if(Revision.token_.starts_with("file:")){
            const auto At=Revision.token_.find(":identity:");
            if(At!=std::string::npos)Revision.token_.resize(At);
        }
        return Revision;
    }
    inline bool IsRedirectedPath(std::wstring_view Path)
    {
        std::wstring Value(Path);
        std::replace(Value.begin(), Value.end(), L'/', L'\\');
        std::transform(Value.begin(), Value.end(), Value.begin(), [](wchar_t Character) { return towlower(Character); });
        if (Value.starts_with(L"\\\\?\\unc\\")) Value = L"\\\\" + Value.substr(8);
        else if (Value.starts_with(L"\\\\?\\")) Value.erase(0, 4);
        if (Value == L"\\\\tsclient" || Value.starts_with(L"\\\\tsclient\\")) return true;
        if (Value.starts_with(L"\\device\\rdpdr\\")) return true;
        // Mapped redirected drives have the same handle semantics as tsclient.
        if (Value.size() >= 2 && Value[1] == L':') {
            const wchar_t Drive[]{Value[0], L':', L'\0'};
            wchar_t Target[1024]{};
            if (QueryDosDeviceW(Drive, Target, DWORD(std::size(Target)))) {
                std::wstring Device(Target);
                std::transform(Device.begin(), Device.end(), Device.begin(), [](wchar_t Character) { return towlower(Character); });
                return Device.starts_with(L"\\device\\rdpdr\\") || Device.find(L"\\tsclient\\") != std::wstring::npos;
            }
        }
        return false;
    }
    inline bool IsRedirectedHandle(HANDLE File)
    {
        wchar_t Path[32768]{};
        const auto Length = GetFinalPathNameByHandleW(File, Path, DWORD(std::size(Path)), FILE_NAME_OPENED);
        return Length && Length < std::size(Path) && IsRedirectedPath(std::wstring_view(Path, Length));
    }
    inline ResourceRevision RedirectedRevision(const BY_HANDLE_FILE_INFORMATION& Info)
    {
        // RDP supplies an ID for the opened handle, not a persistent file ID.
        // Creation time still detects replacement; directory mtime must be
        // excluded because removing its children legitimately changes it.
        const auto Birth = std::to_string(FileTimeTicks(Info.ftCreationTime));
        if (Info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) return {"rdp:directory:" + Birth};
        const WIN32_FILE_ATTRIBUTE_DATA Data{Info.dwFileAttributes, Info.ftCreationTime,
            Info.ftLastAccessTime, Info.ftLastWriteTime, Info.nFileSizeHigh, Info.nFileSizeLow};
        return {"rdp:" + Birth + ":" + ListingRevision(Data).token_};
    }
    inline std::string PersistentIdentity(HANDLE File, const BY_HANDLE_FILE_INFORMATION& Info)
    {
        // Product PCHs retain an older NTDDI declaration guard. This is the
        // Windows 8 FileIdInfo ABI; querying it need not change platform macros.
        struct FileIdentityInfo { ULONGLONG volumeSerial_; BYTE identifier_[16]; } Identity{};
        static_assert(sizeof(FileIdentityInfo) == 24);
        constexpr auto IdentityClass = static_cast<FILE_INFO_BY_HANDLE_CLASS>(18);
        if (GetFileInformationByHandleEx(File, IdentityClass, &Identity, sizeof(Identity))
            && std::any_of(std::begin(Identity.identifier_), std::end(Identity.identifier_), [](BYTE Value) { return Value != 0; })) {
            constexpr char Hex[] = "0123456789abcdef";
            auto Token = "id128:" + std::to_string(Identity.volumeSerial_) + ":";
            for (const auto Byte : Identity.identifier_) { Token += Hex[Byte >> 4]; Token += Hex[Byte & 15]; }
            return Token;
        }
        return std::to_string(Info.dwVolumeSerialNumber) + ":" + std::to_string(Info.nFileIndexHigh) + ":" + std::to_string(Info.nFileIndexLow);
    }
    inline bool NeedsContentIdentity(HANDLE File, std::wstring_view Path)
    {
        if (IsRedirectedPath(Path)) return true;
        wchar_t FileSystem[32]{};
        return GetVolumeInformationByHandleW(File, nullptr, 0, nullptr, nullptr, nullptr, FileSystem, DWORD(std::size(FileSystem)))
            && (!_wcsicmp(FileSystem, L"FAT") || !_wcsicmp(FileSystem, L"FAT32") || !_wcsicmp(FileSystem, L"exFAT"));
    }
    inline ResourceRevision HandleRevision(HANDLE File, const BY_HANDLE_FILE_INFORMATION& Info, std::wstring_view Path = {})
    {
        if (IsRedirectedPath(Path) || (Path.empty() && IsRedirectedHandle(File))) return RedirectedRevision(Info);
        const auto Identity = PersistentIdentity(File, Info);
        if (Info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (Info.nFileIndexHigh || Info.nFileIndexLow || Identity.starts_with("id128:")) return {"directory:identity:" + Identity};
            return HandleRevision(Info);
        }
        const WIN32_FILE_ATTRIBUTE_DATA Data{Info.dwFileAttributes, Info.ftCreationTime,
            Info.ftLastAccessTime, Info.ftLastWriteTime, Info.nFileSizeHigh, Info.nFileSizeLow};
        return {ListingRevision(Data).token_ + ":identity:" + Identity};
    }
    inline DWORD RefreshRenamedHandle(HANDLE& File, const std::wstring& Path, DWORD Access,
        const BY_HANDLE_FILE_INFORMATION& Before)
    {
        if (!IsRedirectedPath(Path)) return ERROR_SUCCESS;
        // RDP set-information requests retain the name used by CreateFile.
        // Reopen the published name and verify it before further mutation.
        CloseHandle(std::exchange(File, INVALID_HANDLE_VALUE));
        const auto Fresh = CreateFileW(Path.c_str(), Access, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (Fresh == INVALID_HANDLE_VALUE) return GetLastError();
        BY_HANDLE_FILE_INFORMATION After{};
        DWORD Error = ERROR_SUCCESS;
        if(!GetFileInformationByHandle(Fresh,&After))Error=GetLastError();
        else {
            // NTFS tunneling can restore the deleted target's creation time
            // when our file is renamed over its name. RDP IDs are per-handle;
            // validate this known rename by type, size and modification time.
            // Source snapshot checks retain their stricter creation-time test.
            const bool Directory=(Before.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)!=0;
            const bool SameType=Directory==((After.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)!=0);
            const bool Same=Directory?RedirectedRevision(After)==RedirectedRevision(Before)
                : After.nFileSizeHigh==Before.nFileSizeHigh&&After.nFileSizeLow==Before.nFileSizeLow
                    &&FileTimeTicks(After.ftLastWriteTime)==FileTimeTicks(Before.ftLastWriteTime);
            if((After.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)||!SameType||!Same)Error=ERROR_FILE_INVALID;
        }
        if (Error) { CloseHandle(Fresh); return Error; }
        File = Fresh;
        return ERROR_SUCCESS;
    }
    inline bool MatchesRevision(const ResourceRevision& Expected, const BY_HANDLE_FILE_INFORMATION& Info,
        HANDLE File = INVALID_HANDLE_VALUE, std::wstring_view Path = {})
    {
        if (File != INVALID_HANDLE_VALUE) {
            if (Expected == HandleRevision(File, Info, Path)) return true;
            // Existing journals/snapshots may predate 128-bit identity tokens.
            if (!IsRedirectedPath(Path) && !Expected.token_.starts_with("rdp:") && Expected == HandleRevision(Info)) return true;
        }
        else if (Expected == HandleRevision(Info)) return true;
        const WIN32_FILE_ATTRIBUTE_DATA Data{Info.dwFileAttributes, Info.ftCreationTime,
            Info.ftLastAccessTime, Info.ftLastWriteTime, Info.nFileSizeHigh, Info.nFileSizeLow};
        // Cheap listings have no file ID. Accept only the exact
        // metadata-only token; Stat then upgrades it to a handle revision.
        return Expected == ListingRevision(Data);
    }
}
