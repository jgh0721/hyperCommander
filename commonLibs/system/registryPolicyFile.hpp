#pragma once
#include "registryAccess.hpp"

namespace hc::registry {
enum class PolicyDirective { Value, DeleteValue, DeleteValues, DeleteAllValues, DeleteKeys, SecureKey, Unknown, SoftValue };
struct RegistryPolRecord
{
    QString key;
    RegistryValue value;
    PolicyDirective Directive() const;
    bool operator==(const RegistryPolRecord&) const = default;
};
struct RegistryPolResult
{
    QList<RegistryPolRecord> records;
    QString error;
    qsizetype offset = 0;
    explicit operator bool() const { return error.isEmpty(); }
};
class RegistryPolCodec
{
public:
    static RegistryPolResult Read(const QByteArray& Bytes);
    static RegistryResult<QByteArray> Write(const QList<RegistryPolRecord>& Records);
};
} // namespace hc::registry
