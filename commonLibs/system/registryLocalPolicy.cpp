#include "stdafx.h"
#include "registryLocalPolicy.hpp"

/// 모듈
/// UIs
/// 기타 헤더파일
/// 고유 라이브러리
/// 공통 라이브러리
/// 외부 라이브러리
/// 선언 헤더파일들
/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QFile>
#include <QProcess>
#include <QElapsedTimer>
#include <QMap>
#include <QScopeGuard>
#include <initguid.h>
#include <gpedit.h>

DECLARE_CMNLIBSV2_NAMESPACE
Q_LOGGING_CATEGORY(lcRegistryLocalPolicy, "hc.registry.localPolicy")
namespace hc::registry {
namespace {
constexpr qsizetype MaximumPolicyBytes = 32 * 1024 * 1024;
QString PolicyPath(bool Machine)
{
    wchar_t System[32768]{};
    const auto Length = GetSystemDirectoryW(System, DWORD(std::size(System)));
    if (!Length || Length >= std::size(System)) return {};
    return QString::fromWCharArray(System, int(Length)) + (Machine ? "/GroupPolicy/Machine/Registry.pol" : "/GroupPolicy/User/Registry.pol");
}
QString RecordId(const RegistryPolRecord& Record) { return Record.key.toCaseFolded() + QChar(0) + Record.value.name.toCaseFolded(); }
RegistryResult<QMap<QString, RegistryPolRecord>> Records(const QByteArray& Bytes)
{
    if (Bytes.size() > MaximumPolicyBytes) return {{}, ERROR_FILE_TOO_LARGE};
    if (Bytes.isEmpty()) return {};
    const auto Parsed = RegistryPolCodec::Read(Bytes); if (!Parsed) return {{}, ERROR_INVALID_DATA};
    QMap<QString, RegistryPolRecord> Result;
    for (const auto& Record : Parsed.records) {
        const auto Id = RecordId(Record);
        // A duplicate can have sequencing semantics which cannot be represented
        // by the GPO registry hive without loss. Never silently collapse it.
        if (Result.contains(Id)) return {{}, ERROR_NOT_SUPPORTED};
        Result.insert(Id, Record);
    }
    return {Result};
}
quint32 HResult(HRESULT Result) { return HRESULT_FACILITY(Result) == FACILITY_WIN32 ? HRESULT_CODE(Result) : quint32(Result); }
}
RegistryResult<QByteArray> LocalPolicyStore::Read(bool Machine)
{
    const auto Path = PolicyPath(Machine); if (Path.isEmpty()) return {{}, ERROR_PATH_NOT_FOUND};
    const auto Native = Path.toStdWString();
    if (GetFileAttributesW(Native.c_str()) == INVALID_FILE_ATTRIBUTES) {
        const auto Error = GetLastError();
        return Error == ERROR_FILE_NOT_FOUND || Error == ERROR_PATH_NOT_FOUND ? RegistryResult<QByteArray>{} : RegistryResult<QByteArray>{{}, Error};
    }
    QFile File(Path);
    if (!File.open(QIODevice::ReadOnly)) return {{}, quint32(File.error() == QFileDevice::PermissionsError ? ERROR_ACCESS_DENIED : ERROR_READ_FAULT)};
    if (File.size() > MaximumPolicyBytes) return {{}, ERROR_FILE_TOO_LARGE};
    const auto Bytes = File.read(MaximumPolicyBytes + 1);
    if (File.error() != QFileDevice::NoError || Bytes.size() > MaximumPolicyBytes) return {{}, ERROR_READ_FAULT};
    const auto Valid = Records(Bytes); if (!Valid) return {{}, Valid.error};
    return {Bytes};
}
RegistryResult<QByteArray> LocalPolicyStore::Canonical(const QByteArray& Bytes)
{
    const auto Parsed = Records(Bytes); if (!Parsed) return {{}, Parsed.error};
    QList<RegistryPolRecord> Sorted;
    for (auto Record : Parsed.value) { Record.key = Record.key.toCaseFolded(); Record.value.name = Record.value.name.toCaseFolded(); Sorted.append(std::move(Record)); }
    return RegistryPolCodec::Write(Sorted);
}
RegistryResult<QByteArray> LocalPolicyStore::Apply(bool Machine, const QByteArray& Expected, const QByteArray& Desired)
{
    const auto Before = Records(Expected), After = Records(Desired);
    if (!Before) return {{}, Before.error}; if (!After) return {{}, After.error};
    const auto ExpectedCanonical = Canonical(Expected), DesiredCanonical = Canonical(Desired);
    if (!ExpectedCanonical) return {{}, ExpectedCanonical.error};
    if (!DesiredCanonical) return {{}, DesiredCanonical.error};
    auto Current = Read(Machine); if (!Current) return Current;
    const auto CurrentCanonical = Canonical(Current.value);
    if (!CurrentCanonical || CurrentCanonical.value != ExpectedCanonical.value) return {{}, ERROR_REVISION_MISMATCH};
    if (ExpectedCanonical.value == DesiredCanonical.value) return Current;

    const auto Initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(Initialized)) return {{}, HResult(Initialized)};
    const auto Uninitialize = qScopeGuard([] { CoUninitialize(); });
    IGroupPolicyObject* Policy = nullptr;
    auto Status = CoCreateInstance(CLSID_GroupPolicyObject, nullptr, CLSCTX_INPROC_SERVER, IID_IGroupPolicyObject, reinterpret_cast<void**>(&Policy));
    if (FAILED(Status)) return {{}, HResult(Status)};
    const auto Release = qScopeGuard([&] { Policy->Release(); });
    Status = Policy->OpenLocalMachineGPO(GPO_OPEN_LOAD_REGISTRY);
    if (FAILED(Status)) return {{}, HResult(Status)};
    HKEY Root = nullptr; Status = Policy->GetRegistryKey(Machine ? GPO_SECTION_MACHINE : GPO_SECTION_USER, &Root);
    if (FAILED(Status)) return {{}, HResult(Status)};
    KeyHandle RootHandle(Root);
    // The load may have raced with gpedit. Recheck before altering its in-memory
    // hive; a mismatch is a conflict, never permission to replace another edit.
    Current = Read(Machine); if (!Current) return Current;
    const auto Loaded = Canonical(Current.value);
    if (!Loaded || Loaded.value != ExpectedCanonical.value) return {{}, ERROR_REVISION_MISMATCH};
    QStringList ChangedKeys;
    for (auto I = Before.value.cbegin(); I != Before.value.cend(); ++I) if (!After.value.contains(I.key())) {
        const auto& Record = I.value(); const auto KeyName = Record.key.toStdWString(); HKEY Key = nullptr;
        auto Error = RegOpenKeyExW(Root, KeyName.c_str(), 0, KEY_SET_VALUE, &Key);
        if (Error == ERROR_FILE_NOT_FOUND || Error == ERROR_PATH_NOT_FOUND) continue;
        if (Error) return {{}, quint32(Error)};
        KeyHandle Handle(Key); const auto Name = Record.value.name.toStdWString();
        Error = RegDeleteValueW(Key, Name.c_str()); if (Error && Error != ERROR_FILE_NOT_FOUND) return {{}, quint32(Error)};
        ChangedKeys.append(Record.key);
    }
    for (auto I = After.value.cbegin(); I != After.value.cend(); ++I) {
        if (Before.value.contains(I.key()) && Before.value[I.key()].value == I.value().value) continue;
        const auto& Record = I.value(); const auto KeyName = Record.key.toStdWString(); HKEY Key = nullptr;
        auto Error = RegCreateKeyExW(Root, KeyName.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &Key, nullptr);
        if (Error) return {{}, quint32(Error)};
        KeyHandle Handle(Key); const auto Name = Record.value.name.toStdWString();
        Error = RegSetValueExW(Key, Name.c_str(), 0, Record.value.type,
            reinterpret_cast<const BYTE*>(Record.value.bytes.constData()), DWORD(Record.value.bytes.size()));
        if (Error) return {{}, quint32(Error)};
    }
    // Delete only now-empty keys created by the affected settings. Other policy
    // values and child keys must survive removal of a single policy.
    for (auto KeyName : ChangedKeys) while (!KeyName.isEmpty()) {
        const auto Native = KeyName.toStdWString(); HKEY Key = nullptr;
        if (RegOpenKeyExW(Root, Native.c_str(), 0, KEY_QUERY_VALUE | KEY_ENUMERATE_SUB_KEYS, &Key)) break;
        DWORD Subkeys = 0, Values = 0;
        const auto Error = RegQueryInfoKeyW(Key, nullptr, nullptr, nullptr, &Subkeys, nullptr, nullptr, &Values, nullptr, nullptr, nullptr, nullptr);
        RegCloseKey(Key);
        if (Error || Subkeys || Values || RegDeleteKeyW(Root, Native.c_str())) break;
        const auto Slash = KeyName.lastIndexOf('\\'); KeyName = Slash >= 0 ? KeyName.left(Slash) : QString();
    }
    GUID Extension = REGISTRY_EXTENSION_GUID;
    GUID Editor = {0xe815b735,0x8460,0x436f,{0x91,0xfb,0xab,0xe1,0x20,0xae,0xba,0x80}};
    // FALSE removes the extension only when its LAST setting is removed.
    Status = Policy->Save(Machine ? TRUE : FALSE, After.value.isEmpty() ? FALSE : TRUE, &Extension, &Editor);
    if (FAILED(Status)) return {{}, HResult(Status)};
    const auto Saved = Read(Machine); if (!Saved) return Saved;
    const auto SavedCanonical = Canonical(Saved.value);
    if (!SavedCanonical || SavedCanonical.value != DesiredCanonical.value) return {{}, ERROR_REVISION_MISMATCH};
    return Saved;
}
quint32 LocalPolicyStore::Refresh(bool Machine, const std::atomic_bool* Cancel, int TimeoutMs)
{
    if (Cancel && Cancel->load()) return ERROR_CANCELLED;
    wchar_t System[32768]{}; const auto Length = GetSystemDirectoryW(System, DWORD(std::size(System)));
    if (!Length || Length >= std::size(System)) return ERROR_PATH_NOT_FOUND;
    QProcess Process;
    Process.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* Arguments) { Arguments->flags |= CREATE_NO_WINDOW; });
    Process.setStandardOutputFile(QProcess::nullDevice()); Process.setStandardErrorFile(QProcess::nullDevice());
    // A finite gpupdate /wait value can exit while policy processing continues.
    // Wait indefinitely in gpupdate and enforce our own cancellable deadline.
    // Never pass /boot, /logoff, /sync or any caller-supplied executable/argument.
    Process.start(QString::fromWCharArray(System, int(Length)) + "/gpupdate.exe",
        {Machine ? "/target:computer" : "/target:user", "/force", "/wait:-1"});
    if (!Process.waitForStarted(5000)) return ERROR_PROCESS_ABORTED;
    QElapsedTimer Timer; Timer.start();
    while (Process.state() != QProcess::NotRunning) {
        const bool Cancelled = Cancel && Cancel->load();
        if (Cancelled || Timer.elapsed() >= qBound(1, TimeoutMs, 600000)) {
            // Stop only our waiter. Windows policy processing may still finish;
            // the caller must retain a recovery-required journal, never roll back
            // managed values through direct registry writes.
            Process.kill(); Process.waitForFinished(5000);
            return Cancelled ? ERROR_CANCELLED : ERROR_TIMEOUT;
        }
        Process.waitForFinished(50);
    }
    return Process.exitStatus() == QProcess::NormalExit && Process.exitCode() == 0 ? ERROR_SUCCESS : ERROR_GEN_FAILURE;
}
} // namespace hc::registry
