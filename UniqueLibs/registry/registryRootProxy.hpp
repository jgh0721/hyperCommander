#pragma once

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QAbstractProxyModel>
#include <QHash>
#include <QPersistentModelIndex>
#include <memory>
#include <vector>

namespace hc::registry {

// Exposes the children of a source key as top-level rows. No registry access is
// performed here; the key and item models retain ownership of the shared cache.
class RegistryRootProxy final : public QAbstractProxyModel
{
    Q_OBJECT
public:
    explicit RegistryRootProxy(QObject* Parent = nullptr);
    void setSourceModel(QAbstractItemModel* Source) override;
    void SetRoot(const QModelIndex& Root);
    QModelIndex Root() const { return root_; }
    void SetFlat(bool Flat);
    bool Flat() const { return flat_; }
    QModelIndex mapToSource(const QModelIndex& Index) const override;
    QModelIndex mapFromSource(const QModelIndex& Index) const override;
    QModelIndex index(int Row, int Column, const QModelIndex& Parent = {}) const override;
    QModelIndex parent(const QModelIndex& Index) const override;
    int rowCount(const QModelIndex& Parent = {}) const override;
    int columnCount(const QModelIndex& Parent = {}) const override;
    bool hasChildren(const QModelIndex& Parent = {}) const override;
    bool canFetchMore(const QModelIndex& Parent) const override;
    void fetchMore(const QModelIndex& Parent) override;
    QVariant data(const QModelIndex& Index, int Role = Qt::DisplayRole) const override;
    bool setData(const QModelIndex& Index, const QVariant& Value, int Role = Qt::EditRole) override;
    Qt::ItemFlags flags(const QModelIndex& Index) const override;
    QVariant headerData(int Section, Qt::Orientation Orientation, int Role) const override;
    QHash<int, QByteArray> roleNames() const override;
    void sort(int Column, Qt::SortOrder Order = Qt::AscendingOrder) override;
signals:
    // The view captures row IDs before reset and restores cursor, selection,
    // expansion and scroll after reset; QModelIndex is never a persisted ID.
    void RootAboutToChange();
    void RootChanged(const QModelIndex& Root);
private:
    struct Entry { QPersistentModelIndex source; };
    enum class Change { None, Insert, Remove, Move, Reset };
    bool containsParent(const QModelIndex& Parent) const;
    QModelIndex sourceParent(const QModelIndex& Parent) const;
    void startReset();
    void finishReset();
    void rebuildLookup();
    void finishRows();
    QPersistentModelIndex root_;
    mutable std::vector<std::unique_ptr<Entry>> entries_;
    mutable QHash<QPersistentModelIndex, Entry*> lookup_;
    QList<QMetaObject::Connection> connections_;
    Change change_ = Change::None;
    bool flat_ = false;
};
} // namespace hc::registry
