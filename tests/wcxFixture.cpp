#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <cstring>
#include "externalLibs/TotalCommander/wcxhead.h"

// Deterministic ABI/error fixture. Real plugin coverage is separate.
struct ArchiveState { int index_ = 0; char mode_ = 0; tProcessDataProcW progress_ = nullptr; tChangeVolProcW volume_ = nullptr; };
extern "C" __declspec(dllexport) HANDLE __stdcall OpenArchiveW(tOpenArchiveDataW* Open)
{
    auto File = CreateFileW(Open->ArcName, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    char Magic[7]{};
    DWORD Count = 0;
    const bool Valid = File != INVALID_HANDLE_VALUE && ReadFile(File, Magic, sizeof(Magic), &Count, nullptr)
        && Count == sizeof(Magic) && memcmp(Magic, "HCWCX01", sizeof(Magic)) == 0;
    char Mode = 0;
    if (File != INVALID_HANDLE_VALUE) { ReadFile(File, &Mode, 1, &Count, nullptr); CloseHandle(File); }
    if (!Valid) { Open->OpenResult = E_UNKNOWN_FORMAT; return nullptr; }
    return new ArchiveState{0, Mode};
}
extern "C" __declspec(dllexport) int __stdcall ReadHeaderExW(HANDLE Handle, tHeaderDataExW* Header)
{
    const auto State = static_cast<ArchiveState*>(Handle);
    const auto Index = State->index_;
    if (Index >= 2) return E_END_ARCHIVE;
    wcscpy_s(Header->FileName, Index == 0 ? L"alpha.txt" : L"sub/한글.txt");
    if (State->mode_ == 'T') wcscpy_s(Header->FileName, L"../escape.txt");
    if (State->mode_ == 'D') wcscpy_s(Header->FileName, L"alpha.txt");
    Header->FileAttr = FILE_ATTRIBUTE_NORMAL;
    Header->UnpSize = Index == 0 ? 5 : 9;
    Header->PackSize = Header->UnpSize;
    Header->FileTime = (46 << 25) | (9 << 21) | (7 << 16);
    return 0;
}
extern "C" __declspec(dllexport) int __stdcall ProcessFileW(HANDLE Handle, int Operation, wchar_t*, wchar_t* Destination)
{
    const auto State = static_cast<ArchiveState*>(Handle);
    if (Operation == PK_EXTRACT)
    {
        if (State->mode_ == 'V' && State->volume_) { State->volume_(nullptr, PK_VOL_ASK); return E_EABORTED; }
        if (State->progress_ && !State->progress_(nullptr, 0)) return E_EABORTED;
        const auto File = CreateFileW(Destination, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (File == INVALID_HANDLE_VALUE) return E_ECREATE;
        const char* Bytes = State->index_ == 0 ? "alpha" : "한글!\r\n";
        DWORD Count = 0;
        const auto Size = State->mode_ == 'S' ? 1 : State->index_ == 0 ? 5 : 9;
        const auto Written = WriteFile(File, Bytes, Size, &Count, nullptr); CloseHandle(File);
        if (!Written || Count != Size) return E_EWRITE;
        if (State->mode_ == 'C') return E_BAD_DATA;
        if (State->progress_ && !State->progress_(nullptr, Size)) return E_EABORTED;
    }
    else if (Operation != PK_SKIP) return E_NOT_SUPPORTED;
    ++State->index_;
    return 0;
}
extern "C" __declspec(dllexport) int __stdcall CloseArchive(HANDLE Handle)
{
    delete static_cast<ArchiveState*>(Handle);
    return 0;
}
extern "C" __declspec(dllexport) int __stdcall GetBackgroundFlags() { return BACKGROUND_UNPACK; }
extern "C" __declspec(dllexport) void __stdcall SetProcessDataProcW(HANDLE Handle, tProcessDataProcW Callback) { static_cast<ArchiveState*>(Handle)->progress_ = Callback; }
extern "C" __declspec(dllexport) void __stdcall SetChangeVolProcW(HANDLE Handle, tChangeVolProcW Callback) { static_cast<ArchiveState*>(Handle)->volume_ = Callback; }
