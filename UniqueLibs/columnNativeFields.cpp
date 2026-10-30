#include "stdafx.h"
#include "columnNativeFields.hpp"
#include "UniqueLibs/nativeFilePath.hpp"
#include <QBuffer>
#include <QFile>
#include <QImageReader>
#include <QtEndian>
#include <Windows.h>
#include <winver.h>
#pragma comment(lib, "version.lib")

namespace hc::columns
{
    FieldValues ReadNativeFields(const QString& Path, const QString& Family, std::stop_token Stop)
    {
        FieldValues Result;
        const auto Missing = [&](const QStringList& Keys, ValueState State, const QString& Error = {}) {
            for (const auto& Key : Keys) Result[Key] = CellValue::Missing(State, Error);
            return Result;
        };
        if (Stop.stop_requested()) return Result;
        const auto Native = hc::storage::native::FilePath(Path).toStdWString();
        if (Family == "allocation" || Family == "link") {
            const QString Key = Family == "allocation" ? "file.allocatedSize" : "file.linkTarget";
            const auto Attributes = GetFileAttributesW(Native.c_str());
            if (Attributes == INVALID_FILE_ATTRIBUTES) return Missing({Key}, ValueState::Error, QString::number(GetLastError()));
            if (Family == "link" && !(Attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return Missing({Key}, ValueState::Empty);
            const auto Handle = CreateFileW(Native.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
            if (Handle == INVALID_HANDLE_VALUE) return Missing({Key}, ValueState::Error, QString::number(GetLastError()));
            if (Family == "allocation") {
                FILE_STANDARD_INFO Info{};
                if (GetFileInformationByHandleEx(Handle, FileStandardInfo, &Info, sizeof(Info)) && Info.AllocationSize.QuadPart >= 0)
                    Result[Key] = CellValue::Ready(quint64(Info.AllocationSize.QuadPart));
                else Missing({Key}, ValueState::Error, QString::number(GetLastError()));
            } else {
                std::wstring Buffer(32768, L'\0');
                const auto Length = GetFinalPathNameByHandleW(Handle, Buffer.data(), DWORD(Buffer.size()), FILE_NAME_NORMALIZED);
                if (Length && Length < Buffer.size()) {
                    auto Target = QString::fromWCharArray(Buffer.data(), int(Length));
                    if (Target.startsWith("\\\\?\\UNC\\")) Target = "\\\\" + Target.mid(8);
                    else if (Target.startsWith("\\\\?\\")) Target = Target.mid(4);
                    Result[Key] = CellValue::Ready(Target);
                } else Missing({Key}, ValueState::Error, QString::number(GetLastError()));
            }
            CloseHandle(Handle); return Result;
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
