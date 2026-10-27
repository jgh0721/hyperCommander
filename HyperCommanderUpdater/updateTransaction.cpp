#include "stdafx.h"
#include "updateTransaction.hpp"

#include <TlHelp32.h>
#include <algorithm>

namespace hc::update {
namespace {
void Require(bool Value, const char* Error) { if (!Value) throw std::runtime_error(Error); }
struct Handle {
    HANDLE value_ = INVALID_HANDLE_VALUE;
    Handle() = default;
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& Other) noexcept : value_(Other.value_) { Other.value_ = INVALID_HANDLE_VALUE; }
    ~Handle() { if (value_ != INVALID_HANDLE_VALUE && value_ != nullptr) CloseHandle(value_); }
};
bool Same(std::wstring_view A, std::wstring_view B) {
    return CompareStringOrdinal(A.data(), int(A.size()), B.data(), int(B.size()), TRUE) == CSTR_EQUAL;
}
bool Under(const Path& File, const Path& Root) {
    const auto A = File.lexically_normal().wstring(), B = Root.lexically_normal().wstring() + L"\\";
    return A.size() > B.size() && Same(std::wstring_view(A).substr(0, B.size()), B);
}
Path Canonical(const Path& Value) {
    Require(Value.is_absolute(), "update-absolute-path"); CheckPlainPath(Value);
    return std::filesystem::weakly_canonical(Value);
}
void CheckTransaction(const Path& Root, const Path& Tx) {
    Require(Tx.parent_path() == Root / L".update", "update-transaction-location");
    const auto Name = Tx.filename().wstring(); Require(Name.size() == 36, "update-transaction-name");
    for (std::size_t Index = 0; Index < Name.size(); ++Index)
        Require((Index == 8 || Index == 13 || Index == 18 || Index == 23) ? Name[Index] == '-' :
            (Name[Index] >= '0' && Name[Index] <= '9') || (Name[Index] >= 'a' && Name[Index] <= 'f'), "update-transaction-name");
    CheckPlainPath(Tx);
}
Path Active(const Path& Root) {
    const auto Name = ParseJson(ReadBounded(Root / L".update/active.json", 4096)).At("transaction", Json::Kind::String).text_;
    const auto Tx = Root / L".update" / Wide(Name); CheckTransaction(Root, Tx); return Tx;
}
Handle Lock(const Path& Root) {
    CheckInstallRoot(Root); std::filesystem::create_directories(Root / L".update"); CheckPlainPath(Root / L".update/lock");
    Handle Result; Result.value_ = CreateFileW((Root / L".update/lock").c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_HIDDEN, nullptr);
    Require(Result.value_ != INVALID_HANDLE_VALUE, "update-root-locked"); return Result;
}
struct Entry { std::string name_, original_, replacement_; std::uint64_t size_ = 0; bool existed_ = false; };
struct Journal { std::string state_; std::vector<Entry> files_; };
void Save(const Path& Tx, const Journal& Value) {
    std::string Bytes = "{\"schema\":1,\"state\":" + QuoteJson(Value.state_) + ",\"files\":[";
    for (const auto& Entry : Value.files_) {
        if (Bytes.back() != '[') Bytes += ',';
        Bytes += "{\"path\":" + QuoteJson(Entry.name_) + ",\"existed\":" + (Entry.existed_ ? "true" : "false")
            + ",\"original\":" + QuoteJson(Entry.original_) + ",\"replacement\":" + QuoteJson(Entry.replacement_) + ",\"size\":" + std::to_string(Entry.size_) + '}';
    }
    Bytes += "]}"; ParseJson(Bytes); DurableWrite(Tx / L"journal.json", Bytes);
}
bool ValidHash(std::string_view Text) {
    if (Text.size() != 64) return false;
    return std::all_of(Text.begin(), Text.end(), [](char Ch) { return (Ch >= '0' && Ch <= '9') || (Ch >= 'a' && Ch <= 'f'); });
}
Journal Load(const Path& Tx) {
    const auto Value = ParseJson(ReadBounded(Tx / L"journal.json", 1048576)); Journal Result;
    Require(Value.At("schema", Json::Kind::Number).number_ == 1, "update-journal-schema");
    Result.state_ = Value.At("state", Json::Kind::String).text_;
    std::set<std::wstring> Seen;
    for (const auto& Item : Value.At("files", Json::Kind::Array).array_) {
        Require(Result.files_.size() < 4096, "update-journal-count");
        Entry Entry{Item.At("path", Json::Kind::String).text_, Item.At("original", Json::Kind::String).text_,
            Item.At("replacement", Json::Kind::String).text_, Item.At("size", Json::Kind::Number).number_, Item.At("existed", Json::Kind::Boolean).boolean_};
        CheckProductRelative(Entry.name_); auto Name = Wide(Entry.name_); CharLowerBuffW(Name.data(), DWORD(Name.size()));
        Require(Seen.insert(Name).second && Entry.size_ <= 536870912 && (Entry.existed_ ? ValidHash(Entry.original_) : Entry.original_.empty())
            && (Entry.replacement_.empty() || ValidHash(Entry.replacement_)), "update-journal-entry");
        Result.files_.push_back(std::move(Entry));
    }
    Require(!Result.files_.empty() && Result.files_.back().name_ == "product-inventory.json", "update-journal-inventory"); return Result;
}
void Move(const Path& From, const Path& To, bool Replace = false) {
    CheckPlainPath(From); CheckPlainPath(To); std::filesystem::create_directories(To.parent_path()); CheckPlainPath(To);
    if (!MoveFileExW(From.c_str(), To.c_str(), MOVEFILE_WRITE_THROUGH | (Replace ? MOVEFILE_REPLACE_EXISTING : 0))) {
        const auto Error = GetLastError();
        throw std::runtime_error("update-move-failed: " + Utf8(From.wstring()) + " -> " + Utf8(To.wstring())
            + " (win32=" + std::to_string(Error) + ")");
    }
}
void Rollback(const Path& Root, const Path& Tx, Journal& Journal) {
    Journal.state_ = "rollingBack"; Save(Tx, Journal);
    for (std::size_t Count = Journal.files_.size(); Count; --Count) {
        const auto Index = Count - 1; const auto& Entry = Journal.files_[Index];
        const auto Destination = Root / Wide(Entry.name_), Backup = Tx / L"backup" / std::to_wstring(Index);
        CheckPlainPath(Destination); CheckPlainPath(Backup);
        if (std::filesystem::exists(Backup)) {
            Require(Entry.existed_ && HashFile(Backup) == Entry.original_, "update-backup-modified");
            if (std::filesystem::exists(Destination)) {
                const auto Hash = HashFile(Destination); Require(Hash == Entry.replacement_ || Hash == Entry.original_, "update-rollback-user-file-conflict");
            }
            Move(Backup, Destination, true);
        } else if (Entry.existed_) {
            Require(std::filesystem::exists(Destination) && HashFile(Destination) == Entry.original_, "update-backup-missing");
        } else if (std::filesystem::exists(Destination)) {
            Require(!Entry.replacement_.empty() && HashFile(Destination) == Entry.replacement_, "update-rollback-user-file-conflict");
            Require(DeleteFileW(Destination.c_str()), "update-rollback-delete");
        }
    }
    Journal.state_ = "rolledBack"; Save(Tx, Journal);
    Require(DeleteFileW((Root / L".update/active.json").c_str()), "update-active-remove");
}
void WaitParent(const Path& Root, unsigned Pid, std::uint64_t Created, std::wstring_view Handoff, bool* HandoffReady) {
    Require(Pid != 0 && Created != 0 && Pid != GetCurrentProcessId(), "update-parent-invalid");
    Handle Parent; Parent.value_ = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, Pid);
    if (!Parent.value_) { Require(Handoff.empty() && GetLastError() == ERROR_INVALID_PARAMETER, "update-parent-open"); return; }
    // An exited process object can remain queryable while another handle is
    // retained, but its image path may already be unavailable.
    if (WaitForSingleObject(Parent.value_, 0) == WAIT_OBJECT_0) { Require(Handoff.empty(), "update-parent-exited-before-handoff"); return; }
    FILETIME Creation{}, Exit{}, Kernel{}, User{}; wchar_t Name[32768]{}; DWORD Length = DWORD(std::size(Name));
    Require(GetProcessTimes(Parent.value_, &Creation, &Exit, &Kernel, &User), "update-parent-query");
    // Verification can outlive the parent. Windows may already have reused
    // its PID; a different creation time proves that the original exited.
    // ApplyTransaction still checks every process under Root before writing.
    if (((std::uint64_t(Creation.dwHighDateTime) << 32) | Creation.dwLowDateTime) != Created) { Require(Handoff.empty(), "update-parent-changed-before-handoff"); return; }
    const bool Queried = QueryFullProcessImageNameW(Parent.value_, 0, Name, &Length);
    if (WaitForSingleObject(Parent.value_, 0) == WAIT_OBJECT_0) { Require(Handoff.empty(), "update-parent-exited-before-handoff"); return; }
    Require(Queried && Same(Name, (Root / L"HyperCommander.exe").wstring()), "update-parent-identity");
    if (!Handoff.empty()) {
        constexpr std::wstring_view Prefix = L"Local\\HyperCommander.Update.";
        Require(Handoff.starts_with(Prefix) && Handoff.size() == Prefix.size() + 36, "update-handoff-name");
        for (std::size_t Index = Prefix.size(); Index < Handoff.size(); ++Index) {
            const auto Ch = Handoff[Index];
            Require(Ch == '-' || Ch >= '0' && Ch <= '9' || Ch >= 'a' && Ch <= 'f', "update-handoff-name");
        }
        Handle Ready, Cancel;
        Ready.value_ = OpenEventW(EVENT_MODIFY_STATE, FALSE, (std::wstring(Handoff) + L".ready").c_str());
        Cancel.value_ = OpenEventW(SYNCHRONIZE, FALSE, (std::wstring(Handoff) + L".cancel").c_str());
        Require(Ready.value_ && Cancel.value_, "update-handoff-open");
        Require(WaitForSingleObject(Cancel.value_, 0) == WAIT_TIMEOUT, "update-handoff-cancelled");
        // All signed update inputs and the live parent have been checked. The
        // GUI may now close; cancellation must still prevent a delayed apply.
        Require(SetEvent(Ready.value_), "update-handoff-ready");
        if (HandoffReady) *HandoffReady = true;
        const HANDLE Waits[]{Cancel.value_, Parent.value_};
        const auto Result = WaitForMultipleObjects(DWORD(std::size(Waits)), Waits, FALSE, 30000);
        Require(Result != WAIT_OBJECT_0, "update-handoff-cancelled");
        Require(Result == WAIT_OBJECT_0 + 1, "update-parent-still-running");
        return;
    }
    Require(WaitForSingleObject(Parent.value_, 30000) == WAIT_OBJECT_0, "update-parent-still-running");
}
struct ProcessWindows { DWORD pid_; bool visible_ = false; };
BOOL CALLBACK FindVisibleProcessWindow(HWND Window, LPARAM Data) {
    auto& Query = *reinterpret_cast<ProcessWindows*>(Data);
    DWORD Pid = 0; GetWindowThreadProcessId(Window, &Pid);
    if (Pid == Query.pid_ && IsWindowVisible(Window)) Query.visible_ = true;
    return !Query.visible_;
}
BOOL CALLBACK FindProcessWindows(HWND Window, LPARAM Data) {
    auto& Query = *reinterpret_cast<ProcessWindows*>(Data);
    if (IsWindowVisible(Window)) {
        FindVisibleProcessWindow(Window, Data);
        if (!Query.visible_) EnumChildWindows(Window, FindVisibleProcessWindow, Data);
    }
    return !Query.visible_;
}
bool HasVisibleWindows(DWORD Pid) {
    ProcessWindows Query{Pid};
    Require(EnumWindows(FindProcessWindows, reinterpret_cast<LPARAM>(&Query)) || Query.visible_, "update-window-list");
    return Query.visible_;
}
void WaitProductExit(const Path& Root) {
    // The parent's job handles close during shutdown. Give its plugin hosts
    // time to exit, then retain the strict check before any file replacement.
    const auto Until = GetTickCount64() + 5000;
    while (OtherProductProcesses(Root)) {
        Require(GetTickCount64() < Until, "update-close-product-processes");
        Sleep(50);
    }
}
void CleanAcknowledged(const Path& Root, const Path& Tx, bool Retire) {
    CheckTransaction(Root, Tx); const auto Journal = Load(Tx); Require(Journal.state_ == "completed", "update-clean-state");
    const auto Release = ParseManifest(AuthenticateManifest(Tx / L"update-manifest.cat", Tx / L"update-manifest.json"));
    if (std::filesystem::exists(Tx / L"staging")) VerifyTree(Tx / L"staging", Release);
    if (std::filesystem::exists(Tx / L"portable.zip")) Require(HashFile(Tx / L"portable.zip", 1073741824) == Release.zipHash_, "update-clean-zip");
    std::set<Path> Allowed{Tx / L"journal.json", Tx / L"update-manifest.json", Tx / L"update-manifest.cat", Tx / L"restart.json", Tx / L"hc-updater.exe", Tx / L"HyperCommanderUpdater.exe", Tx / L"portable.zip"};
    if (std::filesystem::exists(Tx / L"hc-updater.exe")) VerifyProductSignature(Tx / L"hc-updater.exe");
    if (std::filesystem::exists(Tx / L"HyperCommanderUpdater.exe")) VerifyProductSignature(Tx / L"HyperCommanderUpdater.exe");
    for (std::size_t Index = 0; Index < Journal.files_.size(); ++Index) {
        const auto& Entry = Journal.files_[Index];
        const auto Backup = Tx / L"backup" / std::to_wstring(Index); Allowed.insert(Backup);
        if (std::filesystem::exists(Backup)) Require(Entry.existed_ && HashFile(Backup) == Entry.original_, "update-clean-backup");
        if (!Entry.replacement_.empty()) Allowed.insert(Tx / L"staging" / Wide(Entry.name_));
    }
    std::vector<Path> Files, Directories;
    for (const auto& Entry : std::filesystem::recursive_directory_iterator(Tx)) {
        CheckPlainPath(Entry.path()); Require(Under(Entry.path(), Tx), "update-clean-boundary");
        if (Entry.is_directory()) { if (Retire || Under(Entry.path(), Tx / L"staging") || Entry.path() == Tx / L"staging") Directories.push_back(Entry.path()); }
        else {
            Require(Allowed.contains(Entry.path()), "update-clean-unrecognized-file");
            if (Retire || Under(Entry.path(), Tx / L"staging") || Entry.path() == Tx / L"portable.zip") Files.push_back(Entry.path());
        }
    }
    for (const auto& File : Files) { CheckPlainPath(File); Require(DeleteFileW(File.c_str()), "update-clean-delete"); }
    std::sort(Directories.begin(), Directories.end(), [](const auto& A, const auto& B) { return A.native().size() > B.native().size(); });
    for (const auto& Directory : Directories) { CheckPlainPath(Directory); std::filesystem::remove(Directory); }
    if (Retire) std::filesystem::remove(Tx);
}
}
void CheckProductRelative(std::string_view Relative) {
    CheckRelative(Relative);
    const bool Root = Relative.find('/') == std::string_view::npos;
    const std::set<std::string_view> Names{"HyperCommander.exe", "HyperCommanderAdmin.exe", "HyperEditor.exe", "HyperCompare.exe", "HyperCompareCli.exe", "HyperEditorWeb.exe", "QtWebEngineProcess.exe",
        "hc-plugin-host-x64.exe", "hc-plugin-inspect.exe", "hc-search-index.exe", "HyperCommanderUpdater.exe", "hc-updater.exe", "Everything64.dll", "pdfium.dll", "runtime-inventory.json", "product-inventory.json"};
    const bool Owned = (Root && (Names.contains(Relative) || ((Relative.starts_with("Qt6") || Relative.starts_with("vcruntime") || Relative.starts_with("msvcp")
        || Relative.starts_with("concrt") || Relative.starts_with("d3dcompiler_") || Relative == "opengl32sw.dll") && Relative.ends_with(".dll"))))
        || Relative.starts_with("Libs/") || Relative.starts_with("Licenses/") || Relative.starts_with("Addons/WCX/IsoImage/")
        || Relative.starts_with("Addons/WLX/HyperViewer/") || Relative.starts_with("Third-Party/SevenZip/") || Relative.starts_with("Third-Party/AlzEgg/");
    if (!Owned) throw std::runtime_error("update-unowned-path: " + std::string(Relative));
}
void CheckInstallRoot(const Path& Root) {
    Require(Root.is_absolute() && Root == Canonical(Root) && std::filesystem::is_directory(Root), "update-install-root");
    wchar_t Volume[32768]{}, Type[32]{};
    Require(GetVolumePathNameW(Root.c_str(), Volume, DWORD(std::size(Volume))) && GetDriveTypeW(Volume) != DRIVE_REMOTE
        && GetVolumeInformationW(Volume, nullptr, 0, nullptr, nullptr, nullptr, Type, DWORD(std::size(Type))) && (Same(Type, L"NTFS") || Same(Type, L"ReFS")), "update-requires-local-ntfs-or-refs");
    Handle Token; TOKEN_ELEVATION Elevation{}; DWORD Size = 0;
    Require(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &Token.value_) && GetTokenInformation(Token.value_, TokenElevation, &Elevation, sizeof(Elevation), &Size)
        && !Elevation.TokenIsElevated, "update-elevated-process");
}
Path ExecutableRoot() {
    wchar_t Image[32768]{}; const auto Length = GetModuleFileNameW(nullptr, Image, DWORD(std::size(Image)));
    Require(Length && Length < std::size(Image), "update-executable-root"); return Path(Image).parent_path();
}
void DurableWrite(const Path& File, std::string_view Bytes) {
    CheckPlainPath(File); const auto Temp = Path(File.wstring() + L".new"); CheckPlainPath(Temp);
    Handle Output; Output.value_ = CreateFileW(Temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    Require(Output.value_ != INVALID_HANDLE_VALUE, "update-journal-open"); DWORD Written = 0;
    Require(Bytes.size() <= 1048576 && WriteFile(Output.value_, Bytes.data(), DWORD(Bytes.size()), &Written, nullptr) && Written == Bytes.size()
        && FlushFileBuffers(Output.value_), "update-journal-flush");
    CloseHandle(Output.value_); Output.value_ = INVALID_HANDLE_VALUE; Move(Temp, File, true);
}
bool StartupAllowed(const Path& Root) {
    try {
        CheckPlainPath(Root / L".update/active.json"); if (!std::filesystem::exists(Root / L".update/active.json")) return true;
        const auto State = Load(Active(Root)).state_; return State == "installed" || State == "completed" || State == "rolledBack";
    } catch (...) { return false; }
}
bool OtherProductProcesses(const Path& Root, unsigned IgnorePid, ProductProcessCheck Check) {
    Handle Snapshot; Snapshot.value_ = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0); Require(Snapshot.value_ != INVALID_HANDLE_VALUE, "update-process-list");
    PROCESSENTRY32W Entry{sizeof(Entry)};
    if (!Process32FirstW(Snapshot.value_, &Entry)) return false;
    do {
        if (Entry.th32ProcessID == GetCurrentProcessId() || Entry.th32ProcessID == IgnorePid) continue;
        Handle Process; Process.value_ = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, Entry.th32ProcessID);
        wchar_t Name[32768]{}; DWORD Size = DWORD(std::size(Name));
        if (Process.value_ && QueryFullProcessImageNameW(Process.value_, 0, Name, &Size)) {
            if (!Under(Name, Root)) continue;
            // Only our own windowless plugin host belongs to normal app
            // shutdown. Other instances and visible/embedded viewers still block.
            if (Check == ProductProcessCheck::BeforeRestart && Entry.th32ParentProcessID == GetCurrentProcessId()
                && Same(Path(Name).lexically_normal().wstring(), (Root / L"hc-plugin-host-x64.exe").lexically_normal().wstring())
                && !HasVisibleWindows(Entry.th32ProcessID)) continue;
            return true;
        }
        else if (Same(Entry.szExeFile, L"HyperCommander.exe") || Same(Entry.szExeFile, L"HyperCommanderAdmin.exe") || Same(Entry.szExeFile, L"HyperEditor.exe")) return true;
    } while (Process32NextW(Snapshot.value_, &Entry)); return false;
}
void ApplyTransaction(const Path& InputRoot, const Path& InputTx, unsigned ParentPid, std::uint64_t ParentCreated, std::wstring_view Handoff, bool* HandoffReady) {
    if (HandoffReady) *HandoffReady = false;
    const auto Root = Canonical(InputRoot), Tx = Canonical(InputTx); CheckTransaction(Root, Tx); auto RootLock = Lock(Root);
    Require(!std::filesystem::exists(Root / L".update/active.json"), "update-recovery-required");
    const auto Manifest = ParseManifest(AuthenticateManifest(Tx / L"update-manifest.cat", Tx / L"update-manifest.json"));
    const auto Next = VerifyTree(Tx / L"staging", Manifest); const auto OriginalBytes = ReadBounded(Root / L"product-inventory.json", 1048576);
    const auto Old = ParseInventory(OriginalBytes); VerifyImageVersion(Root / L"HyperCommander.exe", Old.version_);
    Require(ParseVersion(Old.version_) >= ParseVersion(Manifest.minimum_) && ParseVersion(Old.version_) < ParseVersion(Manifest.version_), "update-version-order");
    std::map<std::string, ProductFile> Originals, Replacements;
    for (const auto& File : Old.files_) { CheckProductRelative(File.path_); Originals.emplace(File.path_, File); }
    for (const auto& File : Next.files_) { CheckProductRelative(File.path_); Replacements.emplace(File.path_, File); }
    Originals.emplace("product-inventory.json", ProductFile{"product-inventory.json", HashBytes(OriginalBytes), OriginalBytes.size()});
    const auto NextBytes = ReadBounded(Tx / L"staging/product-inventory.json", 1048576);
    Replacements.emplace("product-inventory.json", ProductFile{"product-inventory.json", Manifest.inventoryHash_, NextBytes.size()});
    WaitParent(Root, ParentPid, ParentCreated, Handoff, HandoffReady); WaitProductExit(Root);
    std::map<std::string, int> Names;
    for (const auto& [Name, File] : Originals) Names.emplace(Name, 0);
    for (const auto& [Name, File] : Replacements) Names.emplace(Name, 0);
    Names.erase("product-inventory.json"); std::vector<std::string> Ordered;
    for (const auto& [Name, Index] : Names) Ordered.push_back(Name); Ordered.push_back("product-inventory.json");
    Require(Ordered.size() <= 4096, "update-journal-count"); Journal Journal{"prepared", {}};
    for (const auto& Name : Ordered) {
        const auto Destination = Root / Wide(Name); CheckPlainPath(Destination); const bool Exists = std::filesystem::exists(Destination);
        const auto Original = Originals.find(Name), Replacement = Replacements.find(Name);
        Require(!Exists || Original != Originals.end(), "update-user-file-conflict");
        if (Exists) VerifyProductFile(Destination, Name, Original->second.hash_, Original->second.size_);
        Journal.files_.push_back({Name, Exists ? Original->second.hash_ : "", Replacement == Replacements.end() ? "" : Replacement->second.hash_,
            Exists ? Original->second.size_ : 0, Exists});
    }
    std::filesystem::create_directories(Tx / L"backup"); Require(std::filesystem::is_empty(Tx / L"backup"), "update-backup-not-empty");
    Save(Tx, Journal); DurableWrite(Root / L".update/active.json", "{\"transaction\":" + QuoteJson(Utf8(Tx.filename().wstring())) + "}");
    try {
        Require(!OtherProductProcesses(Root), "update-close-product-processes"); Journal.state_ = "applying"; Save(Tx, Journal);
        for (std::size_t Index = 0; Index < Journal.files_.size(); ++Index) {
            const auto& Entry = Journal.files_[Index]; const auto Destination = Root / Wide(Entry.name_);
            if (Entry.existed_) { Require(HashFile(Destination) == Entry.original_, "update-product-changed"); Move(Destination, Tx / L"backup" / std::to_wstring(Index)); }
            if (!Entry.replacement_.empty()) {
                const auto& File = Replacements.at(Entry.name_); const auto Temp = Tx / L"replacement.tmp"; CheckPlainPath(Temp);
                Require(CopyFileW((Tx / L"staging" / Wide(Entry.name_)).c_str(), Temp.c_str(), FALSE), "update-copy-failed");
                VerifyProductFile(Temp, Entry.name_, File.hash_, File.size_);
                Handle Flush; Flush.value_ = CreateFileW(Temp.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                Require(Flush.value_ != INVALID_HANDLE_VALUE && FlushFileBuffers(Flush.value_), "update-product-flush");
                CloseHandle(Flush.value_); Flush.value_ = INVALID_HANDLE_VALUE; Move(Temp, Destination);
            }
        }
        VerifyTree(Root, Manifest, false); Journal.state_ = "installed"; Save(Tx, Journal);
    } catch (...) { const auto Error = std::current_exception(); Rollback(Root, Tx, Journal); std::rethrow_exception(Error); }
}
void RecoverTransaction(const Path& InputRoot) {
    const auto Root = Canonical(InputRoot); auto RootLock = Lock(Root); const auto Tx = Active(Root);
    Require(!OtherProductProcesses(Root), "update-close-product-processes"); auto Journal = Load(Tx);
    Require(Journal.state_ != "completed", "update-already-acknowledged"); Rollback(Root, Tx, Journal);
}
void AcknowledgeTransaction(const Path& InputRoot) {
    const auto Root = Canonical(InputRoot); auto RootLock = Lock(Root);
    if (!std::filesystem::exists(Root / L".update/active.json")) return;
    const auto Tx = Active(Root); auto Journal = Load(Tx); Require(Journal.state_ == "installed" || Journal.state_ == "completed", "update-not-installed");
    const auto Manifest = ParseManifest(AuthenticateManifest(Tx / L"update-manifest.cat", Tx / L"update-manifest.json")); VerifyTree(Root, Manifest, false);
    Journal.state_ = "completed"; Save(Tx, Journal); Require(DeleteFileW((Root / L".update/active.json").c_str()), "update-active-remove");
    // A healthy version retains one original-file backup. Unknown or modified
    // transaction contents are preserved instead of recursively deleted.
    try {
        const auto Ready = Root / L".update/ready.json";
        if (std::filesystem::exists(Ready) && ParseJson(ReadBounded(Ready, 4096)).At("transaction", Json::Kind::String).text_ == Utf8(Tx.filename().wstring())) DeleteFileW(Ready.c_str());
        CleanAcknowledged(Root, Tx, false);
        for (const auto& Entry : std::filesystem::directory_iterator(Root / L".update")) {
            if (!Entry.is_directory() || Entry.path() == Tx) continue;
            try { CleanAcknowledged(Root, Entry.path(), true); } catch (...) {}
        }
    } catch (...) {}
}
}
