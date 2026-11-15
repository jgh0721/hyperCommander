#pragma once
#include "registryAccess.hpp"

namespace hc::registry {
enum class OperationKind { CreateKey, DeleteKey, SetValue, DeleteValue };
struct RegistryOperation
{
    OperationKind kind = OperationKind::CreateKey;
    RegistryLocation location;
    RegistryValue value;
    int line = 0;
};
enum class RegEncoding { Detect, Utf8, Ansi };
struct RegFileResult
{
    QList<RegistryOperation> operations;
    QString error;
    int line = 0;
    bool encodingConfirmation = false;
    explicit operator bool() const { return error.isEmpty() && !encodingConfirmation; }
};
class RegFileParser
{
public:
    static constexpr qsizetype MaxFileBytes = 256 * 1024 * 1024;
    static constexpr qsizetype MaxOperations = 1000000;
    static RegFileResult Parse(const QByteArray& Bytes, View SelectedView, RegEncoding Encoding = RegEncoding::Detect);
};
class RegFileWriter
{
public:
    static QByteArray Write(const QList<RegistryOperation>& Operations);
    static QString Text(const QList<RegistryOperation>& Operations);
};
} // namespace hc::registry
