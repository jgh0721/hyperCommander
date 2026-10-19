#include "stdafx.h"
#include "nativeFileLocks.hpp"

/// Platform headers
#include <TlHelp32.h>
#include <restartmanager.h>
#include <sddl.h>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QSet>
#include <QHash>
#include <memory>
#include <vector>

namespace hc::filelocks {
namespace {
struct Handle {
    HANDLE value_ = INVALID_HANDLE_VALUE;
    explicit Handle(HANDLE Value = INVALID_HANDLE_VALUE) : value_(Value) {}
    ~Handle() { if (value_ && value_ != INVALID_HANDLE_VALUE) CloseHandle(value_); }
    Handle(const Handle&) = delete;
    bool Valid() const { return value_ && value_ != INVALID_HANDLE_VALUE; }
};
QString normalized(QString Path) {
    Path = QDir::fromNativeSeparators(Path);
    if (Path.startsWith("//?/")) Path.remove(0, 4);
    return QDir::cleanPath(Path);
}
QString finalName(HANDLE File) {
    std::vector<wchar_t> Buffer(32768);
    const DWORD Size = GetFinalPathNameByHandleW(File, Buffer.data(), DWORD(Buffer.size()), FILE_NAME_NORMALIZED | VOLUME_NAME_NT);
    return Size && Size < Buffer.size() ? normalized(QString::fromWCharArray(Buffer.data(), int(Size))) : QString();
}
QString kernelName(const QString& Path) {
    const auto Native = QDir::toNativeSeparators(Path).toStdWString();
    Handle File(CreateFileW(Native.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    if (File.Valid()) { const auto Result = finalName(File.value_); if (!Result.isEmpty()) return Result; }
    wchar_t Device[32768]{};
    const auto Drive = Path.left(2).toStdWString();
    return QueryDosDeviceW(Drive.c_str(), Device, DWORD(std::size(Device)))
        ? normalized(QString::fromWCharArray(Device) + Path.mid(2)) : QString();
}
struct Target { QString path_, kernel_, identity_; bool directory_ = false; };
bool matches(const QString& Kernel, const QString& Identity, const std::vector<Target>& Targets) {
    for (const auto& Target : Targets) {
        if (!Identity.isEmpty() && !Target.identity_.isEmpty() && Identity == Target.identity_) return true;
        if (!Kernel.isEmpty() && !Target.kernel_.isEmpty()
            && (Kernel.compare(Target.kernel_, Qt::CaseInsensitive) == 0
                || (Target.directory_ && Kernel.startsWith(Target.kernel_ + '/', Qt::CaseInsensitive)))) return true;
    }
    return false;
}
QString displayPath(const QString& Kernel, const std::vector<Target>& Targets) {
    for (const auto& Target : Targets)
        if (Kernel.compare(Target.kernel_, Qt::CaseInsensitive) == 0 || (Target.directory_ && Kernel.startsWith(Target.kernel_ + '/', Qt::CaseInsensitive)))
            return QDir::toNativeSeparators(Target.path_ + Kernel.mid(Target.kernel_.size()));
    return QDir::toNativeSeparators(Kernel);
}
bool actionAllowed(HANDLE Process, DWORD Pid, DWORD CallerPid, QString& Reason) {
    BOOL Critical = TRUE;
    const auto Image = QFileInfo(ProcessImage(Process)).fileName();
    if (Pid <= 4 || Pid == CallerPid || Pid == GetCurrentProcessId()
        || Image.compare("HyperCommander.exe", Qt::CaseInsensitive) == 0
        || Image.compare("HyperCommanderAdmin.exe", Qt::CaseInsensitive) == 0
        || Image.compare("HyperCommanderUpdater.exe", Qt::CaseInsensitive) == 0
        || Image.compare("hc-plugin-host-x64.exe", Qt::CaseInsensitive) == 0
        || Image.compare("HyperViewer.exe", Qt::CaseInsensitive) == 0
        || Image.compare("HyperEditor.exe", Qt::CaseInsensitive) == 0
        || Image.compare("HyperEditorWeb.exe", Qt::CaseInsensitive) == 0
        || Image.compare("QtWebEngineProcess.exe", Qt::CaseInsensitive) == 0
        || Image.compare("hc-search-index.exe", Qt::CaseInsensitive) == 0) {
        Reason = QStringLiteral("owned-or-system-process"); return false;
    }
    if (!IsProcessCritical(Process, &Critical) || Critical) { Reason = QStringLiteral("protected-process"); return false; }
    PROCESS_PROTECTION_LEVEL_INFORMATION Protection{};
    if (!GetProcessInformation(Process, ProcessProtectionLevelInfo, &Protection, sizeof(Protection))
        || Protection.ProtectionLevel != PROTECTION_LEVEL_NONE) { Reason = QStringLiteral("protected-process"); return false; }
    if (Image.isEmpty()) { Reason = QStringLiteral("process-unverified"); return false; }
    return true;
}
QJsonObject processRow(DWORD Pid, const QString& Fallback, DWORD CallerPid) {
    Handle Process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, Pid));
    QJsonObject Row{{"pid", int(Pid)}, {"name", Fallback}, {"kind", "process"}, {"closeAllowed", false}, {"terminateAllowed", false}};
    if (!Process.Valid()) { Row["restriction"] = "access-denied"; return Row; }
    const auto Image = ProcessImage(Process.value_);
    Row["image"] = Image; Row["created"] = ProcessCreationTime(Process.value_);
    Row["userSid"] = ProcessUserSid(Process.value_);
    if (!Image.isEmpty()) Row["name"] = QFileInfo(Image).fileName();
    QString Reason;
    Row["terminateAllowed"] = actionAllowed(Process.value_, Pid, CallerPid, Reason) && !Row["created"].toString().isEmpty();
    Row["restriction"] = Reason;
    const auto Sid = Row["userSid"].toString();
    PSID RawSid = nullptr;
    if (ConvertStringSidToSidW(reinterpret_cast<LPCWSTR>(Sid.utf16()), &RawSid)) {
        wchar_t User[256]{}, Domain[256]{}; DWORD UserSize = 256, DomainSize = 256; SID_NAME_USE Use;
        if (LookupAccountSidW(nullptr, RawSid, User, &UserSize, Domain, &DomainSize, &Use))
            Row["user"] = QString::fromWCharArray(Domain) + '\\' + QString::fromWCharArray(User);
        LocalFree(RawSid);
    }
    return Row;
}
struct NativeHandleEntry {
    void* object_; ULONG_PTR pid_, handle_; ULONG access_; USHORT trace_, type_; ULONG attributes_, reserved_;
};
struct NativeHandleTable { ULONG_PTR count_, reserved_; NativeHandleEntry entries_[1]; };
using NtQuerySystemInformationFunction = LONG(NTAPI*)(ULONG, void*, ULONG, ULONG*);
QJsonObject result(const QString& Outcome, DWORD Code = 0, const QString& Stage = QStringLiteral("validate")) {
    return {{"outcome", Outcome}, {"nativeCode", QString::number(Code)}, {"stage",Stage}, {"requiresRefresh", true}};
}
}

QString ProcessCreationTime(void* Process) {
    FILETIME Created{}, Exited{}, Kernel{}, User{};
    if (!GetProcessTimes(Process, &Created, &Exited, &Kernel, &User)) return {};
    return QString::number((quint64(Created.dwHighDateTime) << 32) | Created.dwLowDateTime);
}
QString ProcessImage(void* Process) {
    std::vector<wchar_t> Buffer(32768); DWORD Size = DWORD(Buffer.size());
    return QueryFullProcessImageNameW(Process, 0, Buffer.data(), &Size) ? QString::fromWCharArray(Buffer.data(), int(Size)) : QString();
}
QString ProcessUserSid(void* Process) {
    HANDLE Value = nullptr;
    if (!OpenProcessToken(Process, TOKEN_QUERY, &Value)) return {};
    Handle Token(Value); DWORD Needed = 0;
    GetTokenInformation(Token.value_, TokenUser, nullptr, 0, &Needed);
    if (!Needed || Needed > 65536) return {};
    std::vector<BYTE> Buffer(Needed);
    if (!GetTokenInformation(Token.value_, TokenUser, Buffer.data(), Needed, &Needed)) return {};
    LPWSTR Text = nullptr;
    if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(Buffer.data())->User.Sid, &Text)) return {};
    const auto Sid = QString::fromWCharArray(Text); LocalFree(Text); return Sid;
}
QString NativeFileIdentity(void* File) {
    FILE_ID_INFO Info{};
    if (!GetFileInformationByHandleEx(File, FileIdInfo, &Info, sizeof(Info))) return {};
    return QString::number(Info.VolumeSerialNumber, 16) + ':'
        + QString::fromLatin1(QByteArray(reinterpret_cast<const char*>(Info.FileId.Identifier), 16).toHex());
}
bool IsLocalLockPath(const QString& Path) {
    const auto P = normalized(Path);
    if (Path.contains(QChar::Null) || !QDir::isAbsolutePath(P) || P.size() > 32700 || P.count(':') != 1 || P.indexOf(':') != 1
        || !P[0].isLetter() || !P.startsWith(P.left(2) + '/') || QDir::fromNativeSeparators(Path).split('/').contains("..")) return false;
    const auto Drive = QDir::toNativeSeparators(P.left(3)).toStdWString();
    const auto Kind = GetDriveTypeW(Drive.c_str());
    return Kind == DRIVE_FIXED || Kind == DRIVE_REMOVABLE || Kind == DRIVE_RAMDISK;
}

ScanResult Scan(const QStringList& Paths, quint32 CallerPid, StopRequested Stop) {
    ScanResult Result; QElapsedTimer Timer; Timer.start();
    const auto Stopped = [&] { return Timer.elapsed() > 25000 || (Stop && Stop()); };
    const auto Partial = [&](const QString& Reason) { Result.partial_ = true; if (!Result.reasons_.contains(Reason)) Result.reasons_.append(Reason); };
    std::vector<Target> Targets;
    QHash<DWORD, QJsonObject> Processes; QSet<QString> Seen;
    const auto Append = [&](QJsonObject Row) {
        const auto Key = QString::number(Row["pid"].toInt()) + ':' + Row["kind"].toString() + ':' + Row["handle"].toString() + ':' + Row["path"].toString();
        if (Seen.contains(Key)) return;
        if (Result.rows_.size() >= 2000) { Partial("result-limit"); return; }
        Seen.insert(Key); Row["observedAt"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs); Result.rows_.append(Row);
    };
    const auto Process = [&](DWORD Pid, const QString& Name = QString()) -> QJsonObject {
        if (!Processes.contains(Pid)) Processes.insert(Pid, processRow(Pid, Name, CallerPid));
        return Processes.value(Pid);
    };
    for (const auto& Raw : Paths) {
        if (!IsLocalLockPath(Raw)) { Partial("non-local-path"); continue; }
        const auto Path = normalized(Raw); const auto Native = QDir::toNativeSeparators(Path).toStdWString();
        Handle File(CreateFileW(Native.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
        Target Target{Path, kernelName(Path), File.Valid() ? NativeFileIdentity(File.value_) : QString(), QFileInfo(Path).isDir()};
        Targets.push_back(Target);
        if (Target.directory_) continue;
        DWORD Session = 0; wchar_t Key[CCH_RM_SESSION_KEY + 1]{};
        if (RmStartSession(&Session, 0, Key) != ERROR_SUCCESS) { Partial("restart-manager"); continue; }
        LPCWSTR Resource = Native.c_str();
        if (RmRegisterResources(Session, 1, &Resource, 0, nullptr, 0, nullptr) == ERROR_SUCCESS) {
            UINT Needed = 0, Count = 0; DWORD Reasons = 0;
            DWORD Status = RmGetList(Session, &Needed, &Count, nullptr, &Reasons);
            for (int Attempt = 0; Status == ERROR_MORE_DATA && Needed <= 2000 && Attempt < 3; ++Attempt) {
                std::vector<RM_PROCESS_INFO> Info(Needed); Count = Needed;
                Status = RmGetList(Session, &Needed, &Count, Info.data(), &Reasons);
                if (Status == ERROR_SUCCESS) for (UINT I = 0; I < Count; ++I) {
                    // The diagnostic's temporary metadata handle must not appear
                    // as a file owner created by the query itself.
                    if (Info[I].Process.dwProcessId == GetCurrentProcessId()) continue;
                    auto Row = Process(Info[I].Process.dwProcessId, QString::fromWCharArray(Info[I].strAppName));
                    const auto& Created = Info[I].Process.ProcessStartTime;
                    const auto Observed = QString::number((quint64(Created.dwHighDateTime) << 32) | Created.dwLowDateTime);
                    if (!Row["created"].toString().isEmpty() && Row["created"].toString() != Observed) { Partial("process-changed"); continue; }
                    if (Row["created"].toString().isEmpty()) Partial("process-unverified");
                    Row["path"] = QDir::toNativeSeparators(Path); Append(Row);
                }
            }
            if (Status != ERROR_SUCCESS) Partial("restart-manager");
        } else Partial("restart-manager");
        RmEndSession(Session);
        if (Stopped()) { Partial("deadline"); return Result; }
    }
    if (Targets.empty()) return Result;
    const auto Query = reinterpret_cast<NtQuerySystemInformationFunction>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQuerySystemInformation"));
    Handle Marker(CreateFileW(L"NUL", 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr));
    std::vector<BYTE> Buffer(1024 * 1024); ULONG Needed = 0; LONG Status = -1;
    while (Query && Buffer.size() <= 256ull * 1024 * 1024 && !Stopped()) {
        Status = Query(64, Buffer.data(), ULONG(Buffer.size()), &Needed);
        if (Status != LONG(0xC0000004)) break;
        const auto Next = std::max<quint64>(Buffer.size() * 2, quint64(Needed) + 65536);
        if (Next > 256ull * 1024 * 1024) break;
        Buffer.resize(size_t(Next));
    }
    if (Status >= 0 && Buffer.size() >= sizeof(NativeHandleTable)) {
        const auto* Table = reinterpret_cast<const NativeHandleTable*>(Buffer.data());
        const auto Capacity = (Buffer.size() - offsetof(NativeHandleTable, entries_)) / sizeof(NativeHandleEntry);
        USHORT FileType = 0;
        for (ULONG_PTR I = 0; I < Table->count_ && I < Capacity; ++I)
            if (Table->entries_[I].pid_ == GetCurrentProcessId() && Table->entries_[I].handle_ == reinterpret_cast<ULONG_PTR>(Marker.value_)) FileType = Table->entries_[I].type_;
        QHash<DWORD, std::shared_ptr<Handle>> Duplicators;
        if (!FileType || Table->count_ > Capacity) Partial("handle-snapshot");
        for (ULONG_PTR I = 0; FileType && I < Table->count_ && I < Capacity; ++I) {
            if (Stopped() || Result.rows_.size() >= 2000) { Partial("deadline-or-result-limit"); break; }
            const auto& Item = Table->entries_[I];
            if (Item.type_ != FileType || Item.pid_ == GetCurrentProcessId() || Item.pid_ > MAXDWORD) continue;
            const auto Pid = DWORD(Item.pid_);
            if (!Duplicators.contains(Pid)) Duplicators.insert(Pid, std::make_shared<Handle>(OpenProcess(PROCESS_DUP_HANDLE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, Pid)));
            const auto Owner = Duplicators.value(Pid);
            if (!Owner->Valid()) { Partial("access-denied"); continue; }
            HANDLE Copy = nullptr;
            if (!DuplicateHandle(Owner->value_, reinterpret_cast<HANDLE>(Item.handle_), GetCurrentProcess(), &Copy, 0, FALSE, DUPLICATE_SAME_ACCESS)) continue;
            Handle Local(Copy);
            if (GetFileType(Copy) != FILE_TYPE_DISK) continue;
            const auto Kernel = finalName(Copy);
            const auto Identity = NativeFileIdentity(Copy);
            if (!matches(Kernel, Identity, Targets)) continue;
            auto Row = Process(Pid);
            // Bind metadata to the exact process object used for duplication.
            if (Row["created"].toString() != ProcessCreationTime(Owner->value_)) continue;
            Row["kind"] = "handle"; Row["handle"] = QString::number(Item.handle_, 16);
            Row["path"] = displayPath(Kernel, Targets); Row["kernelPath"] = Kernel; Row["identity"] = Identity;
            if (Kernel.isEmpty()) for (const auto& Target : Targets)
                if (!Identity.isEmpty() && Identity == Target.identity_) { Row["path"] = QDir::toNativeSeparators(Target.path_); break; }
            Row["access"] = QString::number(Item.access_, 16);
            FILE_ATTRIBUTE_TAG_INFO Attributes{};
            const bool Verified = GetFileInformationByHandleEx(Copy, FileAttributeTagInfo, &Attributes, sizeof(Attributes))
                && !(Attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) && !Identity.isEmpty();
            Row["closeAllowed"] = Verified && Row["terminateAllowed"].toBool(); Append(Row);
        }
    } else Partial("handle-snapshot");
    Handle Snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    PROCESSENTRY32W Entry{}; Entry.dwSize = sizeof(Entry);
    if (Snapshot.Valid() && Process32FirstW(Snapshot.value_, &Entry)) do {
        if (Stopped() || Result.rows_.size() >= 2000) { Partial("deadline-or-result-limit"); break; }
        if (Entry.th32ProcessID == GetCurrentProcessId()) continue;
        Handle Owner(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, Entry.th32ProcessID));
        if (!Owner.Valid()) continue;
        const auto Image = ProcessImage(Owner.value_);
        auto AddModule = [&](const QString& Path, const char* Kind) {
            if (Path.isEmpty() || !IsLocalLockPath(Path)) return;
            if (!matches(kernelName(Path), {}, Targets)) return;
            auto Row = Process(Entry.th32ProcessID, QString::fromWCharArray(Entry.szExeFile));
            if (Row["created"].toString() != ProcessCreationTime(Owner.value_)) return;
            Row["kind"] = Kind; Row["path"] = Path; Append(Row);
        };
        AddModule(Image, "image");
        const bool ModulesNeeded = std::any_of(Targets.begin(), Targets.end(), [](const auto& T) { return T.directory_ || T.path_.endsWith(".dll", Qt::CaseInsensitive); });
        if (!ModulesNeeded) continue;
        Handle Modules(CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, Entry.th32ProcessID));
        MODULEENTRY32W Module{}; Module.dwSize = sizeof(Module);
        if (Modules.Valid() && Module32FirstW(Modules.value_, &Module)) do {
            if (Stopped()) break;
            AddModule(QString::fromWCharArray(Module.szExePath), "module");
        } while (Module32NextW(Modules.value_, &Module));
    } while (Process32NextW(Snapshot.value_, &Entry));
    if (Stopped()) Partial("deadline");
    return Result;
}

QJsonObject ExecuteAction(const QJsonObject& Row, const QString& Action, quint32 CallerPid) {
    if ((Action != "close" && Action != "terminate") || !Row[Action == "close" ? "closeAllowed" : "terminateAllowed"].toBool()) return result("Rejected", ERROR_ACCESS_DENIED);
    const auto Pid = DWORD(Row["pid"].toInt());
    Handle Process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | (Action == "close" ? PROCESS_DUP_HANDLE : PROCESS_TERMINATE | SYNCHRONIZE), FALSE, Pid));
    if (!Process.Valid()) return result(GetLastError() == ERROR_INVALID_PARAMETER ? "AlreadyGone" : "Rejected", GetLastError());
    if (Row["created"].toString().isEmpty() || ProcessCreationTime(Process.value_) != Row["created"].toString()
        || ProcessImage(Process.value_).compare(Row["image"].toString(), Qt::CaseInsensitive)) return result("TargetChanged");
    QString Reason;
    if (!actionAllowed(Process.value_, Pid, CallerPid, Reason)) return result("Rejected", ERROR_ACCESS_DENIED);
    if (Action == "terminate") {
        if (!TerminateProcess(Process.value_, 0)) return result("Rejected", GetLastError());
        return result(WaitForSingleObject(Process.value_, 3000) == WAIT_OBJECT_0 ? "Completed" : "Unknown",0,"terminate");
    }
    bool Valid = false; const auto Number = Row["handle"].toString().toULongLong(&Valid, 16);
    if (!Valid || !Number || Row["kind"] != "handle" || Row["identity"].toString().isEmpty()) return result("Rejected", ERROR_INVALID_PARAMETER);
    HANDLE Copy = nullptr;
    if (!DuplicateHandle(Process.value_, reinterpret_cast<HANDLE>(quintptr(Number)), GetCurrentProcess(), &Copy, 0, FALSE, DUPLICATE_SAME_ACCESS)) return result("AlreadyGone", GetLastError());
    Handle Local(Copy);
    if (GetFileType(Copy) != FILE_TYPE_DISK || NativeFileIdentity(Copy) != Row["identity"].toString()
        || finalName(Copy).compare(Row["kernelPath"].toString(), Qt::CaseInsensitive)) return result("TargetChanged");
    HANDLE ClosedCopy = nullptr;
    const bool Closed = DuplicateHandle(Process.value_, reinterpret_cast<HANDLE>(quintptr(Number)), GetCurrentProcess(), &ClosedCopy, 0, FALSE, DUPLICATE_SAME_ACCESS | DUPLICATE_CLOSE_SOURCE);
    const DWORD Error = Closed ? ERROR_SUCCESS : GetLastError();
    Handle Cleanup(ClosedCopy);
    // Even a failed CLOSE_SOURCE can have closed the original. Never retry it.
    return result(Closed ? "Completed" : "Unknown", Error,"close-source");
}
}
