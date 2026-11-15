#pragma once

/// 공통 라이브러리
#include "registryAccess.hpp"

namespace hc::registry {
struct RegistryTextView { QString text, warning; bool lossless = false; };
// Typed conversion; non-editor callers may preserve line breaks. Original bytes remain authoritative until the
// user changes a draft; malformed text is edited as raw bytes, never repaired.
class RegistryValueCodec final
{
public:
    static RegistryTextView DecodeText(const RegistryValue& Value, bool SingleLineEditor = true);
    static RegistryResult<QByteArray> EncodeText(quint32 Type, const QString& Text);
    static RegistryResult<quint64> DecodeNumber(const RegistryValue& Value);
    static RegistryResult<QByteArray> EncodeNumber(quint32 Type, quint64 Value);
};
}
