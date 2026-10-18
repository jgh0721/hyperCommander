#pragma once
#include "fileBatchOperations.hpp"
#include "settings/appSettings.hpp"
#include <Shellapi.h>
#include <Aclapi.h>
#include <Sddl.h>
namespace hc::permissions {
inline bool Elevated() {
    HANDLE Raw = nullptr; if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &Raw)) return false;
    filetools::Handle Token(Raw); TOKEN_ELEVATION Value{}; DWORD Size = 0;
    return GetTokenInformation(Raw, TokenElevation, &Value, sizeof(Value), &Size) && Value.TokenIsElevated;
}
struct Inspection { QString path_, owner_, error_; bool read_ = false, remove_ = false, create_ = false; };
inline bool Access(const QString& Path, DWORD Desired) {
    const auto P = filetools::NativePath(Path);
    filetools::Handle File(CreateFileW(reinterpret_cast<LPCWSTR>(P.utf16()), Desired, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    return File.value_ != INVALID_HANDLE_VALUE;
}
inline Inspection Inspect(const QString& Path) {
    Inspection I; I.path_ = Path;
    if (!filetools::LocalFolder(QFileInfo(Path).absolutePath())) { I.error_ = QObject::tr("로컬 드라이브만 검사합니다."); return I; }
    I.read_ = Access(Path, FILE_READ_DATA); I.remove_ = Access(Path, DELETE);
    I.create_ = QFileInfo(Path).isDir() && Access(Path, FILE_ADD_FILE | FILE_ADD_SUBDIRECTORY);
    const auto P = filetools::NativePath(Path);
    filetools::Handle File(CreateFileW(reinterpret_cast<LPCWSTR>(P.utf16()), READ_CONTROL, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (File.value_ == INVALID_HANDLE_VALUE) { I.error_ = QObject::tr("소유자 조회 실패: %1").arg(GetLastError()); return I; }
    PSECURITY_DESCRIPTOR Descriptor = nullptr; PSID Owner = nullptr;
    const auto Code = GetSecurityInfo(File.value_, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION, &Owner, nullptr, nullptr, nullptr, &Descriptor);
    if (!Code) {
        wchar_t Name[256]{}, Domain[256]{}; DWORD N = 256, D = 256; SID_NAME_USE Use{};
        if (LookupAccountSidW(nullptr, Owner, Name, &N, Domain, &D, &Use)) I.owner_ = QString::fromWCharArray(Domain) + '\\' + QString::fromWCharArray(Name);
        else { LPWSTR Sid = nullptr; if (ConvertSidToStringSidW(Owner, &Sid)) { I.owner_ = QString::fromWCharArray(Sid); LocalFree(Sid); } }
        LocalFree(Descriptor);
    } else I.error_ = QObject::tr("소유자 조회 실패: %1").arg(Code);
    return I;
}
inline QString HelperPath() { return QDir(QCoreApplication::applicationDirPath()).filePath("HyperCommanderAdmin.exe"); }
inline QJsonObject Request(const QString& Operation, const QString& Folder, const QString& Name, const QString& Source = {}, int Type = 0, bool Bom = false) {
    return {{"version",1}, {"id",QUuid::createUuid().toString(QUuid::WithoutBraces)}, {"utc",QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
        {"operation",Operation}, {"folder",QDir::cleanPath(Folder)}, {"name",Name}, {"parentId",QString::fromLatin1(filetools::Inspect(Folder).identity_)},
        {"source",Source}, {"sourceId",Source.isEmpty() ? QString() : QString::fromLatin1(filetools::Inspect(Source).identity_)}, {"type",Type}, {"bom",Bom}};
}
inline DWORD Validate(const QJsonObject& R) {
    const QStringList Keys{"version","id","utc","operation","folder","name","parentId","source","sourceId","type","bom"};
    for (const auto& Key : Keys) if (!R.contains(Key)) return ERROR_INVALID_PARAMETER;
    if (R.size() != 11 || R["version"].toInt() != 1 || QUuid(R["id"].toString()).isNull()) return ERROR_INVALID_PARAMETER;
    const auto Age = QDateTime::fromString(R["utc"].toString(), Qt::ISODateWithMs).msecsTo(QDateTime::currentDateTimeUtc());
    if (!QDateTime::fromString(R["utc"].toString(), Qt::ISODateWithMs).isValid() || Age < -5000 || Age > 120000) return ERROR_TIMEOUT;
    const auto Op = R["operation"].toString(), Folder = R["folder"].toString(), Name = R["name"].toString();
    if (!QStringList{"create","mkdir","rename"}.contains(Op) || !filetools::LocalFolder(Folder) || !filetools::NameError(Name).isEmpty()
        || R["parentId"].toString().isEmpty() || !R["bom"].isBool() || R["type"].toInt(-1) < 0 || R["type"].toInt() >= 8) return ERROR_INVALID_PARAMETER;
    if (Op == "rename" && (R["sourceId"].toString().isEmpty() || QFileInfo(R["source"].toString()).absolutePath().compare(Folder, Qt::CaseInsensitive))) return ERROR_INVALID_PARAMETER;
    return ERROR_SUCCESS;
}
// Called only by the single-request helper (after token validation) and isolated
// fixture tests. The immutable command line replaces a writable request file/IPC.
inline DWORD Execute(const QJsonObject& R) {
    if (const auto Code = Validate(R)) return Code;
    const auto Folder = R["folder"].toString(), P = filetools::NativePath(Folder);
    // Keep every ancestor stable while path-based Win32 operations run. A leaf-only
    // identity check would allow an ancestor to be renamed/replaced after validation.
    std::vector<std::unique_ptr<filetools::Handle>> Ancestors;
    auto Ancestor = QDir::fromNativeSeparators(Folder).left(3);
    const auto Parts = QDir::fromNativeSeparators(Folder).mid(3).split('/', Qt::SkipEmptyParts);
    for (int Index = -1; Index < Parts.size(); ++Index) {
        if (Index >= 0) Ancestor = QDir(Ancestor).filePath(Parts[Index]);
        const auto NativeAncestor = filetools::NativePath(Ancestor);
        auto Lock = std::make_unique<filetools::Handle>(CreateFileW(reinterpret_cast<LPCWSTR>(NativeAncestor.utf16()), FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        if (Lock->value_ == INVALID_HANDLE_VALUE) return GetLastError();
        BY_HANDLE_FILE_INFORMATION AncestorInfo{};
        if (!GetFileInformationByHandle(Lock->value_, &AncestorInfo) || !(AncestorInfo.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            || (AncestorInfo.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) return ERROR_FILE_INVALID;
        Ancestors.push_back(std::move(Lock));
    }
    filetools::Handle Parent(CreateFileW(reinterpret_cast<LPCWSTR>(P.utf16()), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (Parent.value_ == INVALID_HANDLE_VALUE) return GetLastError();
    BY_HANDLE_FILE_INFORMATION Info{};
    if (!GetFileInformationByHandle(Parent.value_, &Info) || !(Info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (Info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
        || filetools::Identity(Parent.value_) != R["parentId"].toString().toLatin1()) return ERROR_FILE_INVALID;
    wchar_t Final[32768]{}; const auto Length = GetFinalPathNameByHandleW(Parent.value_, Final, 32768, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (!Length || Length >= 32768 || QDir::fromNativeSeparators(QString::fromWCharArray(Final)).compare(QDir::fromNativeSeparators(P), Qt::CaseInsensitive)) return ERROR_FILE_INVALID;
    const auto Destination = QDir(Folder).filePath(R["name"].toString()), Op = R["operation"].toString();
    if (Op == "mkdir") { const auto Native = filetools::NativePath(Destination); return CreateDirectoryW(reinterpret_cast<LPCWSTR>(Native.utf16()), nullptr) ? ERROR_SUCCESS : GetLastError(); }
    if (Op == "create") return filetools::Create(Folder, R["name"].toString(), R["type"].toInt(), R["bom"].toBool()).isEmpty() ? ERROR_SUCCESS : ERROR_WRITE_FAULT;
    const auto Source = R["source"].toString(), Native = filetools::NativePath(Source);
    filetools::Handle File(CreateFileW(reinterpret_cast<LPCWSTR>(Native.utf16()), DELETE | FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (File.value_ == INVALID_HANDLE_VALUE) return GetLastError();
    BY_HANDLE_FILE_INFORMATION SourceInfo{};
    if (!GetFileInformationByHandle(File.value_, &SourceInfo) || (SourceInfo.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) return ERROR_FILE_INVALID;
    if (filetools::Identity(File.value_) != R["sourceId"].toString().toLatin1()) return ERROR_FILE_INVALID;
    return filetools::RenameHandle(File.value_, Destination);
}
struct LaunchResult { DWORD code_ = ERROR_SUCCESS; bool started_ = false; };
inline LaunchResult Launch(QJsonObject Request) {
    Request["utc"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    if (const auto Code = Validate(Request)) return {Code, false};
    const auto Payload = QJsonDocument(Request).toJson(QJsonDocument::Compact).toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
    if (Payload.size() > 16000) return {ERROR_BUFFER_OVERFLOW, false};
    const auto Exe = QDir::toNativeSeparators(HelperPath()), Arguments = "--request " + QString::fromLatin1(Payload);
    SHELLEXECUTEINFOW Info{}; Info.cbSize = sizeof(Info); Info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    Info.lpVerb = L"runas"; Info.lpFile = reinterpret_cast<LPCWSTR>(Exe.utf16()); Info.lpParameters = reinterpret_cast<LPCWSTR>(Arguments.utf16()); Info.nShow = SW_HIDE;
    if (!ShellExecuteExW(&Info)) return {GetLastError(), false};
    if (!Info.hProcess) return {ERROR_INVALID_HANDLE, true};
    filetools::Handle Process(Info.hProcess); WaitForSingleObject(Process.value_, INFINITE); DWORD Code = ERROR_GEN_FAILURE;
    GetExitCodeProcess(Process.value_, &Code); return {Code, true};
}
inline QString ResultText(LaunchResult R) {
    if (!R.code_) return QObject::tr("요청한 작업을 완료했습니다. 도우미는 종료되었습니다.");
    if (R.code_ == ERROR_CANCELLED && !R.started_) return QObject::tr("UAC 요청이 취소되었습니다. 도우미가 시작되지 않아 작업을 실행하지 않았습니다.");
    if (R.code_ == ERROR_TIMEOUT) return QObject::tr("요청 유효 시간이 지났습니다. 작업을 실행하지 않았습니다.");
    return QObject::tr("도우미 오류 %1 · %2").arg(R.code_).arg(R.started_ ? QObject::tr("결과 위치를 확인하세요. 자동 재실행하지 않습니다.") : QObject::tr("작업을 시작하지 못했습니다."));
}
inline QString HistoryPath() { return QDir(settings::Paths().directory_).filePath("permission-history.json"); }
inline std::mutex& HistoryMutex(){static std::mutex Mutex;return Mutex;}
inline QJsonArray historyUnlocked() {
    QFile File(HistoryPath()); if (!File.open(QIODevice::ReadOnly) || File.size() > 1024*1024) return {};
    QJsonArray Result; for (const auto V : QJsonDocument::fromJson(File.readAll()).array())
        if (QDateTime::fromString(V.toObject()["utc"].toString(), Qt::ISODateWithMs).daysTo(QDateTime::currentDateTimeUtc()) <= 30) Result.append(V);
    return Result;
}
inline bool saveHistoryUnlocked(QJsonArray Records) {
    while (Records.size() > 100) Records.removeFirst(); QDir().mkpath(settings::Paths().directory_);
    const auto Bytes = QJsonDocument(Records).toJson();
    QSaveFile File(HistoryPath()); return File.open(QIODevice::WriteOnly) && File.write(Bytes) == Bytes.size() && File.commit();
}
inline QJsonArray History(){std::lock_guard Lock(HistoryMutex());return historyUnlocked();}
inline bool SaveHistory(QJsonArray Records){std::lock_guard Lock(HistoryMutex());return saveHistoryUnlocked(std::move(Records));}
inline bool AppendHistory(const QJsonObject& Record){std::lock_guard Lock(HistoryMutex());auto Records=historyUnlocked();Records.append(Record);return saveHistoryUnlocked(std::move(Records));}
} // namespace hc::permissions
