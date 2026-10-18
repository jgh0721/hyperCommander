#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <string>
#include "externalLibs/TotalCommander/wcxhead.h"

#if HC_HOST_FAULT == 4
BOOL WINAPI DllMain(HINSTANCE, DWORD Reason, LPVOID)
{ if (Reason == DLL_PROCESS_DETACH) Sleep(INFINITE); return TRUE; }
#elif HC_HOST_FAULT == 5
BOOL WINAPI DllMain(HINSTANCE, DWORD Reason, LPVOID)
{ if (Reason == DLL_PROCESS_ATTACH) Sleep(750); return TRUE; }
#elif HC_HOST_FAULT == 6
BOOL WINAPI DllMain(HINSTANCE, DWORD Reason, LPVOID)
{ if (Reason == DLL_PROCESS_ATTACH) Sleep(INFINITE); return TRUE; }
#endif

extern "C" __declspec(dllexport) HANDLE __stdcall OpenArchiveW(tOpenArchiveDataW*) { return nullptr; }
extern "C" __declspec(dllexport) int __stdcall ReadHeaderExW(HANDLE, tHeaderDataExW*) { return E_END_ARCHIVE; }
extern "C" __declspec(dllexport) int __stdcall ProcessFileW(HANDLE, int, wchar_t*, wchar_t*) { return 0; }
extern "C" __declspec(dllexport) int __stdcall CloseArchive(HANDLE) { return 0; }
extern "C" __declspec(dllexport) int __stdcall GetPackerCaps()
{
#if HC_HOST_FAULT == 1
    RaiseException(EXCEPTION_ACCESS_VIOLATION, EXCEPTION_NONCONTINUABLE, 0, nullptr);
#elif HC_HOST_FAULT == 2
    Sleep(INFINITE);
#elif HC_HOST_FAULT == 3
    wchar_t System[MAX_PATH]{}; GetSystemDirectoryW(System, MAX_PATH);
    const auto Path = std::wstring(System) + L"\\cmd.exe"; wchar_t Command[] = L"cmd.exe /c exit 0";
    STARTUPINFOW Startup{}; Startup.cb = sizeof(Startup); PROCESS_INFORMATION Child{};
    if (CreateProcessW(Path.c_str(), Command, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &Startup, &Child))
    { TerminateProcess(Child.hProcess, 1); WaitForSingleObject(Child.hProcess, 1000); CloseHandle(Child.hThread); CloseHandle(Child.hProcess); return 1; }
#endif
    return 0;
}
