#include "stdafx.h"
#include "registryFilterProxy.hpp"

/// 모듈
/// UIs
/// 기타 헤더파일
/// 고유 라이브러리
#include "registryModels.hpp"
/// 공통 라이브러리
/// 외부 라이브러리
/// 선언 헤더파일들
/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )

DECLARE_CMNLIBSV2_NAMESPACE
Q_LOGGING_CATEGORY(lcRegistryFilter, "hc.registry.filter")

namespace hc::registry {
RegistryFilterProxy::RegistryFilterProxy(QObject* Parent) : QSortFilterProxyModel(Parent)
{
    setDynamicSortFilter(true); setRecursiveFilteringEnabled(true);
    // Qtitan owns ordering; retain the source's stable sort ranks unchanged.
    setSortRole(RegistryTreeModel::GridSortRole);
}
void RegistryFilterProxy::SetText(const QString& Text)
{
    if (text_ == Text) return;
    beginFilterChange(); text_ = Text; endFilterChange(Direction::Rows);
}
void RegistryFilterProxy::SetKnownOnly(bool KnownOnly)
{
    if (knownOnly_ == KnownOnly) return;
    beginFilterChange(); knownOnly_ = KnownOnly; endFilterChange(Direction::Rows);
}
bool RegistryFilterProxy::filterAcceptsRow(int Row, const QModelIndex& Parent) const
{
    if (!sourceModel()) return false;
    const auto Index = sourceModel()->index(Row, 0, Parent);
    if (Index.data(RegistryTreeModel::KindRole).toInt() == int(RegistryTreeModel::Kind::Status)) return true;
    if (knownOnly_ && sourceModel()->index(Row, RegistryTreeModel::Description, Parent).data().toString().isEmpty()) return false;
    if (text_.isEmpty()) return true;
    for (int Column = 0; Column < sourceModel()->columnCount(Parent); ++Column)
        if (sourceModel()->index(Row, Column, Parent).data().toString().contains(text_, Qt::CaseInsensitive)) return true;
    return false;
}
} // namespace hc::registry
