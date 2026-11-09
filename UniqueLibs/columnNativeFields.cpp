#include "stdafx.h"
#include "columnNativeFields.hpp"
#include "UniqueLibs/nativeFilePath.hpp"
#include "UniqueLibs/productDefaults.hpp"
#include <QBuffer>
#include <QFile>
#include <QImageReader>
#include <QScopeGuard>
#include <QtEndian>
#include <Windows.h>
#include <winver.h>
#pragma comment(lib, "version.lib")

namespace hc::columns
{
    namespace
    {
        // ZoneId from the Mark-of-the-Web stream. The raw zone sorts; the display names it.
        CellValue ZoneCell(const std::wstring& Native)
        {
            const auto Stream = Native + L":Zone.Identifier";
            const auto Handle = CreateFileW(Stream.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
            if (Handle == INVALID_HANDLE_VALUE) {
                const auto Error = GetLastError();
                if (Error == ERROR_FILE_NOT_FOUND || Error == ERROR_PATH_NOT_FOUND) return CellValue::Missing(ValueState::Empty);
                return CellValue::Missing(Error == ERROR_INVALID_PARAMETER || Error == ERROR_INVALID_FUNCTION || Error == ERROR_NOT_SUPPORTED
                    ? ValueState::Unsupported : ValueState::Error, QString::number(Error));
            }
            const auto Close = qScopeGuard([Handle] { CloseHandle(Handle); });
            // The marker is a few lines; a larger stream is not read past this budget.
            QByteArray Bytes(4096, '\0'); DWORD Read = 0;
            if (!ReadFile(Handle, Bytes.data(), DWORD(Bytes.size()), &Read, nullptr)) return CellValue::Missing(ValueState::Error, QString::number(GetLastError()));
            Bytes.truncate(int(Read));
            // Browsers write ANSI text; a few tools write UTF-16 with a byte-order mark.
            const auto Text = Bytes.startsWith("\xFF\xFE")
                ? QString::fromUtf16(reinterpret_cast<const char16_t*>(Bytes.constData() + 2), (Bytes.size() - 2) / 2) : QString::fromUtf8(Bytes);
            for (const auto& Line : Text.split('\n')) {
                const auto Equal = Line.indexOf('=');
                if (Equal < 0 || Line.left(Equal).trimmed().compare("ZoneId", Qt::CaseInsensitive)) continue;
                bool Valid = false; const auto Zone = Line.mid(Equal + 1).trimmed().toUInt(&Valid);
                if (!Valid) break;
                const auto Names = hc::defaults::columnPolicy::ZoneNames();
                return CellValue::Ready(quint64(Zone)).WithDisplay(Zone < uint(Names.size()) ? QString("%1 (%2)").arg(Names[int(Zone)]).arg(Zone) : QString::number(Zone));
            }
            return CellValue::Missing(ValueState::Error, "zone-id-missing");
        }
    }

    FieldValues ReadNativeFields(const QString& Path, const QString& Family, std::stop_token Stop)
    {
        FieldValues Result;
        const auto Missing = [&](const QStringList& Keys, ValueState State, const QString& Error = {}) {
            for (const auto& Key : Keys) Result[Key] = CellValue::Missing(State, Error);
            return Result;
        };
        if (Stop.stop_requested()) return Result;
        const auto Native = hc::storage::native::FilePath(Path).toStdWString();
        // Zone-only requests need one bounded stream read, never an enumeration of all ADS.
        if (Family == "zone") return {{"file.alternateDataStream.zone", ZoneCell(Native)}};
        if (Family == "standard") {
            const QStringList Keys{"file.allocatedSize", "file.hardlink.count"};
            // Follows a link, like the size column: the target's allocation and its link count.
            const auto Handle = CreateFileW(Native.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
            if (Handle == INVALID_HANDLE_VALUE) return Missing(Keys, ValueState::Error, QString::number(GetLastError()));
            const auto Close = qScopeGuard([Handle] { CloseHandle(Handle); });
            // One call carries both values; nsCmnPath::GetHardLinkCount would query the same count again.
            FILE_STANDARD_INFO Info{};
            if (!GetFileInformationByHandleEx(Handle, FileStandardInfo, &Info, sizeof(Info)) || Info.AllocationSize.QuadPart < 0)
                return Missing(Keys, ValueState::Error, QString::number(GetLastError()));
            Result[Keys[0]] = CellValue::Ready(quint64(Info.AllocationSize.QuadPart));
            Result[Keys[1]] = CellValue::Ready(quint64(Info.NumberOfLinks));
            return Result;
        }
        if (Family == "link") {
            const QStringList Keys{"file.linkTarget", "file.softlink.target", "file.junction.target"};
            const auto Attributes = GetFileAttributesW(Native.c_str());
            if (Attributes == INVALID_FILE_ATTRIBUTES) return Missing(Keys, ValueState::Error, QString::number(GetLastError()));
            if (!(Attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return Missing(Keys, ValueState::Empty);
            // Opens the reparse point itself: the stored target is read, never followed,
            // so a dangling link still shows where it points.
            const auto Handle = CreateFileW(Native.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
            if (Handle == INVALID_HANDLE_VALUE) return Missing(Keys, ValueState::Error, QString::number(GetLastError()));
            const auto Close = qScopeGuard([Handle] { CloseHandle(Handle); });
            // Cloud placeholders, dedup and similar tags are not links. The tag alone decides,
            // so their filters are never asked for reparse data.
            FILE_ATTRIBUTE_TAG_INFO Tag{};
            if (!GetFileInformationByHandleEx(Handle, FileAttributeTagInfo, &Tag, sizeof(Tag)))
                return Missing(Keys, ValueState::Error, QString::number(GetLastError()));
            if (Tag.ReparseTag != IO_REPARSE_TAG_SYMLINK && Tag.ReparseTag != IO_REPARSE_TAG_MOUNT_POINT) return Missing(Keys, ValueState::Empty);
            const auto Link = nsCmn::nsCmnPath::ReadReparseLink(Handle);
            if (!Link.Value) return Missing(Keys, ValueState::Error, QString::number(Link.ErrorCode.ErrorCode));
            using Kind = nsCmn::nsCmnPath::TyReparseLink;
            const auto Target = QString::fromStdWString(Link.Value->Target);
            const auto Cell = [&Target](bool Present) { return Present && !Target.isEmpty() ? CellValue::Ready(Target) : CellValue::Missing(ValueState::Empty); };
            Result[Keys[0]] = Cell(Link.Value->Kind != Kind::REPARSE_LINK_OTHER);
            Result[Keys[1]] = Cell(Link.Value->Kind == Kind::REPARSE_LINK_SYMLINK);
            Result[Keys[2]] = Cell(Link.Value->Kind == Kind::REPARSE_LINK_JUNCTION);
            return Result;
        }
        if (Family == "streams") {
            const QStringList Keys{"file.alternateDataStream.count", "file.alternateDataStream.names",
                "file.alternateDataStream.totalSize"};
            QStringList Names; quint64 Count = 0, Bytes = 0;
            WIN32_FIND_STREAM_DATA Data{};
            const auto Find = FindFirstStreamW(Native.c_str(), FindStreamInfoStandard, &Data, 0);
            if (Find == INVALID_HANDLE_VALUE) {
                // A folder without named streams has no stream at all. FAT, exFAT and some
                // network file systems have no streams to enumerate.
                const auto Error = GetLastError();
                if (Error != ERROR_HANDLE_EOF) return Missing(Keys, Error == ERROR_INVALID_PARAMETER || Error == ERROR_INVALID_FUNCTION ||
                    Error == ERROR_NOT_SUPPORTED ? ValueState::Unsupported : ValueState::Error, QString::number(Error));
            } else {
                const auto Close = qScopeGuard([Find] { FindClose(Find); });
                do {
                    if (Stop.stop_requested()) return Result;
                    // ":name:$DATA". The unnamed "::$DATA" stream is the file body, not an ADS.
                    auto Name = QString::fromWCharArray(Data.cStreamName);
                    if (!Name.compare("::$DATA", Qt::CaseInsensitive)) continue;
                    if (Name.endsWith(":$DATA", Qt::CaseInsensitive)) Name.chop(6);
                    if (Name.startsWith(':')) Name.remove(0, 1);
                    ++Count; Bytes += quint64(std::max<LONGLONG>(0, Data.StreamSize.QuadPart));
                    if (Names.size() < 64) Names.append(Name);
                } while (FindNextStreamW(Find, &Data));
                if (const auto Error = GetLastError(); Error != ERROR_HANDLE_EOF) return Missing(Keys, ValueState::Error, QString::number(Error));
            }
            Result[Keys[0]] = CellValue::Ready(Count);
            Result[Keys[1]] = Names.isEmpty() ? CellValue::Missing(ValueState::Empty)
                : CellValue::Ready(Names.join(", ") + (Count > quint64(Names.size()) ? QStringLiteral(", …") : QString{}));
            Result[Keys[2]] = CellValue::Ready(Bytes);
            return Result;
        }
        if (Family == "version") {
            const QStringList Keys{"file.fileVersion", "file.productVersion", "file.productName", "file.company", "file.description"};
            const QStringList Names{"FileVersion", "ProductVersion", "ProductName", "CompanyName", "FileDescription"};
            DWORD Ignored = 0; const auto Size = GetFileVersionInfoSizeW(Native.c_str(), &Ignored);
            if (!Size) { const auto Error = GetLastError(); return Missing(Keys, Error == ERROR_RESOURCE_DATA_NOT_FOUND || Error == ERROR_RESOURCE_TYPE_NOT_FOUND ? ValueState::Empty : ValueState::Error, QString::number(Error)); }
            if (Size > 1024 * 1024) return Missing(Keys, ValueState::Error, "version-resource-budget");
            if (Stop.stop_requested()) return Result;
            QByteArray Data(int(Size), '\0');
            if (!GetFileVersionInfoW(Native.c_str(), 0, Size, Data.data())) return Missing(Keys, ValueState::Error, QString::number(GetLastError()));
            struct Language { WORD language_, codepage_; }; Language* Languages = nullptr; UINT Bytes = 0;
            VerQueryValueW(Data.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<void**>(&Languages), &Bytes);
            for (int Index = 0; Index < Keys.size(); ++Index) {
                Result[Keys[Index]] = CellValue::Missing(ValueState::Empty);
                for (UINT Entry = 0; Entry < Bytes / sizeof(Language); ++Entry) {
                    const auto Sub = QString("\\StringFileInfo\\%1%2\\%3").arg(Languages[Entry].language_, 4, 16, QChar('0')).arg(Languages[Entry].codepage_, 4, 16, QChar('0')).arg(Names[Index]).toStdWString();
                    wchar_t* Value = nullptr; UINT Length = 0;
                    if (VerQueryValueW(Data.data(), Sub.c_str(), reinterpret_cast<void**>(&Value), &Length) && Length > 1) {
                        Result[Keys[Index]] = CellValue::Ready(QString::fromWCharArray(Value, int(Length) - 1)); break;
                    }
                }
            }
            VS_FIXEDFILEINFO* Fixed = nullptr; UINT FixedBytes = 0;
            if (VerQueryValueW(Data.data(), L"\\", reinterpret_cast<void**>(&Fixed), &FixedBytes) &&
                FixedBytes >= sizeof(VS_FIXEDFILEINFO) && Fixed->dwSignature == 0xfeef04bd) {
                const auto Version = [](DWORD High, DWORD Low) {
                    return QString("%1.%2.%3.%4").arg(HIWORD(High)).arg(LOWORD(High)).arg(HIWORD(Low)).arg(LOWORD(Low));
                };
                if (Result[Keys[0]].State() == ValueState::Empty) Result[Keys[0]] = CellValue::Ready(Version(Fixed->dwFileVersionMS, Fixed->dwFileVersionLS));
                if (Result[Keys[1]].State() == ValueState::Empty) Result[Keys[1]] = CellValue::Ready(Version(Fixed->dwProductVersionMS, Fixed->dwProductVersionLS));
            }
            return Result;
        }
        if (Family == "image") {
            const QStringList Keys{"file.imageWidth", "file.imageHeight", "file.imageDimensions", "file.imageBitDepth"};
            // Header-only metadata: never decompress a bitmap on behalf of a column.
            QFile File(Path); if (!File.open(QIODevice::ReadOnly)) return Missing(Keys, ValueState::Error, File.errorString());
            auto Data = File.read(256 * 1024); if (Stop.stop_requested()) return Result;
            QBuffer Buffer(&Data); Buffer.open(QIODevice::ReadOnly); QImageReader Reader(&Buffer);
            const auto Size = Reader.size();
            if (!Size.isValid()) return Missing(Keys, ValueState::Unsupported, "image-header-unsupported-or-budget");
            Result[Keys[0]] = CellValue::Ready(quint64(Size.width())); Result[Keys[1]] = CellValue::Ready(quint64(Size.height()));
            Result[Keys[2]] = CellValue::Ready(QString::number(Size.width()) + " × " + QString::number(Size.height()));
            Result[Keys[3]] = CellValue::Missing(ValueState::Unsupported);
            if (Data.size() >= 26 && Data.startsWith(QByteArray::fromHex("89504e470d0a1a0a"))) {
                const auto Color = quint8(Data[25]); const int Channels = Color == 2 ? 3 : Color == 4 ? 2 : Color == 6 ? 4 : 1;
                Result[Keys[3]] = CellValue::Ready(quint64(quint8(Data[24]) * Channels));
            } else if (Data.size() >= 30 && Data.startsWith("BM")) Result[Keys[3]] = CellValue::Ready(quint64(qFromLittleEndian<quint16>(Data.constData() + 28)));
            else if (Data.size() >= 11 && Data.startsWith("GIF8")) Result[Keys[3]] = CellValue::Ready(quint64((quint8(Data[10]) & 7) + 1));
            // Other decoder formats still provide dimensions; depth is explicitly unsupported.
            return Result;
        }
        return Result;
    }
}
