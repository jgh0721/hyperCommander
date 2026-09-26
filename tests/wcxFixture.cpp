#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>
#include "externalLibs/TotalCommander/wcxhead.h"

// Deterministic ABI/error fixture. Real plugin coverage is separate.
struct Entry { std::wstring name_; std::string bytes_; bool directory_ = false; };
struct ArchiveState {
    std::vector<Entry> entries_;
    std::size_t index_ = 0;
    char mode_ = 0;
    tProcessDataProcW progress_ = nullptr;
    tChangeVolProcW volume_ = nullptr;
};
thread_local tProcessDataProcW PackingProgress = nullptr;
thread_local tPkCryptProcW PasswordCallback = nullptr;
int CheckPassword(wchar_t* Archive)
{
    if (!PasswordCallback) return E_NOT_SUPPORTED;
    wchar_t Password[128]{};
    if (PasswordCallback(1, PK_CRYPT_LOAD_PASSWORD_NO_UI, Archive, Password, 128) != E_NO_FILES) return E_BAD_DATA;
    for (int Attempt = 0; Attempt < 4; ++Attempt) {
        const auto Code = PasswordCallback(1, PK_CRYPT_LOAD_PASSWORD, Archive, Password, 128);
        const bool Correct = wcscmp(Password, L"비밀-pass") == 0; SecureZeroMemory(Password, sizeof(Password));
        if (Code) return Code; if (Correct) return 0;
    }
    return E_BAD_DATA;
}
extern "C" __declspec(dllexport) void __stdcall PkSetCryptCallbackW(tPkCryptProcW Callback, int, int) { PasswordCallback = Callback; }
std::string ReadBytes(const wchar_t* Path)
{
    const auto File = CreateFileW(Path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (File == INVALID_HANDLE_VALUE) return {};
    LARGE_INTEGER Size{};
    if (!GetFileSizeEx(File, &Size) || Size.QuadPart < 0 || Size.QuadPart > 64 * 1024 * 1024) { CloseHandle(File); return {}; }
    std::string Bytes(static_cast<std::size_t>(Size.QuadPart), 0); DWORD Count = 0;
    const auto Read = ReadFile(File, Bytes.data(), static_cast<DWORD>(Bytes.size()), &Count, nullptr); CloseHandle(File);
    return Read && Count == Bytes.size() ? Bytes : std::string{};
}
bool Load(const wchar_t* Path, ArchiveState& State)
{
    const auto Bytes = ReadBytes(Path);
    if (Bytes.starts_with("HCWCX01")) {
        State.mode_ = Bytes.size() > 7 ? Bytes[7] : 0;
        State.entries_ = {{L"alpha.txt", "alpha"}, {L"sub/한글.txt", "한글!\r\n"}}; return true;
    }
    if (!Bytes.starts_with("HCWCX02") || Bytes.size() < 12) return false;
    State.mode_ = Bytes[7]; std::size_t Offset = 8;
    const auto Number = [&]() -> std::uint32_t { std::uint32_t Value = 0; if (Bytes.size() - Offset < 4) { Offset = Bytes.size(); return 0xffffffff; }
        std::memcpy(&Value, Bytes.data() + Offset, 4); Offset += 4; return Value; };
    const auto Count = Number(); if (Count > 10000) return false;
    for (std::uint32_t Index = 0; Index < Count; ++Index) {
        const auto NameLength = Number(), ByteLength = Number(), Directory = Number();
        if (NameLength > 1023 || ByteLength > 64 * 1024 * 1024 || Bytes.size() - Offset < NameLength * 2ULL + ByteLength) return false;
        Entry Item; Item.name_.resize(NameLength); std::memcpy(Item.name_.data(), Bytes.data() + Offset, NameLength * 2); Offset += NameLength * 2;
        Item.bytes_ = Bytes.substr(Offset, ByteLength); Offset += ByteLength; Item.directory_ = Directory != 0; State.entries_.push_back(std::move(Item));
    }
    return Offset == Bytes.size();
}
bool Save(const wchar_t* Path, const ArchiveState& State)
{
    std::string Bytes = "HCWCX02"; Bytes += State.mode_;
    const auto Number = [&](std::uint32_t Value) { Bytes.append(reinterpret_cast<const char*>(&Value), 4); };
    Number(static_cast<std::uint32_t>(State.entries_.size()));
    for (const auto& Item : State.entries_) { Number(static_cast<std::uint32_t>(Item.name_.size())); Number(static_cast<std::uint32_t>(Item.bytes_.size())); Number(Item.directory_);
        Bytes.append(reinterpret_cast<const char*>(Item.name_.data()), Item.name_.size() * 2); Bytes += Item.bytes_; }
    const auto File = CreateFileW(Path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr); if (File == INVALID_HANDLE_VALUE) return false;
    DWORD Count = 0; const auto Written = WriteFile(File, Bytes.data(), static_cast<DWORD>(Bytes.size()), &Count, nullptr); CloseHandle(File);
    return Written && Count == Bytes.size();
}
std::uint32_t Crc(const std::string& Bytes)
{
    std::uint32_t Value = 0xffffffff;
    for (const auto Byte : Bytes) { Value ^= static_cast<unsigned char>(Byte); for (int Bit = 0; Bit < 8; ++Bit) Value = (Value >> 1) ^ ((Value & 1) ? 0xedb88320u : 0); }
    return Value ^ 0xffffffff;
}
std::wstring Normalize(std::wstring Name) { std::replace(Name.begin(), Name.end(), L'\\', L'/'); if (!Name.empty() && Name.back() == L'/') Name.pop_back(); return Name; }
extern "C" __declspec(dllexport) HANDLE __stdcall OpenArchiveW(tOpenArchiveDataW* Open)
{
    auto State = new ArchiveState;
    if (!Load(Open->ArcName, *State)) { delete State; Open->OpenResult = E_UNKNOWN_FORMAT; return nullptr; }
    if (State->mode_ == 'U') { Open->OpenResult = CheckPassword(Open->ArcName); if (Open->OpenResult) { delete State; return nullptr; } }
    return State;
}
extern "C" __declspec(dllexport) int __stdcall ReadHeaderExW(HANDLE Handle, tHeaderDataExW* Header)
{
    const auto State = static_cast<ArchiveState*>(Handle);
    if (State->index_ >= State->entries_.size()) return E_END_ARCHIVE;
    const auto& Item = State->entries_[State->index_];
    wcscpy_s(Header->FileName, Item.name_.c_str());
    if (State->mode_ == 'T') wcscpy_s(Header->FileName, L"../escape.txt");
    if (State->mode_ == 'D') wcscpy_s(Header->FileName, L"alpha.txt");
    Header->FileAttr = Item.directory_ ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
    Header->Flags = 0x12345600;
    if (State->mode_ == 'Q' || State->mode_ == 'R') Header->Flags |= 8;
    Header->UnpSize = static_cast<unsigned int>(Item.bytes_.size()); Header->PackSize = Header->UnpSize;
    Header->FileCRC = Crc(Item.bytes_); Header->FileTime = (46 << 25) | (9 << 21) | (7 << 16);
    return 0;
}
extern "C" __declspec(dllexport) int __stdcall ProcessFileW(HANDLE Handle, int Operation, wchar_t*, wchar_t* Destination)
{
    const auto State = static_cast<ArchiveState*>(Handle);
    if (State->index_ >= State->entries_.size()) return E_NO_FILES;
    const auto& Item = State->entries_[State->index_];
    if (Operation == PK_EXTRACT || Operation == PK_TEST)
    {
        if (State->mode_ == 'Q') { wchar_t Name[] = L"encrypted.hct"; const auto Code = CheckPassword(Name); if (Code) return Code; }
        if (State->mode_ == 'V' && State->volume_) { State->volume_(nullptr, PK_VOL_ASK); return E_EABORTED; }
        if (State->progress_ && !State->progress_(nullptr, 0)) return E_EABORTED;
        if (Operation == PK_EXTRACT) {
            const auto File = CreateFileW(Destination, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (File == INVALID_HANDLE_VALUE) return E_ECREATE;
            DWORD Count = 0; const auto Size = State->mode_ == 'S' ? 1 : static_cast<DWORD>(Item.bytes_.size());
            const auto Written = WriteFile(File, Item.bytes_.data(), Size, &Count, nullptr); CloseHandle(File);
            if (!Written || Count != Size) return E_EWRITE;
        }
        if (State->mode_ == 'C' || State->mode_ == 'K') return E_BAD_DATA;
        if (State->progress_ && !State->progress_(nullptr, static_cast<int>(Item.bytes_.size()))) return E_EABORTED;
    }
    else if (Operation != PK_SKIP) return E_NOT_SUPPORTED;
    ++State->index_; return 0;
}
extern "C" __declspec(dllexport) int __stdcall CloseArchive(HANDLE Handle) { delete static_cast<ArchiveState*>(Handle); return 0; }
extern "C" __declspec(dllexport) int __stdcall GetBackgroundFlags() { return BACKGROUND_UNPACK | BACKGROUND_PACK; }
extern "C" __declspec(dllexport) int __stdcall GetPackerCaps() { return PK_CAPS_NEW | PK_CAPS_MODIFY | PK_CAPS_DELETE | PK_CAPS_MULTIPLE | PK_CAPS_ENCRYPT; }
extern "C" __declspec(dllexport) void __stdcall SetProcessDataProcW(HANDLE Handle, tProcessDataProcW Callback) {
    if (Handle == INVALID_HANDLE_VALUE) PackingProgress = Callback;
    else if (Handle) static_cast<ArchiveState*>(Handle)->progress_ = Callback;
    else RaiseException(EXCEPTION_ACCESS_VIOLATION, EXCEPTION_NONCONTINUABLE, 0, nullptr);
}
extern "C" __declspec(dllexport) void __stdcall SetChangeVolProcW(HANDLE Handle, tChangeVolProcW Callback) { if (Handle && Handle != INVALID_HANDLE_VALUE) static_cast<ArchiveState*>(Handle)->volume_ = Callback; }
extern "C" __declspec(dllexport) int __stdcall PackFilesW(wchar_t* Archive, wchar_t*, wchar_t* Source, wchar_t* List, int Flags)
{
    ArchiveState State;
    if (GetFileAttributesW(Archive) != INVALID_FILE_ATTRIBUTES && !Load(Archive, State)) return E_UNKNOWN_FORMAT;
    if (State.mode_ == 'R' || (State.mode_ == 'Q' && !(Flags & PK_PACK_ENCRYPT))) State.mode_ = 0;
    for (auto Name = List; *Name; Name += wcslen(Name) + 1) {
        if (PackingProgress && !PackingProgress(nullptr, 0)) return E_EABORTED;
        Entry Item; Item.name_ = Normalize(Name); const auto Path = std::wstring(Source) + Name;
        const auto Attr = GetFileAttributesW(Path.c_str()); if (Attr == INVALID_FILE_ATTRIBUTES) return E_EREAD;
        Item.directory_ = Attr & FILE_ATTRIBUTE_DIRECTORY; if (!Item.directory_) Item.bytes_ = ReadBytes(Path.c_str());
        std::erase_if(State.entries_, [&](const Entry& Old) { return Old.name_ == Item.name_; }); State.entries_.push_back(std::move(Item));
    }
    if (!Save(Archive, State)) return E_EWRITE;
    if (State.mode_ == 'L') Sleep(1000); // Synchronous writer exceeding an ordinary RPC budget.
    if (State.mode_ == 'E' || State.mode_ == 'F' || State.mode_ == 'G')
    {
        HMODULE Module = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&PackFilesW), &Module);
        wchar_t ModulePath[32768]{}; GetModuleFileNameW(Module, ModulePath, DWORD(std::size(ModulePath)));
        const auto Path = std::wstring(ModulePath); const auto Helper = Path.substr(0, Path.find_last_of(L"\\")) + L"\\64\\7zG.exe";
        auto Command = L"\"" + Helper + L"\" " + std::to_wstring(GetCurrentProcessId()) + L" " + wchar_t(State.mode_);
        STARTUPINFOW Startup{}; Startup.cb = sizeof(Startup); PROCESS_INFORMATION Child{};
        if (!CreateProcessW(Helper.c_str(), Command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &Startup, &Child)) return E_ECREATE;
        CloseHandle(Child.hThread); CloseHandle(Child.hProcess);
        if (State.mode_ == 'G') { Sleep(200); RaiseException(EXCEPTION_ACCESS_VIOLATION, EXCEPTION_NONCONTINUABLE, 0, nullptr); }
    }
    // Host-only fault cases occur after the staged archive has been written.
    if (State.mode_ == 'H' || State.mode_ == 'J') {
        const auto Name = L"Local\\hc-wcx-stage-" + std::to_wstring(GetCurrentProcessId());
        const auto Signal = OpenEventW(EVENT_MODIFY_STATE, FALSE, Name.c_str());
        if (Signal) { SetEvent(Signal); CloseHandle(Signal); }
    }
    if (State.mode_ == 'H') Sleep(INFINITE);
    if (State.mode_ == 'J') RaiseException(EXCEPTION_ACCESS_VIOLATION, EXCEPTION_NONCONTINUABLE, 0, nullptr);
    if (PackingProgress && !PackingProgress(nullptr, 0)) return E_EABORTED;
    return State.mode_ == 'P' ? E_EWRITE : 0;
}
extern "C" __declspec(dllexport) int __stdcall DeleteFilesW(wchar_t* Archive, wchar_t* List)
{
    ArchiveState State; if (!Load(Archive, State)) return E_UNKNOWN_FORMAT;
    for (auto Name = List; *Name; Name += wcslen(Name) + 1) { const auto Path = Normalize(Name); std::erase_if(State.entries_, [&](const Entry& Old) { return Old.name_ == Path; }); }
    if (!Save(Archive, State)) return E_EWRITE;
    return State.mode_ == 'X' ? E_EWRITE : 0;
}
