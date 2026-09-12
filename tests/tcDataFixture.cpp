#include <windows.h>
#include <string>
#include <map>
#include <vector>
#include <cstring>
#include <cstdio>

#define API extern "C" __declspec(dllexport)
namespace
{
    std::map<std::wstring, std::string> Files{{L"\\hello.txt", "hello from WFX"}, {L"\\한글.txt", "unicode"}, {L"\\folder\\child.txt", "child"}};
    std::map<std::wstring, DWORD> Attributes;
    std::map<std::wstring, FILETIME> Modified;
    struct Enumeration { std::vector<std::wstring> names_; std::size_t at_ = 0; std::wstring parent_; };
    void Fill(Enumeration& Cursor, WIN32_FIND_DATAW* Data)
    {
        *Data = {}; const auto Name = Cursor.names_[Cursor.at_++]; wcscpy_s(Data->cFileName, Name.c_str());
        Data->dwFileAttributes = Name == L"folder" ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
        Data->nFileSizeLow = DWORD(Files[Cursor.parent_ + Name].size());
        Data->ftLastWriteTime = {123456789, 31000000};
        const auto Path = Cursor.parent_ + Name;
        if (Attributes.contains(Path)) Data->dwFileAttributes = Attributes[Path];
        if (Modified.contains(Path)) Data->ftLastWriteTime = Modified[Path];
    }
    void Fault(const wchar_t* Name)
    { if (wcsstr(Name, L"crash")) RaiseException(0xc0000005, 0, 0, nullptr); if (wcsstr(Name, L"hang")) Sleep(INFINITE); }
}
API int __stdcall FsInit(int, void*, void*, void*) { return 0; }
API int __stdcall FsInitW(int, void*, void*, void*) { return 0; }
API HANDLE __stdcall FsFindFirstW(wchar_t* Path, WIN32_FIND_DATAW* Data)
{
    Fault(Path); auto* Cursor = new Enumeration; Cursor->parent_ = Path; if (Cursor->parent_.back() != L'\\') Cursor->parent_ += L'\\';
    if (Cursor->parent_ == L"\\") Cursor->names_.push_back(L"folder");
    for (const auto& [Key, Value] : Files)
        if (Key.starts_with(Cursor->parent_)) { const auto Tail = Key.substr(Cursor->parent_.size()); if (!Tail.empty() && Tail.find(L'\\') == std::wstring::npos && Tail != L"folder") Cursor->names_.push_back(Tail); }
    if (Cursor->names_.empty()) { delete Cursor; SetLastError(ERROR_NO_MORE_FILES); return INVALID_HANDLE_VALUE; }
    Fill(*Cursor, Data); return Cursor;
}
API BOOL __stdcall FsFindNextW(HANDLE Handle, WIN32_FIND_DATAW* Data)
{ auto& Cursor = *static_cast<Enumeration*>(Handle); if (Cursor.at_ == Cursor.names_.size()) { SetLastError(ERROR_NO_MORE_FILES); return FALSE; } Fill(Cursor, Data); return TRUE; }
API int __stdcall FsFindClose(HANDLE Handle) { delete static_cast<Enumeration*>(Handle); return 0; }
API int __stdcall FsGetFileW(wchar_t* Remote, wchar_t* Local, int, void*)
{
    Fault(Remote); auto At = Files.find(Remote); if (At == Files.end()) return 2;
    const auto File = CreateFileW(Local, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr); if (File == INVALID_HANDLE_VALUE) return 4;
    DWORD Written; const auto Ok = WriteFile(File, At->second.data(), DWORD(At->second.size()), &Written, nullptr); CloseHandle(File); return Ok ? 0 : 4;
}
API int __stdcall FsPutFileW(wchar_t* Local, wchar_t* Remote, int Flags)
{
    Fault(Remote); if (!Flags && Files.contains(Remote)) return 1;
    const auto File = CreateFileW(Local, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr); if (File == INVALID_HANDLE_VALUE) return 3;
    std::string Bytes(GetFileSize(File, nullptr), '\0'); DWORD Read; const auto Ok = ReadFile(File, Bytes.data(), DWORD(Bytes.size()), &Read, nullptr); CloseHandle(File);
    if (!Ok) return 3; Files[Remote] = std::move(Bytes); return 0;
}
API BOOL __stdcall FsDeleteFileW(wchar_t* Name) { return Files.erase(Name) != 0; }
API BOOL __stdcall FsMkDirW(wchar_t*) { return TRUE; }
API BOOL __stdcall FsRemoveDirW(wchar_t*) { return TRUE; }
API BOOL __stdcall FsSetAttrW(wchar_t* Name, int Value) { if (!Files.contains(Name)) return FALSE; Attributes[Name] = DWORD(Value); return TRUE; }
API BOOL __stdcall FsSetTimeW(wchar_t* Name, FILETIME*, FILETIME*, FILETIME* Value) { if (!Files.contains(Name)) return FALSE; if (Value) Modified[Name] = *Value; return TRUE; }
API int __stdcall FsRenMovFileW(wchar_t* From, wchar_t* To, BOOL, BOOL, void*)
{ if (!Files.contains(From) || Files.contains(To)) return 1; Files[To] = std::move(Files[From]); Files.erase(From); return 0; }
API int __stdcall ContentGetSupportedField(int Index, char* Name, char* Units, int Max)
{
    if (Index >= 7) return 0; sprintf_s(Name, Max, "field%d", Index); Units[0] = 0;
    const int Types[]{2, 3, 6, 11, 4, 10, 11}; return Types[Index];
}
API int __stdcall ContentGetValueW(wchar_t* Name, int Field, int Unit, void* Value, int Max, int Flags)
{
    Fault(Name); if (Unit == 99) { memset(Value, 'X', Max); return 11; }
    if (Field == 0) { const long long Number = 9007199254740993LL; memcpy(Value, &Number, 8); return 2; }
    if (Field == 1) { const double Number = 3.25; memcpy(Value, &Number, 8); return 3; }
    if (Field == 2) { const int Number = 1; memcpy(Value, &Number, 4); return 6; }
    if (Field == 3) { wcscpy_s(static_cast<wchar_t*>(Value), Max / 2, L"한글 metadata"); return 11; }
    if (Field == 4) { const WORD Date[]{2026, 9, 12}; memcpy(Value, Date, 6); return 4; }
    if (Field == 5) { const long long Time = 134000000000000000LL; memcpy(Value, &Time, 8); return 10; }
    if (Field == 6) { if (Flags) return 0; wcscpy_s(static_cast<wchar_t*>(Value), Max / 2, L"resolved"); return 11; }
    return -1;
}
