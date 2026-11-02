#include "stdafx.h"
#include "directoryTreeModel.hpp"
#include "directoryView.hpp"
#include <QDir>
#include <QApplication>
#include <QStyle>
#include <QFont>
#include <QLocale>
#include <QJsonDocument>
#include <QIconEngine>
#include <QPainter>
#include <QPainterPath>
#include <QPixmapCache>
#include <algorithm>

namespace hc::compare
{
    namespace
    {
        enum class FolderState { Equal, Different, LeftOnly, RightOnly, Error, Pending, Conflict, Unknown };

        std::array<int, 4> directoryIssues(const DirectoryRow& Entry)
        {
            const auto Status = Entry.Status();
            std::array<int, 4> Issues{};
            if (Status == "Error") Issues[0] = 1;
            else if (Status == "Conflict" || Status == "TypeConflict") Issues[1] = 1;
            else if (Status == "Pending" || Status.isEmpty()) Issues[2] = 1;
            else if (!DirectoryContentEqual(Status)) Issues[3] = 1;
            return Issues;
        }

        // Paint at the requested device scale, including fractional DPI. The glyphs
        // are paths, not font characters, so 16px icons do not depend on font fallback.
        class FolderIconEngine final : public QIconEngine
        {
        public:
            explicit FolderIconEngine(FolderState State) : state_(State) {}
            QIconEngine* clone() const override { return new FolderIconEngine(state_); }
            QString key() const override { return QStringLiteral("HyperCompareFolderStatus"); }
            QString iconName() override { return QStringLiteral("hypercompare-folder-%1").arg(int(state_)); }
            bool isNull() override { return false; }
            void paint(QPainter* Painter, const QRect& Rect, QIcon::Mode Mode, QIcon::State) override
            {
                static const std::array<QColor, 8> Colors{QColor("#AED091"), QColor("#F3C568"), QColor("#8ED6E8"),
                    QColor("#C9B1EB"), QColor("#EF9991"), QColor("#A9B8CC"), QColor("#EF9991"), QColor("#B8BEC5")};
                const auto Fill = Colors[std::size_t(state_)];
                Painter->save();
                Painter->setRenderHint(QPainter::Antialiasing);
                const qreal Extent = std::min(Rect.width(), Rect.height());
                Painter->translate(Rect.x() + (Rect.width() - Extent) / 2, Rect.y() + (Rect.height() - Extent) / 2);
                Painter->scale(Extent / 16, Extent / 16);
                if (Mode == QIcon::Disabled) Painter->setOpacity(Painter->opacity() * 0.5);
                QPainterPath Folder;
                Folder.moveTo(1, 4); Folder.quadTo(1, 3, 2, 3); Folder.lineTo(6, 3);
                Folder.lineTo(7.5, 4.5); Folder.lineTo(14, 4.5); Folder.quadTo(15, 4.5, 15, 5.5);
                Folder.lineTo(15, 13); Folder.quadTo(15, 14, 14, 14); Folder.lineTo(2, 14);
                Folder.quadTo(1, 14, 1, 13); Folder.closeSubpath();
                Painter->setPen(QPen(Fill.darker(150), 0.7)); Painter->setBrush(Fill); Painter->drawPath(Folder);
                Painter->setPen(QPen(Fill.lighter(115), 0.8)); Painter->drawLine(QPointF(2, 5.5), QPointF(14, 5.5));
                Painter->setPen(QPen(QColor("#25313B"), 1.35, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
                Painter->setBrush(Qt::NoBrush);
                QPainterPath Glyph;
                switch (state_)
                {
                case FolderState::Equal:
                case FolderState::Different:
                    Glyph.moveTo(5.5, 8); Glyph.lineTo(10.5, 8);
                    Glyph.moveTo(5.5, 10.5); Glyph.lineTo(10.5, 10.5);
                    if (state_ == FolderState::Different) { Glyph.moveTo(9.5, 6.7); Glyph.lineTo(6.5, 11.8); }
                    break;
                case FolderState::LeftOnly:
                    Glyph.moveTo(6.5, 7); Glyph.lineTo(6.5, 11.5); Glyph.lineTo(10, 11.5); break;
                case FolderState::RightOnly:
                    Glyph.moveTo(6, 11.5); Glyph.lineTo(6, 7); Glyph.lineTo(8.5, 7);
                    Glyph.cubicTo(11, 7, 11, 9.5, 8.5, 9.5); Glyph.lineTo(6, 9.5);
                    Glyph.moveTo(8.5, 9.5); Glyph.lineTo(10.5, 11.5); break;
                case FolderState::Error:
                    Glyph.moveTo(8, 7); Glyph.lineTo(8, 9.5);
                    Painter->drawPoint(QPointF(8, 11.5)); break;
                case FolderState::Pending:
                    Painter->drawEllipse(QPointF(8, 9.3), 2.8, 2.8);
                    Glyph.moveTo(8, 7.5); Glyph.lineTo(8, 9.3); Glyph.lineTo(9.5, 10.2); break;
                case FolderState::Conflict:
                    Glyph.moveTo(6, 7.2); Glyph.lineTo(10, 11.3);
                    Glyph.moveTo(10, 7.2); Glyph.lineTo(6, 11.3); break;
                case FolderState::Unknown:
                    Glyph.moveTo(6.5, 7.8); Glyph.cubicTo(6.5, 6, 10, 6, 10, 7.8);
                    Glyph.cubicTo(10, 9, 8, 8.8, 8, 10); Painter->drawPoint(QPointF(8, 11.7)); break;
                }
                Painter->drawPath(Glyph); Painter->restore();
            }
            QPixmap pixmap(const QSize& Size, QIcon::Mode Mode, QIcon::State State) override
            { return scaledPixmap(Size, Mode, State, 1); }
            QPixmap scaledPixmap(const QSize& Size, QIcon::Mode Mode, QIcon::State State, qreal Scale) override
            {
                const auto CacheKey = QStringLiteral("hc-folder:%1:%2:%3x%4:%5").arg(int(state_)).arg(int(Mode))
                    .arg(Size.width()).arg(Size.height()).arg(Scale, 0, 'g', 16);
                QPixmap Pixmap;
                if (QPixmapCache::find(CacheKey, &Pixmap)) return Pixmap;
                Pixmap = QPixmap(Size * Scale); Pixmap.fill(Qt::transparent);
                QPainter Painter(&Pixmap); paint(&Painter, QRect(QPoint{}, Pixmap.size()), Mode, State); Painter.end();
                Pixmap.setDevicePixelRatio(Scale); QPixmapCache::insert(CacheKey, Pixmap); return Pixmap;
            }
        private:
            FolderState state_;
        };

        QIcon folderStatusIcon(FolderState State)
        {
            // A fixed, shared set avoids per-row allocations and keeps icon cache keys stable.
            static const auto Icons = [] {
                std::array<QIcon, 8> Result;
                for (std::size_t I = 0; I < Result.size(); ++I) Result[I] = QIcon(new FolderIconEngine(FolderState(I)));
                return Result;
            }();
            return Icons[std::size_t(State)];
        }
    }
    DirectoryTreeModel::Node* DirectoryTreeModel::node(const QModelIndex& Index) const
    { return Index.isValid() ? static_cast<Node*>(Index.internalPointer()) : const_cast<Node*>(&root_); }
    QModelIndex DirectoryTreeModel::indexFor(const Node* Item) const
    { return Item && Item != &root_ ? createIndex(Item->row_, 0, const_cast<Node*>(Item)) : QModelIndex{}; }
    QModelIndex DirectoryTreeModel::index(int Row, int Column, const QModelIndex& Parent) const
    {
        if (Row < 0 || Column < 0 || Column >= columnCount() || (Parent.isValid() && Parent.column())) return {};
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
        streaming_ = false; beginResetModel(); root_.children_.clear(); nodes_.clear(); order_.clear(); entries_ = std::move(Entries);
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
            parent->entry_ = i; parent->issues_ = directoryIssues(entry);
            for (const auto& side : entry.sides_) parent->folder_ |= side.directory_ && !side.reparse_;
            entriesToNodes_[std::size_t(i)] = parent;
        }
        root_.issues_ = {};
        for (auto It = nodes_.rbegin(); It != nodes_.rend(); ++It)
        {
            auto* Item = It->get();
            Item->different_ = Item->issues_ != std::array<int, 4>{};
            for (std::size_t I = 0; I < Item->issues_.size(); ++I) Item->parent_->issues_[I] += Item->issues_[I];
        }
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
        const bool WasStreaming = streaming_; streaming_ = true;
        if (paths_.isEmpty()) paths_.insert({}, &root_);
        QHash<Node*, std::vector<Node*>> additions; std::vector<Node*> parents;
        QSet<Node*> changed;
        if (!WasStreaming) for (const auto& Item : nodes_) if (Item->folder_) changed.insert(Item.get());
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
            const auto PreviousIssues = parent->entry_ < 0 ? std::array<int, 4>{} : directoryIssues(entries_.at(parent->entry_));
            if (parent->entry_ < 0)
            { parent->entry_ = int(entries_.size()); entries_.append(row); entriesToNodes_.push_back(parent); ++visibleEntries_; }
            else if (Progress.side_ == -2)
            {
                for (int side = 0; side < 3; ++side) if (row.sides_[side].present_) entries_[parent->entry_].sides_[side] = row.sides_[side];
            }
            else if (Progress.side_ >= 0) entries_[parent->entry_].sides_[Progress.side_] = row.sides_[Progress.side_];
            else entries_[parent->entry_] = row;
            const auto& entry = entries_.at(parent->entry_); parent->matched_ = true;
            const auto CurrentIssues = directoryIssues(entry);
            for (const auto& side : entry.sides_) parent->folder_ |= side.directory_ && !side.reparse_;
            changed.insert(parent);
            if (PreviousIssues != CurrentIssues) for (auto* Ancestor = parent; Ancestor; Ancestor = Ancestor->parent_)
            {
                for (std::size_t I = 0; I < CurrentIssues.size(); ++I) Ancestor->issues_[I] += CurrentIssues[I] - PreviousIssues[I];
                Ancestor->different_ = Ancestor->issues_ != std::array<int, 4>{}; changed.insert(Ancestor);
            }
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
            emit dataChanged(index(it->first, 0, indexFor(it.key())), index(it->second, columnCount() - 1, indexFor(it.key())), roles);
    }
    bool DirectoryTreeModel::AwaitingChildren(const QModelIndex& Index) const
    { return streaming_ && Index.isValid() && node(Index)->folder_ && node(Index)->children_.empty(); }
    bool DirectoryTreeModel::hasChildren(const QModelIndex& Parent) const
    { return !(Parent.isValid() && Parent.column()) && (rowCount(Parent) > 0 || AwaitingChildren(Parent)); }
    bool DirectoryTreeFilter::hasChildren(const QModelIndex& Parent) const
    {
        const auto* model = static_cast<DirectoryTreeModel*>(sourceModel());
        return model && (QSortFilterProxyModel::hasChildren(Parent) ||
            (!(Parent.isValid() && Parent.column()) && model->AwaitingChildren(mapToSource(Parent))));
    }
    void DirectoryTreeModel::FinishProgress()
    {
        streaming_ = false;
        std::vector<bool> previous; previous.reserve(nodes_.size());
        for (const auto& item : nodes_)
        { previous.push_back(item->different_); item->different_ = item->entry_ >= 0 && !DirectoryContentEqual(entries_.at(item->entry_).Status()); }
        // Sort existing nodes without resetting the model or losing persistent
        // selections/expanded branches. Preserve the established folder-first order.
        emit layoutAboutToBeChanged({}, QAbstractItemModel::VerticalSortHint);
        const auto before = persistentIndexList();
        const auto sort = [](Node* Parent) {
            auto& children = Parent->children_;
            std::sort(children.begin(), children.end(), [](const Node* A, const Node* B) {
                if (A->folder_ != B->folder_) return A->folder_;
                return QString::compare(A->path_, B->path_, Qt::CaseInsensitive) < 0;
            });
            for (std::size_t i = 0; i < children.size(); ++i) children[i]->row_ = int(i);
        };
        sort(&root_); for (const auto& item : nodes_) sort(item.get());
        QModelIndexList after; after.reserve(before.size());
        for (const auto& index : before) after.append(createIndex(node(index)->row_, index.column(), node(index)));
        changePersistentIndexList(before, after);
        emit layoutChanged({}, QAbstractItemModel::VerticalSortHint);
        // Keep difference navigation in visible tree order after streamed inserts.
        order_.clear(); std::vector<Node*> pending(root_.children_.rbegin(), root_.children_.rend());
        while (!pending.empty())
        {
            auto* item = pending.back(); pending.pop_back(); item->order_ = int(order_.size()); order_.push_back(item);
            pending.insert(pending.end(), item->children_.rbegin(), item->children_.rend());
        }
        root_.different_ = false;
        for (auto it = nodes_.rbegin(); it != nodes_.rend(); ++it) if ((*it)->different_) (*it)->parent_->different_ = true;
        QHash<Node*, std::pair<int, int>> ranges;
        for (std::size_t i = 0; i < nodes_.size(); ++i) if (previous[i] != nodes_[i]->different_ || nodes_[i]->folder_)
        {
            auto* item = nodes_[i].get(); auto it = ranges.find(item->parent_);
            if (it == ranges.end()) ranges.insert(item->parent_, {item->row_, item->row_});
            else { it->first = std::min(it->first, item->row_); it->second = std::max(it->second, item->row_); }
        }
        for (auto it = ranges.cbegin(); it != ranges.cend(); ++it)
            emit dataChanged(index(it->first, 0, indexFor(it.key())), index(it->second, columnCount() - 1, indexFor(it.key())), {Qt::DisplayRole, Qt::AccessibleTextRole, Qt::FontRole, Qt::DecorationRole, Qt::ToolTipRole});
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
        const auto& side = entry.sides_[Index.column() / 3];
        const bool present = side.present_; const auto column = Index.column() % 3;
        const auto status = entry.Status();
        auto Folder = FolderState::Unknown;
        if (n->issues_[0]) Folder = FolderState::Error;
        else if (n->issues_[1]) Folder = FolderState::Conflict;
        else if (n->issues_[2] || (streaming_ && !side.reparse_)) Folder = FolderState::Pending;
        else if (status == "LeftOnly") Folder = FolderState::LeftOnly;
        else if (status == "RightOnly") Folder = FolderState::RightOnly;
        else if (n->issues_[3]) Folder = FolderState::Different;
        else if (DirectoryContentEqual(status)) Folder = FolderState::Equal;
        auto DisplayStatus = DirectoryStatusName(status);
        if (present && side.directory_)
        {
            if (Folder == FolderState::Error && status != "Error") DisplayStatus = tr("하위 항목 비교 불가");
            else if (Folder == FolderState::Conflict && status != "Conflict" && status != "TypeConflict") DisplayStatus = tr("하위 항목 충돌");
            else if (Folder == FolderState::Pending) DisplayStatus = tr("비교 대기");
            else if (Folder == FolderState::Different && DirectoryContentEqual(status)) DisplayStatus = tr("하위 항목 차이");
        }
        if (Role == Qt::FontRole) { QFont font; font.setBold(n->different_); font.setItalic(!present); return font; }
        if (Role == Qt::DecorationRole)
        {
            if (column || !present) return {};
            if (side.directory_) return folderStatusIcon(Folder);
            if (iconStyle_ != qApp->style() || iconPalette_ != qApp->palette().cacheKey())
            {
                iconStyle_ = qApp->style(); iconPalette_ = qApp->palette().cacheKey();
                fileIcon_ = qApp->style()->standardIcon(QStyle::SP_FileIcon);
            }
            return fileIcon_;
        }
        if (Role == Qt::ToolTipRole)
            return QDir::toNativeSeparators(n->path_) + '\n' + DisplayStatus +
                tr("\n1↔2: %1 · 1↔3/BASE: %2 · 2↔3/BASE: %3").arg(entry.Status(1), entry.Status(2), entry.Status(3)) + (side.link_ ? tr("\n링크 대상(탐색하지 않음): ") + side.link_->target_ : QString{}) + '\n' + QString::fromUtf8(QJsonDocument(entry.errors_).toJson(QJsonDocument::Compact));
        if (!column)
        {
            const auto& path = present ? (side.path_.isEmpty() ? entry.path_ : side.path_) : n->path_;
            return path.mid(path.lastIndexOf('/') + 1);
        }
        if (column == 1)
        {
            if (present && side.directory_) return DisplayStatus;
            if (status == "Pending") return tr("비교 대기");
            if (status == "Error") return tr("비교 불가");
            if (!present) return n->entry_ < 0 ? tr("상위 경로") : tr("없음");
            return DisplayStatus;
        }
        if (!present) return QString{};
        if (side.reparse_) return tr("링크(읽기 전용)");
        return side.directory_ ? tr("폴더") : tr("%1 B").arg(QLocale().toString(qulonglong(side.size_)));
    }

    QVariant DirectoryTreeModel::headerData(int Section, Qt::Orientation Orientation, int Role) const
    {
        if (Orientation != Qt::Horizontal || Role != Qt::DisplayRole || Section < 0 || Section >= columnCount()) return {};
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
        beginFilterChange(); static_cast<DirectoryTreeModel*>(sourceModel())->SetFilter(QDir::fromNativeSeparators(Query), Status);
        endFilterChange(Direction::Rows);
    }
    bool DirectoryTreeFilter::filterAcceptsRow(int Row, const QModelIndex& Parent) const
    { return sourceModel()->index(Row, 0, Parent).data(DirectoryTreeModel::VisibleRole).toBool(); }
}
