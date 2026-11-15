#pragma once

/// 고유 라이브러리
#include "commonLibs/system/registryAccess.hpp"
/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QMap>
#include <QHash>
#include <QStringList>

namespace hc::registry {
struct AdmxNode
{
    QString tag, text;
    QMap<QString, QString> attributes;
    QList<AdmxNode> children;
    bool operator==(const AdmxNode&) const = default;
    const AdmxNode* Child(const QString& Tag) const;
    QString Attribute(const QString& Name, const QString& Default = {}) const;
};
struct AdmxPolicy
{
    QString id, nameSpace, sourcePath, category, supportedOn, title, explanation, scope, key;
    QString unsupportedReason;
    AdmxNode definition, presentation;
    QMap<QString, QString> strings;
    QString Text(const QString& Reference) const;
};
struct AdmxCatalog
{
    QList<AdmxPolicy> policies;
    QStringList diagnostics;
    QMap<QString, QString> categories, supported;
    QHash<QString, QList<int>> keyIndex;
    qint64 elapsedMs = 0;
    int parsedFiles = 0, cachedFiles = 0;
    static AdmxCatalog Load(const QString& Directory, const QString& Language, const QString& CacheDirectory,
        const std::atomic_bool* Cancel = nullptr);
    static std::optional<AdmxNode> Parse(const QByteArray& Xml, QString* Error = nullptr);
    QList<int> Find(const RegistryLocation& Location, const std::optional<QString>& ValueName = {}) const;
};
} // namespace hc::registry
