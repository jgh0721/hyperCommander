// Total Commander compatibility entry points; the implementation uses Unicode.
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include "externalLibs/TotalCommander/wcxhead.h"
#include "hcIso/isoImage.h"
#include <string>

// TC's plugin loader probes the original ANSI API even when all Unicode entry
// points are present. Fail explicitly if an ANSI-only host actually calls it.
extern "C" __declspec(dllexport) HANDLE __stdcall OpenArchive(tOpenArchiveData* Data)
{ if (Data) Data->OpenResult = E_NOT_SUPPORTED; return nullptr; }
extern "C" __declspec(dllexport) int __stdcall ReadHeader(HANDLE, tHeaderData*) { return E_NOT_SUPPORTED; }
extern "C" __declspec(dllexport) int __stdcall ReadHeaderEx(HANDLE, tHeaderDataEx*) { return E_NOT_SUPPORTED; }
extern "C" __declspec(dllexport) int __stdcall ProcessFile(HANDLE, int, char*, char*) { return E_NOT_SUPPORTED; }
extern "C" __declspec(dllexport) void __stdcall SetChangeVolProc(HANDLE, tChangeVolProc) {}
extern "C" __declspec(dllexport) void __stdcall SetProcessDataProc(HANDLE, tProcessDataProc) {}
extern "C" __declspec(dllexport) int __stdcall PackFiles(char*, char*, char*, char*, int) { return E_NOT_SUPPORTED; }
extern "C" __declspec(dllexport) int __stdcall DeleteFiles(char*, char*) { return E_NOT_SUPPORTED; }
extern "C" __declspec(dllexport) BOOL __stdcall CanYouHandleThisFile(char*) { return FALSE; }
extern "C" __declspec(dllexport) BOOL __stdcall CanYouHandleThisFileW(wchar_t* Path)
{
    if (!Path) return FALSE;
    try {
        const auto Length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, Path, -1, nullptr, 0, nullptr, nullptr);
        if (Length <= 0) return FALSE;
        std::string Utf8(size_t(Length), 0);
        if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, Path, -1, Utf8.data(), Length, nullptr, nullptr)) return FALSE;
        HcIsoImage* Image = nullptr; const auto Status = hcIsoOpen(Utf8.c_str(), HC_ISO_AUTO, &Image); hcIsoClose(Image);
        return Status == HC_ISO_OK;
    } catch (...) { return FALSE; }
}
