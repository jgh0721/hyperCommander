#include "stdafx.h"
#include <QtTest>
#include <QTemporaryDir>
#include <QProcess>
#include <QFile>
#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QElapsedTimer>
#include <QScopeGuard>
#include "UniqueLibs/qtJobJournal.hpp"
#include "UniqueLibs/nativeWritableStorage.hpp"
#include "UniqueLibs/storageProviders.hpp"
#include "UniqueLibs/operationMetrics.hpp"
#include "commonLibs/operations/fileOperation.hpp"
#include <Windows.h>
#include <TlHelp32.h>
#include <winioctl.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <execution>
#include <map>
#include <mutex>
#include <random>
#include <thread>

using namespace hc::storage;
using namespace hc::operations;

// Opt-in copy/move comparison. Set HC_TRANSFER_BENCH_ROOT to an owned folder
// and, for cross-volume runs, HC_TRANSFER_BENCH_OTHER_ROOT on another volume.
// Fixtures, restoration and verification stay outside the measured interval;
// the product path includes planning, scheduler steps and result recording.
namespace {
constexpr std::uint64_t MiB = 1024ull * 1024;
// Tree shapes follow D:\QtSrc\qt (measured 2026-11-03). Sizes fit a log-normal
// distribution: qtbase 23,477 files / 4,106 folders, median 921 B, p90 17 KB,
// p99 156 KB, average file depth 6.2 (max 15); qtwebengine 269,995 files /
// 28,553 folders, median 2.6 KB, p90 20 KB, p99 166 KB, depth 8.9 (max 20).
struct Scenario
{
    const char* name_ = "";
    const char* description_ = "";
    std::size_t files_ = 0, directories_ = 0;
    double depthMean_ = 1, depthSpread_ = 0;
    std::size_t maxDepth_ = 1;
    double sizeMu_ = 0, sizeSigma_ = 0;
    std::uint64_t sizeMax_ = 0;
    std::vector<std::uint64_t> sizes_; // explicit large-file payloads
    bool heavy_ = false;
};
std::vector<Scenario> Scenarios()
{
    std::vector<Scenario> Values;
    Values.push_back({"qt-repo", "Qt repository shape: many small files", 23477, 4106, 5.2, 2.4, 15, std::log(921.0), 2.28, 14 * MiB});
    Values.push_back({"many-folders", "30,000 folders holding 10,000 small files", 10000, 30000, 4.0, 1.8, 8, std::log(1024.0), 1.0, 64 * 1024});
    Values.push_back({"webengine", "Qt WebEngine shape: very many files and folders", 269995, 28553, 7.9, 3.0, 20, std::log(2616.0), 1.7, 13 * MiB, {}, true});
    Scenario Few{"medium-few", "Four files of 200-500 MiB"};
    Few.sizes_ = {200 * MiB, 300 * MiB, 400 * MiB, 500 * MiB};
    Values.push_back(Few);
    Scenario Many{"medium-many", "32 files of 100-500 MiB in four folders"};
    Many.directories_ = 4; Many.heavy_ = true;
    std::mt19937_64 Random(7);
    std::uniform_int_distribution<std::uint64_t> Size(100, 500);
    for (int Index = 0; Index < 32; ++Index) Many.sizes_.push_back(Size(Random) * MiB);
    Values.push_back(Many);
    Scenario Huge{"huge", "Three files of 2, 4 and 6 GiB"};
    Huge.sizes_ = {2048 * MiB, 4096 * MiB, 6144 * MiB}; Huge.heavy_ = true;
    Values.push_back(Huge);
    return Values;
}
struct TreePlan
{
    // Relative to the tree root, parents first; [0] is the root itself.
    std::vector<std::wstring> directories_{L""};
    std::vector<std::size_t> depths_{0};
    struct File { std::size_t directory_ = 0; std::wstring name_; std::uint64_t size_ = 0; };
    std::vector<File> files_;
    std::uint64_t bytes_ = 0;
    double averageFileDepth_ = 0;
};
std::wstring RandomName(std::mt19937_64& Random, std::size_t Index, std::size_t MinLetters, std::size_t MaxLetters)
{
    static constexpr wchar_t Letters[] = L"abcdefghijklmnopqrstuvwxyz";
    static constexpr wchar_t Digits[] = L"0123456789abcdefghijklmnopqrstuvwxyz";
    std::uniform_int_distribution<std::size_t> Length(MinLetters, MaxLetters), Letter(0, 25);
    std::wstring Name;
    for (auto Count = Length(Random); Count; --Count) Name += Letters[Letter(Random)];
    Name += L'_';
    for (auto Value = Index;; Value /= 36) { Name += Digits[Value % 36]; if (Value < 36) break; }
    return Name;
}
std::uint64_t Seed(const char* Name)
{
    std::uint64_t Value = 1469598103934665603ull; // FNV-1a: stable across runs and compilers
    for (; *Name; ++Name) { Value ^= std::uint8_t(*Name); Value *= 1099511628211ull; }
    return Value;
}
TreePlan PlanTree(const Scenario& Shape, double Scale)
{
    TreePlan Plan;
    std::mt19937_64 Random(Seed(Shape.name_));
    const auto Scaled = [&](std::size_t Count) { return Count ? std::max<std::size_t>(1, std::size_t(std::llround(double(Count) * Scale))) : std::size_t{0}; };
    double DepthSum = 0;
    if (!Shape.sizes_.empty()) {
        for (std::size_t Index = 1; Index <= Shape.directories_; ++Index) { Plan.directories_.push_back(L"part_" + std::to_wstring(Index)); Plan.depths_.push_back(1); }
        for (std::size_t Index = 0; Index < Shape.sizes_.size(); ++Index) {
            const auto Size = std::max<std::uint64_t>(4096, std::uint64_t(double(Shape.sizes_[Index]) * Scale));
            const auto Directory = Shape.directories_ ? 1 + Index % Shape.directories_ : 0;
            Plan.files_.push_back({Directory, L"payload_" + std::to_wstring(Index) + L".bin", Size});
            Plan.bytes_ += Size; DepthSum += double(Plan.depths_[Directory] + 1);
        }
    } else {
        // Sorted target depths guarantee a parent level for every new folder.
        std::normal_distribution<double> Depth(Shape.depthMean_, Shape.depthSpread_);
        std::vector<std::size_t> Targets(Scaled(Shape.directories_));
        for (auto& Target : Targets) Target = std::size_t(std::clamp<long long>(std::llround(Depth(Random)), 1, (long long)Shape.maxDepth_));
        std::sort(Targets.begin(), Targets.end());
        std::vector<std::vector<std::size_t>> Levels(Shape.maxDepth_ + 1);
        Levels[0].push_back(0);
        for (std::size_t Index = 0; Index < Targets.size(); ++Index) {
            auto Target = Targets[Index];
            while (Levels[Target - 1].empty()) --Target;
            const auto& Parents = Levels[Target - 1];
            const auto Parent = Parents[std::uniform_int_distribution<std::size_t>(0, Parents.size() - 1)(Random)];
            const auto Base = Plan.directories_[Parent];
            Plan.directories_.push_back((Base.empty() ? std::wstring{} : Base + L"\\") + RandomName(Random, Index, 3, 10));
            Plan.depths_.push_back(Target);
            Levels[Target].push_back(Plan.directories_.size() - 1);
        }
        static constexpr const wchar_t* Extensions[] = {L".cpp", L".h", L".txt", L".png", L".qml", L".json", L".py", L".cmake"};
        std::normal_distribution<double> Size(Shape.sizeMu_, Shape.sizeSigma_);
        std::uniform_int_distribution<std::size_t> Directory(0, Plan.directories_.size() - 1), Extension(0, std::size(Extensions) - 1);
        for (std::size_t Index = 0, Count = Scaled(Shape.files_); Index < Count; ++Index) {
            const auto Bytes = std::min<std::uint64_t>(Shape.sizeMax_, std::uint64_t(std::exp(Size(Random))));
            const auto At = Directory(Random);
            Plan.files_.push_back({At, RandomName(Random, Index, 4, 14) + Extensions[Extension(Random)], Bytes});
            Plan.bytes_ += Bytes; DepthSum += double(Plan.depths_[At] + 1);
        }
    }
    Plan.averageFileDepth_ = Plan.files_.empty() ? 0 : DepthSum / double(Plan.files_.size());
    return Plan;
}
std::wstring Long(const QString& Path) { return L"\\\\?\\" + QDir::toNativeSeparators(QDir::cleanPath(Path)).toStdWString(); }
std::wstring Join(const std::wstring& Parent, const std::wstring& Name) { return Name.empty() ? Parent : Parent + L"\\" + Name; }
bool Exists(const std::wstring& Path) { return GetFileAttributesW(Path.c_str()) != INVALID_FILE_ATTRIBUTES; }
QString Win32Text(const char* Stage, DWORD Code, const std::wstring& Path = {})
{
    return QStringLiteral("%1: win32:%2 %3").arg(QLatin1String(Stage)).arg(Code).arg(QString::fromStdWString(Path));
}
template<class F> void Parallel(std::size_t Count, std::size_t Workers, F&& Work)
{
    std::atomic<std::size_t> Next{0};
    std::vector<std::jthread> Threads;
    for (std::size_t Index = 0; Index < std::max<std::size_t>(1, std::min(Workers, Count)); ++Index)
        Threads.emplace_back([&] { for (std::size_t Item; (Item = Next.fetch_add(1)) < Count;) Work(Item); });
}
const std::vector<char>& Pool()
{
    // Incompressible bytes; no filesystem may shortcut identical blocks.
    static const std::vector<char> Bytes = [] {
        std::vector<char> Value(64 * MiB);
        std::uint64_t State = 0x9e3779b97f4a7c15ull;
        for (std::size_t Offset = 0; Offset + 8 <= Value.size(); Offset += 8) {
            State ^= State << 13; State ^= State >> 7; State ^= State << 17;
            std::memcpy(Value.data() + Offset, &State, 8);
        }
        return Value;
    }();
    return Bytes;
}
DWORD WritePayload(const std::wstring& Path, std::uint64_t Size, std::uint64_t Salt)
{
    const auto& Bytes = Pool();
    const auto File = CreateFileW(Path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (File == INVALID_HANDLE_VALUE) return GetLastError();
    DWORD Error = ERROR_SUCCESS;
    constexpr std::uint64_t Chunk = 8 * MiB;
    for (std::uint64_t Done = 0, Block = 0; Done < Size && !Error; ++Block) {
        const auto Count = std::min(Chunk, Size - Done);
        const auto Offset = (Salt * 2654435761ull + Block * 1048583ull) % (Bytes.size() - Count + 1);
        DWORD Written = 0;
        if (!WriteFile(File, Bytes.data() + Offset, DWORD(Count), &Written, nullptr)) Error = GetLastError();
        else if (Written != Count) Error = ERROR_WRITE_FAULT;
        Done += Count;
    }
    // Large payloads reach the device before any measured run starts.
    if (!Error && Size >= 64 * MiB && !FlushFileBuffers(File)) Error = GetLastError();
    CloseHandle(File);
    return Error;
}
QString Materialize(const TreePlan& Plan, const std::wstring& Root, bool DirectoriesOnly = false)
{
    for (const auto& Directory : Plan.directories_)
        if (!CreateDirectoryW(Join(Root, Directory).c_str(), nullptr)) return Win32Text("create-directory", GetLastError(), Join(Root, Directory));
    if (DirectoriesOnly) return {};
    std::atomic<DWORD> Failure{0};
    Parallel(Plan.files_.size(), 8, [&](std::size_t Index) {
        if (Failure) return;
        const auto& File = Plan.files_[Index];
        if (const auto Error = WritePayload(Join(Join(Root, Plan.directories_[File.directory_]), File.name_), File.size_, Index)) Failure = Error;
    });
    return Failure ? Win32Text("write-payload", Failure) : QString{};
}
// Unmeasured: open every file once for reading. Real-time scanning then has
// seen the tree, as it has seen files that already sat on a disk; otherwise the
// first implementation to touch freshly generated files pays every scan
// (qt-repo merge: 6.8 s on the first run, 2.6 s on the next).
void WarmTree(const TreePlan& Plan, const std::wstring& Root)
{
    Parallel(Plan.files_.size(), 8, [&](std::size_t Index) {
        const auto& File = Plan.files_[Index];
        const auto Handle = CreateFileW(Join(Join(Root, Plan.directories_[File.directory_]), File.name_).c_str(), GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (Handle != INVALID_HANDLE_VALUE) CloseHandle(Handle);
    });
}
struct TreeCount { std::uint64_t files_ = 0, directories_ = 0, bytes_ = 0; DWORD error_ = 0; };
void CountTree(const std::wstring& Path, TreeCount& Total)
{
    WIN32_FIND_DATAW Found{};
    const auto Search = FindFirstFileExW((Path + L"\\*").c_str(), FindExInfoBasic, &Found, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (Search == INVALID_HANDLE_VALUE) { Total.error_ = GetLastError(); return; }
    do {
        if (!wcscmp(Found.cFileName, L".") || !wcscmp(Found.cFileName, L"..")) continue;
        if (Found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { ++Total.directories_; CountTree(Path + L"\\" + Found.cFileName, Total); }
        else { ++Total.files_; Total.bytes_ += (std::uint64_t(Found.nFileSizeHigh) << 32) | Found.nFileSizeLow; }
    } while (!Total.error_ && FindNextFileW(Search, &Found));
    FindClose(Search);
}
QString Verify(const TreePlan& Plan, const std::wstring& Root)
{
    TreeCount Count;
    CountTree(Root, Count);
    if (Count.error_) return Win32Text("verify", Count.error_, Root);
    if (Count.files_ != Plan.files_.size() || Count.directories_ != Plan.directories_.size() - 1 || Count.bytes_ != Plan.bytes_)
        return QStringLiteral("verify: files %1/%2 folders %3/%4 bytes %5/%6").arg(Count.files_).arg(Plan.files_.size())
            .arg(Count.directories_).arg(Plan.directories_.size() - 1).arg(Count.bytes_).arg(Plan.bytes_);
    return {};
}
// Unmeasured cleanup: files per folder in parallel, then folders deepest first.
QString RemoveTree(const std::wstring& Root)
{
    if (!Exists(Root)) return {};
    std::mutex Mutex;
    std::condition_variable Changed;
    std::deque<std::pair<std::wstring, std::size_t>> Queue{{Root, 0}};
    std::vector<std::pair<std::size_t, std::wstring>> Directories{{0, Root}};
    std::size_t Active = 0;
    DWORD Failure = 0; std::wstring FailedPath;
    {
        std::vector<std::jthread> Workers;
        for (int Index = 0; Index < 8; ++Index) Workers.emplace_back([&] {
            for (;;) {
                std::pair<std::wstring, std::size_t> Item;
                {
                    std::unique_lock Lock(Mutex);
                    Changed.wait(Lock, [&] { return !Queue.empty() || !Active || Failure; });
                    if (Queue.empty() || Failure) { Changed.notify_all(); return; }
                    Item = std::move(Queue.front()); Queue.pop_front(); ++Active;
                }
                std::vector<std::pair<std::wstring, std::size_t>> Found;
                DWORD Error = 0; std::wstring ErrorPath;
                WIN32_FIND_DATAW Entry{};
                const auto Search = FindFirstFileExW((Item.first + L"\\*").c_str(), FindExInfoBasic, &Entry, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
                if (Search == INVALID_HANDLE_VALUE) { Error = GetLastError(); ErrorPath = Item.first; }
                else {
                    do {
                        if (!wcscmp(Entry.cFileName, L".") || !wcscmp(Entry.cFileName, L"..")) continue;
                        const auto Child = Item.first + L"\\" + Entry.cFileName;
                        if (Entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) Found.push_back({Child, Item.second + 1});
                        else if (!DeleteFileW(Child.c_str())) {
                            SetFileAttributesW(Child.c_str(), FILE_ATTRIBUTE_NORMAL);
                            if (!DeleteFileW(Child.c_str())) { Error = GetLastError(); ErrorPath = Child; }
                        }
                    } while (!Error && FindNextFileW(Search, &Entry));
                    FindClose(Search);
                }
                std::lock_guard Lock(Mutex);
                for (auto& Child : Found) { Directories.push_back({Child.second, Child.first}); Queue.push_back(std::move(Child)); }
                if (Error && !Failure) { Failure = Error; FailedPath = ErrorPath; }
                --Active; Changed.notify_all();
            }
        });
    }
    if (Failure) return Win32Text("remove-file", Failure, FailedPath);
    std::sort(Directories.begin(), Directories.end(), [](const auto& A, const auto& B) { return A.first > B.first; });
    std::atomic<DWORD> Error{0};
    for (auto Begin = Directories.begin(); Begin != Directories.end();) {
        const auto End = std::find_if(Begin, Directories.end(), [&](const auto& Item) { return Item.first != Begin->first; });
        std::for_each(std::execution::par, Begin, End, [&](const auto& Item) { if (!RemoveDirectoryW(Item.second.c_str())) Error = GetLastError(); });
        if (Error) return Win32Text("remove-directory", Error);
        Begin = End;
    }
    return {};
}
// Explorer-style single-threaded baselines through documented Win32 calls.
struct Child { std::wstring name_; bool directory_ = false; };
DWORD Children(const std::wstring& Path, std::vector<Child>& Items)
{
    WIN32_FIND_DATAW Found{};
    const auto Search = FindFirstFileExW((Path + L"\\*").c_str(), FindExInfoBasic, &Found, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (Search == INVALID_HANDLE_VALUE) return GetLastError();
    do {
        if (wcscmp(Found.cFileName, L".") && wcscmp(Found.cFileName, L".."))
            Items.push_back({Found.cFileName, (Found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0});
    } while (FindNextFileW(Search, &Found));
    const auto Error = GetLastError();
    FindClose(Search);
    return Error == ERROR_NO_MORE_FILES ? ERROR_SUCCESS : Error;
}
// The step and path of the last failed Win32 reference call, for the report.
std::wstring Win32Failure;
DWORD Failed(const wchar_t* Step, const std::wstring& Path, DWORD Code = GetLastError())
{
    Win32Failure = std::wstring(Step) + L" " + Path;
    return Code;
}
DWORD Win32Copy(const std::wstring& From, const std::wstring& To)
{
    if (!CreateDirectoryW(To.c_str(), nullptr)) return Failed(L"mkdir", To);
    std::vector<Child> Items;
    if (const auto Error = Children(From, Items)) return Failed(L"list", From, Error);
    for (const auto& Item : Items) {
        const auto Source = From + L"\\" + Item.name_, Target = To + L"\\" + Item.name_;
        if (Item.directory_) { if (const auto Error = Win32Copy(Source, Target)) return Error; continue; }
        COPYFILE2_EXTENDED_PARAMETERS Parameters{sizeof(Parameters), COPY_FILE_FAIL_IF_EXISTS};
        if (const auto Result = CopyFile2(Source.c_str(), Target.c_str(), &Parameters); FAILED(Result)) return Failed(L"copy", Source, HRESULT_CODE(Result));
    }
    return ERROR_SUCCESS;
}
DWORD Win32Merge(const std::wstring& From, const std::wstring& To)
{
    std::vector<Child> Items;
    if (const auto Error = Children(From, Items)) return Failed(L"list", From, Error);
    for (const auto& Item : Items) {
        const auto Source = From + L"\\" + Item.name_, Target = To + L"\\" + Item.name_;
        if (Item.directory_ && Exists(Target)) { if (const auto Error = Win32Merge(Source, Target)) return Error; }
        else if (!MoveFileExW(Source.c_str(), Target.c_str(), 0)) return Failed(L"move", Source);
    }
    return RemoveDirectoryW(From.c_str()) ? ERROR_SUCCESS : Failed(L"rmdir", From);
}
DWORD Win32CrossMove(const std::wstring& From, const std::wstring& To)
{
    if (!CreateDirectoryW(To.c_str(), nullptr)) return Failed(L"mkdir", To);
    std::vector<Child> Items;
    if (const auto Error = Children(From, Items)) return Failed(L"list", From, Error);
    for (const auto& Item : Items) {
        const auto Source = From + L"\\" + Item.name_, Target = To + L"\\" + Item.name_;
        if (Item.directory_) { if (const auto Error = Win32CrossMove(Source, Target)) return Error; }
        else if (!MoveFileExW(Source.c_str(), Target.c_str(), MOVEFILE_COPY_ALLOWED)) return Failed(L"move", Source);
    }
    return RemoveDirectoryW(From.c_str()) ? ERROR_SUCCESS : Failed(L"rmdir", From);
}
QString Robocopy(const QString& From, const QString& To, bool Move, int Threads)
{
    QProcess Process;
    Process.setProcessChannelMode(QProcess::MergedChannels);
    QStringList Arguments{QDir::toNativeSeparators(From), QDir::toNativeSeparators(To), "/E", "/COPY:DAT", "/DCOPY:DA",
        QStringLiteral("/MT:%1").arg(Threads), "/R:0", "/W:0", "/NFL", "/NDL", "/NJH", "/NJS", "/NP"};
    if (Move) Arguments << "/MOVE";
    Process.start("robocopy.exe", Arguments);
    if (!Process.waitForStarted(30000)) return "robocopy-start";
    Process.waitForFinished(-1);
    // Robocopy exit codes 0-7 are success bit masks; 8 and above are failures.
    if (Process.exitStatus() != QProcess::NormalExit || Process.exitCode() >= 8)
        return QStringLiteral("robocopy exit %1: %2").arg(Process.exitCode()).arg(QString::fromLocal8Bit(Process.readAll()).left(2000));
    return {};
}
// Writes reach the device: the elapsed time hidden by write-back caching.
QString FlushTree(const std::wstring& Path)
{
    std::vector<Child> Items;
    if (const auto Error = Children(Path, Items)) return Win32Text("flush-list", Error, Path);
    for (const auto& Item : Items) {
        const auto Child = Path + L"\\" + Item.name_;
        if (Item.directory_) { if (auto Error = FlushTree(Child); !Error.isEmpty()) return Error; continue; }
        const auto File = CreateFileW(Child.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (File == INVALID_HANDLE_VALUE) return Win32Text("flush-open", GetLastError(), Child);
        const bool Flushed = FlushFileBuffers(File);
        const auto Error = GetLastError();
        CloseHandle(File);
        if (!Flushed) return Win32Text("flush", Error, Child);
    }
    return {};
}
int BuildProcesses()
{
    // Concurrent compilers on the same machine are recorded, not prevented.
    const auto Snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (Snapshot == INVALID_HANDLE_VALUE) return -1;
    PROCESSENTRY32W Entry{}; Entry.dwSize = sizeof(Entry);
    int Count = 0;
    for (BOOL More = Process32FirstW(Snapshot, &Entry); More; More = Process32NextW(Snapshot, &Entry))
        for (const auto* Name : {L"cl.exe", L"link.exe", L"MSBuild.exe", L"ninja.exe"}) if (!_wcsicmp(Entry.szExeFile, Name)) ++Count;
    CloseHandle(Snapshot);
    return Count;
}
QJsonObject VolumeInfo(const QString& Path)
{
    const auto Native = QDir::toNativeSeparators(Path).toStdWString();
    wchar_t Mount[MAX_PATH]{}, Name[64]{}, FileSystem[32]{};
    if (!GetVolumePathNameW(Native.c_str(), Mount, MAX_PATH) || !GetVolumeNameForVolumeMountPointW(Mount, Name, 64)) return {};
    GetVolumeInformationW(Mount, nullptr, 0, nullptr, nullptr, nullptr, FileSystem, 32);
    QJsonObject Value{{"mount", QString::fromWCharArray(Mount)}, {"volume", QString::fromWCharArray(Name)}, {"fileSystem", QString::fromWCharArray(FileSystem)}};
    std::wstring Device(Name);
    if (Device.ends_with(L'\\')) Device.pop_back();
    const auto Handle = CreateFileW(Device.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (Handle != INVALID_HANDLE_VALUE) {
        VOLUME_DISK_EXTENTS Extents{}; DWORD Returned = 0;
        if (DeviceIoControl(Handle, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, nullptr, 0, &Extents, sizeof(Extents), &Returned, nullptr))
            Value["disk"] = int(Extents.Extents[0].DiskNumber);
        CloseHandle(Handle);
    }
    return Value;
}
struct RunOutcome { QString error_; double milliseconds_ = 0; QJsonObject details_; };
RunOutcome RunHyperCommander(hc::operations::FileOperation Operation, const QString& Source, const QString& DestinationParent,
    std::size_t Workers, const QString& Scratch, bool Detailed)
{
    RunOutcome Value;
    // FileJobUi uses three scheduler workers and session-only results when
    // job recovery is off (product default). Its journal lives below the user's
    // local application data, not on the measured volume: result pages written
    // beside same-volume renames slowed them.
    QTemporaryDir JournalDirectory(QDir::temp().filePath("hc-transfer-journal-XXXXXX"));
    if (!JournalDirectory.isValid()) { Value.error_ = "journal-directory"; return Value; }
    auto Journal = MakeFileJobJournal(JournalDirectory.filePath("journal"), false, true, false);
    JobScheduler Scheduler(std::make_shared<hc::commands::EventHub>(), 3, 1024, Journal);
    auto Reader = MakeNativeReadProvider(LocatorKind::Local);
    auto Writer = MakeNativeWritableStorage(LocatorKind::Local, Reader);
    auto Destination = MakeNativeWritableStorage(LocatorKind::Local, MakeNativeReadProvider(LocatorKind::Local));
    const Locator From{LocatorKind::Local, QDir::toNativeSeparators(Source).toStdString()};
    const Locator To{LocatorKind::Local, QDir::toNativeSeparators(DestinationParent).toStdString()};
    OperationMetrics::Instance().Reset(Detailed);
    QElapsedTimer Clock; Clock.start();
    auto Entry = Reader->Resolve(From, {}).get();
    if (!Entry.HasValue()) { Value.error_ = QString::fromStdString("resolve: " + Entry.Failure().code_); return Value; }
    FileOperationRequest Request;
    Request.operation_ = Operation;
    Request.sources_ = {{Entry.Value(), Reader, Writer}};
    Request.destination_ = To; Request.destinationWriter_ = Destination;
    // FileJobUi::Submit with productDefaults.hpp: folder sizes first, lock
    // prompts enabled, delayed file writes (no per-file flush).
    Request.fileLockInteractions_ = true; Request.calculateSizeBefore_ = true;
    OperationContext Context;
    Context.flushFileContents_ = false; Context.fileCopyWorkers_ = Workers;
    // Local folders use the folder engine unless job recovery is on. Set
    // HC_TRANSFER_BENCH_FOLDER_WORKERS to 1 for the per-item path, or to a count.
    Context.fileTransferWorkers_ = qEnvironmentVariableIsSet("HC_TRANSFER_BENCH_FOLDER_WORKERS")
        ? std::size_t(qMax(1, qEnvironmentVariableIntValue("HC_TRANSFER_BENCH_FOLDER_WORKERS"))) : 0;
    auto Job = SubmitFileOperation(Scheduler, std::move(Request), Context);
    if (!Job.HasValue()) { Value.error_ = QString::fromStdString("submit: " + Job.Failure().code_); return Value; }
    const auto Done = Job.Value().completion_.get();
    Value.milliseconds_ = double(Clock.nsecsElapsed()) / 1e6;
    if (Done.snapshot_.state_ != JobState::Completed || Done.snapshot_.failedItems_)
        Value.error_ = QStringLiteral("job state %1 failed %2 error %3").arg(int(Done.snapshot_.state_)).arg(Done.snapshot_.failedItems_)
            .arg(Done.snapshot_.error_ ? QString::fromStdString(Done.snapshot_.error_->code_) : QString{});
    const auto MetricsPath = QDir(Scratch).filePath("metrics.json");
    QJsonObject Metrics;
    if (OperationMetrics::Instance().Save(MetricsPath, Value.milliseconds_)) {
        QFile File(MetricsPath);
        if (File.open(QIODevice::ReadOnly)) Metrics = QJsonDocument::fromJson(File.readAll()).object();
    }
    Value.details_ = QJsonObject{{"completedItems", double(Done.snapshot_.completedItems_)}, {"skippedItems", double(Done.snapshot_.skippedItems_)},
        {"fileCopyWorkers", int(Workers)}, {"preferredCopyWorkers", int(Destination->PreferredCopyWorkers(From, To))},
        {"folderWorkers", int(Context.fileTransferWorkers_)}, {"preferredFolderWorkers", int(Writer->PreferredTreeTransferWorkers(From, To))}, {"metrics", Metrics}};
    return Value;
}
double Median(std::vector<double> Values)
{
    if (Values.empty()) return 0;
    std::sort(Values.begin(), Values.end());
    const auto Middle = Values.size() / 2;
    return Values.size() % 2 ? Values[Middle] : (Values[Middle - 1] + Values[Middle]) / 2;
}
QStringList Selected(const char* Variable, const QStringList& All)
{
    const auto Requested = qEnvironmentVariable(Variable).split(',', Qt::SkipEmptyParts);
    return Requested.isEmpty() ? All : Requested;
}
const QStringList AllOperations{"copy-same", "copy-cross", "move-same", "move-same-merge", "move-cross"};
const QStringList AllImplementations{"hypercommander", "hypercommander-auto", "win32", "robocopy"};
QStringList Applicable(const QString& Operation)
{
    // Robocopy has no rename move. hypercommander-auto differs only where the
    // endpoint policy can select workers: small files between NVMe disks.
    if (Operation == "copy-same") return {"hypercommander", "win32", "robocopy"};
    if (Operation == "copy-cross") return {"hypercommander", "hypercommander-auto", "win32", "robocopy"};
    if (Operation == "move-cross") return {"hypercommander", "win32", "robocopy"};
    return {"hypercommander", "win32"};
}
}

class TransferBenchmark : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        root_ = qEnvironmentVariable("HC_TRANSFER_BENCH_ROOT");
        other_ = qEnvironmentVariable("HC_TRANSFER_BENCH_OTHER_ROOT");
        if (root_.isEmpty()) return;
        for (const auto& Path : {root_, other_}) {
            if (Path.isEmpty()) continue;
            QVERIFY2(QDir::isAbsolutePath(Path) && !QDir(Path).isRoot(), "benchmark roots must be absolute folders below a volume root");
            QVERIFY(QDir().mkpath(Path));
        }
        const auto Root = VolumeInfo(root_), Other = other_.isEmpty() ? QJsonObject{} : VolumeInfo(other_);
        if (!other_.isEmpty()) QVERIFY2(Root["volume"] != Other["volume"], "HC_TRANSFER_BENCH_OTHER_ROOT must be on another volume");
        MEMORYSTATUSEX Memory{}; Memory.dwLength = sizeof(Memory); GlobalMemoryStatusEx(&Memory);
        environment_ = QJsonObject{{"createdUtc", QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},
            {"label", qEnvironmentVariable("HC_TRANSFER_BENCH_LABEL")}, {"logicalProcessors", int(std::thread::hardware_concurrency())},
            {"memoryGiB", double(Memory.ullTotalPhys) / double(1024 * MiB)}, {"root", root_}, {"rootVolume", Root},
            {"other", other_}, {"otherVolume", Other}, {"scale", Scale()}, {"settleMs", Settle()},
            {"detailedMetrics", qEnvironmentVariableIsSet("HC_TRANSFER_BENCH_DETAILED")}};
        configured_ = true;
    }
    void transfer_data()
    {
        QTest::addColumn<QString>("scenario");
        for (const auto& Shape : Scenarios()) QTest::newRow(Shape.name_) << QString::fromLatin1(Shape.name_);
    }
    void transfer()
    {
        QFETCH(QString, scenario);
        if (!configured_) QSKIP("Set HC_TRANSFER_BENCH_ROOT (and HC_TRANSFER_BENCH_OTHER_ROOT) to owned benchmark folders");
        QStringList AllScenarios;
        for (const auto& Shape : Scenarios()) AllScenarios << Shape.name_;
        if (!Selected("HC_TRANSFER_BENCH_SCENARIOS", AllScenarios).contains(scenario)) QSKIP("scenario not selected");
        const auto Shapes = Scenarios();
        const auto& Shape = *std::find_if(Shapes.begin(), Shapes.end(), [&](const auto& Value) { return scenario == Value.name_; });
        auto Operations = Selected("HC_TRANSFER_BENCH_OPERATIONS", AllOperations);
        const auto Implementations = Selected("HC_TRANSFER_BENCH_IMPLEMENTATIONS", AllImplementations);
        for (const auto& Operation : Operations) QVERIFY2(AllOperations.contains(Operation), qPrintable(Operation));
        if (other_.isEmpty()) Operations.removeIf([](const QString& Operation) { return Operation.endsWith("-cross"); });
        const bool Moves = std::any_of(Operations.begin(), Operations.end(), [](const QString& Operation) { return Operation.startsWith("move"); });
        const bool Cross = std::any_of(Operations.begin(), Operations.end(), [](const QString& Operation) { return Operation.endsWith("-cross"); });
        const int Repeats = qEnvironmentVariableIntValue("HC_TRANSFER_BENCH_REPEATS") > 0
            ? qEnvironmentVariableIntValue("HC_TRANSFER_BENCH_REPEATS") : Shape.heavy_ ? 2 : 3;
        QVERIFY(Repeats <= 10);

        const auto Plan = PlanTree(Shape, Scale());
        // Pristine source, consumable move source and one destination at a time.
        const auto Needed = [&](double Copies) { return std::uint64_t(double(Plan.bytes_) * Copies) + 2 * 1024 * MiB; };
        ULARGE_INTEGER Free{};
        QVERIFY(GetDiskFreeSpaceExW(QDir::toNativeSeparators(root_).toStdWString().c_str(), &Free, nullptr, nullptr));
        if (Free.QuadPart < Needed(Moves ? 3 : 2)) QSKIP(qPrintable(QStringLiteral("%1 needs %2 GiB free on the source volume").arg(scenario).arg(Needed(Moves ? 3 : 2) / (1024 * MiB))));
        if (Cross) {
            QVERIFY(GetDiskFreeSpaceExW(QDir::toNativeSeparators(other_).toStdWString().c_str(), &Free, nullptr, nullptr));
            if (Free.QuadPart < Needed(1)) QSKIP(qPrintable(QStringLiteral("%1 needs %2 GiB free on the other volume").arg(scenario).arg(Needed(1) / (1024 * MiB))));
        }
        const auto Owner = QJsonDocument(QJsonObject{{"task", "copy-move-benchmark"}, {"scenario", scenario},
            {"createdUtc", QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},
            {"expiresUtc", QDateTime::currentDateTimeUtc().addDays(7).toString(Qt::ISODate)}}).toJson();
        QTemporaryDir Base(QDir(root_).filePath("hc-transfer-XXXXXX")); QVERIFY(Base.isValid());
        { QFile File(Base.filePath("owner.json")); QVERIFY(File.open(QIODevice::WriteOnly) && File.write(Owner) == Owner.size()); }
        std::optional<QTemporaryDir> Far;
        if (Cross) {
            Far.emplace(QDir(other_).filePath("hc-transfer-XXXXXX")); QVERIFY(Far->isValid());
            QFile File(Far->filePath("owner.json")); QVERIFY(File.open(QIODevice::WriteOnly) && File.write(Owner) == Owner.size());
        }
        const auto Pristine = Base.filePath("pristine/data"), Work = Base.filePath("work/data");
        // Long paths and large trees: remove every fixture with the parallel
        // helper, also after a failed comparison. The report keeps the evidence.
        const auto Cleanup = qScopeGuard([&] {
            for (const auto& Folder : {"pristine", "work", "copy-same", "move-same"}) RemoveTree(Long(Base.filePath(Folder)));
            if (Far) for (const auto& Folder : {"copy-cross", "move-cross"}) RemoveTree(Long(Far->filePath(Folder)));
        });
        for (const auto& Folder : {"pristine", "work", "copy-same", "move-same", "scratch"}) QVERIFY(QDir(Base.path()).mkdir(Folder));
        if (Far) for (const auto& Folder : {"copy-cross", "move-cross"}) QVERIFY(QDir(Far->path()).mkdir(Folder));

        QElapsedTimer Generation; Generation.start();
        const auto Generated = Materialize(Plan, Long(Pristine));
        QVERIFY2(Generated.isEmpty(), qPrintable(Generated));
        const auto GenerationMs = double(Generation.nsecsElapsed()) / 1e6;
        const auto Checked = Verify(Plan, Long(Pristine));
        QVERIFY2(Checked.isEmpty(), qPrintable(Checked));
        scenarios_.append(QJsonObject{{"scenario", scenario}, {"description", Shape.description_}, {"files", double(Plan.files_.size())},
            {"directories", double(Plan.directories_.size() - 1)}, {"bytes", double(Plan.bytes_)}, {"averageFileDepth", Plan.averageFileDepth_},
            {"generationMs", GenerationMs}, {"repeats", Repeats}});
        qInfo().noquote() << "scenario" << scenario << Plan.files_.size() << "files" << Plan.directories_.size() - 1 << "folders"
            << QString::number(double(Plan.bytes_) / double(MiB), 'f', 1) << "MiB generated in" << qRound(GenerationMs) << "ms";
        // Let write-back from generation finish before the first comparison.
        WarmTree(Plan, Long(Pristine));
        std::this_thread::sleep_for(std::chrono::milliseconds(Settle() * 5));

        const auto Clone = [&]() -> QString {
            if (auto Error = Robocopy(Pristine, Work, false, 16); !Error.isEmpty()) return Error;
            WarmTree(Plan, Long(Work));
            return Verify(Plan, Long(Work));
        };
        bool WorkReady = false;
        for (const auto& Operation : Operations) {
            QStringList Candidates;
            for (const auto& Implementation : Applicable(Operation)) if (Implementations.contains(Implementation)) Candidates << Implementation;
            const bool Moving = Operation.startsWith("move");
            if (Moving && !WorkReady) { const auto Error = Clone(); QVERIFY2(Error.isEmpty(), qPrintable(Error)); WorkReady = true; }
            const auto DestinationParent = Operation == "copy-same" ? Base.filePath("copy-same") : Operation == "copy-cross" ? Far->filePath("copy-cross")
                : Operation == "move-cross" ? Far->filePath("move-cross") : Base.filePath("move-same");
            const auto Source = Moving ? Work : Pristine;
            const auto Target = QDir(DestinationParent).filePath("data");
            const bool WritesData = Operation.startsWith("copy") || Operation == "move-cross";
            for (int Repeat = 0; Repeat < Repeats; ++Repeat) for (int Position = 0; Position < Candidates.size(); ++Position) {
                // Rotate the order so no implementation always runs first.
                const auto Implementation = Candidates[(Position + Repeat) % Candidates.size()];
                QVERIFY2(!Exists(Long(Target)) && Exists(Long(Source)), qPrintable(Target));
                if (Operation == "move-same-merge") {
                    // Every destination folder already exists: each file moves alone.
                    const auto Skeleton = Materialize(Plan, Long(Target), true);
                    QVERIFY2(Skeleton.isEmpty(), qPrintable(Skeleton));
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(Settle()));
                const auto Building = BuildProcesses();
                RunOutcome Run;
                if (Implementation.startsWith("hypercommander")) {
                    Run = RunHyperCommander(Moving ? hc::operations::FileOperation::Move : hc::operations::FileOperation::Copy, Source, DestinationParent,
                        Implementation == "hypercommander-auto" ? 0 : 1, Base.filePath("scratch"), qEnvironmentVariableIsSet("HC_TRANSFER_BENCH_DETAILED"));
                } else {
                    QElapsedTimer Clock; Clock.start();
                    if (Implementation == "robocopy") {
                        Run.error_ = Robocopy(Source, Target, Moving, 8);
                        // Remove the emptied selected folder as a user-visible move would.
                        if (Run.error_.isEmpty() && Moving && Exists(Long(Source)) && !RemoveDirectoryW(Long(Source).c_str())) Run.error_ = Win32Text("robocopy-root", GetLastError());
                    } else {
                        DWORD Error = ERROR_SUCCESS;
                        Win32Failure.clear();
                        if (Operation.startsWith("copy")) Error = Win32Copy(Long(Source), Long(Target));
                        else if (Operation == "move-same") Error = MoveFileExW(Long(Source).c_str(), Long(Target).c_str(), 0) ? ERROR_SUCCESS : GetLastError();
                        else if (Operation == "move-same-merge") Error = Win32Merge(Long(Source), Long(Target));
                        else Error = Win32CrossMove(Long(Source), Long(Target));
                        if (Error) Run.error_ = Win32Text("win32", Error, Win32Failure);
                    }
                    Run.milliseconds_ = double(Clock.nsecsElapsed()) / 1e6;
                }
                QJsonObject Result{{"scenario", scenario}, {"operation", Operation}, {"implementation", Implementation}, {"repeat", Repeat + 1},
                    {"position", Position + 1}, {"files", double(Plan.files_.size())}, {"directories", double(Plan.directories_.size() - 1)},
                    {"bytes", double(Plan.bytes_)}, {"milliseconds", Run.milliseconds_}, {"buildProcesses", Building}, {"details", Run.details_}};
                if (!Run.error_.isEmpty()) { Result["error"] = Run.error_; results_.append(Result); writeReport(); QFAIL(qPrintable(Run.error_)); }
                if (WritesData && !Shape.sizes_.empty()) {
                    QElapsedTimer Clock; Clock.start();
                    const auto Flushed = FlushTree(Long(Target));
                    QVERIFY2(Flushed.isEmpty(), qPrintable(Flushed));
                    Result["flushMilliseconds"] = double(Clock.nsecsElapsed()) / 1e6;
                }
                auto Error = Verify(Plan, Long(Target));
                if (Error.isEmpty() && Moving && Exists(Long(Source))) Error = "move left its source folder";
                if (!Error.isEmpty()) { Result["error"] = Error; results_.append(Result); writeReport(); QFAIL(qPrintable(Error)); }
                const auto Seconds = Run.milliseconds_ / 1000;
                Result["filesPerSecond"] = double(Plan.files_.size()) / Seconds;
                Result["mibPerSecond"] = double(Plan.bytes_) / double(MiB) / Seconds;
                results_.append(Result);
                qInfo().noquote() << scenario << Operation << Implementation << "repeat" << Repeat + 1 << qRound(Run.milliseconds_) << "ms"
                    << qRound(double(Plan.files_.size()) / Seconds) << "files/s" << QString::number(double(Plan.bytes_) / double(MiB) / Seconds, 'f', 1) << "MiB/s"
                    << (Result.contains("flushMilliseconds") ? QStringLiteral("flush %1 ms").arg(qRound(Result["flushMilliseconds"].toDouble())) : QString{})
                    << (Building ? QStringLiteral("(build processes %1)").arg(Building) : QString{});
                // Restore the source state outside the measured interval.
                if (Operation.startsWith("copy")) Error = RemoveTree(Long(Target));
                else if (Operation == "move-cross") { Error = RemoveTree(Long(Target)); if (Error.isEmpty()) Error = Clone(); }
                else if (!MoveFileExW(Long(Target).c_str(), Long(Source).c_str(), 0)) Error = Win32Text("restore", GetLastError());
                QVERIFY2(Error.isEmpty(), qPrintable(Error));
            }
        }
        writeReport();
    }
    void cleanupTestCase()
    {
        if (!configured_ || results_.isEmpty()) return;
        // Median per scenario, operation and implementation.
        std::map<QString, std::vector<double>> Times, Flushes;
        std::map<QString, QJsonObject> Rows;
        for (const auto& Item : results_) {
            const auto Value = Item.toObject();
            if (Value.contains("error")) continue;
            const auto Key = Value["scenario"].toString() + '|' + Value["operation"].toString() + '|' + Value["implementation"].toString();
            Times[Key].push_back(Value["milliseconds"].toDouble());
            if (Value.contains("flushMilliseconds")) Flushes[Key].push_back(Value["milliseconds"].toDouble() + Value["flushMilliseconds"].toDouble());
            Rows[Key] = Value;
        }
        QJsonArray Summary;
        for (const auto& [Key, Values] : Times) {
            const auto& Row = Rows[Key];
            const auto Ms = Median(Values), Seconds = Ms / 1000;
            QJsonObject Line{{"scenario", Row["scenario"]}, {"operation", Row["operation"]}, {"implementation", Row["implementation"]},
                {"runs", int(Values.size())}, {"medianMs", Ms}, {"minMs", *std::min_element(Values.begin(), Values.end())},
                {"maxMs", *std::max_element(Values.begin(), Values.end())}, {"filesPerSecond", Row["files"].toDouble() / Seconds},
                {"mibPerSecond", Row["bytes"].toDouble() / double(MiB) / Seconds}};
            if (Flushes.contains(Key)) Line["medianDurableMs"] = Median(Flushes[Key]);
            Summary.append(Line);
            qInfo().noquote() << "median" << Key << qRound(Ms) << "ms" << qRound(Row["files"].toDouble() / Seconds) << "files/s"
                << QString::number(Row["bytes"].toDouble() / double(MiB) / Seconds, 'f', 1) << "MiB/s"
                << (Flushes.contains(Key) ? QStringLiteral("durable %1 ms").arg(qRound(Median(Flushes[Key]))) : QString{});
        }
        summary_ = Summary;
        writeReport();
    }
private:
    static double Scale()
    {
        const auto Text = qEnvironmentVariable("HC_TRANSFER_BENCH_SCALE");
        bool Valid = false; const auto Value = Text.toDouble(&Valid);
        return Valid && Value > 0 && Value <= 4 ? Value : 1.0;
    }
    static int Settle()
    {
        return qEnvironmentVariableIsSet("HC_TRANSFER_BENCH_SETTLE_MS") ? std::clamp(qEnvironmentVariableIntValue("HC_TRANSFER_BENCH_SETTLE_MS"), 0, 60000) : 1000;
    }
    void writeReport() const
    {
        const auto Path = qEnvironmentVariable("HC_TRANSFER_BENCH_REPORT");
        if (Path.isEmpty()) return;
        QFile File(Path);
        if (File.open(QIODevice::WriteOnly))
            File.write(QJsonDocument(QJsonObject{{"environment", environment_}, {"scenarios", scenarios_}, {"results", results_}, {"summary", summary_}}).toJson());
    }
    QString root_, other_;
    QJsonObject environment_;
    QJsonArray scenarios_, results_, summary_;
    bool configured_ = false;
};
int main(int Argc, char** Argv)
{
    QCoreApplication App(Argc, Argv);
    // A full comparison takes hours; QTest's default watchdog would abort it.
    if (qEnvironmentVariableIsSet("HC_TRANSFER_BENCH_ROOT") && !qEnvironmentVariableIsSet("QTEST_FUNCTION_TIMEOUT"))
        qputenv("QTEST_FUNCTION_TIMEOUT", "172800000");
    TransferBenchmark Tests;
    return QTest::qExec(&Tests, Argc, Argv);
}
#include "tstFileTransferBenchmark.moc"
