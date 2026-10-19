#include "pluginHost/dopusAbi.hpp"
#include <string>
#include <algorithm>
#include <vector>
#include <commctrl.h>
#include <filesystem>
#include <fstream>

using namespace hc::plugins::dopus;
#define API extern "C" __declspec(dllexport)
namespace
{
    HMODULE Module;
    bool Initialized = false;
    unsigned Instances = 0;
    struct Instance { long error_ = 0; unsigned cursors_ = 0, files_ = 0; std::filesystem::path cache_; };
    struct File { HANDLE handle_; bool shortWrite_, failWrite_; unsigned writes_ = 0; };
    void Require(bool Value) { if (!Value) RaiseException(0xc0000005, 0, 0, nullptr); }
    bool Fail(const wchar_t* Path) { if (wcsstr(Path, L"hang")) Sleep(INFINITE); if (wcsstr(Path, L"crash")) RaiseException(0xc0000005, 0, 0, nullptr); return false; }
    Instance& State(HANDLE Handle) { Require(Initialized && Handle); return *static_cast<Instance*>(Handle); }
    BOOL Result(HANDLE Handle, BOOL Good) { State(Handle).error_ = Good ? 0 : GetLastError(); return Good; }
    bool ArchivePath(const std::wstring& Path) { return Path.ends_with(L".hcvfs"); }
    void StoreArchive(const std::filesystem::path& Target, const std::vector<std::filesystem::path>& Sources)
    {
        std::vector<std::pair<std::filesystem::path, std::wstring>> Entries;
        for (const auto& Source : Sources) {
            Entries.emplace_back(Source, Source.filename().wstring());
            if (std::filesystem::is_directory(Source)) for (const auto& Child : std::filesystem::recursive_directory_iterator(Source))
                Entries.emplace_back(Child.path(), (Source.filename() / std::filesystem::relative(Child.path(), Source)).wstring());
        }
        std::ranges::sort(Entries, {}, [](const auto& Item) { return Item.second; });
        std::ofstream Out(Target, std::ios::binary | std::ios::trunc); const uint32_t Count = uint32_t(Entries.size()); Out.write(reinterpret_cast<const char*>(&Count), 4);
        for (const auto& [Source, Name] : Entries) {
            const uint32_t Length = uint32_t(Name.size()), Directory = std::filesystem::is_directory(Source); const uint64_t Size = Directory ? 0 : std::filesystem::file_size(Source);
            Out.write(reinterpret_cast<const char*>(&Length), 4); Out.write(reinterpret_cast<const char*>(Name.data()), Length * sizeof(wchar_t));
            Out.write(reinterpret_cast<const char*>(&Directory), 4); Out.write(reinterpret_cast<const char*>(&Size), 8);
            if (!Directory && Size) { std::ifstream Input(Source, std::ios::binary); Out << Input.rdbuf(); }
        }
        Require(Out.good());
    }
    std::wstring Mapped(HANDLE Handle, const wchar_t* Path)
    {
        const std::wstring Value(Path); const auto End = Value.find(L".hcvfs"); if (End == std::wstring::npos) return Value;
        const auto Root = Value.substr(0, End + 6); if (!std::filesystem::is_regular_file(Root)) return Value;
        auto& Cache = State(Handle).cache_;
        if (Cache.empty()) {
            wchar_t Base[MAX_PATH]{}; Require(GetTempPathW(MAX_PATH, Base)); Cache = std::filesystem::path(Base) / (L"hc-dopus-fixture-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
            Require(std::filesystem::create_directory(Cache)); std::ifstream Input(Root, std::ios::binary); uint32_t Count = 0; Input.read(reinterpret_cast<char*>(&Count), 4); Require(Count < 1000);
            for (uint32_t I = 0; I < Count; ++I) {
                uint32_t Length = 0, Directory = 0; uint64_t Size = 0; Input.read(reinterpret_cast<char*>(&Length), 4); Require(Length && Length < 1024);
                std::wstring Name(Length, 0); Input.read(reinterpret_cast<char*>(Name.data()), Length * sizeof(wchar_t)); Input.read(reinterpret_cast<char*>(&Directory), 4); Input.read(reinterpret_cast<char*>(&Size), 8); Require(Size < 16 * 1024 * 1024 && Name.find(L"..") == std::wstring::npos && Name.front() != L'\\');
                const auto To = Cache / Name; std::filesystem::create_directories(Directory ? To : To.parent_path());
                if (!Directory) { std::string Bytes(size_t(Size), 0); Input.read(Bytes.data(), std::streamsize(Size)); std::ofstream Output(To, std::ios::binary); Output.write(Bytes.data(), std::streamsize(Size)); Require(Output.good()); }
                Require(Input.good());
            }
        }
        auto Tail = Value.substr(End + 6); while (!Tail.empty() && Tail.front() == L'\\') Tail.erase(Tail.begin()); return (Cache / Tail).wstring();
    }
}
BOOL WINAPI DllMain(HINSTANCE Instance, DWORD Reason, void*) { if (Reason == DLL_PROCESS_ATTACH) Module = Instance; return TRUE; }
API BOOL VFS_Init(InitData* Init) { Require(Init->size_ == sizeof(*Init) && !Initialized); Initialized = true; return TRUE; }
API void VFS_Uninit() { Require(Initialized && !Instances); Initialized = false; }
namespace
{
    bool ViewerInitialized = false;
    unsigned ViewerWindows = 0;
    struct NativeViewerState { DWORD flags_; HANDLE abort_ = nullptr; };
    LRESULT CALLBACK ViewerProc(HWND Window, UINT Message, WPARAM WParam, LPARAM LParam)
    {
        if (Message == WM_CREATE)
        {
            auto* State = new NativeViewerState{DWORD(reinterpret_cast<CREATESTRUCTW*>(LParam)->lpCreateParams ? ViewerPreview : 0)};
            SetWindowLongPtrW(Window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(State)); ++ViewerWindows;
            NMHDR Notification{Window, 0, UINT(0 - 2000)};
            Require(SendMessageW(GetParent(Window), WM_NOTIFY, 0, reinterpret_cast<LPARAM>(&Notification)) == RGB(32, 32, 32));
        }
        auto* State = reinterpret_cast<NativeViewerState*>(GetWindowLongPtrW(Window, GWLP_USERDATA));
        if (Message == ViewerSetAbort) { Require(State); State->abort_ = reinterpret_cast<HANDLE>(LParam); return TRUE; }
        if (Message == WM_DESTROY)
        {
            Require(State && State->abort_ && WaitForSingleObject(State->abort_, 0) == WAIT_OBJECT_0);
            delete State; SetWindowLongPtrW(Window, GWLP_USERDATA, 0); --ViewerWindows;
        }
        if (Message == ViewerLoadW)
        {
            auto* Name = reinterpret_cast<wchar_t*>(LParam); Fail(Name);
            Require(State && State->abort_ && WaitForSingleObject(State->abort_, 0) == WAIT_TIMEOUT);
            if (wcsstr(Name, L"preview")) Require(State->flags_ == ViewerPreview);
            if (wcsstr(Name, L"full")) Require(State->flags_ == 0);
            SetWindowTextW(Window, Name); return wcsstr(Name, L"reject") ? FALSE : TRUE;
        }
        if (Message == ViewerResize) return TRUE;
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
    Require(ViewerInitialized && Info->size_ == sizeof(*Info) && Abort && WaitForSingleObject(Abort, 0) == WAIT_TIMEOUT); Fail(Name);
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
    Require(ViewerInitialized && (Flags == 0 || Flags == ViewerPreview)); WNDCLASSW Class{}; Class.lpfnWndProc = ViewerProc; Class.hInstance = Module; Class.lpszClassName = L"HC.Dopus.Fixture";
    RegisterClassW(&Class); return CreateWindowExW(0, Class.lpszClassName, L"", WS_CHILD, Rect->left, Rect->top, Rect->right - Rect->left, Rect->bottom - Rect->top, Parent, nullptr, Module, reinterpret_cast<void*>(ULONG_PTR(Flags)));
}
API BOOL DVP_LoadTextW(LoadTextData<wchar_t>* Data)
{
    Require(ViewerInitialized && Data->size_ == sizeof(*Data) && !Data->output_ && WaitForSingleObject(Data->abort_, 0) == WAIT_TIMEOUT);
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
    Info->flags_ = wcsstr(Path, L"multi") ? 5 : 1;
    Info->capabilities_ = wcsstr(Path, L"readonly") ? VfsReadOnly : VfsRandomSeek | VfsMoveByRename; return TRUE;
}
API HWND VFS_Configure(HWND Owner, HWND Notify, DWORD Data)
{
    Require(IsWindow(Owner) && IsWindow(Notify) && Data == 1);
    WNDCLASSW Class{}; Class.hInstance = Module; Class.lpszClassName = L"HC.Dopus.Configure";
    Class.lpfnWndProc = [](HWND W, UINT M, WPARAM P, LPARAM L) -> LRESULT { if (M == WM_TIMER) { DestroyWindow(W); return 0; } return DefWindowProcW(W, M, P, L); };
    RegisterClassW(&Class); const auto Window = CreateWindowExW(0, Class.lpszClassName, L"Fixture configuration", WS_OVERLAPPEDWINDOW, 0, 0, 200, 100, Owner, nullptr, Module, nullptr);
    Require(Window); SetTimer(Window, 1, 650, nullptr); PostMessageW(Notify, WM_APP + 0xfff, Data, 0); return Window;
}
API HANDLE VFS_Create(GUID*, HWND) { Require(Initialized); ++Instances; return new Instance; }
API BOOL VFS_QueryPathW(wchar_t* Extension, BOOL Prefix, GUID* Id) { if (Prefix || wcscmp(Extension, L".hcvfs")) return FALSE; *Id = {0x12345678, 0, 0, {}}; return TRUE; }
API BOOL VFS_PropGetW(HANDLE Handle, int Property, void* Out, void*, void*, void*)
{
    State(Handle); if (Property != VfsBatchProperty) return FALSE;
    wchar_t Path[1024]{}; GetModuleFileNameW(Module, Path, 1024);
    *static_cast<DWORD*>(Out) = wcsstr(Path, L"batch") || wcsstr(Path, L"archive") ? 2 : 0; return TRUE;
}
API UINT VFS_BatchOperationW(HANDLE Handle, void*, wchar_t* Path, VfsBatchData* Data)
{
    State(Handle); Require(Data->size_ == sizeof(*Data) && Data->count_ > 0 && Data->abort_);
    if (Data->operation_ == 1 && ArchivePath(Path)) {
        std::vector<std::filesystem::path> Sources; const auto* Name = Data->files_;
        for (int I = 0; I < Data->count_; ++I) { Sources.emplace_back(Name); Data->results_[I] = 0; Name += wcslen(Name) + 1; }
        StoreArchive(Path, Sources); return 4;
    }
    const auto* File = Data->files_;
    for (int Index = 0; Index < Data->count_; ++Index)
    {
        Fail(File);
        if (wcsstr(File, L"batch-partial")) return 4; // Missing per-file result must fail.
        if (wcsstr(File, L"batch-default")) return 0;
        if (wcsstr(File, L"batch-skip")) return 1;
        const auto* Slash = wcsrchr(File, L'\\'); const auto* Leaf = Slash ? Slash + 1 : File;
        const auto To = std::wstring(Data->operation_ == 1 ? Path : Data->destination_ ? Data->destination_ : L"") + L"\\" + Leaf;
        const auto From = Mapped(Handle, File);
        const bool Good = Data->operation_ == 3 ? DeleteFileW(File) : CopyFileW(From.c_str(), To.c_str(), FALSE);
        Data->results_[Index] = Good ? 0 : GetLastError();
        if (wcsstr(File, L"batch-each")) return 2 | 4096;
        File += wcslen(File) + 1;
    }
    return 4;
}
API void VFS_Destroy(HANDLE Handle) { auto& Value = State(Handle); Require(!Value.cursors_ && !Value.files_); if (!Value.cache_.empty()) std::filesystem::remove_all(Value.cache_); delete &Value; --Instances; }
API long VFS_GetLastError(HANDLE Handle) { return State(Handle).error_; }
API VfsCustomColumn<wchar_t>* VFS_GetCustomColumnsW(HANDLE Handle)
{
    State(Handle);
    static wchar_t Numeric[] = L"Native number", NumericKey[] = L"number", Text[] = L"Native text", TextKey[] = L"text", Percent[] = L"Native percent", PercentKey[] = L"percent";
    static VfsCustomColumn<wchar_t> Three{sizeof(Three), nullptr, Percent, PercentKey, 32, 3};
    static VfsCustomColumn<wchar_t> Two{sizeof(Two), &Three, Text, TextKey, 0, 2};
    static VfsCustomColumn<wchar_t> One{sizeof(One), &Two, Numeric, NumericKey, 16 | 1, 1}; return &One;
}
API int VFS_ReadDirectoryW(HANDLE Handle, void*, VfsReadDirectoryData* Data)
{
    State(Handle); Require(Data && Data->size_ == sizeof(*Data) && Data->operation_ == 1 && Data->heap_); Fail(Data->path_);
    std::wstring Path = Mapped(Handle, Data->path_); if (!Path.ends_with(L'\\')) Path += L'\\'; Path += L'*';
    WIN32_FIND_DATAW File{}; const auto Find = FindFirstFileW(Path.c_str(), &File);
    if (Find == INVALID_HANDLE_VALUE) return Result(Handle, FALSE);
    std::vector<WIN32_FIND_DATAW> Entries;
    do { Entries.push_back(File); } while (FindNextFileW(Find, &File)); FindClose(Find);
    if (wcsstr(Data->path_, L"large-list")) {
        Entries.clear(); Entries.reserve(65537);
        for (int Index = 0; Index < 65537; ++Index) {
            WIN32_FIND_DATAW Entry{}; Entry.dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
            const auto Name = L"entry" + std::to_wstring(Index) + L".txt"; wcscpy_s(Entry.cFileName, Name.c_str());
            Entry.nFileSizeLow = Index; Entries.push_back(Entry);
        }
    }
    const auto Bytes = sizeof(VfsFileHeader) + Entries.size() * sizeof(VfsFileData);
    auto* Header = static_cast<VfsFileHeader*>(HeapAlloc(Data->heap_, HEAP_ZERO_MEMORY, Bytes)); Require(Header);
    Header->size_ = sizeof(*Header); Header->count_ = int(Entries.size()); Header->entrySize_ = sizeof(VfsFileData); Data->files_ = Header;
    const auto String = [&](const std::wstring& Text) {
        auto* Value = static_cast<wchar_t*>(HeapAlloc(Data->heap_, 0, (Text.size() + 1) * sizeof(wchar_t))); Require(Value);
        memcpy(Value, Text.c_str(), (Text.size() + 1) * sizeof(wchar_t)); return Value;
    };
    auto* Items = reinterpret_cast<VfsFileData*>(Header + 1);
    for (std::size_t Index = 0; Index < Entries.size(); ++Index) {
        auto& Item = Items[Index]; Item.file_ = Entries[Index]; Item.columnCount_ = 3;
        Item.columns_ = static_cast<VfsFileColumn*>(HeapAlloc(Data->heap_, HEAP_ZERO_MEMORY, 3 * sizeof(VfsFileColumn))); Require(Item.columns_);
        Item.columns_[0] = {1, String(std::to_wstring(Item.file_.nFileSizeLow))};
        Item.columns_[1] = {2, String(L"VFS value 한글")}; Item.columns_[2] = {3, String(L"12.5")};
        if (wcsstr(Data->path_, L"bad-column-count")) Item.columnCount_ = 65;
    }
    return Result(Handle, TRUE);
}
API HANDLE VFS_FindFirstFileW(HANDLE Handle, void*, wchar_t* Path, WIN32_FIND_DATAW* Data, HANDLE)
{
    Fail(Path); const auto Found = FindFirstFileW(Path, Data); Result(Handle, Found != INVALID_HANDLE_VALUE);
    if (Found == INVALID_HANDLE_VALUE) return nullptr; ++State(Handle).cursors_; return Found;
}
API BOOL VFS_FindNextFileW(HANDLE Handle, void*, HANDLE Find, WIN32_FIND_DATAW* Data) { return Result(Handle, FindNextFileW(Find, Data)); }
API void VFS_FindClose(HANDLE Handle, HANDLE Find) { Require(FindClose(Find)); --State(Handle).cursors_; }
API HANDLE VFS_CreateFileW(HANDLE Handle, void*, wchar_t* Path, DWORD Mode, DWORD Attributes, DWORD Flags, FILETIME*)
{
    Fail(Path); const auto Local = Mapped(Handle, Path); const auto Native = CreateFileW(Local.c_str(), Mode, FILE_SHARE_READ, nullptr, Flags & 0xf, Attributes, nullptr);
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
API BOOL VFS_CreateDirectoryW(HANDLE Handle, void*, wchar_t* Path, DWORD) { if (ArchivePath(Path)) { StoreArchive(Path, {}); return TRUE; } return Result(Handle, CreateDirectoryW(Path, nullptr)); }
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
