#pragma once

#include <atomic>
#include <vector>

// Patch only this test executable's imports. No drive mapping, credentials or
// system DLL is changed. The product code sees a slow, disconnected drive even
// on CI machines with no network mappings.
class DriveProbeFixture
{
public:
    DriveProbeFixture()
    {
        const auto Drives = logicalDrives_();
        for (wchar_t Letter = L'A'; Letter <= L'Z'; ++Letter)
            if (!(Drives & (1u << (Letter - L'A')))) { letter_ = Letter; break; }
        if (!letter_) return;
        guiThread_ = GetCurrentThreadId();
        installed_ = patch("GetLogicalDrives", reinterpret_cast<ULONG_PTR>(&getLogicalDrives))
            && patch("GetDriveTypeW", reinterpret_cast<ULONG_PTR>(&getDriveType))
            && patch("GetVolumeInformationW", reinterpret_cast<ULONG_PTR>(&getVolumeInformation))
            && patch("GetDiskFreeSpaceExW", reinterpret_cast<ULONG_PTR>(&getDiskFreeSpace))
            && patch("GetFileAttributesW", reinterpret_cast<ULONG_PTR>(&getFileAttributes));
    }
    ~DriveProbeFixture()
    {
        for (auto It = patches_.rbegin(); It != patches_.rend(); ++It)
        {
            DWORD Previous = 0;
            if (VirtualProtect(It->slot_, sizeof(ULONG_PTR), PAGE_READWRITE, &Previous))
            {
                *It->slot_ = It->original_;
                VirtualProtect(It->slot_, sizeof(ULONG_PTR), Previous, &Previous);
            }
        }
    }
    DriveProbeFixture(const DriveProbeFixture&) = delete;
    DriveProbeFixture& operator=(const DriveProbeFixture&) = delete;
    bool IsInstalled() const { return installed_; }
    static QString Root() { return QString(QChar(letter_)) + ':'; }
    static int MetadataCalls() { return metadataCalls_.load(); }
    static int GuiMetadataCalls() { return guiMetadataCalls_.load(); }
    static int AttributeCalls() { return attributeCalls_.load(); }

private:
    bool patch(const char* Name, ULONG_PTR Replacement)
    {
        auto* Base = reinterpret_cast<BYTE*>(GetModuleHandleW(nullptr));
        const auto* Dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(Base);
        const auto* Nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(Base + Dos->e_lfanew);
        auto* Import = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(Base + Nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress);
        for (; Import->Name; ++Import)
        {
            if (!Import->OriginalFirstThunk) continue;
            auto* Names = reinterpret_cast<IMAGE_THUNK_DATA*>(Base + Import->OriginalFirstThunk);
            auto* Slots = reinterpret_cast<IMAGE_THUNK_DATA*>(Base + Import->FirstThunk);
            for (; Names->u1.AddressOfData; ++Names, ++Slots)
            {
                if (IMAGE_SNAP_BY_ORDINAL(Names->u1.Ordinal)) continue;
                auto* Entry = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(Base + Names->u1.AddressOfData);
                if (strcmp(reinterpret_cast<const char*>(Entry->Name), Name)) continue;
                DWORD Previous = 0;
                if (!VirtualProtect(&Slots->u1.Function, sizeof(ULONG_PTR), PAGE_READWRITE, &Previous)) return false;
                patches_.push_back({&Slots->u1.Function, Slots->u1.Function});
                Slots->u1.Function = Replacement;
                VirtualProtect(&Slots->u1.Function, sizeof(ULONG_PTR), Previous, &Previous);
                return true;
            }
        }
        return false;
    }
    static bool isFixture(LPCWSTR Path)
    {
        if (!Path) return false;
        if (!wcsncmp(Path, L"\\\\?\\", 4)) Path += 4;
        return towupper(Path[0]) == letter_ && Path[1] == L':';
    }
    static BOOL failMetadata()
    {
        ++metadataCalls_;
        if (GetCurrentThreadId() == guiThread_) ++guiMetadataCalls_;
        Sleep(600);
        SetLastError(ERROR_BAD_NETPATH);
        return FALSE;
    }
    static DWORD WINAPI getLogicalDrives() { return logicalDrives_() | (1u << (letter_ - L'A')); }
    static UINT WINAPI getDriveType(LPCWSTR Root)
    {
        if (!isFixture(Root)) return driveType_(Root);
        // Match the observed Windows result for extended mapped-drive roots.
        return !wcsncmp(Root, L"\\\\?\\", 4) ? DRIVE_NO_ROOT_DIR : DRIVE_REMOTE;
    }
    static BOOL WINAPI getVolumeInformation(LPCWSTR Root, LPWSTR Label, DWORD Length, LPDWORD Serial, LPDWORD Component, LPDWORD Flags, LPWSTR Fs, DWORD FsLength)
    {
        return isFixture(Root) ? failMetadata() : volumeInformation_(Root, Label, Length, Serial, Component, Flags, Fs, FsLength);
    }
    static BOOL WINAPI getDiskFreeSpace(LPCWSTR Root, PULARGE_INTEGER Available, PULARGE_INTEGER Total, PULARGE_INTEGER Free)
    {
        return isFixture(Root) ? failMetadata() : diskFreeSpace_(Root, Available, Total, Free);
    }
    static DWORD WINAPI getFileAttributes(LPCWSTR Path)
    {
        if (!isFixture(Path)) return fileAttributes_(Path);
        ++attributeCalls_;
        SetLastError(ERROR_BAD_NETPATH);
        return INVALID_FILE_ATTRIBUTES;
    }
    struct Patch { ULONG_PTR* slot_; ULONG_PTR original_; };
    std::vector<Patch> patches_;
    bool installed_ = false;
    static inline wchar_t letter_ = 0;
    static inline DWORD guiThread_ = 0;
    static inline std::atomic<int> metadataCalls_{0};
    static inline std::atomic<int> guiMetadataCalls_{0};
    static inline std::atomic<int> attributeCalls_{0};
    static inline const auto logicalDrives_ = &GetLogicalDrives;
    static inline const auto driveType_ = &GetDriveTypeW;
    static inline const auto volumeInformation_ = &GetVolumeInformationW;
    static inline const auto diskFreeSpace_ = &GetDiskFreeSpaceExW;
    static inline const auto fileAttributes_ = &GetFileAttributesW;
};
