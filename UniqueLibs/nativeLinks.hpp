#pragma once
#include "fileBatchOperations.hpp"
#include <winioctl.h>
#include <winternl.h>

namespace hc::links {
enum class Kind { Hard, Symbolic, Junction };
inline QString KindName(Kind Value) {
    return Value == Kind::Hard ? QObject::tr("하드링크") : Value == Kind::Junction ? QObject::tr("정션") : QObject::tr("소프트링크");
}
inline QString CanonicalParent(const QString& Path) {
    return QDir::cleanPath(QDir::fromNativeSeparators(Path));
}
// The same validation is used by the preview, worker and authenticated admin helper.
inline DWORD Validate(const QString& Source, const QString& Destination, Kind Type, bool Relative) {
    if (!filetools::NativeFolder(Source) || !filetools::NativeFolder(Destination)
        || !filetools::NameError(QFileInfo(Destination).fileName()).isEmpty()) return ERROR_INVALID_NAME;
    const auto Native = filetools::NativePath(Source);
    const auto Attributes = GetFileAttributesW(reinterpret_cast<LPCWSTR>(Native.utf16()));
    if (Attributes == INVALID_FILE_ATTRIBUTES) return GetLastError();
    if (Attributes & FILE_ATTRIBUTE_REPARSE_POINT) return ERROR_NOT_SUPPORTED;
    const bool Directory = Attributes & FILE_ATTRIBUTE_DIRECTORY;
    if ((Type == Kind::Hard && Directory) || (Type == Kind::Junction && !Directory)) return ERROR_NOT_SUPPORTED;
    const auto From = CanonicalParent(Source), To = CanonicalParent(Destination);
    if (From.compare(To, Qt::CaseInsensitive) == 0
        || (Directory && To.startsWith(From.endsWith('/') ? From : From + '/', Qt::CaseInsensitive))) return ERROR_INVALID_PARAMETER;
    if (Type == Kind::Junction && (!filetools::LocalFolder(Source) || !filetools::LocalFolder(Destination))) return ERROR_NOT_SUPPORTED;
    const auto Parent = QFileInfo(To).absolutePath();
    if (Relative && QDir::isAbsolutePath(QDir(Parent).relativeFilePath(From))) return ERROR_NOT_SAME_DEVICE;
    if (Type == Kind::Hard) {
        // Compare real volume roots, not drive letters (mount points can change them).
        QString Existing = Parent;
        while (!QFileInfo::exists(Existing)) {
            const auto Up = QFileInfo(Existing).absolutePath();
            if (Up == Existing) return ERROR_PATH_NOT_FOUND;
            Existing = Up;
        }
        wchar_t SourceRoot[32768]{}, TargetRoot[32768]{}, SourceVolume[32768]{}, TargetVolume[32768]{};
        const auto Target = filetools::NativePath(Existing);
        if (!GetVolumePathNameW(reinterpret_cast<LPCWSTR>(Native.utf16()), SourceRoot, 32768)
            || !GetVolumePathNameW(reinterpret_cast<LPCWSTR>(Target.utf16()), TargetRoot, 32768)) return GetLastError();
        if (GetVolumeNameForVolumeMountPointW(SourceRoot, SourceVolume, 32768)
            && GetVolumeNameForVolumeMountPointW(TargetRoot, TargetVolume, 32768)) {
            if (_wcsicmp(SourceVolume, TargetVolume)) return ERROR_NOT_SAME_DEVICE;
        } else if (_wcsicmp(SourceRoot, TargetRoot)) return ERROR_NOT_SAME_DEVICE;
    }
    return ERROR_SUCCESS;
}
// Never replace an existing entry. Hold the source and parent identities during creation.
inline DWORD Create(const QString& Source, const QString& Destination, Kind Type, bool Relative = false) {
    if (const auto Code = Validate(Source, Destination, Type, Relative)) return Code;
    std::vector<std::unique_ptr<filetools::Handle>> Pins;
    if (!filetools::PinNativeParents(QFileInfo(Source).absolutePath(), Pins).isEmpty()
        || !filetools::PinNativeParents(QFileInfo(Destination).absolutePath(), Pins).isEmpty()) return ERROR_ACCESS_DENIED;
    const auto From = filetools::NativePath(Source), To = filetools::NativePath(Destination);
    filetools::Handle SourceLock(CreateFileW(reinterpret_cast<LPCWSTR>(From.utf16()), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (SourceLock.value_ == INVALID_HANDLE_VALUE) return GetLastError();
    BY_HANDLE_FILE_INFORMATION Info{};
    if (!GetFileInformationByHandle(SourceLock.value_, &Info)) return GetLastError();
    if (Info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) return ERROR_NOT_SUPPORTED;
    if (Type == Kind::Hard) return CreateHardLinkW(reinterpret_cast<LPCWSTR>(To.utf16()), reinterpret_cast<LPCWSTR>(From.utf16()), nullptr) ? 0 : GetLastError();
    if (Type == Kind::Symbolic) {
        const auto Reference = Relative ? QDir::toNativeSeparators(QDir(QFileInfo(Destination).absolutePath()).relativeFilePath(Source)) : From;
        DWORD Flags = (Info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY ? SYMBOLIC_LINK_FLAG_DIRECTORY : 0) | SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE;
        if (CreateSymbolicLinkW(reinterpret_cast<LPCWSTR>(To.utf16()), reinterpret_cast<LPCWSTR>(Reference.utf16()), Flags)) return 0;
        const auto Code = GetLastError();
        if (Code != ERROR_INVALID_PARAMETER) return Code;
        Flags &= ~SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE;
        return CreateSymbolicLinkW(reinterpret_cast<LPCWSTR>(To.utf16()), reinterpret_cast<LPCWSTR>(Reference.utf16()), Flags) ? 0 : GetLastError();
    }
    if (!(Info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return ERROR_DIRECTORY;
    // Create and acquire the directory handle atomically. CreateDirectory + Open
    // would allow replacement of the leaf between the two calls, particularly after UAC.
    using NativeCreate = NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK,
        PLARGE_INTEGER, ULONG, ULONG, ULONG, ULONG, PVOID, ULONG);
    using NativeError = ULONG(NTAPI*)(NTSTATUS);
    static const auto Open = reinterpret_cast<NativeCreate>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtCreateFile"));
    static const auto ToError = reinterpret_cast<NativeError>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlNtStatusToDosError"));
    if (!Open || !ToError || Pins.empty()) return ERROR_NOT_SUPPORTED;
    auto Leaf = QFileInfo(Destination).fileName().toStdWString();
    UNICODE_STRING Name{}; Name.Buffer = Leaf.data(); Name.Length = Name.MaximumLength = USHORT(Leaf.size() * sizeof(wchar_t));
    OBJECT_ATTRIBUTES Attributes{}; Attributes.Length = sizeof(Attributes); Attributes.RootDirectory = Pins.back()->value_;
    Attributes.ObjectName = &Name; Attributes.Attributes = 0x40; // OBJ_CASE_INSENSITIVE
    IO_STATUS_BLOCK Status{}; HANDLE Created = INVALID_HANDLE_VALUE;
    const auto Opened = Open(&Created, GENERIC_WRITE | DELETE | SYNCHRONIZE, &Attributes, &Status, nullptr,
        FILE_ATTRIBUTE_DIRECTORY, 0, 2, 0x1 | 0x20 | 0x200000, nullptr, 0); // CREATE, DIRECTORY, SYNCHRONOUS, OPEN_REPARSE
    if (Opened < 0) return ToError(Opened);
    filetools::Handle Link(Created);
    const auto Print = QDir::toNativeSeparators(QDir::cleanPath(Source));
    const auto Substitute = QStringLiteral("\\??\\") + Print;
    const auto SubBytes = Substitute.size() * 2, PrintBytes = Print.size() * 2;
    const auto DataLength = 8 + SubBytes + 2 + PrintBytes + 2;
    DWORD Code = ERROR_SUCCESS;
    if (DataLength + 8 > MAXIMUM_REPARSE_DATA_BUFFER_SIZE) Code = ERROR_FILENAME_EXCED_RANGE;
    else {
        QByteArray Data(int(DataLength + 8), '\0');
        const DWORD Tag = IO_REPARSE_TAG_MOUNT_POINT;
        const WORD Header[] = {WORD(DataLength), 0, 0, WORD(SubBytes), WORD(SubBytes + 2), WORD(PrintBytes)};
        memcpy(Data.data(), &Tag, 4); memcpy(Data.data() + 4, Header, sizeof(Header));
        memcpy(Data.data() + 16, Substitute.utf16(), SubBytes);
        memcpy(Data.data() + 16 + SubBytes + 2, Print.utf16(), PrintBytes);
        DWORD Returned = 0;
        if (!DeviceIoControl(Link.value_, FSCTL_SET_REPARSE_POINT, Data.data(), DWORD(Data.size()), nullptr, 0, &Returned, nullptr)) Code = GetLastError();
    }
    if (Code) { FILE_DISPOSITION_INFO Remove{TRUE}; SetFileInformationByHandle(Link.value_, FileDispositionInfo, &Remove, sizeof(Remove)); }
    return Code;
}
}
