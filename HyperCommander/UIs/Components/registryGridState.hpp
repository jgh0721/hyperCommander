#pragma once

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QJsonObject>
#include <QModelIndex>
#include <QStringList>
#include <functional>

namespace Qtitan { class GridTreeView; }
// Stable model IDs survive inserts, rank changes and root-proxy resets. Never
// persist Qtitan row numbers or QModelIndex internal pointers in a workspace.
struct RegistryGridState
{
    QString cursor;
    QStringList selected, expanded;
    int scroll = 0;
    static RegistryGridState Capture(Qtitan::GridTreeView* View);
    bool Restore(Qtitan::GridTreeView* View, const std::function<QModelIndex(const QString&)>& Lookup) const;
    QJsonObject ToJson() const;
    static RegistryGridState FromJson(const QJsonObject& Object);
};
