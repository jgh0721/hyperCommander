#pragma once
#include <QByteArray>
#include <QString>
#include "commonLibs/storage/storage.hpp"
namespace hc::document
{
    struct DecodedText { QString text_, encoding_; bool bom_=false; };
    storage::Result<DecodedText> DecodeDocumentText(const QByteArray& Bytes, const QString& Encoding = {});
    storage::Result<QByteArray> EncodeDocumentText(const QString& Text, const QString& Encoding, bool Bom);
}
