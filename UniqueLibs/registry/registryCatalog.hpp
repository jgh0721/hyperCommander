#pragma once

/// 고유 라이브러리
#include "commonLibs/system/registryAccess.hpp"
/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QJsonObject>
#include <QStringList>

namespace hc::registry {
struct RegistryDescription
{
    QString id, title, text, category;
    QStringList warnings, sourceUrls;
    QJsonObject metadata;
    bool ambiguous = false;
    explicit operator bool() const { return !id.isEmpty() && !ambiguous; }
};
class BuiltinRegistryCatalog
{
public:
    static BuiltinRegistryCatalog Load(const QByteArray& Catalog, const QByteArray& English, const QByteArray& Korean);
    static BuiltinRegistryCatalog Bundled();
    RegistryDescription Find(const RegistryLocation& Location, const std::optional<QString>& ValueName,
        std::optional<quint32> Type, const QString& Language) const;
    QStringList Errors() const { return errors_; }
    int Count() const { return int(entries_.size()); }
    QJsonArray KnownLocations() const;
    static bool PathMatches(const QString& Pattern, const QString& Path);
private:
    QList<QJsonObject> entries_;
    QJsonObject english_, korean_, catalog_;
    QStringList errors_;
};
} // namespace hc::registry
