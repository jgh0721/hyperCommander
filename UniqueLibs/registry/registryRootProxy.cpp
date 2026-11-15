#include "stdafx.h"
#include "registryRootProxy.hpp"

/// 모듈
/// UIs
/// 기타 헤더파일
/// 고유 라이브러리
/// 공통 라이브러리
/// 외부 라이브러리
/// 선언 헤더파일들
/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QLoggingCategory>

DECLARE_CMNLIBSV2_NAMESPACE
Q_LOGGING_CATEGORY(lcRegistryRootProxy, "hc.registry.rootProxy")

namespace hc::registry {
RegistryRootProxy::RegistryRootProxy(QObject* Parent) : QAbstractProxyModel(Parent) {}

bool RegistryRootProxy::containsParent(const QModelIndex& Parent) const
{
    if (!sourceModel() || (Parent.isValid() && Parent.model() != sourceModel())) return false;
    if (flat_) return Parent == root_;
    if (!root_.isValid()) return true;
    for (QModelIndex Cursor = Parent; Cursor.isValid(); Cursor = Cursor.parent())
        if (Cursor == root_) return true;
    return false;
}

QModelIndex RegistryRootProxy::sourceParent(const QModelIndex& Parent) const
{
    return Parent.isValid() ? mapToSource(Parent) : QModelIndex(root_);
}

QModelIndex RegistryRootProxy::mapToSource(const QModelIndex& Index) const
{
    if (!Index.isValid() || Index.model() != this) return {};
    return static_cast<Entry*>(Index.internalPointer())->source;
}

QModelIndex RegistryRootProxy::mapFromSource(const QModelIndex& Index) const
{
    if (!Index.isValid() || Index.model() != sourceModel() || Index == root_ || !containsParent(Index.parent())) return {};
    Entry* Item = lookup_.value(Index, nullptr);
    if (!Item) {
        auto New = std::make_unique<Entry>();
        New->source = Index;
        Item = New.get();
        entries_.push_back(std::move(New));
        lookup_.insert(Index, Item);
    }
    return createIndex(Index.row(), Index.column(), Item);
}

QModelIndex RegistryRootProxy::index(int Row, int Column, const QModelIndex& Parent) const
{
    if (flat_ && Parent.isValid()) return {};
    if (!sourceModel() || (Parent.isValid() && (Parent.model() != this || Parent.column() != 0))) return {};
    return mapFromSource(sourceModel()->index(Row, Column, sourceParent(Parent)));
}

QModelIndex RegistryRootProxy::parent(const QModelIndex& Index) const { return mapFromSource(mapToSource(Index).parent()); }
int RegistryRootProxy::rowCount(const QModelIndex& Parent) const
{
    if (flat_ && Parent.isValid()) return 0;
    return sourceModel() && (!Parent.isValid() || (Parent.model() == this && Parent.column() == 0))
        ? sourceModel()->rowCount(sourceParent(Parent)) : 0;
}
int RegistryRootProxy::columnCount(const QModelIndex& Parent) const
{
    return sourceModel() ? sourceModel()->columnCount(sourceParent(Parent)) : 0;
}
bool RegistryRootProxy::hasChildren(const QModelIndex& Parent) const
{
    if ((flat_ && Parent.isValid()) || (Parent.isValid() && Parent.column() != 0)) return false;
    return sourceModel() && sourceModel()->hasChildren(sourceParent(Parent));
}
bool RegistryRootProxy::canFetchMore(const QModelIndex& Parent) const
{
    if ((flat_ && Parent.isValid()) || (Parent.isValid() && Parent.column() != 0)) return false;
    return sourceModel() && sourceModel()->canFetchMore(sourceParent(Parent));
}
void RegistryRootProxy::fetchMore(const QModelIndex& Parent)
{
    if (canFetchMore(Parent)) sourceModel()->fetchMore(sourceParent(Parent));
}
QVariant RegistryRootProxy::data(const QModelIndex& Index, int Role) const { return mapToSource(Index).data(Role); }
bool RegistryRootProxy::setData(const QModelIndex& Index, const QVariant& Value, int Role)
{
    return sourceModel() && Index.isValid() && sourceModel()->setData(mapToSource(Index), Value, Role);
}
Qt::ItemFlags RegistryRootProxy::flags(const QModelIndex& Index) const
{
    return sourceModel() ? sourceModel()->flags(mapToSource(Index)) : Qt::NoItemFlags;
}
QVariant RegistryRootProxy::headerData(int Section, Qt::Orientation Orientation, int Role) const
{
    return sourceModel() ? sourceModel()->headerData(Section, Orientation, Role) : QVariant();
}
QHash<int, QByteArray> RegistryRootProxy::roleNames() const
{
    return sourceModel() ? sourceModel()->roleNames() : QHash<int, QByteArray>();
}
void RegistryRootProxy::sort(int Column, Qt::SortOrder Order) { if (sourceModel()) sourceModel()->sort(Column, Order); }

void RegistryRootProxy::startReset()
{
    emit RootAboutToChange();
    beginResetModel();
    change_ = Change::Reset;
}
void RegistryRootProxy::finishReset()
{
    lookup_.clear();
    entries_.clear();
    change_ = Change::None;
    endResetModel();
    emit RootChanged(root_);
}
void RegistryRootProxy::SetRoot(const QModelIndex& Root)
{
    if ((Root.isValid() && (Root.model() != sourceModel() || Root.column() != 0)) || Root == root_) return;
    startReset();
    root_ = Root;
    finishReset();
}
void RegistryRootProxy::SetFlat(bool Flat)
{
    if (flat_ == Flat) return;
    startReset(); flat_ = Flat; finishReset();
}
void RegistryRootProxy::rebuildLookup()
{
    // Persistent source indexes change their hash when rows move. Rebuild only
    // after the source mutation, keeping Entry addresses stable for proxy indexes.
    lookup_.clear();
    for (const auto& Item : entries_)
        if (Item->source.isValid()) lookup_.insert(Item->source, Item.get());
}
void RegistryRootProxy::finishRows()
{
    rebuildLookup();
    const Change Finished = change_;
    change_ = Change::None;
    switch (Finished) {
    case Change::Insert: endInsertRows(); break;
    case Change::Remove: endRemoveRows(); break;
    case Change::Move: endMoveRows(); break;
    case Change::Reset: finishReset(); break;
    default: break;
    }
}

void RegistryRootProxy::setSourceModel(QAbstractItemModel* Source)
{
    if (Source == sourceModel()) return;
    startReset();
    for (const auto& Connection : connections_) disconnect(Connection);
    connections_.clear();
    root_ = {};
    QAbstractProxyModel::setSourceModel(Source);
    if (Source) {
        connections_ << connect(Source, &QAbstractItemModel::dataChanged, this,
            [this](const QModelIndex& First, const QModelIndex& Last, const QList<int>& Roles) {
                if (containsParent(First.parent())) emit dataChanged(mapFromSource(First), mapFromSource(Last), Roles);
            });
        connections_ << connect(Source, &QAbstractItemModel::headerDataChanged, this, &QAbstractItemModel::headerDataChanged);
        connections_ << connect(Source, &QAbstractItemModel::rowsAboutToBeInserted, this,
            [this](const QModelIndex& Parent, int First, int Last) {
                if (containsParent(Parent)) { beginInsertRows(mapFromSource(Parent), First, Last); change_ = Change::Insert; }
            });
        connections_ << connect(Source, &QAbstractItemModel::rowsInserted, this, [this] { finishRows(); });
        connections_ << connect(Source, &QAbstractItemModel::rowsAboutToBeRemoved, this,
            [this](const QModelIndex& Parent, int First, int Last) {
                for (QModelIndex Cursor = root_; Cursor.isValid(); Cursor = Cursor.parent()) {
                    if (Cursor.parent() == Parent && Cursor.row() >= First && Cursor.row() <= Last) {
                        startReset();
                        root_ = Parent; // Nearest ancestor outside the removed subtree.
                        return;
                    }
                }
                if (containsParent(Parent)) { beginRemoveRows(mapFromSource(Parent), First, Last); change_ = Change::Remove; }
            });
        connections_ << connect(Source, &QAbstractItemModel::rowsRemoved, this, [this] { finishRows(); });
        connections_ << connect(Source, &QAbstractItemModel::rowsAboutToBeMoved, this,
            [this](const QModelIndex& From, int First, int Last, const QModelIndex& To, int Row) {
                const bool FromInside = containsParent(From), ToInside = containsParent(To);
                if (FromInside && ToInside) {
                    if (beginMoveRows(mapFromSource(From), First, Last, mapFromSource(To), Row)) change_ = Change::Move;
                } else if (FromInside) {
                    beginRemoveRows(mapFromSource(From), First, Last); change_ = Change::Remove;
                } else if (ToInside) {
                    beginInsertRows(mapFromSource(To), Row, Row + Last - First); change_ = Change::Insert;
                }
            });
        connections_ << connect(Source, &QAbstractItemModel::rowsMoved, this, [this] { finishRows(); });
        connections_ << connect(Source, &QAbstractItemModel::modelAboutToBeReset, this, [this] { startReset(); });
        connections_ << connect(Source, &QAbstractItemModel::modelReset, this, [this] { root_ = {}; finishReset(); });
        connections_ << connect(Source, &QAbstractItemModel::layoutAboutToBeChanged, this,
            [this](const QList<QPersistentModelIndex>& Parents) {
                if (Parents.isEmpty()) { startReset(); return; }
                for (const auto& Parent : Parents) if (containsParent(Parent)) { startReset(); return; }
            });
        connections_ << connect(Source, &QAbstractItemModel::layoutChanged, this, [this] {
            if (change_ == Change::Reset) finishReset(); else rebuildLookup();
        });
        // Registry models have a fixed column schema. Still handle schema changes
        // for model reuse, ensuring no stale column indexes escape a source reset.
        connections_ << connect(Source, &QAbstractItemModel::columnsAboutToBeInserted, this, [this](const QModelIndex& Parent) { if (containsParent(Parent)) startReset(); });
        connections_ << connect(Source, &QAbstractItemModel::columnsInserted, this, [this] { if (change_ == Change::Reset) finishReset(); });
        connections_ << connect(Source, &QAbstractItemModel::columnsAboutToBeRemoved, this, [this](const QModelIndex& Parent) { if (containsParent(Parent)) startReset(); });
        connections_ << connect(Source, &QAbstractItemModel::columnsRemoved, this, [this] { if (change_ == Change::Reset) finishReset(); });
        connections_ << connect(Source, &QAbstractItemModel::columnsAboutToBeMoved, this, [this](const QModelIndex& From, int, int, const QModelIndex& To) { if (containsParent(From) || containsParent(To)) startReset(); });
        connections_ << connect(Source, &QAbstractItemModel::columnsMoved, this, [this] { if (change_ == Change::Reset) finishReset(); });
    }
    finishReset();
}
} // namespace hc::registry
