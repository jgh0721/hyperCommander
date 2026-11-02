#include "stdafx.h"
#include "searchWorkerProtocol.hpp"

/// 모듈
/// UIs
/// 기타 헤더파일
/// 고유 라이브러리
/// 공통 라이브러리
/// 외부 라이브러리
/// 선언 헤더파일들
/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QtEndian>

namespace hc::document::search::wire {
void Writer::U32(quint32 Value) { const auto N = qToLittleEndian(Value); bytes_.append(reinterpret_cast<const char*>(&N), sizeof(N)); }
void Writer::U64(quint64 Value) { const auto N = qToLittleEndian(Value); bytes_.append(reinterpret_cast<const char*>(&N), sizeof(N)); }
void Writer::Bytes(const QByteArray& Value) { U32(quint32(Value.size())); bytes_ += Value; }
void Writer::Text(const QString& Value) { Bytes(Value.toUtf8()); }
quint32 Reader::U32() { if (bytes_.size() - offset_ < 4) { valid_ = false; return 0; } const auto N = qFromLittleEndian<quint32>(bytes_.constData() + offset_); offset_ += 4; return N; }
quint64 Reader::U64() { if (bytes_.size() - offset_ < 8) { valid_ = false; return 0; } const auto N = qFromLittleEndian<quint64>(bytes_.constData() + offset_); offset_ += 8; return N; }
QByteArray Reader::Bytes() { const auto N = U32(); if (!valid_ || N > quint64(bytes_.size() - offset_)) { valid_ = false; return {}; } const auto Result = bytes_.mid(offset_, N); offset_ += N; return Result; }
QString Reader::Text() { const auto Data = Bytes(); QStringDecoder Decoder(QStringConverter::Utf8); QString Result = Decoder(Data); if (Decoder.hasError()) valid_ = false; return Result; }
QByteArray Encode(const Frame& Value)
{
    if (Value.payload_.size() > MaxPayload) return {};
    Writer Header; Header.U32(Magic); Header.U32(quint32(Version) | (quint32(Value.type_) << 16));
    Header.U32(quint32(Value.payload_.size())); Header.U32(Value.sequence_); Header.U64(Value.instance_); Header.U64(Value.generation_);
    return Header.bytes_ + Value.payload_;
}
bool Decoder::Append(QByteArray Bytes)
{
    if (!error_.isEmpty()) return false;
    if (bytes_.size() + Bytes.size() > HeaderBytes + MaxPayload + 64 * 1024) { error_ = "ipc-buffer-overflow"; return false; }
    bytes_ += Bytes; return true;
}
std::optional<Frame> Decoder::Take()
{
    if (!error_.isEmpty() || bytes_.size() < HeaderBytes) return {};
    Reader Header(bytes_.first(HeaderBytes));
    const auto Tag = Header.U32(), Kind = Header.U32(), Size = Header.U32(), Seq = Header.U32();
    const auto Instance = Header.U64(), Generation = Header.U64();
    if (Tag != Magic || (Kind & 0xffff) != Version || Size > MaxPayload || (Kind >> 16) < 1 || (Kind >> 16) > quint32(Type::SourceMetadata)) { error_ = "ipc-invalid-header"; return {}; }
    if (bytes_.size() < HeaderBytes + Size) return {};
    Frame Result{Type(Kind >> 16), Instance, Generation, Seq, bytes_.mid(HeaderBytes, Size)};
    bytes_.remove(0, HeaderBytes + Size); return Result;
}
QByteArray EncodeText(const QString& Text)
{
    QByteArray Result(Text.size() * 2, Qt::Uninitialized);
    for (qsizetype I = 0; I < Text.size(); ++I) qToLittleEndian<quint16>(Text[I].unicode(), Result.data() + 2 * I);
    return Result;
}
std::optional<QString> DecodeText(const QByteArray& Bytes)
{
    if (Bytes.size() % 2) return {};
    QString Result(Bytes.size() / 2, Qt::Uninitialized);
    for (qsizetype I = 0; I < Result.size(); ++I) Result[I] = QChar(qFromLittleEndian<quint16>(Bytes.constData() + 2 * I));
    if (!Result.isValidUtf16()) return {};
    return Result;
}
}
