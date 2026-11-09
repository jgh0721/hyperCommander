#pragma once

/// 모듈
/// UIs
/// 기타 헤더파일
/// 고유 라이브러리
/// 공통 라이브러리
#include "commonLibs/columns/columnGrouping.hpp"
/// 외부 라이브러리
#include <QtitanGrid.h>
#include <QPointer>
#include <QTimer>
/// 선언 헤더파일들
/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )

namespace hc::columns
{
    inline QString NativeGroupKey(Qtitan::GridModelController* Controller, const Qtitan::GridModelGroup* Group)
    {
        QStringList Parts;
        for (int Depth = 0; Group && Depth < 3; ++Depth) {
            Parts.prepend(Group->_groupValue.toString());
            Group = Group->parentIndex >= 0 && Group->parentIndex < Controller->groupRows().size()
                ? &Controller->groupRows()[Group->parentIndex] : nullptr;
        }
        return GroupPathKey(Parts);
    }
    // The timer reads only already published model values, never storage providers.
    // Painting performs a bounded ancestry lookup in this cache.
    class GroupCaptionCache final : public QObject
    {
    public:
        explicit GroupCaptionCache(Qtitan::GridBandedTableView* View) : QObject(View), view_(View) {
            setObjectName("hc.groupCaptionCache"); timer_.setSingleShot(true);
            connect(&timer_, &QTimer::timeout, this, [this] { rebuild(); });
            connect(View, &Qtitan::GridViewBase::groupingChanged, this, [this] { timer_.start(); });
            const QPointer<GroupCaptionCache> Self(this);
            View->setGroupCaptionProvider([Self](const Qtitan::GridRow& Row) {
                if (!Self) return Row.groupText().toString();
                const auto Key = NativeGroupKey(Self->view_->modelController(), Row.modelGroup());
                return " " + Self->summaries_.value(Key).Caption(Row.groupText().toString(), Self->options_);
            });
        }
        void Configure(const QVector<int>& Columns, const ProfileGrouping& Options) {
            columns_ = Columns; options_ = Options;
            if (model_ != view_->model()) {
                for (const auto& Connection : connections_) disconnect(Connection);
                connections_.clear(); model_ = view_->model();
                if (model_) {
                    const auto Schedule = [this] { timer_.start(); };
                    connections_.append(connect(model_, &QAbstractItemModel::modelReset, this, Schedule));
                    connections_.append(connect(model_, &QAbstractItemModel::rowsInserted, this, Schedule));
                    connections_.append(connect(model_, &QAbstractItemModel::rowsRemoved, this, Schedule));
                    connections_.append(connect(model_, &QAbstractItemModel::layoutChanged, this, Schedule));
                    connections_.append(connect(model_, &QAbstractItemModel::dataChanged, this,
                        [this](const QModelIndex&, const QModelIndex&, const QList<int>& Roles) {
                            if (Roles.isEmpty() || Roles.contains(GroupKeyRole) || Roles.contains(GroupSizeRole)
                                || Roles.contains(GroupLatestRole) || Roles.contains(Qt::DisplayRole)) timer_.start();
                        }));
                }
            }
            timer_.start();
        }
    private:
        void rebuild() {
            summaries_.clear();
            if (model_ && !columns_.isEmpty()) for (int Row = 0; Row < model_->rowCount(); ++Row) {
                const auto Index = model_->index(Row, 0);
                if (!Index.data(GroupEntryRole).toBool()) continue;
                const auto Size = Index.data(GroupSizeRole); const auto Date = Index.data(GroupLatestRole).toDateTime();
                QStringList Parts;
                for (const int Column : columns_) {
                    Parts.append(model_->index(Row, Column).data(GroupKeyRole).toString());
                    summaries_[GroupPathKey(Parts)].Add(Size, Date);
                }
            }
            if (view_ && view_->grid()) view_->grid()->viewport()->update();
        }
        QPointer<Qtitan::GridBandedTableView> view_;
        QPointer<QAbstractItemModel> model_;
        QVector<QMetaObject::Connection> connections_;
        QVector<int> columns_;
        ProfileGrouping options_;
        QHash<QString, GroupSummary> summaries_;
        QTimer timer_;
    };
    inline void ConfigureGroupCaptions(Qtitan::GridBandedTableView* View, const QVector<int>& Columns, const ProfileGrouping& Options)
    {
        auto* Cache = static_cast<GroupCaptionCache*>(View->findChild<QObject*>("hc.groupCaptionCache", Qt::FindDirectChildrenOnly));
        if (!Cache) Cache = new GroupCaptionCache(View);
        Cache->Configure(Columns, Options);
    }
    // Product and settings preview must use the same independent group roles.
    inline void ApplyNativeGroup(Qtitan::GridBandedTableView* View, Qtitan::GridColumn* Column,
        const GroupingState& State, int Level)
    {
        Column->dataBinding()->setGroupingRoles(GroupKeyRole, GroupOrderRole, GroupTextRole);
        Column->setGroupIndex(Level);
        View->modelController()->setGroupOrder(Column->dataBinding(), Level,
            State.order_ == Qt::AscendingOrder ? Qtitan::SortAscending : Qtitan::SortDescending, false, true);
    }
}
