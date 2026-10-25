#include "stdafx.h"
#include "archiveCompareView.hpp"
#include <QComboBox>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QSortFilterProxyModel>
#include <QTableView>
#include <QVBoxLayout>
#include <QHBoxLayout>

namespace hc::compare
{
    namespace
    {
        class ArchiveFilter final : public QSortFilterProxyModel
        {
        public:
            using QSortFilterProxyModel::QSortFilterProxyModel;
            void SetQuery(QString Query, bool Different) { beginFilterChange(); query_ = std::move(Query); different_ = Different; endFilterChange(Direction::Rows); }
        protected:
            bool filterAcceptsRow(int Row, const QModelIndex& Parent) const override
            {
                const auto item = sourceModel()->data(sourceModel()->index(Row, 0, Parent), Qt::UserRole).toJsonObject();
                return item["path"].toString().contains(query_, Qt::CaseInsensitive) && (!different_ || item["state"] != "Equal");
            }
        private:
            QString query_; bool different_ = false;
        };
    }
    QVariant ArchiveModel::data(const QModelIndex& Index, int Role) const
    {
        if (!Index.isValid() || Index.row() < 0 || Index.row() >= entries_.size() || Index.column() < 0 || Index.column() >= 4) return {};
        const auto item = entries_[Index.row()].toObject();
        if (Role == Qt::UserRole) return item;
        if (Role == Qt::UserRole + 1)
        {
            if (Index.column() == 0) return item["path"].toString();
            if (Index.column() == 1) return item["state"].toString();
            const auto entry = item[Index.column() == 2 ? "left" : "right"].toObject();
            return entry.isEmpty() ? qint64(-2) : entry["directory"].toBool() ? qint64(-1) : entry["size"].toString().toLongLong();
        }
        if (Role == Qt::ToolTipRole) return item["path"].toString();
        if (Role != Qt::DisplayRole && Role != Qt::AccessibleTextRole) return {};
        if (Index.column() == 0) return item["path"].toString();
        if (Index.column() == 1)
        {
            const auto state = item["state"].toString();
            return state == "Equal" ? tr("동일") : state == "Changed" ? tr("내용 다름") : state == "TypeChanged" ? tr("종류 다름") : state == "LeftOnly" ? tr("왼쪽만 존재") : tr("오른쪽만 존재");
        }
        const auto entry = item[Index.column() == 2 ? "left" : "right"].toObject();
        return entry.isEmpty() ? tr("없음") : entry["directory"].toBool() ? tr("폴더") : tr("%1 B").arg(QLocale().toString(entry["size"].toString().toULongLong()));
    }
    QVariant ArchiveModel::headerData(int Section, Qt::Orientation Orientation, int Role) const
    {
        if (Orientation == Qt::Horizontal && Role == Qt::DisplayRole && Section >= 0 && Section < 4) return QStringList{tr("내부 경로"), tr("상태"), "LEFT", "RIGHT"}[Section];
        return {};
    }
    ArchiveCompareView::ArchiveCompareView(const QJsonObject& Comparison, const QStringList& Paths, QWidget* Parent) : ExtensionView(Parent)
    {
        setObjectName("archiveCompareView"); auto* layout = new QVBoxLayout(this);
        auto* roots = new QLabel(Paths.join("\n"), this); roots->setTextFormat(Qt::PlainText); roots->setWordWrap(true); layout->addWidget(roots);
        auto* summary = new QLabel((Comparison["contentsEqual"].toBool() ? (Comparison["rawEqual"].toBool() ? tr("파일·내부 항목 동일") : tr("내부 항목 동일 · 압축 파일 바이트 다름")) : tr("내부 항목 내용 또는 구성 다름")) + tr(" · 읽기 전용"), this);
        summary->setObjectName("archiveSummary"); layout->addWidget(summary);
        auto* row = new QHBoxLayout; auto* search = new QLineEdit(this); search->setObjectName("archiveQuery"); search->setPlaceholderText(tr("내부 경로 검색")); search->setAccessibleName(tr("압축 내부 경로 검색"));
        auto* state = new QComboBox(this); state->setObjectName("archiveStatus"); state->addItems({tr("모든 항목"), tr("차이만")}); row->addWidget(search, 1); row->addWidget(state); layout->addLayout(row);
        auto* model = new ArchiveModel(Comparison["entries"].toArray(), this); auto* filter = new ArchiveFilter(this); filter->setSourceModel(model); filter->setSortRole(Qt::UserRole + 1);
        table_ = new QTableView(this); table_->setObjectName("archiveTable"); table_->setModel(filter); table_->setSelectionBehavior(QAbstractItemView::SelectRows); table_->setSelectionMode(QAbstractItemView::SingleSelection);
        table_->setEditTriggers(QAbstractItemView::NoEditTriggers); table_->verticalHeader()->hide(); table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
        table_->setColumnWidth(1, 150); table_->setColumnWidth(2, 140); table_->setColumnWidth(3, 140); table_->setSortingEnabled(true); layout->addWidget(table_, 1);
        connect(search, &QLineEdit::textChanged, this, [filter, search, state] { filter->SetQuery(search->text(), state->currentIndex() == 1); });
        connect(state, &QComboBox::currentIndexChanged, this, [filter, search, state] { filter->SetQuery(search->text(), state->currentIndex() == 1); });
    }
    void ArchiveCompareView::SetListFont(const QFont& Font)
    { table_->setFont(Font); table_->verticalHeader()->setDefaultSectionSize(QFontMetrics(Font).height() + 10); }
}
