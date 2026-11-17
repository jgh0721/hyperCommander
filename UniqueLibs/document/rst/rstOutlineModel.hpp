#pragma once

#include "rstOutlineService.hpp"
#include <QAbstractItemModel>
#include <QCoreApplication>
#include <algorithm>
#include <memory>

namespace hc::rst {
    // Immutable row metadata is prepared on a worker. QTreeView requests display
    // data only for visible rows; a large document never constructs widget items.
    class OutlineModel final : public QAbstractItemModel {
    public:
        static constexpr int LineRole = Qt::UserRole, PathRole = Qt::UserRole + 1;
        struct Node {
            QString name_, detail_, path_;
            int line_ = 0, parent_ = -1, row_ = 0, cutChildren_ = 0;
            QVector<int> children_;
            bool operator==(const Node&) const = default;
        };
        struct Rows {
            QVector<Node> nodes_;
            QVector<int> roots_, byLine_;
            bool operator==(const Rows&) const = default;
        };
        using Snapshot = std::shared_ptr<const Rows>;
        explicit OutlineModel(QObject* Parent = nullptr) : QAbstractItemModel(Parent) {}
        Snapshot Capture() const { return rows_; }
        static Snapshot Prepare(const QVector<OutlineSymbol>& Symbols, int MaxDepth, const QString& Filter) {
            auto Result = std::make_shared<Rows>();
            const auto Append = [&](auto&& Self, const QVector<OutlineSymbol>& Entries, int Parent, int Depth) -> QVector<int> {
                QVector<int> Children;
                for (const auto& Entry : Entries) {
                    const int Id = int(Result->nodes_.size());
                    const bool Cut = MaxDepth > 0 && Depth >= MaxDepth && !Entry.children.isEmpty();
                    Result->nodes_.append({Entry.name, Entry.detail, Entry.path, Entry.line, Parent, 0,
                        Cut ? int(Entry.children.size()) : 0, {}});
                    auto Nested = Cut ? QVector<int>{} : Self(Self, Entry.children, Id, Depth + 1);
                    const bool Match = Filter.isEmpty() || Entry.name.contains(Filter, Qt::CaseInsensitive)
                        || Entry.detail.contains(Filter, Qt::CaseInsensitive) || !Nested.isEmpty();
                    auto& Value = Result->nodes_[Id]; Value.children_ = std::move(Nested);
                    if (Match) {
                        Value.row_ = int(Children.size()); Children.append(Id);
                        if (Entry.line > 0) Result->byLine_.append(Id);
                    }
                }
                return Children;
            };
            Result->roots_ = Append(Append, Symbols, -1, 1);
            if (Symbols.isEmpty() && Filter.isEmpty()) {
                Result->nodes_.append({QCoreApplication::translate("RstDocumentView", "문서 제목이 없습니다."), {}, {}, 0, -1, 0, 0, {}});
                Result->roots_.append(0);
            }
            std::sort(Result->byLine_.begin(), Result->byLine_.end(), [&](int Left, int Right) {
                const auto A = Result->nodes_[Left].line_, B = Result->nodes_[Right].line_;
                return A != B ? A < B : Left < Right;
            });
            return Result;
        }
        void Publish(Snapshot Value) { beginResetModel(); rows_ = std::move(Value); endResetModel(); }
        QModelIndex IndexForLine(int Line) const {
            if (!rows_) return {};
            const auto It = std::upper_bound(rows_->byLine_.begin(), rows_->byLine_.end(), Line,
                [&](int Value, int Id) { return Value < rows_->nodes_[Id].line_; });
            if (It == rows_->byLine_.begin()) return {};
            const auto Id = *std::prev(It); return createIndex(rows_->nodes_[Id].row_, 0, quintptr(Id + 1));
        }
        QModelIndex index(int Row, int Column, const QModelIndex& Parent = {}) const override {
            if (!rows_ || Column != 0 || Row < 0 || Parent.column() > 0) return {};
            const auto& Children = Parent.isValid() ? rows_->nodes_[int(Parent.internalId()) - 1].children_ : rows_->roots_;
            if (Row >= Children.size()) return {};
            return createIndex(Row, Column, quintptr(Children[Row] + 1));
        }
        QModelIndex parent(const QModelIndex& Child) const override {
            if (!rows_ || !Child.isValid()) return {};
            const auto Parent = rows_->nodes_[int(Child.internalId()) - 1].parent_;
            return Parent < 0 ? QModelIndex{} : createIndex(rows_->nodes_[Parent].row_, 0, quintptr(Parent + 1));
        }
        int rowCount(const QModelIndex& Parent = {}) const override {
            if (!rows_ || Parent.column() > 0) return 0;
            return int(Parent.isValid() ? rows_->nodes_[int(Parent.internalId()) - 1].children_.size() : rows_->roots_.size());
        }
        int columnCount(const QModelIndex& = {}) const override { return 1; }
        Qt::ItemFlags flags(const QModelIndex& Index) const override {
            return Index.isValid() && Index.data(LineRole).toInt() > 0 ? Qt::ItemIsEnabled | Qt::ItemIsSelectable : Qt::NoItemFlags;
        }
        QVariant data(const QModelIndex& Index, int Role = Qt::DisplayRole) const override {
            if (!rows_ || !Index.isValid()) return {};
            const auto& Value = rows_->nodes_[int(Index.internalId()) - 1];
            if (Role == LineRole) return Value.line_;
            if (Role == PathRole) return Value.path_;
            if (Role == Qt::DisplayRole) {
                auto Name = Value.detail_.isEmpty() ? Value.name_ : QStringLiteral("%1 — %2").arg(Value.name_, Value.detail_);
                if (Value.cutChildren_) Name += QStringLiteral(" …");
                return Name;
            }
            if (Role == Qt::ToolTipRole) {
                return Value.cutChildren_ ? QCoreApplication::translate("RstDocumentView", "줄 %1 · 아래 %2개 항목은 표시 깊이 제한에 걸려 생략했습니다.").arg(Value.line_).arg(Value.cutChildren_)
                    : QCoreApplication::translate("RstDocumentView", "줄 %1").arg(Value.line_);
            }
            return {};
        }
    private:
        Snapshot rows_;
    };
}
