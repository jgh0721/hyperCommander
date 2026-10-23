#include "stdafx.h"
#include "directoryTreeModel.hpp"
#include "directoryView.hpp"
#include <QApplication>
#include <QStyle>
#include <QFont>
#include <QLocale>
#include <algorithm>

namespace hc::compare
{
    DirectoryTreeModel::Node* DirectoryTreeModel::node(const QModelIndex& Index) const
    { return Index.isValid() ? static_cast<Node*>(Index.internalPointer()) : const_cast<Node*>(&root_); }
    QModelIndex DirectoryTreeModel::indexFor(const Node* Item) const
    { return Item && Item != &root_ ? createIndex(Item->row_, 0, const_cast<Node*>(Item)) : QModelIndex{}; }
    QModelIndex DirectoryTreeModel::index(int Row, int Column, const QModelIndex& Parent) const
    {
        if (Row < 0 || Column < 0 || Column >= 6 || (Parent.isValid() && Parent.column())) return {};
        const auto* p = node(Parent);
        return std::size_t(Row) < p->children_.size() ? createIndex(Row, Column, p->children_[Row]) : QModelIndex{};
    }
    QModelIndex DirectoryTreeModel::parent(const QModelIndex& Child) const
    { return Child.isValid() ? indexFor(node(Child)->parent_) : QModelIndex{}; }
    int DirectoryTreeModel::rowCount(const QModelIndex& Parent) const
    { return Parent.isValid() && Parent.column() ? 0 : int(node(Parent)->children_.size()); }
    QJsonObject DirectoryTreeModel::Entry(const QModelIndex& Index) const
    { const auto* n = node(Index); return n->entry_ < 0 ? QJsonObject{} : entries_[n->entry_].toObject(); }
    QModelIndex DirectoryTreeModel::ForEntry(int Row) const
    { return Row >= 0 && std::size_t(Row) < entriesToNodes_.size() ? indexFor(entriesToNodes_[Row]) : QModelIndex{}; }
    void DirectoryTreeModel::SetEntries(QJsonArray Entries)
    {
        beginResetModel(); root_.children_.clear(); nodes_.clear(); order_.clear(); entries_ = std::move(Entries);
        entriesToNodes_.assign(std::size_t(entries_.size()), nullptr); QHash<QString, Node*> paths; paths.insert({}, &root_);
        for (int i = 0; i < entries_.size(); ++i)
        {
            const auto entry = entries_[i].toObject(); const auto path = entry["path"].toString();
            QStringList missing; auto at = path;
            while (!paths.contains(at.toCaseFolded()))
            { missing.push_back(at); const auto slash = at.lastIndexOf('/'); at = slash < 0 ? QString{} : at.left(slash); }
            auto* parent = paths.value(at.toCaseFolded());
            for (auto it = missing.crbegin(); it != missing.crend(); ++it)
            {
                auto n = std::make_unique<Node>(); n->path_ = *it; n->parent_ = parent;
                auto* ptr = n.get(); parent->children_.push_back(ptr); paths.insert(it->toCaseFolded(), ptr);
                parent->folder_ = true; nodes_.push_back(std::move(n)); parent = ptr;
            }
            if (parent == &root_) continue;
            parent->entry_ = i; parent->different_ = entry["status"] != "Equal";
            for (const auto* side : {"left", "right", "base"}) parent->folder_ |= entry[side].toObject()["directory"].toBool();
            entriesToNodes_[std::size_t(i)] = parent;
        }
        for (auto it = nodes_.rbegin(); it != nodes_.rend(); ++it)
            if ((*it)->different_) (*it)->parent_->different_ = true;
        const auto sort = [](Node* Parent) {
            auto& children = Parent->children_;
            std::sort(children.begin(), children.end(), [](const Node* A, const Node* B) {
                const bool a = A->folder_, b = B->folder_; if (a != b) return a;
                return QString::compare(A->path_, B->path_, Qt::CaseInsensitive) < 0;
            });
            for (std::size_t i = 0; i < children.size(); ++i) children[i]->row_ = int(i);
        };
        sort(&root_); for (const auto& n : nodes_) sort(n.get());
        std::vector<Node*> pending(root_.children_.rbegin(), root_.children_.rend());
        while (!pending.empty())
        {
            auto* n = pending.back(); pending.pop_back(); n->order_ = int(order_.size()); order_.push_back(n);
            pending.insert(pending.end(), n->children_.rbegin(), n->children_.rend());
        }
        SetFilter({}, 0); endResetModel();
    }
    void DirectoryTreeModel::SetFilter(const QString& Query, int Status)
    {
        visibleEntries_ = 0;
        for (const auto& n : nodes_)
        {
            const auto entry = n->entry_ < 0 ? QJsonObject{} : entries_[n->entry_].toObject();
            n->matched_ = n->entry_ >= 0 && DirectoryEntryMatches(entry, Query, Status);
            n->visible_ = n->matched_; if (n->matched_) ++visibleEntries_;
        }
        // Parents precede children in storage; a reverse pass retains ancestor context.
        for (auto it = nodes_.rbegin(); it != nodes_.rend(); ++it)
            if ((*it)->visible_) (*it)->parent_->visible_ = true;
    }
    QVariant DirectoryTreeModel::data(const QModelIndex& Index, int Role) const
    {
        if (!Index.isValid()) return {};
        const auto* n = node(Index);
        if (Role == VisibleRole) return n->visible_;
        if (Role == PathRole) return n->path_;
        if (Role == EntryRole) return n->entry_;
        const auto entry = Entry(Index);
        if (Role == Qt::UserRole) return entry;
        const auto side = entry[Index.column() < 3 ? "left" : "right"].toObject();
        const bool present = side["present"].toBool(); const auto column = Index.column() % 3;
        if (Role == Qt::FontRole) { QFont font; font.setBold(n->different_); font.setItalic(!present); return font; }
        if (Role == Qt::DecorationRole && !column && present)
            return qApp->style()->standardIcon(side["directory"].toBool() ? QStyle::SP_DirIcon : QStyle::SP_FileIcon);
        if (Role == Qt::ToolTipRole)
            return n->path_ + '\n' + DirectoryStatusName(entry["status"].toString()) +
                tr("\nLEFT↔RIGHT: %1 · LEFT↔BASE: %2 · RIGHT↔BASE: %3").arg(entry["leftRight"].toString(), entry["leftBase"].toString(), entry["rightBase"].toString());
        if (Role != Qt::DisplayRole && Role != Qt::AccessibleTextRole) return {};
        if (!column)
        {
            const auto path = present ? side["path"].toString(n->path_) : n->path_;
            return path.mid(path.lastIndexOf('/') + 1);
        }
        if (column == 1)
        {
            if (!present) return n->entry_ < 0 ? tr("상위 경로") : tr("없음");
            if (entry["status"] == "Equal" && n->different_) return tr("하위 항목 차이");
            return DirectoryStatusName(entry["status"].toString());
        }
        if (!present) return QString{};
        return side["directory"].toBool() ? tr("폴더") : tr("%1 B").arg(QLocale().toString(side["size"].toString().toULongLong()));
    }
    QVariant DirectoryTreeModel::headerData(int Section, Qt::Orientation Orientation, int Role) const
    {
        if (Orientation != Qt::Horizontal || Role != Qt::DisplayRole || Section < 0 || Section >= 6) return {};
        return QStringList{tr("이름"), tr("상태"), tr("크기")}[Section % 3];
    }
    QModelIndex DirectoryTreeModel::NextDifference(const QModelIndex& Current, int Direction) const
    {
        const int count = int(order_.size()); if (!count || !Direction) return {};
        const int current = Current.isValid() ? node(Current)->order_ : -1;
        for (int offset = 1; offset <= count; ++offset)
        {
            const auto row = current < 0 ? (Direction > 0 ? offset - 1 : count - offset) : (current + (Direction > 0 ? offset : -offset) + count) % count;
            const auto* n = order_[std::size_t(row)];
            if (n->matched_ && n->entry_ >= 0 && entries_[n->entry_].toObject()["status"] != "Equal") return indexFor(n);
        }
        return {};
    }
    void DirectoryTreeFilter::SetQuery(const QString& Query, int Status)
    {
        beginFilterChange(); static_cast<DirectoryTreeModel*>(sourceModel())->SetFilter(Query, Status);
        endFilterChange(Direction::Rows);
    }
    bool DirectoryTreeFilter::filterAcceptsRow(int Row, const QModelIndex& Parent) const
    { return sourceModel()->index(Row, 0, Parent).data(DirectoryTreeModel::VisibleRole).toBool(); }
}
