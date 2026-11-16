#pragma once
#include "environmentStorage.hpp"
namespace hc::environment {
enum class EnvironmentFormat { DotEnv, Json, Reg };
struct ImportedEnvironmentValue { Scope scope_ = Scope::User; QString name_; std::optional<Value> value_; };
struct EnvironmentImport { QList<ImportedEnvironmentValue> values_; QString error_; explicit operator bool() const { return error_.isEmpty(); } };
EnvironmentImport ParseEnvironmentFile(const QByteArray& Bytes, EnvironmentFormat Format);
registry::RegistryResult<QByteArray> ExportEnvironmentFile(const std::vector<Value>& Values, EnvironmentFormat Format);
EnvironmentPreview PreviewEnvironmentImport(const EnvironmentImport& Import, EnvironmentStorage& Storage);
}
