#pragma once
#include <QAbstractItemModel>
#include <QSortFilterProxyModel>
#include "runtime/directory.hpp"
#include <QJsonObject>
#include <QHash>
#include <QIcon>
#include <QPointer>
#include <QStyle>
#include <memory>
#include <vector>

namespace hc::compare
{
    class DirectoryTreeModel final : public QAbstractItemModel
    {
    public:
        enum { VisibleRole = Qt::UserRole + 1, PathRole, EntryRole };
        explicit DirectoryTreeModel(QObject* Parent = nullptr) : QAbstractItemModel(Parent) {}
        void SetEntries(DirectoryEntries Entries);
        DirectoryEntries Entries() const { return entries_; }
        void ApplyProgress(const DirectoryProgress& Progress);
        void FinishProgress();
        bool AwaitingChildren(const QModelIndex& Index) const;
        bool hasChildren(const QModelIndex& Parent = {}) const override;
        void SetFilter(const QString& Query, int Status);
        QModelIndex index(int Row, int Column, const QModelIndex& Parent = {}) const override;
        QModelIndex parent(const QModelIndex& Child) const override;
        int rowCount(const QModelIndex& Parent = {}) const override;
        int columnCount(const QModelIndex& = {}) const override { return 9; }
        QVariant data(const QModelIndex& Index, int Role = Qt::DisplayRole) const override;
        QVariant headerData(int Section, Qt::Orientation Orientation, int Role) const override;
        QJsonObject Entry(const QModelIndex& Index) const;
        QModelIndex ForEntry(int Row) const;
        QModelIndex ForPath(const QString& Path) const { return indexFor(paths_.value(Path.toCaseFolded())); }
        QModelIndex NextDifference(const QModelIndex& Current, int Direction) const;
        int VisibleEntries() const { return visibleEntries_; }
    private:
        struct Node
        {
            QString path_;
            Node* parent_ = nullptr;
            std::vector<Node*> children_;
            int row_ = 0, entry_ = -1, order_ = -1;
            bool visible_ = true, matched_ = true, different_ = false, folder_ = false;
        };
        Node* node(const QModelIndex& Index) const;
        QModelIndex indexFor(const Node* Item) const;
        QHash<QString, Node*> paths_;
        Node root_;
        std::vector<std::unique_ptr<Node>> nodes_;
        std::vector<Node*> order_, entriesToNodes_;
        DirectoryEntries entries_;
        int visibleEntries_ = 0;
        bool streaming_ = false;
        mutable QPointer<QStyle> iconStyle_;
        mutable qint64 iconPalette_ = 0;
        mutable QIcon folderIcon_, fileIcon_;
    };
    class DirectoryTreeFilter final : public QSortFilterProxyModel
    {
    public:
        explicit DirectoryTreeFilter(QObject* Parent) : QSortFilterProxyModel(Parent) { setFilterRole(DirectoryTreeModel::VisibleRole); }
        void SetQuery(const QString& Query, int Status);
        bool hasChildren(const QModelIndex& Parent = {}) const override;
    protected:
        bool filterAcceptsRow(int Row, const QModelIndex& Parent) const override;
    };
}
