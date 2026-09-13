#include "pluginHost/dopusAbi.hpp"
#include <string>
#include <algorithm>
#include <vector>

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
namespace
{
    bool ViewerInitialized = false;
    unsigned ViewerWindows = 0;
    LRESULT CALLBACK ViewerProc(HWND Window, UINT Message, WPARAM WParam, LPARAM LParam)
    {
        if (Message == WM_CREATE) ++ViewerWindows;
        if (Message == WM_DESTROY) --ViewerWindows;
        if (Message == ViewerLoadW)
        {
            auto* Name = reinterpret_cast<wchar_t*>(LParam); Fail(Name);
            SetWindowTextW(Window, Name); return wcsstr(Name, L"reject") ? FALSE : TRUE;
        }
        if (Message == ViewerResize) { SetWindowLongPtrW(Window, GWLP_USERDATA, LParam); return TRUE; }
        return DefWindowProcW(Window, Message, WParam, LParam);
    }
}
API BOOL DVP_InitEx(InitData* Init) { Require(!ViewerInitialized && Init->size_ == sizeof(*Init)); ViewerInitialized = true; return TRUE; }
API BOOL DVP_Init() { Require(false); return FALSE; } // InitEx must take precedence.
API void DVP_Uninit() { Require(ViewerInitialized && !ViewerWindows); ViewerInitialized = false; }
API BOOL DVP_IdentifyW(ViewerInfo<wchar_t>* Info)
{
    Require(ViewerInitialized && Info->size_ == sizeof(*Info));
    wcscpy_s(Info->name_, Info->nameCapacity_, L"DOpus viewer fixture");
    wcscpy_s(Info->extensions_, Info->extensionsCapacity_, L".bitmap;.native;.text"); return TRUE;
}
API BOOL DVP_IdentifyFileW(HWND, wchar_t* Name, ViewerFileInfo<wchar_t>* Info, HANDLE Abort)
{
    Require(ViewerInitialized && Info->size_ == sizeof(*Info) && Abort); Fail(Name);
    if (wcsstr(Name, L"unknown")) return FALSE;
    Info->flags_ = wcsstr(Name, L".native") ? ViewerWindow : wcsstr(Name, L".text") ? ViewerText : ViewerBitmap;
    Info->imageSize_ = {2, 2}; Info->bits_ = 32; Info->majorType_ = 1; Info->minorType_ = 42;
    Info->privateData_[0] = 0x12345678;
    wcscpy_s(Info->description_, Info->descriptionCapacity_, L"fixture metadata 한글"); return TRUE;
}
API HBITMAP DVP_LoadBitmapW(HWND, wchar_t*, ViewerFileInfo<wchar_t>* Info, SIZE*, HANDLE)
{
    Require(Info->privateData_[0] == 0x12345678);
    BITMAPINFO Data{}; Data.bmiHeader = {sizeof(BITMAPINFOHEADER), 2, -2, 1, 32, BI_RGB};
    void* Pixels = nullptr; auto Bitmap = CreateDIBSection(nullptr, &Data, DIB_RGB_COLORS, &Pixels, nullptr, 0);
    Require(Bitmap && Pixels); const DWORD Colors[]{0xff0000, 0x00ff00, 0x0000ff, 0xffffff}; memcpy(Pixels, Colors, sizeof(Colors)); return Bitmap;
}
API HWND DVP_CreateViewer(HWND Parent, RECT* Rect, DWORD Flags)
{
    Require(ViewerInitialized && Flags == 0); WNDCLASSW Class{}; Class.lpfnWndProc = ViewerProc; Class.hInstance = Module; Class.lpszClassName = L"HC.Dopus.Fixture";
    RegisterClassW(&Class); return CreateWindowExW(0, Class.lpszClassName, L"", WS_CHILD, Rect->left, Rect->top, Rect->right - Rect->left, Rect->bottom - Rect->top, Parent, nullptr, Module, nullptr);
}
API BOOL DVP_LoadTextW(LoadTextData<wchar_t>* Data)
{
    Require(ViewerInitialized && Data->size_ == sizeof(*Data) && !Data->output_);
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &Data->output_))) return FALSE;
    const bool Large = wcsstr(Data->file_, L"large");
    std::string Bytes = Large ? std::string(600 * 1024, 'x') : "converted fixture text";
    Data->textType_ = wcsstr(Data->file_, L"html") ? 2 : wcsstr(Data->file_, L"rich") ? 1 : 0;
    ULONG Written = 0; return SUCCEEDED(Data->output_->Write(Bytes.data(), ULONG(Bytes.size()), &Written)) && Written == Bytes.size();
}
API BOOL VFS_IdentifyW(VfsInfo<wchar_t>* Info)
{
    Require(Initialized && Info->size_ == sizeof(*Info));
    wcscpy_s(Info->name_, Info->nameCapacity_, L"HC DOpus fixture"); wcscpy_s(Info->extensions_, Info->extensionsCapacity_, L".hcvfs");
    Info->id_ = {0x11223344, 0x5566, 0x7788, {1,2,3,4,5,6,7,8}};
    wchar_t Path[1024]{}; GetModuleFileNameW(Module, Path, 1024);
    Info->capabilities_ = wcsstr(Path, L"readonly") ? VfsReadOnly : VfsRandomSeek | VfsMoveByRename; return TRUE;
}
API HANDLE VFS_Create(GUID*, HWND) { Require(Initialized); ++Instances; return new Instance; }
API BOOL VFS_PropGetW(HANDLE Handle, int Property, void* Out, void*, void*, void*)
{
    State(Handle); if (Property != VfsBatchProperty) return FALSE;
    wchar_t Path[1024]{}; GetModuleFileNameW(Module, Path, 1024);
    *static_cast<DWORD*>(Out) = wcsstr(Path, L"batch") ? 2 : 0; return TRUE;
}
API UINT VFS_BatchOperationW(HANDLE Handle, void*, wchar_t* Path, VfsBatchData* Data)
{
    State(Handle); Require(Data->size_ == sizeof(*Data) && Data->count_ > 0 && Data->abort_);
    const auto* File = Data->files_;
    for (int Index = 0; Index < Data->count_; ++Index)
    {
        Fail(File);
        if (wcsstr(File, L"batch-partial")) return 4; // Missing per-file result must fail.
        if (wcsstr(File, L"batch-default")) return 0;
        if (wcsstr(File, L"batch-skip")) return 1;
        const auto* Slash = wcsrchr(File, L'\\'); const auto* Leaf = Slash ? Slash + 1 : File;
        const auto To = std::wstring(Data->operation_ == 1 ? Path : Data->destination_ ? Data->destination_ : L"") + L"\\" + Leaf;
        const bool Good = Data->operation_ == 3 ? DeleteFileW(File) : CopyFileW(File, To.c_str(), FALSE);
        Data->results_[Index] = Good ? 0 : GetLastError();
        if (wcsstr(File, L"batch-each")) return 2 | 4096;
        File += wcslen(File) + 1;
    }
    return 4;
}
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
