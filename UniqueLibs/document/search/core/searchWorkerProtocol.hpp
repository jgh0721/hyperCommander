#pragma once
#include <QByteArray>
#include <QString>
#include <optional>

namespace hc::document::search::wire {
constexpr quint32 Magic = 0x31534348;
constexpr quint16 Version = 1;
constexpr qsizetype HeaderBytes = 32;
constexpr quint32 MaxPayload = 256 * 1024;
enum class Type : quint16 { Hello = 1, Ready, PutBegin, PutChunk, PutEnd, SnapshotReady, Search, MatchBatch, FileComplete, Complete, Error, Credit, Cancel, Canceled, Release, Shutdown, Started, SourceMetadata };
struct Frame { Type type_; quint64 instance_ = 0, generation_ = 0; quint32 sequence_ = 0; QByteArray payload_; };
class Writer {
public:
    QByteArray bytes_;
    void U32(quint32 Value);
    void U64(quint64 Value);
    void Text(const QString& Value);
    void Bytes(const QByteArray& Value);
};
class Reader {
public:
    explicit Reader(QByteArray Bytes) : bytes_(std::move(Bytes)) {}
    quint32 U32();
    quint64 U64();
    QString Text();
    QByteArray Bytes();
    bool Done() const { return valid_ && offset_ == bytes_.size(); }
private:
    QByteArray bytes_; qsizetype offset_ = 0; bool valid_ = true;
};
QByteArray Encode(const Frame& Value);
// Retains at most one incomplete frame. Call Take repeatedly after each bounded read.
class Decoder {
public:
    bool Append(QByteArray Bytes);
    std::optional<Frame> Take();
    QString Error() const { return error_; }
private:
    QByteArray bytes_; QString error_;
};
QByteArray EncodeText(const QString& Text);
std::optional<QString> DecodeText(const QByteArray& Bytes);
}
