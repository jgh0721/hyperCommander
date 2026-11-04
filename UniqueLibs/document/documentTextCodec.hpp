#pragma once
#include <QByteArray>
#include <QList>
#include <QString>
#include "commonLibs/storage/storage.hpp"
namespace hc::document
{
    // How DecodeDocumentText chose the encoding. The detector has no numeric score;
    // TextDetectionLabel maps each outcome to the confidence word the encoding popup shows.
    enum class TextDetection { Explicit, Bom, Utf8Strict, AsciiOnly, Cp949Fallback };
    struct DecodedText { QString text_, encoding_; bool bom_=false; TextDetection detection_=TextDetection::Explicit; };
    // name_ is the value DecodeDocumentText/EncodeDocumentText take and report; label_ and note_ are display text.
    struct TextEncodingInfo { QString name_, label_, note_; bool bomCapable_=false; };
    // firstOffset_ is a UTF-16 index into the scanned text. A surrogate pair counts once.
    struct UnrepresentableScan { qsizetype count_=0; qsizetype firstOffset_=-1; };
    // Encoding names: UTF-8, UTF-16LE, UTF-16BE, CP949, Shift_JIS, GB18030, ISO-8859-1, System (active ANSI
    // code page). Aliases, labels and case variants are accepted; results carry the canonical name.
    storage::Result<DecodedText> DecodeDocumentText(const QByteArray& Bytes, const QString& Encoding = {});
    // Bom applies to the UTF encodings only; the legacy code pages have no byte order mark.
    storage::Result<QByteArray> EncodeDocumentText(const QString& Text, const QString& Encoding, bool Bom);
    // Canonical name for a name, alias ("EUC-KR") or menu label ("EUC-KR (CP949)"); empty when unsupported.
    QString CanonicalEncodingName(const QString& Encoding);
    // Entry of ReadEncodings() for any accepted spelling; empty name_ when unsupported.
    TextEncodingInfo EncodingInfo(const QString& Encoding);
    // Every encoding a document can be read as, in menu order.
    QList<TextEncodingInfo> ReadEncodings();
    // The conversion targets offered for saving. EncodeDocumentText also takes every ReadEncodings() name,
    // so the current read encoding can be offered on top of these.
    QList<TextEncodingInfo> SaveEncodings();
    QString TextDetectionLabel(TextDetection Detection);
    // Scalars that EncodeDocumentText could not write in Encoding. Lone surrogates count as unrepresentable;
    // an unsupported encoding yields an empty scan.
    UnrepresentableScan FindUnrepresentable(const QString& Text, const QString& Encoding);
}
