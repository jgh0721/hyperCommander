#include "stdafx.h"
#include "registryValue.hpp"

/// 모듈
/// UIs
/// 기타 헤더파일
/// 고유 라이브러리
/// 공통 라이브러리
/// 외부 라이브러리
/// 선언 헤더파일들
/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QtEndian>
#include <QCoreApplication>
#include <limits>

DECLARE_CMNLIBSV2_NAMESPACE
Q_LOGGING_CATEGORY(lcRegistryValue, "hc.registry.value")

namespace hc::registry {
namespace {
bool ValidUtf16(const QString& Text)
{
    for (qsizetype I = 0; I < Text.size(); ++I) {
        if (Text[I].isHighSurrogate()) { if (++I == Text.size() || !Text[I].isLowSurrogate()) return false; }
        else if (Text[I].isLowSurrogate()) return false;
    }
    return true;
}
QString Warning(const char* Text) { return QCoreApplication::translate("RegistryValueCodec", Text); }
}
RegistryTextView RegistryValueCodec::DecodeText(const RegistryValue& Value, bool SingleLineEditor)
{
    RegistryTextView Result; Result.warning = Value.Warning(); if (!Result.warning.isEmpty()) return Result;
    if (Value.type != REG_SZ && Value.type != REG_EXPAND_SZ && Value.type != REG_MULTI_SZ) {
        Result.warning = Warning("문자열 편집 형식이 아닙니다."); return Result;
    }
    QString Text; Text.reserve(Value.bytes.size() / 2);
    for (qsizetype I = 0; I < Value.bytes.size(); I += 2) Text.append(QChar(qFromLittleEndian<quint16>(Value.bytes.constData() + I)));
    Text.chop(Value.type == REG_MULTI_SZ ? 2 : 1);
    if (!ValidUtf16(Text)) { Result.warning = Warning("잘못된 UTF-16 원문입니다. 16진 편집기로 원문을 보존합니다."); return Result; }
    if (Value.type == REG_MULTI_SZ) {
        const auto Parts = Text.split(QChar(0), Qt::KeepEmptyParts);
        if (!Text.isEmpty()) for (const auto& Part : Parts) if (Part.isEmpty() || Part.contains('\n') || Part.contains('\r')) {
            Result.warning = Warning("줄 목록으로 손실 없이 표시할 수 없는 다중 문자열입니다."); return Result;
        }
        Result.text = Parts.join('\n');
    } else {
        if (Text.contains(QChar(0)) || (SingleLineEditor && (Text.contains('\n') || Text.contains('\r')))) {
            Result.warning = Warning("한 줄 입력으로 손실 없이 표시할 수 없는 문자열입니다."); return Result;
        }
        Result.text = Text;
    }
    Result.lossless = true; return Result;
}
RegistryResult<QByteArray> RegistryValueCodec::EncodeText(quint32 Type, const QString& Text)
{
    if (Type != REG_SZ && Type != REG_EXPAND_SZ && Type != REG_MULTI_SZ) return {{}, ERROR_NOT_SUPPORTED};
    if (Text.size() > WinRegistryBackend::MaxValueBytes / 2 - 2) return {{}, ERROR_FILE_TOO_LARGE};
    if (!ValidUtf16(Text) || Text.contains(QChar(0))) return {{}, ERROR_INVALID_DATA};
    QString Encoded;
    if (Type == REG_MULTI_SZ) {
        auto Normalized = Text; Normalized.replace("\r\n", "\n");
        for (const auto& Part : Normalized.split('\n', Qt::KeepEmptyParts)) if (!Part.isEmpty()) { Encoded += Part; Encoded += QChar(0); }
        if (Encoded.isEmpty()) Encoded += QChar(0);
        Encoded += QChar(0);
    } else { Encoded = Text; Encoded += QChar(0); }
    if (Encoded.size() > WinRegistryBackend::MaxValueBytes / 2) return {{}, ERROR_FILE_TOO_LARGE};
    QByteArray Bytes; Bytes.resize(Encoded.size() * 2);
    for (qsizetype I = 0; I < Encoded.size(); ++I) qToLittleEndian<quint16>(Encoded[I].unicode(), Bytes.data() + I * 2);
    return {Bytes};
}
RegistryResult<quint64> RegistryValueCodec::DecodeNumber(const RegistryValue& Value)
{
    if (!Value.Warning().isEmpty()) return {{}, ERROR_INVALID_DATA};
    if (Value.type == REG_DWORD) return {qFromLittleEndian<quint32>(Value.bytes.constData())};
    if (Value.type == REG_DWORD_BIG_ENDIAN) return {qFromBigEndian<quint32>(Value.bytes.constData())};
    if (Value.type == REG_QWORD) return {qFromLittleEndian<quint64>(Value.bytes.constData())};
    return {{}, ERROR_NOT_SUPPORTED};
}
RegistryResult<QByteArray> RegistryValueCodec::EncodeNumber(quint32 Type, quint64 Value)
{
    if (Type != REG_DWORD && Type != REG_DWORD_BIG_ENDIAN && Type != REG_QWORD) return {{}, ERROR_NOT_SUPPORTED};
    if (Type != REG_QWORD && Value > std::numeric_limits<quint32>::max()) return {{}, ERROR_ARITHMETIC_OVERFLOW};
    QByteArray Bytes(Type == REG_QWORD ? 8 : 4, '\0');
    if (Type == REG_QWORD) qToLittleEndian<quint64>(Value, Bytes.data());
    else if (Type == REG_DWORD_BIG_ENDIAN) qToBigEndian<quint32>(quint32(Value), Bytes.data());
    else qToLittleEndian<quint32>(quint32(Value), Bytes.data());
    return {Bytes};
}
}
