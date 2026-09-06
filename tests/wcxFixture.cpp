#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <cstring>
#include "externalLibs/TotalCommander/wcxhead.h"

// A deterministic read-only ABI fixture, not a general archive implementation.
extern "C" __declspec(dllexport) HANDLE __stdcall OpenArchiveW(tOpenArchiveDataW* Open)
{
    auto File = CreateFileW(Open->ArcName, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    char Magic[7]{};
    DWORD Count = 0;
    const bool Valid = File != INVALID_HANDLE_VALUE && ReadFile(File, Magic, sizeof(Magic), &Count, nullptr)
        && Count == sizeof(Magic) && memcmp(Magic, "HCWCX01", sizeof(Magic)) == 0;
    if (File != INVALID_HANDLE_VALUE) CloseHandle(File);
    if (!Valid) { Open->OpenResult = E_UNKNOWN_FORMAT; return nullptr; }
    return new int(0);
}
extern "C" __declspec(dllexport) int __stdcall ReadHeaderExW(HANDLE Handle, tHeaderDataExW* Header)
{
    const auto Index = *static_cast<int*>(Handle);
    if (Index >= 2) return E_END_ARCHIVE;
    wcscpy_s(Header->FileName, Index == 0 ? L"alpha.txt" : L"sub/한글.txt");
    Header->FileAttr = FILE_ATTRIBUTE_NORMAL;
    Header->UnpSize = Index == 0 ? 5 : 9;
    return 0;
}
extern "C" __declspec(dllexport) int __stdcall ProcessFileW(HANDLE Handle, int Operation, wchar_t*, wchar_t*)
{
    if (Operation != PK_SKIP) return E_NOT_SUPPORTED;
    ++*static_cast<int*>(Handle);
    return 0;
}
extern "C" __declspec(dllexport) int __stdcall CloseArchive(HANDLE Handle)
{
    delete static_cast<int*>(Handle);
    return 0;
}
