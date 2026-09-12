#include "pluginHost/dopusAbi.hpp"
#include <string>
#include <algorithm>

using namespace hc::plugins::dopus;
#define API extern "C" __declspec(dllexport)
namespace
{
    HMODULE Module;
    bool Initialized = false;
    unsigned Instances = 0;
    struct Instance { long error_ = 0; unsigned cursors_ = 0, files_ = 0; };
    struct File { HANDLE handle_; bool shortWrite_, failWrite_; unsigned writes_ = 0; };
    void Require(bool Value) { if (!Value) RaiseException(0xc0000005, 0, 0, nullptr); }
    bool Fail(const wchar_t* Path) { if (wcsstr(Path, L"hang")) Sleep(INFINITE); if (wcsstr(Path, L"crash")) RaiseException(0xc0000005, 0, 0, nullptr); return false; }
    Instance& State(HANDLE Handle) { Require(Initialized && Handle); return *static_cast<Instance*>(Handle); }
    BOOL Result(HANDLE Handle, BOOL Good) { State(Handle).error_ = Good ? 0 : GetLastError(); return Good; }
}
BOOL WINAPI DllMain(HINSTANCE Instance, DWORD Reason, void*) { if (Reason == DLL_PROCESS_ATTACH) Module = Instance; return TRUE; }
API BOOL VFS_Init(InitData* Init) { Require(Init->size_ == sizeof(*Init) && !Initialized); Initialized = true; return TRUE; }
API void VFS_Uninit() { Require(Initialized && !Instances); Initialized = false; }
API BOOL VFS_IdentifyW(VfsInfo<wchar_t>* Info)
{
    Require(Initialized && Info->size_ == sizeof(*Info));
    wcscpy_s(Info->name_, Info->nameCapacity_, L"HC DOpus fixture"); wcscpy_s(Info->extensions_, Info->extensionsCapacity_, L".hcvfs");
    Info->id_ = {0x11223344, 0x5566, 0x7788, {1,2,3,4,5,6,7,8}};
    wchar_t Path[1024]{}; GetModuleFileNameW(Module, Path, 1024);
    Info->capabilities_ = wcsstr(Path, L"readonly") ? VfsReadOnly : VfsRandomSeek | VfsMoveByRename; return TRUE;
}
API HANDLE VFS_Create(GUID*, HWND) { Require(Initialized); ++Instances; return new Instance; }
API void VFS_Destroy(HANDLE Handle) { auto& Value = State(Handle); Require(!Value.cursors_ && !Value.files_); delete &Value; --Instances; }
API long VFS_GetLastError(HANDLE Handle) { return State(Handle).error_; }
API HANDLE VFS_FindFirstFileW(HANDLE Handle, void*, wchar_t* Path, WIN32_FIND_DATAW* Data, HANDLE)
{
    Fail(Path); const auto Found = FindFirstFileW(Path, Data); Result(Handle, Found != INVALID_HANDLE_VALUE);
    if (Found == INVALID_HANDLE_VALUE) return nullptr; ++State(Handle).cursors_; return Found;
}
API BOOL VFS_FindNextFileW(HANDLE Handle, void*, HANDLE Find, WIN32_FIND_DATAW* Data) { return Result(Handle, FindNextFileW(Find, Data)); }
API void VFS_FindClose(HANDLE Handle, HANDLE Find) { Require(FindClose(Find)); --State(Handle).cursors_; }
API HANDLE VFS_CreateFileW(HANDLE Handle, void*, wchar_t* Path, DWORD Mode, DWORD Attributes, DWORD Flags, FILETIME*)
{
    Fail(Path); const auto Native = CreateFileW(Path, Mode, FILE_SHARE_READ, nullptr, Flags & 0xf, Attributes, nullptr);
    Result(Handle, Native != INVALID_HANDLE_VALUE); if (Native == INVALID_HANDLE_VALUE) return nullptr;
    ++State(Handle).files_; return new File{Native, bool(wcsstr(Path, L"short-write")), bool(wcsstr(Path, L"fail-write"))};
}
API BOOL VFS_ReadFile(HANDLE Handle, void*, HANDLE FileHandle, void* Data, DWORD Size, DWORD* Read)
{ return Result(Handle, ReadFile(static_cast<File*>(FileHandle)->handle_, Data, Size, Read, nullptr)); }
API BOOL VFS_WriteFile(HANDLE Handle, void*, HANDLE FileHandle, void* Data, DWORD Size, BOOL Flush, DWORD* Written)
{
    auto& Value = *static_cast<File*>(FileHandle);
    if (Value.failWrite_ && Value.writes_++) { SetLastError(ERROR_DISK_FULL); *Written = 0; return Result(Handle, FALSE); }
    const auto Good = WriteFile(Value.handle_, Data, Value.shortWrite_ ? std::min<DWORD>(Size, 17) : Size, Written, nullptr);
    return Result(Handle, Good && (!Flush || FlushFileBuffers(Value.handle_)));
}
API void VFS_CloseFile(HANDLE Handle, void*, HANDLE FileHandle) { auto* Value = static_cast<File*>(FileHandle); Require(CloseHandle(Value->handle_)); delete Value; --State(Handle).files_; }
API BOOL VFS_CreateDirectoryW(HANDLE Handle, void*, wchar_t* Path, DWORD) { return Result(Handle, CreateDirectoryW(Path, nullptr)); }
API BOOL VFS_RemoveDirectoryW(HANDLE Handle, void*, wchar_t* Path, DWORD) { return Result(Handle, RemoveDirectoryW(Path)); }
API BOOL VFS_DeleteFileW(HANDLE Handle, void*, wchar_t* Path, DWORD, int) { return Result(Handle, DeleteFileW(Path)); }
API BOOL VFS_MoveFileW(HANDLE Handle, void*, wchar_t* From, wchar_t* To) { return Result(Handle, MoveFileW(From, To)); }
API BOOL VFS_SetFileAttrW(HANDLE Handle, void*, wchar_t* Path, DWORD Attributes, BOOL) { return Result(Handle, SetFileAttributesW(Path, Attributes)); }
API BOOL VFS_SetFileTimeW(HANDLE Handle, void*, wchar_t* Path, HANDLE, FILETIME* Created, FILETIME* Accessed, FILETIME* Modified)
{
    auto File = CreateFileW(Path, FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (File == INVALID_HANDLE_VALUE) return Result(Handle, FALSE);
    const auto Good = Result(Handle, SetFileTime(File, Created, Accessed, Modified)); CloseHandle(File); return Good;
}
