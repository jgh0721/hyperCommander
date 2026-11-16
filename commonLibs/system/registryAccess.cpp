#include "stdafx.h"
#include "registryAccess.hpp"

/// 모듈
/// UIs
/// 기타 헤더파일
/// 고유 라이브러리
/// 공통 라이브러리
#include "ntApi.hpp"
/// 외부 라이브러리
/// 선언 헤더파일들
/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QLoggingCategory>
#include <array>

DECLARE_CMNLIBSV2_NAMESPACE
Q_LOGGING_CATEGORY(lcRegistryAccess, "hc.registry.access")

namespace hc::registry {
namespace {
constexpr std::array<const wchar_t*, 5> Names{ L"HKEY_CLASSES_ROOT", L"HKEY_CURRENT_USER", L"HKEY_LOCAL_MACHINE", L"HKEY_USERS", L"HKEY_CURRENT_CONFIG" };
constexpr std::array<const wchar_t*, 5> ShortNames{ L"HKCR", L"HKCU", L"HKLM", L"HKU", L"HKCC" };
const wchar_t* Wide(const QString& Value) { return reinterpret_cast<const wchar_t*>(Value.utf16()); }
REGSAM ViewMask(View Value) { return Value == View::Wow32 ? KEY_WOW64_32KEY : KEY_WOW64_64KEY; }
bool ValidLocation(const RegistryLocation& Value)
{
    if (int(Value.hive) < 0 || int(Value.hive) >= int(Names.size()) || Value.key.size() > 32767
        || Value.key.contains(QChar(0)) || Value.sid.contains(QChar(0)) || Value.sid.contains('\\')) return false;
    if (Value.key.isEmpty()) return true;
    for (const auto& Part : Value.key.split('\\')) if (!WinRegistryBackend::ValidName(Part, true)) return false;
    return true;
}
std::pair<HKEY, QString> NativePath(const RegistryLocation& Location)
{
    static const std::array<HKEY, 5> Hives{ HKEY_CLASSES_ROOT, HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE, HKEY_USERS, HKEY_CURRENT_CONFIG };
    if (Location.hive == Hive::CurrentUser && !Location.sid.isEmpty())
        return { HKEY_USERS, Location.sid + (Location.key.isEmpty() ? QString() : "\\" + Location.key) };
    return { Hives[size_t(Location.hive)], Location.key };
}
quint64 Time(const FILETIME& Value) { return quint64(Value.dwLowDateTime) | (quint64(Value.dwHighDateTime) << 32); }
quint32 DeleteLinkHandle(HKEY Key)
{
    using Delete = LONG(NTAPI*)(HANDLE);
    using ToError = ULONG(NTAPI*)(LONG);
    const auto Module = GetModuleHandleW(L"ntdll.dll");
    static const auto Function = reinterpret_cast<Delete>(GetProcAddress(Module, "NtDeleteKey"));
    static const auto Convert = reinterpret_cast<ToError>(GetProcAddress(Module, "RtlNtStatusToDosError"));
    if (!Function || !Convert) return ERROR_CALL_NOT_IMPLEMENTED;
    const LONG Status = Function(Key);
    return Status >= 0 ? ERROR_SUCCESS : Convert(Status);
}
RegistryResult<RegistryValue> ReadHandle(HKEY Key, const QString& Name)
{
    RegistryResult<RegistryValue> Result; Result.value.name = Name;
    for (int Attempt = 0; Attempt != 8; ++Attempt) {
        DWORD Size = 0, Type = 0;
        Result.error = RegQueryValueExW(Key, Wide(Name), nullptr, &Type, nullptr, &Size);
        if (Result.error != ERROR_SUCCESS && Result.error != ERROR_MORE_DATA) return Result;
        if (Size > WinRegistryBackend::MaxValueBytes) { Result.error = ERROR_FILE_TOO_LARGE; return Result; }
        Result.value.bytes.resize(Size);
        DWORD Actual = Size;
        Result.error = RegQueryValueExW(Key, Wide(Name), nullptr, &Type, reinterpret_cast<BYTE*>(Result.value.bytes.data()), &Actual);
        if (Result.error == ERROR_MORE_DATA) continue;
        if (!Result.error) { Result.value.bytes.resize(Actual); Result.value.type = Type; }
        return Result;
    }
    Result.error = ERROR_RETRY; return Result;
}
}
QString RegistryLocation::Path() const
{
    return QString::fromWCharArray(Names.at(size_t(hive))) + (key.isEmpty() ? QString() : "\\" + key);
}
QString RegistryLocation::Id() const
{
    if (hive == Hive::CurrentUser && !sid.isEmpty())
        return QString::number(int(view)) + "::hkey_users\\" + sid.toCaseFolded() + (key.isEmpty() ? QString() : "\\" + key.toCaseFolded());
    return QString::number(int(view)) + ':' + sid.toCaseFolded() + ':' + Path().toCaseFolded();
}
QString RegistryLocation::PhysicalPath() const
{
    if (hive == Hive::CurrentUser && !sid.isEmpty()) return QStringLiteral("HKEY_USERS\\") + sid + (key.isEmpty() ? QString() : "\\" + key);
    return Path();
}
RegistryLocation RegistryLocation::Child(const QString& Name) const
{
    RegistryLocation Result = *this; Result.key += (key.isEmpty() ? QString() : "\\") + Name; return Result;
}
std::optional<RegistryLocation> RegistryLocation::Parent() const
{
    if (key.isEmpty()) return {};
    RegistryLocation Result = *this; const int Separator = key.lastIndexOf('\\');
    Result.key = Separator < 0 ? QString() : key.left(Separator); return Result;
}
std::optional<RegistryLocation> RegistryLocation::Parse(const QString& Path, View SelectedView)
{
    QString Text = Path.trimmed();
    if (Text.startsWith("Computer\\", Qt::CaseInsensitive)) Text.remove(0, 9);
    if (Text.startsWith(QStringLiteral("컴퓨터\\"))) Text.remove(0, 4);
    const int Slash = Text.indexOf('\\'); const QString Prefix = Slash < 0 ? Text : Text.left(Slash);
    for (size_t I = 0; I < Names.size(); ++I) {
        if (Prefix.compare(QString::fromWCharArray(Names[I]), Qt::CaseInsensitive)
            && Prefix.compare(QString::fromWCharArray(ShortNames[I]), Qt::CaseInsensitive)) continue;
        RegistryLocation Result{Hive(I), Slash < 0 ? QString() : Text.mid(Slash + 1), SelectedView};
        if (Result.hive == Hive::ClassesRoot) Result.origin = Origin::Merged;
        if (ValidLocation(Result)) return Result;
    }
    return {};
}
QString RegistryValue::Warning() const
{
    if ((type == REG_DWORD || type == REG_DWORD_BIG_ENDIAN) && bytes.size() != 4) return QStringLiteral("DWORD 데이터 길이가 4바이트가 아닙니다");
    if (type == REG_QWORD && bytes.size() != 8) return QStringLiteral("QWORD 데이터 길이가 8바이트가 아닙니다");
    if (type == REG_SZ || type == REG_EXPAND_SZ || type == REG_MULTI_SZ || type == REG_LINK) {
        if (bytes.size() % 2) return QStringLiteral("UTF-16 데이터 길이가 홀수입니다");
        if (type != REG_LINK && (bytes.size() < 2 || bytes.last(2) != QByteArray(2, '\0')))
            return QStringLiteral("문자열 끝 NUL이 없습니다");
        if (type == REG_MULTI_SZ && (bytes.size() < 4 || bytes.last(4) != QByteArray(4, '\0')))
            return QStringLiteral("다중 문자열 끝 이중 NUL이 없습니다");
    }
    return {};
}
KeyHandle& KeyHandle::operator=(KeyHandle&& Other) noexcept
{
    if (this != &Other) { if (handle_) RegCloseKey(handle_); handle_ = Other.handle_; Other.handle_ = nullptr; } return *this;
}
bool WinRegistryBackend::ValidName(const QString& Name, bool Key)
{
    return !Name.contains(QChar(0)) && Name.size() <= (Key ? 255 : 16383)
        && (!Key || (!Name.isEmpty() && !Name.contains('\\')));
}
RegistryResult<KeyHandle> WinRegistryBackend::Open(const RegistryLocation& Location, REGSAM Access, bool FollowLink)
{
    RegistryResult<KeyHandle> Result;
    if (!ValidLocation(Location)) { Result.error = ERROR_INVALID_PARAMETER; return Result; }
    // Resolve only Windows' verified environment alias, never arbitrary links.
    // All physical ancestors still pass the ordinary no-link traversal below.
    if (!FollowLink && Location.hive == Hive::LocalMachine
        && Location.key.compare(QStringLiteral("SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment"), Qt::CaseInsensitive) == 0) {
        const RegistryLocation Selection{Hive::LocalMachine, QStringLiteral("SYSTEM\\Select"), Location.view};
        auto SelectedKey = Open(Selection, KEY_QUERY_VALUE);
        if (!SelectedKey) { Result.error = SelectedKey.error; return Result; }
        const auto Selected = ReadHandle(SelectedKey.value.Get(), QStringLiteral("Current"));
        if (!Selected) { Result.error = Selected.error; return Result; }
        DWORD Number = 0;
        if (Selected.value.type != REG_DWORD || Selected.value.bytes.size() != sizeof(Number)) { Result.error = ERROR_INVALID_DATA; return Result; }
        memcpy(&Number, Selected.value.bytes.constData(), sizeof(Number));
        if (!Number || Number > 999) { Result.error = ERROR_INVALID_DATA; return Result; }
        const auto ControlSet = QStringLiteral("ControlSet%1").arg(Number, 3, 10, QChar('0'));
        const RegistryLocation Alias{Hive::LocalMachine, QStringLiteral("SYSTEM\\CurrentControlSet"), Location.view};
        auto AliasKey = Open(Alias, KEY_QUERY_VALUE);
        if (!AliasKey) { Result.error = AliasKey.error; return Result; }
        const auto Link = ReadHandle(AliasKey.value.Get(), QStringLiteral("SymbolicLinkValue"));
        const auto Expected = QStringLiteral("\\Registry\\Machine\\SYSTEM\\") + ControlSet;
        if (!Link || Link.value.type != REG_LINK || Link.value.bytes.size() != Expected.size() * 2
            || QString::fromUtf16(reinterpret_cast<const char16_t*>(Link.value.bytes.constData()), Link.value.bytes.size() / 2)
                .compare(Expected, Qt::CaseInsensitive) != 0) { Result.error = ERROR_CANT_ACCESS_FILE; return Result; }
        auto Physical = Location;
        Physical.key = QStringLiteral("SYSTEM\\") + ControlSet + QStringLiteral("\\Control\\Session Manager\\Environment");
        return Open(Physical, Access);
    }
    const auto [Base, Path] = NativePath(Location);
    if (!FollowLink && Path.contains('\\')) {
        // REG_OPTION_OPEN_LINK only protects the final component. Walk ancestors
        // separately so a typed address cannot silently traverse a link and write
        // outside the location shown in the preview or backup manifest.
        const auto Parts = Path.split('\\');
        HKEY Parent = Base; KeyHandle Ancestor;
        for (qsizetype I = 0; I + 1 < Parts.size(); ++I) {
            HKEY Next = nullptr;
            Result.error = RegOpenKeyExW(Parent, Wide(Parts[I]), REG_OPTION_OPEN_LINK,
                KEY_QUERY_VALUE | ViewMask(Location.view), &Next);
            if (Result.error) return Result;
            Ancestor = KeyHandle(Next); Parent = Next;
            const auto Link = ReadHandle(Next, QStringLiteral("SymbolicLinkValue"));
            if (Link && Link.value.type == REG_LINK) { Result.error = ERROR_CANT_ACCESS_FILE; return Result; }
            if (!Link && Link.error != ERROR_FILE_NOT_FOUND) { Result.error = Link.error; return Result; }
        }
        HKEY Handle = nullptr;
        Result.error = RegOpenKeyExW(Parent, Wide(Parts.last()), REG_OPTION_OPEN_LINK, Access | ViewMask(Location.view), &Handle);
        Result.value = KeyHandle(Handle); return Result;
    }
    HKEY Handle = nullptr;
    Result.error = RegOpenKeyExW(Base, Wide(Path), FollowLink ? 0 : REG_OPTION_OPEN_LINK, Access | ViewMask(Location.view), &Handle);
    Result.value = KeyHandle(Handle); return Result;
}
RegistryResult<RegistryValue> WinRegistryBackend::Read(const RegistryLocation& Location, const QString& Name)
{
    if (!ValidName(Name, false)) return {{}, ERROR_INVALID_PARAMETER};
    auto Key = Open(Location, KEY_QUERY_VALUE);
    return Key ? ReadHandle(Key.value.Get(), Name) : RegistryResult<RegistryValue>{{}, Key.error};
}
RegistryResult<KeyInfo> WinRegistryBackend::QueryInfo(const RegistryLocation& Location)
{
    // Cache validation must require the same access as the snapshot it reuses:
    // a changed DACL can revoke enumeration without changing the key timestamp.
    RegistryResult<KeyInfo> Result; auto Key = Open(Location, KEY_QUERY_VALUE | KEY_ENUMERATE_SUB_KEYS);
    if (!Key) { Result.error = Key.error; return Result; }
    FILETIME Last{}; DWORD Subkeys = 0, Values = 0;
    Result.error = RegQueryInfoKeyW(Key.value.Get(), nullptr, nullptr, nullptr, &Subkeys, nullptr, nullptr,
        &Values, nullptr, nullptr, nullptr, &Last);
    Result.value.subkeyCount = Subkeys; Result.value.valueCount = Values;
    Result.value.lastWrite = Time(Last); Result.value.name = Location.key.section('\\', -1);
    return Result;
}
RegistryResult<KeySnapshot> WinRegistryBackend::Enumerate(const RegistryLocation& Location, const std::atomic_bool* Cancel)
{
    RegistryResult<KeySnapshot> Result;
    auto Key = Open(Location, KEY_QUERY_VALUE | KEY_ENUMERATE_SUB_KEYS);
    if (!Key) { Result.error = Key.error; return Result; }
    DWORD Subkeys = 0, Values = 0; FILETIME Last{};
    Result.error = RegQueryInfoKeyW(Key.value.Get(), nullptr, nullptr, nullptr, &Subkeys, nullptr, nullptr, &Values, nullptr, nullptr, nullptr, &Last);
    if (Result.error) return Result;
    Result.value.info = {Location.key.section('\\', -1), Time(Last), Subkeys, Values, false};
    const auto Link = ReadHandle(Key.value.Get(), QStringLiteral("SymbolicLinkValue"));
    Result.value.info.link = Link && Link.value.type == REG_LINK;
    if (Result.value.info.link) { Result.value.values.append(Link.value); return Result; }
    for (DWORD Index = 0;; ++Index) {
        if (Cancel && Cancel->load()) { Result.error = ERROR_CANCELLED; Result.complete = false; return Result; }
        wchar_t Name[256]; DWORD Length = 256; FILETIME Written{};
        const DWORD Error = RegEnumKeyExW(Key.value.Get(), Index, Name, &Length, nullptr, nullptr, nullptr, &Written);
        if (Error == ERROR_NO_MORE_ITEMS) break;
        if (Error) { Result.error = Error; Result.complete = false; return Result; }
        Result.value.keys.append({QString::fromWCharArray(Name, Length), Time(Written)});
    }
    for (DWORD Index = 0;; ++Index) {
        if (Cancel && Cancel->load()) { Result.error = ERROR_CANCELLED; Result.complete = false; return Result; }
        wchar_t Name[16384]; DWORD Length = 16384;
        const DWORD Error = RegEnumValueW(Key.value.Get(), Index, Name, &Length, nullptr, nullptr, nullptr, nullptr);
        if (Error == ERROR_NO_MORE_ITEMS) break;
        if (Error) { Result.error = Error; Result.complete = false; return Result; }
        auto Value = ReadHandle(Key.value.Get(), QString::fromWCharArray(Name, Length));
        if (!Value) { Result.error = Value.error; Result.complete = false; return Result; }
        Result.value.values.append(std::move(Value.value));
    }
    FILETIME After{};
    Result.error = RegQueryInfoKeyW(Key.value.Get(), nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, &After);
    if (!Result.error && Time(After) != Time(Last)) Result.error = ERROR_RETRY;
    Result.complete = Result.error == ERROR_SUCCESS;
    return Result;
}
quint32 WinRegistryBackend::Create(const RegistryLocation& Location)
{
    if (!ValidLocation(Location) || Location.key.isEmpty()) return ERROR_INVALID_PARAMETER;
    const auto Parent = Location.Parent();
    auto Key = Open(*Parent, KEY_CREATE_SUB_KEY | KEY_QUERY_VALUE);
    if (!Key) return Key.error;
    const auto Link = ReadHandle(Key.value.Get(), QStringLiteral("SymbolicLinkValue"));
    if (Link && Link.value.type == REG_LINK) return ERROR_CANT_ACCESS_FILE;
    HKEY Created = nullptr;
    const DWORD Error = RegCreateKeyExW(Key.value.Get(), Wide(Location.key.section('\\', -1)), 0, nullptr, 0,
        KEY_QUERY_VALUE | ViewMask(Location.view), nullptr, &Created, nullptr);
    KeyHandle Cleanup(Created); return Error;
}
quint32 WinRegistryBackend::CreateLink(const RegistryLocation& Location, const QByteArray& Target)
{
    if (!ValidLocation(Location) || Location.key.isEmpty() || Target.isEmpty() || Target.size() % 2 || Target.size() > 32766)
        return ERROR_INVALID_PARAMETER;
    auto Parent = Open(*Location.Parent(), KEY_CREATE_SUB_KEY | KEY_QUERY_VALUE);
    if (!Parent) return Parent.error;
    const auto Link = ReadHandle(Parent.value.Get(), QStringLiteral("SymbolicLinkValue"));
    if (Link && Link.value.type == REG_LINK) return ERROR_CANT_ACCESS_FILE;
    HKEY Created = nullptr; DWORD Disposition = 0;
    const DWORD Error = RegCreateKeyExW(Parent.value.Get(), Wide(Location.key.section('\\', -1)), 0, nullptr, REG_OPTION_CREATE_LINK,
        KEY_QUERY_VALUE | KEY_SET_VALUE | DELETE | ViewMask(Location.view), nullptr, &Created, &Disposition);
    if (Error) return Error;
    KeyHandle Cleanup(Created);
    if (Disposition != REG_CREATED_NEW_KEY) return ERROR_ALREADY_EXISTS;
    const DWORD Written = RegSetValueExW(Created, L"SymbolicLinkValue", 0, REG_LINK,
        reinterpret_cast<const BYTE*>(Target.data()), DWORD(Target.size()));
    if (Written) DeleteLinkHandle(Created);
    return Written;
}
quint32 WinRegistryBackend::Write(const RegistryLocation& Location, const RegistryValue& Value)
{
    if (!ValidName(Value.name, false) || Value.bytes.size() > MaxValueBytes || Value.type == REG_LINK) return ERROR_INVALID_PARAMETER;
    auto Key = Open(Location, KEY_SET_VALUE | KEY_QUERY_VALUE);
    if (!Key) return Key.error;
    const auto Link = ReadHandle(Key.value.Get(), QStringLiteral("SymbolicLinkValue"));
    if (Link && Link.value.type == REG_LINK) return ERROR_CANT_ACCESS_FILE;
    if ((Value.type == REG_SZ || Value.type == REG_EXPAND_SZ || Value.type == REG_MULTI_SZ) && !Value.Warning().isEmpty()) {
        // Win11 RegSetValueExW appended a NUL to the two-byte unterminated
        // "a" fixture in P1. Restore malformed legacy values through the native
        // length-delimited API so undo does not silently repair user data.
        using SetValue = LONG(NTAPI*)(HANDLE, hc::system::nt::UnicodeString*, ULONG, ULONG, PVOID, ULONG);
        using ToError = ULONG(NTAPI*)(LONG);
        const auto Module = GetModuleHandleW(L"ntdll.dll");
        static const auto Set = reinterpret_cast<SetValue>(GetProcAddress(Module, "NtSetValueKey"));
        static const auto Convert = reinterpret_cast<ToError>(GetProcAddress(Module, "RtlNtStatusToDosError"));
        if (!Set || !Convert) return ERROR_CALL_NOT_IMPLEMENTED;
        hc::system::nt::UnicodeString Name{USHORT(Value.name.size() * 2), USHORT(Value.name.size() * 2), const_cast<wchar_t*>(Wide(Value.name))};
        const LONG Status = Set(Key.value.Get(), &Name, 0, Value.type, const_cast<char*>(Value.bytes.constData()), ULONG(Value.bytes.size()));
        return Status >= 0 ? ERROR_SUCCESS : Convert(Status);
    }
    return RegSetValueExW(Key.value.Get(), Wide(Value.name), 0, Value.type,
        reinterpret_cast<const BYTE*>(Value.bytes.constData()), DWORD(Value.bytes.size()));
}
quint32 WinRegistryBackend::DeleteValue(const RegistryLocation& Location, const QString& Name)
{
    if (!ValidName(Name, false)) return ERROR_INVALID_PARAMETER;
    auto Key = Open(Location, KEY_SET_VALUE | KEY_QUERY_VALUE);
    if (!Key) return Key.error;
    const auto Link = ReadHandle(Key.value.Get(), QStringLiteral("SymbolicLinkValue"));
    if (Link && Link.value.type == REG_LINK) return ERROR_CANT_ACCESS_FILE;
    return RegDeleteValueW(Key.value.Get(), Wide(Name));
}
quint32 WinRegistryBackend::DeleteKey(const RegistryLocation& Location)
{
    if (!ValidLocation(Location) || Location.key.isEmpty()) return ERROR_INVALID_PARAMETER;
    auto Target = Open(Location, DELETE | KEY_QUERY_VALUE);
    if (!Target) return Target.error;
    const auto Link = ReadHandle(Target.value.Get(), QStringLiteral("SymbolicLinkValue"));
    if (Link && Link.value.type == REG_LINK) {
        // RegDeleteKeyExW follows a symbolic link (a self-cycle returned
        // ERROR_FILE_NOT_FOUND in the VM). Delete the OPEN_LINK handle itself.
        return DeleteLinkHandle(Target.value.Get());
    }
    const auto Parent = Location.Parent(); auto Key = Open(*Parent, DELETE | KEY_ENUMERATE_SUB_KEYS);
    if (!Key) return Key.error;
    return RegDeleteKeyExW(Key.value.Get(), Wide(Location.key.section('\\', -1)), ViewMask(Location.view), 0);
}
quint32 WinRegistryBackend::RenameKey(const RegistryLocation& Location, const QString& Name)
{
    if (!ValidLocation(Location) || Location.key.isEmpty() || !ValidName(Name, true)) return ERROR_INVALID_PARAMETER;
    using Rename = LSTATUS(WINAPI*)(HKEY, LPCWSTR, LPCWSTR);
    static const auto Function = reinterpret_cast<Rename>(GetProcAddress(GetModuleHandleW(L"advapi32.dll"), "RegRenameKey"));
    if (!Function) return ERROR_CALL_NOT_IMPLEMENTED;
    auto Key = Open(Location, KEY_READ | KEY_WRITE | DELETE);
    if (!Key) return Key.error;
    return Function(Key.value.Get(), nullptr, Wide(Name));
}
} // namespace hc::registry
