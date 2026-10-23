#include "stdafx.h"
#include "directoryTreeModel.hpp"
#include "directoryView.hpp"
#include <QApplication>
#include <QStyle>
#include <QFont>
#include <QLocale>
#include <QJsonDocument>
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
    void DirectoryTreeModel::SetEntries(DirectoryEntries Entries)
    {
        beginResetModel(); root_.children_.clear(); nodes_.clear(); order_.clear(); entries_ = std::move(Entries);
        entriesToNodes_.assign(std::size_t(entries_.size()), nullptr); paths_.clear(); auto& paths = paths_; paths.insert({}, &root_);
        for (int i = 0; i < entries_.size(); ++i)
        {
            const auto& entry = entries_.at(i); const auto path = entry.path_;
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
            parent->entry_ = i; parent->different_ = !DirectoryContentEqual(entry.Status());
            for (const auto& side : entry.sides_) parent->folder_ |= side.directory_;
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
    void DirectoryTreeModel::ApplyProgress(const DirectoryProgress& Progress)
    {
        if (paths_.isEmpty()) paths_.insert({}, &root_);
        QHash<Node*, std::vector<Node*>> additions; std::vector<Node*> parents;
        QSet<Node*> changed;
        for (const auto& row : Progress.rows_)
        {
            auto at = row.path_; QStringList missing;
            while (!paths_.contains(at.toCaseFolded()))
            { missing.push_back(at); const auto slash = at.lastIndexOf('/'); at = slash < 0 ? QString{} : at.left(slash); }
            auto* parent = paths_.value(at.toCaseFolded());
            for (auto it = missing.crbegin(); it != missing.crend(); ++it)
            {
                auto item = std::make_unique<Node>(); item->path_ = *it; item->parent_ = parent;
                item->order_ = int(order_.size()); auto* added = item.get(); order_.push_back(added);
                if (!additions.contains(parent)) parents.push_back(parent);
                additions[parent].push_back(added);
                if (!parent->folder_) { parent->folder_ = true; changed.insert(parent); }
                paths_.insert(it->toCaseFolded(), added); nodes_.push_back(std::move(item)); parent = added;
            }
            if (parent == &root_) continue;
            if (parent->entry_ < 0)
            { parent->entry_ = int(entries_.size()); entries_.append(row); entriesToNodes_.push_back(parent); ++visibleEntries_; }
            else if (Progress.side_ >= 0) entries_[parent->entry_].sides_[Progress.side_] = row.sides_[Progress.side_];
            else entries_[parent->entry_] = row;
            const auto& entry = entries_.at(parent->entry_); parent->matched_ = true;
            parent->different_ = !DirectoryContentEqual(entry.Status());
            for (const auto& side : entry.sides_) parent->folder_ |= side.directory_;
            changed.insert(parent);
            if (parent->different_) for (auto* ancestor = parent->parent_; ancestor && !ancestor->different_; ancestor = ancestor->parent_)
            { ancestor->different_ = true; changed.insert(ancestor); }
        }
        // Parents are discovered before descendants. Publish each contiguous sibling
        // batch once so expanded views do not relayout for every individual file.
        for (auto* parent : parents)
        {
            const auto& children = additions.value(parent); const int first = int(parent->children_.size());
            beginInsertRows(indexFor(parent), first, first + int(children.size()) - 1);
            for (auto* child : children) { child->row_ = int(parent->children_.size()); parent->children_.push_back(child); changed.remove(child); }
            endInsertRows();
        }
        QHash<Node*, std::pair<int, int>> ranges;
        for (auto* item : changed) if (item != &root_)
        {
            auto it = ranges.find(item->parent_);
            if (it == ranges.end()) ranges.insert(item->parent_, {item->row_, item->row_});
            else { it->first = std::min(it->first, item->row_); it->second = std::max(it->second, item->row_); }
        }
        const QList<int> roles{Qt::DisplayRole, Qt::AccessibleTextRole, Qt::FontRole, Qt::DecorationRole, Qt::ToolTipRole, Qt::UserRole};
        for (auto it = ranges.cbegin(); it != ranges.cend(); ++it)
            emit dataChanged(index(it->first, 0, indexFor(it.key())), index(it->second, 5, indexFor(it.key())), roles);
    }
    void DirectoryTreeModel::FinishProgress()
    {
        std::vector<bool> previous; previous.reserve(nodes_.size());
        for (const auto& item : nodes_)
        { previous.push_back(item->different_); item->different_ = item->entry_ >= 0 && !DirectoryContentEqual(entries_.at(item->entry_).Status()); }
        root_.different_ = false;
        for (auto it = nodes_.rbegin(); it != nodes_.rend(); ++it) if ((*it)->different_) (*it)->parent_->different_ = true;
        QHash<Node*, std::pair<int, int>> ranges;
        for (std::size_t i = 0; i < nodes_.size(); ++i) if (previous[i] != nodes_[i]->different_)
        {
            auto* item = nodes_[i].get(); auto it = ranges.find(item->parent_);
            if (it == ranges.end()) ranges.insert(item->parent_, {item->row_, item->row_});
            else { it->first = std::min(it->first, item->row_); it->second = std::max(it->second, item->row_); }
        }
        for (auto it = ranges.cbegin(); it != ranges.cend(); ++it)
            emit dataChanged(index(it->first, 0, indexFor(it.key())), index(it->second, 5, indexFor(it.key())), {Qt::DisplayRole, Qt::AccessibleTextRole, Qt::FontRole});
    }
    void DirectoryTreeModel::SetFilter(const QString& Query, int Status)
    {
        visibleEntries_ = 0;
        for (const auto& n : nodes_)
        {
            n->matched_ = n->entry_ >= 0 && DirectoryEntryMatches(entries_.at(n->entry_), Query, Status);
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
        if (Role == Qt::UserRole) return Entry(Index);
        if (Role != Qt::FontRole && Role != Qt::DecorationRole && Role != Qt::ToolTipRole && Role != Qt::DisplayRole && Role != Qt::AccessibleTextRole) return {};
        const DirectoryRow empty; const auto& entry = n->entry_ >= 0 ? entries_.at(n->entry_) : empty;
        const auto& side = entry.sides_[Index.column() < 3 ? 0 : 1];
        const bool present = side.present_; const auto column = Index.column() % 3;
        if (Role == Qt::FontRole) { QFont font; font.setBold(n->different_); font.setItalic(!present); return font; }
        if (Role == Qt::DecorationRole)
        {
            if (column || !present) return {};
            if (iconStyle_ != qApp->style() || iconPalette_ != qApp->palette().cacheKey())
            {
                iconStyle_ = qApp->style(); iconPalette_ = qApp->palette().cacheKey();
                folderIcon_ = qApp->style()->standardIcon(QStyle::SP_DirIcon); fileIcon_ = qApp->style()->standardIcon(QStyle::SP_FileIcon);
            }
            return side.directory_ ? folderIcon_ : fileIcon_;
        }
        const auto status = entry.Status();
        if (Role == Qt::ToolTipRole)
            return n->path_ + '\n' + DirectoryStatusName(status) +
                tr("\nLEFT↔RIGHT: %1 · LEFT↔BASE: %2 · RIGHT↔BASE: %3").arg(entry.Status(1), entry.Status(2), entry.Status(3)) + '\n' + QString::fromUtf8(QJsonDocument(entry.errors_).toJson(QJsonDocument::Compact));
        if (!column)
        {
            const auto& path = present ? (side.path_.isEmpty() ? entry.path_ : side.path_) : n->path_;
            return path.mid(path.lastIndexOf('/') + 1);
        }
        if (column == 1)
        {
            if (status == "Pending") return tr("비교 대기");
            if (status == "Error") return tr("비교 불가");
            if (!present) return n->entry_ < 0 ? tr("상위 경로") : tr("없음");
            if (DirectoryContentEqual(status) && n->different_) return tr("하위 항목 차이");
            return DirectoryStatusName(status);
        }
        if (!present) return QString{};
        return side.directory_ ? tr("폴더") : tr("%1 B").arg(QLocale().toString(qulonglong(side.size_)));
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
            if (n->matched_ && n->entry_ >= 0 && !DirectoryContentEqual(entries_.at(n->entry_).Status())) return indexFor(n);
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
