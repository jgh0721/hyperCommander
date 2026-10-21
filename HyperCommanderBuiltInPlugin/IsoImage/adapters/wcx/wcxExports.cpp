// HyperCommander ISO WCX adapter. The LGPL engine is a replaceable shared DLL.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include "externalLibs/TotalCommander/wcxhead.h"
#include "hcIso/isoImage.h"
#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
namespace fs = std::filesystem;
struct Failure { int code_; };
void Require(bool Value, int Code = E_BAD_DATA) { if (!Value) throw Failure{Code}; }
void Check(int Code) { if (Code != HC_ISO_OK) throw Failure{Code == HC_ISO_CANCELLED ? E_EABORTED : Code == HC_ISO_UNSUPPORTED ? E_NOT_SUPPORTED : E_BAD_DATA}; }
template<class F> int Boundary(F&& Function) noexcept
{ try { Function(); return 0; } catch (const Failure& E) { return E.code_; } catch (const std::bad_alloc&) { return E_NO_MEMORY; } catch (...) { return E_BAD_DATA; } }
std::string Utf8(const std::wstring& Value)
{
    const int Length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, Value.data(), int(Value.size()), nullptr, 0, nullptr, nullptr);
    Require(Length > 0 || Value.empty()); std::string Result(size_t(Length), 0);
    if (Length) Require(WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, Value.data(), int(Value.size()), Result.data(), Length, nullptr, nullptr) == Length);
    return Result;
}
std::wstring Wide(const char* Value)
{
    const int Length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, Value, -1, nullptr, 0); Require(Length > 0);
    std::wstring Result(size_t(Length), 0); Require(MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, Value, -1, Result.data(), Length) == Length); Result.pop_back(); return Result;
}
std::wstring Name(std::wstring Value)
{
    std::replace(Value.begin(), Value.end(), L'\\', L'/'); if (!Value.empty() && Value.back() == L'/') Value.pop_back();
    Require(!Value.empty() && Value.size() < 1024 && Value.front() != L'/');
    size_t Start = 0;
    for (size_t I = 0; I <= Value.size(); ++I) {
        if (I != Value.size()) Require(Value[I] >= 32 && std::wstring_view(L":*?\"<>|").find(Value[I]) == std::wstring_view::npos);
        if (I == Value.size() || Value[I] == L'/') {
            const auto Part = Value.substr(Start, I - Start); Require(!Part.empty() && Part != L"." && Part != L".." && Part.back() != L'.' && Part.back() != L' ');
            auto Stem = Part.substr(0, Part.find('.')); for (auto& C : Stem) C = wchar_t(towupper(C));
            Require(Stem != L"CON" && Stem != L"PRN" && Stem != L"AUX" && Stem != L"NUL" && !(Stem.size() == 4 && (Stem.starts_with(L"COM") || Stem.starts_with(L"LPT")) && Stem[3] >= '1' && Stem[3] <= '9'));
            Start = I + 1;
        }
    }
    return Value;
}
struct FileInput {
    fs::path path_;
    HANDLE file_ = INVALID_HANDLE_VALUE;
    BY_HANDLE_FILE_INFORMATION identity_{};
    std::mutex mutex_;
    ~FileInput() { if (file_ != INVALID_HANDLE_VALUE) CloseHandle(file_); }
    bool Verify() const {
        const auto Current = CreateFileW(path_.c_str(), FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (Current == INVALID_HANDLE_VALUE) return false;
        BY_HANDLE_FILE_INFORMATION Info{};
        const bool Read = GetFileInformationByHandle(Current, &Info) != FALSE;
        CloseHandle(Current);
        return Read && !(Info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))
            && Info.dwVolumeSerialNumber == identity_.dwVolumeSerialNumber
            && Info.nFileIndexHigh == identity_.nFileIndexHigh && Info.nFileIndexLow == identity_.nFileIndexLow
            && Info.nFileSizeHigh == identity_.nFileSizeHigh && Info.nFileSizeLow == identity_.nFileSizeLow
            && CompareFileTime(&Info.ftLastWriteTime, &identity_.ftLastWriteTime) == 0;
    }
    static int ReadAt(void* Context, uint64_t Offset, void* Buffer, uint32_t Capacity, uint32_t* Read) {
        auto& Input = *static_cast<FileInput*>(Context);
        std::lock_guard Lock(Input.mutex_); *Read = 0;
        if (Offset > uint64_t(INT64_MAX) || !Input.Verify()) return HC_ISO_IO;
        LARGE_INTEGER Position; Position.QuadPart = LONGLONG(Offset); DWORD Count = 0;
        if (!SetFilePointerEx(Input.file_, Position, nullptr, FILE_BEGIN)
            || !ReadFile(Input.file_, Buffer, Capacity, &Count, nullptr) || !Input.Verify()) return HC_ISO_IO;
        *Read = Count; return HC_ISO_OK;
    }
};
struct ImageDeleter {
    std::shared_ptr<FileInput> input_;
    void operator()(HcIsoImage* Value) const { hcIsoClose(Value); }
};
using ImagePtr = std::unique_ptr<HcIsoImage, ImageDeleter>;
ImagePtr Open(const fs::path& Path)
{
    auto Input = std::make_shared<FileInput>(); Input->path_ = Path;
    Input->file_ = CreateFileW(Path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    Require(Input->file_ != INVALID_HANDLE_VALUE, E_EOPEN);
    Require(GetFileInformationByHandle(Input->file_, &Input->identity_)
        && !(Input->identity_.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)), E_EOPEN);
    HcIsoImage* Image = nullptr;
    const HcIsoInput Source{sizeof(HcIsoInput), (uint64_t(Input->identity_.nFileSizeHigh) << 32) | Input->identity_.nFileSizeLow,
        FileInput::ReadAt, Input.get()};
    Check(hcIsoOpenInput(&Source, HC_ISO_AUTO, &Image));
    return ImagePtr(Image, ImageDeleter{std::move(Input)});
}
struct State {
    ImagePtr image_;
    uint64_t count_ = 0, index_ = 0;
    tProcessDataProcW progress_ = nullptr;
};
thread_local tProcessDataProcW PackingProgress = nullptr;
thread_local std::wstring DefaultIni;
int Progress(void*, uint64_t, uint64_t) { return !PackingProgress || PackingProgress(nullptr, 0); }
void Extract(HcIsoImage* Image, uint64_t Index, const HcIsoEntry& Entry, const fs::path* Destination, tProcessDataProcW Callback)
{
    if (Entry.directory_) { if (Destination) fs::create_directories(*Destination); return; }
    std::ofstream Output;
    if (Destination) { fs::create_directories(Destination->parent_path()); Output.open(*Destination, std::ios::binary | std::ios::out | std::ios::noreplace); Require(bool(Output), E_ECREATE); }
    try {
        std::vector<uint8_t> Buffer(1024 * 1024);
        for (uint64_t Offset = 0; Offset < Entry.size_;) {
            Require(!Callback || Callback(nullptr, 0), E_EABORTED);
            uint32_t Read = 0; Check(hcIsoRead(Image, Index, Offset, Buffer.data(), uint32_t(std::min<uint64_t>(Buffer.size(), Entry.size_ - Offset)), &Read)); Require(Read != 0);
            if (Destination) { Output.write(reinterpret_cast<char*>(Buffer.data()), Read); Require(bool(Output), E_EWRITE); }
            Offset += Read; Require(!Callback || Callback(nullptr, int(Read)), E_EABORTED);
        }
        if (Destination) { Output.flush(); Require(bool(Output), E_EWRITE); }
    } catch (...) { Output.close(); if (Destination) { std::error_code Ignored; fs::remove(*Destination, Ignored); } throw; }
}
struct TemporaryDirectory {
    fs::path path_;
    TemporaryDirectory() {
        std::random_device Random;
        for (int I = 0; I < 32; ++I) {
            auto Candidate = fs::temp_directory_path() / (L"hc-iso-" + std::to_wstring(Random()) + L"-" + std::to_wstring(Random()));
            if (fs::create_directory(Candidate)) { path_ = std::move(Candidate); return; }
        }
        throw Failure{E_ECREATE};
    }
    ~TemporaryDirectory() { std::error_code Ignored; if (!path_.empty()) fs::remove_all(path_, Ignored); }
};
struct Item { std::string path_, disk_; bool directory_ = false; };
std::vector<std::wstring> MultiString(const wchar_t* List)
{
    Require(List != nullptr); std::vector<std::wstring> Result; size_t Offset = 0;
    while (List[Offset]) {
        const auto Length = wcsnlen_s(List + Offset, 1025); Require(Length < 1025 && Offset + Length < 2 * 1024 * 1024 && Result.size() < 100000);
        Result.push_back(Name(std::wstring(List + Offset, Length))); Offset += Length + 1;
    }
    return Result;
}
void Update(const wchar_t* Archive, const wchar_t* SubPath, const wchar_t* Source, const wchar_t* List, bool Delete)
{
    Require(Archive && (Delete || Source)); const fs::path Target(Archive); const bool Exists = fs::exists(Target);
    const auto OldSize = Exists ? fs::file_size(Target) : 0; const auto OldTime = Exists ? fs::last_write_time(Target) : fs::file_time_type{};
    TemporaryDirectory Temp; std::map<std::wstring, Item> Items;
    uint32_t Profile = HC_ISO_UDF_BRIDGE;
    std::string Label = "HYPERCOMMANDER";
    if (Exists) {
        auto Image = Open(Target); HcIsoInfo Info{}; Info.structSize_ = sizeof(Info); Check(hcIsoGetInfo(Image.get(), &Info));
        // A full rebuild must not silently discard boot catalogs or Rock Ridge.
        Require(Info.features_ == 0, E_NOT_SUPPORTED);
        const bool Joliet = (Info.namespaces_ & (1u << HC_ISO_JOLIET)) != 0;
        Profile = Info.namespaces_ & (1u << HC_ISO_UDF) ? (Joliet ? HC_ISO_UDF_BRIDGE : HC_ISO_UDF_NO_JOLIET) : Joliet ? HC_ISO_9660_JOLIET : HC_ISO_9660;
        Label = Info.volumeLabel_;
        uint64_t Bytes = 0;
        for (uint64_t I = 0; I < Info.entryCount_; ++I) {
            HcIsoEntry Entry{}; Entry.structSize_ = sizeof(Entry); Check(hcIsoGetEntry(Image.get(), I, &Entry));
            Require(Entry.size_ <= 128ull * 1024 * 1024 * 1024 - Bytes, E_TOO_MANY_FILES); Bytes += Entry.size_;
            const auto Key = Name(Wide(Entry.path_)); const auto Disk = Temp.path_ / std::to_wstring(I);
            if (!Entry.directory_) Extract(Image.get(), I, Entry, &Disk, PackingProgress);
            Items.emplace(Key, Item{Entry.path_, Entry.directory_ ? "" : Utf8(Disk.wstring()), Entry.directory_ != 0});
        }
    } else Require(!Delete, E_EOPEN);
    const auto Prefix = SubPath && *SubPath ? Name(SubPath) + L'/' : L"";
    for (const auto& Input : MultiString(List)) {
        Require(!PackingProgress || PackingProgress(nullptr, 0), E_EABORTED);
        const auto Key = Name(Prefix + Input);
        if (Delete) { std::erase_if(Items, [&](const auto& Pair) { return Pair.first == Key || Pair.first.starts_with(Key + L'/'); }); continue; }
        const auto Disk = fs::path(Source) / fs::path(Input); const auto Status = fs::symlink_status(Disk);
        Require(!fs::is_symlink(Status) && (fs::is_regular_file(Status) || fs::is_directory(Status)), E_EREAD);
        Require((GetFileAttributesW(Disk.c_str()) & FILE_ATTRIBUTE_REPARSE_POINT) == 0, E_NOT_SUPPORTED);
        Items[Key] = Item{Utf8(Key), Utf8(Disk.wstring()), fs::is_directory(Status)};
    }
    if (!Exists && !DefaultIni.empty()) {
        Profile = GetPrivateProfileIntW(L"IsoImage", L"Profile", HC_ISO_UDF_BRIDGE, DefaultIni.c_str());
        wchar_t Buffer[64]{}; GetPrivateProfileStringW(L"IsoImage", L"VolumeLabel", L"HYPERCOMMANDER", Buffer, 64, DefaultIni.c_str()); Label = Utf8(Buffer);
    }
    Require(Profile <= HC_ISO_UDF_NO_JOLIET, E_NOT_SUPPORTED);
    std::vector<HcIsoSource> Sources; for (const auto& [Key, Value] : Items) Sources.push_back({Value.disk_.empty() ? nullptr : Value.disk_.c_str(), Value.path_.c_str(), uint32_t(Value.directory_)});
    // Staging beside the destination keeps the final rename on the same volume.
    const auto Stage = Target.parent_path() / (Target.filename().wstring() + L".hc-" + Temp.path_.filename().wstring());
    HcIsoOptions Options{sizeof(HcIsoOptions), Profile, Label.c_str(), 128ull * 1024 * 1024 * 1024, 0, Progress, nullptr};
    Check(hcIsoBuild(Utf8(Stage.wstring()).c_str(), Sources.data(), Sources.size(), &Options));
    try {
        Require(!PackingProgress || PackingProgress(nullptr, 0), E_EABORTED);
        Require(fs::exists(Target) == Exists && (!Exists || (fs::file_size(Target) == OldSize && fs::last_write_time(Target) == OldTime)), E_EWRITE);
        Require(MoveFileExW(Stage.c_str(), Target.c_str(), MOVEFILE_WRITE_THROUGH | (Exists ? MOVEFILE_REPLACE_EXISTING : 0)), E_EWRITE);
    } catch (...) { std::error_code Ignored; fs::remove(Stage, Ignored); throw; }
}
}

extern "C" __declspec(dllexport) HANDLE __stdcall OpenArchiveW(tOpenArchiveDataW* Data)
{
    if (!Data) return nullptr;
    std::unique_ptr<State> Value;
    Data->OpenResult = Boundary([&] {
        Value = std::make_unique<State>(); Value->image_ = Open(Data->ArcName);
        HcIsoInfo Info{}; Info.structSize_ = sizeof(Info); Check(hcIsoGetInfo(Value->image_.get(), &Info)); Value->count_ = Info.entryCount_;
    });
    return Data->OpenResult ? nullptr : Value.release();
}
extern "C" __declspec(dllexport) int __stdcall ReadHeaderExW(HANDLE Handle, tHeaderDataExW* Header)
{
    return Boundary([&] {
        auto* Value = static_cast<State*>(Handle); Require(Value && Header); Require(Value->index_ < Value->count_, E_END_ARCHIVE);
        HcIsoEntry Entry{}; Entry.structSize_ = sizeof(Entry); Check(hcIsoGetEntry(Value->image_.get(), Value->index_, &Entry));
        auto Path = Name(Wide(Entry.path_)); Require(Path.size() < std::size(Header->FileName), E_SMALL_BUF);
        // TC builds its archive directory tree from Windows path separators.
        std::replace(Path.begin(), Path.end(), L'/', L'\\');
        *Header = {}; std::copy(Path.begin(), Path.end(), Header->FileName);
        Header->FileAttr = Entry.directory_ ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
        Header->UnpSize = Header->PackSize = uint32_t(Entry.size_); Header->UnpSizeHigh = Header->PackSizeHigh = uint32_t(Entry.size_ >> 32);
        Header->FileTime = (20 << 25) | (1 << 21) | (1 << 16);
    });
}
extern "C" __declspec(dllexport) int __stdcall ProcessFileW(HANDLE Handle, int Operation, wchar_t* Path, wchar_t* NameValue)
{
    return Boundary([&] {
        auto* Value = static_cast<State*>(Handle); Require(Value && Value->index_ < Value->count_, E_NO_FILES);
        Require(Operation == PK_SKIP || Operation == PK_TEST || Operation == PK_EXTRACT, E_NOT_SUPPORTED);
        if (Operation != PK_SKIP) {
            HcIsoEntry Entry{}; Entry.structSize_ = sizeof(Entry); Check(hcIsoGetEntry(Value->image_.get(), Value->index_, &Entry));
            fs::path Destination;
            if (Operation == PK_EXTRACT) { Require(NameValue || Path, E_ECREATE); Destination = Path ? fs::path(Path) : fs::path{}; if (NameValue) Destination /= NameValue; else Destination /= Name(Wide(Entry.path_)); }
            Extract(Value->image_.get(), Value->index_, Entry, Operation == PK_EXTRACT ? &Destination : nullptr, Value->progress_);
        }
        ++Value->index_;
    });
}
extern "C" __declspec(dllexport) int __stdcall CloseArchive(HANDLE Handle) { delete static_cast<State*>(Handle); return 0; }
extern "C" __declspec(dllexport) void __stdcall SetProcessDataProcW(HANDLE Handle, tProcessDataProcW Callback)
{ if (Handle == INVALID_HANDLE_VALUE) PackingProgress = Callback; else if (Handle) static_cast<State*>(Handle)->progress_ = Callback; }
extern "C" __declspec(dllexport) void __stdcall SetChangeVolProcW(HANDLE, tChangeVolProcW) {}
extern "C" __declspec(dllexport) void __stdcall PackSetDefaultParams(PackDefaultParamStruct* Params)
{ if (Params && Params->size >= sizeof(*Params)) { const int Length = MultiByteToWideChar(CP_ACP, 0, Params->DefaultIniName, -1, nullptr, 0); DefaultIni.resize(size_t(std::max(0, Length))); if (Length > 0) { MultiByteToWideChar(CP_ACP, 0, Params->DefaultIniName, -1, DefaultIni.data(), Length); DefaultIni.pop_back(); } } }
extern "C" __declspec(dllexport) int __stdcall PackFilesW(wchar_t* Archive, wchar_t* SubPath, wchar_t* Source, wchar_t* List, int Flags)
{ return Boundary([&] { Require((Flags & (PK_PACK_MOVE_FILES | PK_PACK_ENCRYPT)) == 0, E_NOT_SUPPORTED); Update(Archive, SubPath, Source, List, false); }); }
extern "C" __declspec(dllexport) int __stdcall DeleteFilesW(wchar_t* Archive, wchar_t* List)
{ return Boundary([&] { Update(Archive, nullptr, nullptr, List, true); }); }
extern "C" __declspec(dllexport) int __stdcall GetPackerCaps() { return PK_CAPS_NEW | PK_CAPS_MODIFY | PK_CAPS_MULTIPLE | PK_CAPS_DELETE | PK_CAPS_BY_CONTENT; }
extern "C" __declspec(dllexport) int __stdcall GetBackgroundFlags() { return BACKGROUND_UNPACK | BACKGROUND_PACK; }
