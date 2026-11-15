#include "stdafx.h"
#include "registryFile.hpp"

/// 모듈
/// UIs
/// 기타 헤더파일
/// 고유 라이브러리
/// 공통 라이브러리
/// 외부 라이브러리
/// 선언 헤더파일들
/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QLoggingCategory>
#include <QStringConverter>

DECLARE_CMNLIBSV2_NAMESPACE
Q_LOGGING_CATEGORY(lcRegistryFile, "hc.registry.file")

namespace hc::registry {
namespace {
bool Quoted(QStringView Text, QString& Output, qsizetype& Consumed)
{
    if (Text.isEmpty() || Text[0] != '"') return false;
    Output.clear();
    for (qsizetype I = 1; I < Text.size(); ++I) {
        auto C = Text[I];
        if (C == '"') { Consumed = I + 1; return true; }
        if (C == '\\') {
            if (++I == Text.size() || (Text[I] != '\\' && Text[I] != '"')) return false;
            C = Text[I];
        }
        Output += C;
    }
    return false;
}
QString Quote(QString Text)
{
    Text.replace("\\", "\\\\"); Text.replace("\"", "\\\""); return '"' + Text + '"';
}
QByteArray Utf16(const QString& Text, bool Terminate)
{
    QByteArray Result;
    Result.reserve((Text.size() + int(Terminate)) * 2);
    for (const QChar C : Text) { Result += char(C.unicode() & 255); Result += char(C.unicode() >> 8); }
    if (Terminate) Result += QByteArray(2, '\0');
    return Result;
}
QString FromUtf16(const QByteArray& Bytes)
{
    QString Result; Result.reserve(Bytes.size() / 2);
    for (qsizetype I = 0; I + 1 < Bytes.size(); I += 2) Result += QChar(quint8(Bytes[I]) | (quint16(quint8(Bytes[I + 1])) << 8));
    return Result;
}
bool HexNumber(QStringView Text, quint32& Value, int MaxDigits = 8)
{
    if (Text.isEmpty() || Text.size() > MaxDigits) return false;
    Value = 0;
    for (QChar C : Text) {
        const ushort U = C.toLower().unicode();
        const int Digit = U >= '0' && U <= '9' ? U - '0' : U >= 'a' && U <= 'f' ? U - 'a' + 10 : -1;
        if (Digit < 0) return false;
        Value = (Value << 4) | quint32(Digit);
    }
    return true;
}
}
RegFileResult RegFileParser::Parse(const QByteArray& Bytes, View SelectedView, RegEncoding Encoding)
{
    RegFileResult Result;
    const auto Fail = [&Result](const QString& Error, int Line = 0) {
        Result.error = Error; Result.line = Line; Result.operations.clear(); return Result;
    };
    if (Bytes.size() > MaxFileBytes) return Fail(QStringLiteral("파일 크기 제한을 넘었습니다"));
    QString Text;
    if (Bytes.startsWith(QByteArray::fromHex("fffe"))) {
        if (Bytes.size() % 2) return Fail(QStringLiteral("UTF-16 파일 길이가 홀수입니다"));
        Text = FromUtf16(Bytes.sliced(2));
    } else if (Bytes.startsWith(QByteArray::fromHex("feff"))) {
        return Fail(QStringLiteral("UTF-16BE는 지원하지 않습니다"));
    } else if (Bytes.startsWith(QByteArray::fromHex("efbbbf")) || Encoding == RegEncoding::Utf8) {
        QStringDecoder Decoder(QStringDecoder::Utf8);
        Text = Decoder(Bytes.startsWith(QByteArray::fromHex("efbbbf")) ? Bytes.sliced(3) : Bytes);
        if (Decoder.hasError()) return Fail(QStringLiteral("잘못된 UTF-8 데이터입니다"));
    } else if (Encoding == RegEncoding::Ansi || Bytes.startsWith("REGEDIT4")) {
        const int Size = MultiByteToWideChar(CP_ACP, MB_ERR_INVALID_CHARS, Bytes.data(), int(Bytes.size()), nullptr, 0);
        if (!Size) return Fail(QStringLiteral("ANSI 코드 페이지로 읽을 수 없습니다"));
        Text.resize(Size);
        MultiByteToWideChar(CP_ACP, MB_ERR_INVALID_CHARS, Bytes.data(), int(Bytes.size()), reinterpret_cast<wchar_t*>(Text.data()), Size);
    } else {
        Result.encodingConfirmation = true; return Result;
    }
    if (Text.contains(QChar(0))) return Fail(QStringLiteral("파일 본문에 NUL이 있습니다"));
    Text.replace("\r\n", "\n");
    if (Text.contains('\r')) return Fail(QStringLiteral("잘못된 줄바꿈입니다"));
    const auto Lines = Text.split('\n');
    if (Lines.isEmpty() || (Lines[0].trimmed() != "Windows Registry Editor Version 5.00" && Lines[0].trimmed() != "REGEDIT4"))
        return Fail(QStringLiteral("레지스트리 파일 머리글이 없습니다"), 1);
    std::optional<RegistryLocation> Current;
    const bool Legacy = Lines[0].trimmed() == "REGEDIT4";
    for (qsizetype I = 1; I < Lines.size(); ++I) {
        const int LineNumber = int(I + 1);
        QString Line = Lines[I].trimmed();
        if (Line.isEmpty() || Line.startsWith(';') || Line.startsWith('#')) continue;
        if (Result.operations.size() >= MaxOperations) return Fail(QStringLiteral("작업 수 제한을 넘었습니다"), LineNumber);
        if (Line.startsWith('[')) {
            if (!Line.endsWith(']')) return Fail(QStringLiteral("키 경로의 닫는 괄호가 없습니다"), LineNumber);
            QString Path = Line.mid(1, Line.size() - 2); const bool Delete = Path.startsWith('-');
            if (Delete) Path.remove(0, 1);
            Current = RegistryLocation::Parse(Path, SelectedView);
            if (!Current || Current->key.count('\\') > 512) return Fail(QStringLiteral("잘못된 키 경로입니다"), LineNumber);
            if (Delete && Current->key.isEmpty()) return Fail(QStringLiteral("루트 하이브는 삭제할 수 없습니다"), LineNumber);
            Result.operations.append({Delete ? OperationKind::DeleteKey : OperationKind::CreateKey, *Current, {}, LineNumber});
            if (Delete) Current.reset();
            continue;
        }
        if (!Current) return Fail(QStringLiteral("값 앞에 키 경로가 필요합니다"), LineNumber);
        QString Name; qsizetype Used = 0;
        if (Line.startsWith('@')) Used = 1;
        else if (!Quoted(Line, Name, Used)) return Fail(QStringLiteral("잘못된 값 이름입니다"), LineNumber);
        if (!WinRegistryBackend::ValidName(Name, false)) return Fail(QStringLiteral("값 이름 제한을 넘었습니다"), LineNumber);
        QString Data = Line.mid(Used).trimmed();
        if (!Data.startsWith('=')) return Fail(QStringLiteral("값 이름 뒤에 =가 필요합니다"), LineNumber);
        Data = Data.mid(1).trimmed();
        RegistryOperation Operation{OperationKind::SetValue, *Current, {Name}, LineNumber};
        if (Data == "-") Operation.kind = OperationKind::DeleteValue;
        else if (Data.startsWith('"')) {
            QString String;
            if (!Quoted(Data, String, Used) || !Data.mid(Used).trimmed().isEmpty()) return Fail(QStringLiteral("잘못된 문자열입니다"), LineNumber);
            Operation.value.type = REG_SZ; Operation.value.bytes = Utf16(String, true);
        } else if (Data.startsWith("dword:", Qt::CaseInsensitive)) {
            quint32 Value = 0;
            if (!HexNumber(QStringView(Data).sliced(6), Value)) return Fail(QStringLiteral("잘못된 DWORD입니다"), LineNumber);
            Operation.value.type = REG_DWORD;
            for (int Byte = 0; Byte < 4; ++Byte) Operation.value.bytes += char(Value >> (Byte * 8));
        } else {
            const int Colon = Data.indexOf(':'); const QString Prefix = Data.left(Colon).toLower();
            if (Colon < 0) return Fail(QStringLiteral("값 형식을 읽을 수 없습니다"), LineNumber);
            if (Prefix == "hex") Operation.value.type = REG_BINARY;
            else if (!Prefix.startsWith("hex(") || !Prefix.endsWith(')')
                || !HexNumber(QStringView(Prefix).sliced(4, Prefix.size() - 5), Operation.value.type))
                return Fail(QStringLiteral("알 수 없는 값 표기입니다"), LineNumber);
            Data = Data.mid(Colon + 1).trimmed();
            while (Data.endsWith('\\')) {
                Data.chop(1);
                if (++I == Lines.size()) return Fail(QStringLiteral("연속 줄이 끝나지 않았습니다"), LineNumber);
                Data += Lines[I].trimmed();
                if (Data.size() > WinRegistryBackend::MaxValueBytes * 3) return Fail(QStringLiteral("값 크기 제한을 넘었습니다"), LineNumber);
            }
            if (!Data.isEmpty()) {
                const auto BytesText = Data.split(',');
                for (const QString& ByteText : BytesText) {
                    quint32 Value = 0;
                    if (!HexNumber(QStringView(ByteText).trimmed(), Value, 2)) return Fail(QStringLiteral("잘못된 16진 바이트입니다"), LineNumber);
                    Operation.value.bytes += char(Value);
                }
            }
            if (Legacy && !Operation.value.bytes.isEmpty() && (Operation.value.type == REG_SZ
                || Operation.value.type == REG_EXPAND_SZ || Operation.value.type == REG_MULTI_SZ)) {
                const auto& Raw = Operation.value.bytes;
                const int Length = MultiByteToWideChar(CP_ACP, MB_ERR_INVALID_CHARS, Raw.data(), int(Raw.size()), nullptr, 0);
                if (!Length) return Fail(QStringLiteral("REGEDIT4 문자열의 코드 페이지가 잘못되었습니다"), LineNumber);
                QString Unicode(Length, QChar(0));
                MultiByteToWideChar(CP_ACP, MB_ERR_INVALID_CHARS, Raw.data(), int(Raw.size()), reinterpret_cast<wchar_t*>(Unicode.data()), Length);
                Operation.value.bytes = Utf16(Unicode, false);
            }
        }
        if (Operation.value.bytes.size() > WinRegistryBackend::MaxValueBytes) return Fail(QStringLiteral("값 크기 제한을 넘었습니다"), LineNumber);
        Result.operations.append(std::move(Operation));
    }
    return Result;
}
QString RegFileWriter::Text(const QList<RegistryOperation>& Operations)
{
    QString Result = QStringLiteral("Windows Registry Editor Version 5.00\r\n");
    QString Current;
    for (const auto& Operation : Operations) {
        const QString Path = Operation.location.PhysicalPath();
        if (Operation.kind == OperationKind::DeleteKey) { Result += "\r\n[-" + Path + "]\r\n"; Current.clear(); continue; }
        if (Operation.kind == OperationKind::CreateKey || Current != Path) { Result += "\r\n[" + Path + "]\r\n"; Current = Path; }
        if (Operation.kind == OperationKind::CreateKey) continue;
        Result += (Operation.value.name.isEmpty() ? QString("@") : Quote(Operation.value.name)) + '=';
        if (Operation.kind == OperationKind::DeleteValue) { Result += "-\r\n"; continue; }
        const auto& Value = Operation.value;
        // Emit hex(type) for malformed strings and unknown types without repairing
        // them; export/undo must retain the original bytes, including missing NULs.
        QString String;
        if (Value.type == REG_SZ && Value.Warning().isEmpty()) String = FromUtf16(Value.bytes.first(Value.bytes.size() - 2));
        if (Value.type == REG_SZ && Value.Warning().isEmpty() && !String.contains(QChar(0)) && !String.contains('\r') && !String.contains('\n')) {
            Result += Quote(String);
        } else {
            Result += Value.type == REG_BINARY ? "hex:" : "hex(" + QString::number(Value.type, 16) + "):";
            for (qsizetype I = 0; I < Value.bytes.size(); ++I) {
                if (I) Result += I % 24 == 0 ? ",\\\r\n  " : ",";
                Result += QString::number(quint8(Value.bytes[I]), 16).rightJustified(2, '0');
            }
        }
        Result += "\r\n";
    }
    return Result;
}
QByteArray RegFileWriter::Write(const QList<RegistryOperation>& Operations)
{
    return QByteArray::fromHex("fffe") + Utf16(Text(Operations), false);
}
} // namespace hc::registry
