#include "stdafx.h"
#include "registryPolicyFile.hpp"

/// 모듈
/// UIs
/// 기타 헤더파일
/// 고유 라이브러리
/// 공통 라이브러리
#include "registryFile.hpp"
/// 외부 라이브러리
/// 선언 헤더파일들
/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QLoggingCategory>

DECLARE_CMNLIBSV2_NAMESPACE
Q_LOGGING_CATEGORY(lcRegistryPolicyFile, "hc.registry.policyFile")

namespace hc::registry {
namespace {
// MS-GPREG 2.2.1: order is significant; strings and delimiters are UTF-16LE,
// type/size are DWORD, and data is length-delimited (it can contain ']' or NUL).
// https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-gpreg/5c092c22-bf6b-4e7f-b180-b20743d368f5
void Number(QByteArray& Out, quint32 Value, int Count)
{
    for (int I = 0; I < Count; ++I) Out += char(Value >> (8 * I));
}
void String(QByteArray& Out, const QString& Text)
{
    for (const QChar C : Text) Number(Out, C.unicode(), 2);
    Number(Out, 0, 2);
}
class Reader
{
public:
    const QByteArray& bytes;
    qsizetype offset = 0;
    bool failed = false;
    quint32 Number(int Count)
    {
        if (bytes.size() - offset < Count) { failed = true; return 0; }
        quint32 Result = 0;
        for (int I = 0; I < Count; ++I) Result |= quint32(quint8(bytes[offset++])) << (8 * I);
        return Result;
    }
    bool Expect(ushort Value) { return Number(2) == Value && !failed; }
    QString String(qsizetype Limit)
    {
        QString Result;
        while (!failed) {
            const quint32 C = Number(2);
            if (!C) return Result;
            if (Result.size() >= Limit) { failed = true; return {}; }
            Result += QChar(ushort(C));
        }
        return {};
    }
};
}
PolicyDirective RegistryPolRecord::Directive() const
{
    const auto& Name = value.name;
    if (Name.startsWith("**Del.", Qt::CaseInsensitive)) return PolicyDirective::DeleteValue;
    if (Name.compare("**DeleteValues", Qt::CaseInsensitive) == 0) return PolicyDirective::DeleteValues;
    if (Name.compare("**DelVals.", Qt::CaseInsensitive) == 0 || Name.compare("**DelVals", Qt::CaseInsensitive) == 0) return PolicyDirective::DeleteAllValues;
    if (Name.trimmed().compare("**DeleteKeys", Qt::CaseInsensitive) == 0) return PolicyDirective::DeleteKeys;
    if (Name.compare("**SecureKey", Qt::CaseInsensitive) == 0) return PolicyDirective::SecureKey;
    if (Name.startsWith("**soft.", Qt::CaseInsensitive)) return PolicyDirective::SoftValue;
    return Name.startsWith("**") ? PolicyDirective::Unknown : PolicyDirective::Value;
}
RegistryPolResult RegistryPolCodec::Read(const QByteArray& Bytes)
{
    RegistryPolResult Result; Reader Input{Bytes};
    const auto Fail = [&] { Result.error = QStringLiteral("잘못된 Registry.pol 레코드입니다"); Result.offset = Input.offset; Result.records.clear(); return Result; };
    if (Bytes.size() > RegFileParser::MaxFileBytes || Input.Number(4) != 0x67655250 || Input.Number(4) != 1 || Input.failed) return Fail();
    while (Input.offset < Bytes.size()) {
        if (Result.records.size() >= RegFileParser::MaxOperations || !Input.Expect('[')) return Fail();
        RegistryPolRecord Record; Record.key = Input.String(32767);
        if (Record.key.isEmpty() || Input.failed || !Input.Expect(';')) return Fail();
        Record.value.name = Input.String(16383);
        if (Input.failed || !Input.Expect(';')) return Fail();
        Record.value.type = Input.Number(4);
        if (Input.failed || !Input.Expect(';')) return Fail();
        const quint32 Length = Input.Number(4);
        if (Input.failed || !Input.Expect(';') || Length > 65535 || Length > Bytes.size() - Input.offset) return Fail();
        Record.value.bytes = Bytes.sliced(Input.offset, Length); Input.offset += Length;
        if (!Input.Expect(']')) return Fail();
        Result.records.append(std::move(Record));
    }
    return Result;
}
RegistryResult<QByteArray> RegistryPolCodec::Write(const QList<RegistryPolRecord>& Records)
{
    RegistryResult<QByteArray> Result;
    if (Records.size() > RegFileParser::MaxOperations) { Result.error = ERROR_FILE_TOO_LARGE; return Result; }
    Number(Result.value, 0x67655250, 4); Number(Result.value, 1, 4);
    for (const auto& Record : Records) {
        if (Record.key.isEmpty() || Record.key.size() > 32767 || Record.key.contains(QChar(0))
            || !WinRegistryBackend::ValidName(Record.value.name, false) || Record.value.bytes.size() > 65535) {
            Result.value.clear(); Result.error = ERROR_INVALID_DATA; return Result;
        }
        Number(Result.value, '[', 2); String(Result.value, Record.key); Number(Result.value, ';', 2);
        String(Result.value, Record.value.name); Number(Result.value, ';', 2);
        Number(Result.value, Record.value.type, 4); Number(Result.value, ';', 2);
        Number(Result.value, quint32(Record.value.bytes.size()), 4); Number(Result.value, ';', 2);
        Result.value += Record.value.bytes; Number(Result.value, ']', 2);
        if (Result.value.size() > RegFileParser::MaxFileBytes) { Result.value.clear(); Result.error = ERROR_FILE_TOO_LARGE; return Result; }
    }
    return Result;
}
} // namespace hc::registry
