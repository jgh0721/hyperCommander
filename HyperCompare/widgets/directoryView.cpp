#include "stdafx.h"
#include "directoryView.hpp"
#include "directoryTreeModel.hpp"
#include "directoryMergeDialog.hpp"
#include <QDir>
#include <QLocale>
#include <QFontMetrics>
#include <QTableView>
#include <QTreeView>
#include <QSplitter>
#include <QScrollBar>
#include <QStyledItemDelegate>
#include <QScopedValueRollback>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QComboBox>
#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>

namespace hc::compare
{
    namespace
    {
        class TreeDelegate final : public QStyledItemDelegate
        {
        public:
            explicit TreeDelegate(QObject* Parent) : QStyledItemDelegate(Parent) {}
            QSize sizeHint(const QStyleOptionViewItem& Option, const QModelIndex& Index) const override
            {
                auto size = QStyledItemDelegate::sizeHint(Option, Index);
                size.setHeight(std::max(28, Option.fontMetrics.height() + 10)); return size;
            }
        };
        QString statusName(const QString& Status)
        {
            static const QMap<QString, QString> labels{{"Equal", QObject::tr("동일")}, {"Different", QObject::tr("다름")},
                {"LeftOnly", QObject::tr("왼쪽만 존재")}, {"RightOnly", QObject::tr("오른쪽만 존재")},
                {"TypeConflict", QObject::tr("파일/폴더 충돌")}, {"NameDifferent", QObject::tr("이름 다름")},
                {"LeftChanged", QObject::tr("왼쪽 수정")}, {"RightChanged", QObject::tr("오른쪽 수정")},
                {"LeftAdded", QObject::tr("왼쪽 추가")}, {"RightAdded", QObject::tr("오른쪽 추가")},
                {"LeftDeleted", QObject::tr("왼쪽 삭제")}, {"RightDeleted", QObject::tr("오른쪽 삭제")},
                {"BothDeleted", QObject::tr("양쪽 삭제")}, {"BothSame", QObject::tr("양쪽 동일 변경")}, {"Conflict", QObject::tr("충돌")}, {"MirrorExtra", QObject::tr("출력에만 존재")}};
            return labels.value(Status, Status);
        }
        bool file(const QJsonObject& Entry) { return Entry["present"].toBool() && !Entry["directory"].toBool(); }
    }
    bool DirectoryEntryMatches(const QJsonObject& Entry, const QString& Query, int Status)
    {
        const auto status = Entry["status"].toString();
        return Entry["path"].toString().contains(Query, Qt::CaseInsensitive) &&
            (Status == 0 || (Status == 1 && status != "Equal") || (Status == 2 && (status == "Conflict" || status == "TypeConflict")));
    }
    QString DirectoryStatusName(const QString& Status) { return statusName(Status); }
    void DirectoryModel::SetEntries(QJsonArray Entries) { beginResetModel(); entries_ = std::move(Entries); endResetModel(); }
    int DirectoryModel::rowCount(const QModelIndex& Parent) const { return Parent.isValid() ? 0 : int(entries_.size()); }
    int DirectoryModel::columnCount(const QModelIndex& Parent) const { return Parent.isValid() ? 0 : 5; }
    QJsonObject DirectoryModel::Entry(int Row) const { return Row >= 0 && Row < entries_.size() ? entries_[Row].toObject() : QJsonObject{}; }
    QVariant DirectoryModel::data(const QModelIndex& Index, int Role) const
    {
        if (!Index.isValid() || Index.row() < 0 || Index.row() >= rowCount() || Index.column() < 0 || Index.column() >= columnCount()) return {};
        const auto item = Entry(Index.row());
        if (Role == Qt::UserRole) return item;
        if (Role == Qt::ToolTipRole)
            return item["path"].toString() + '\n' + statusName(item["status"].toString()) + '\n' +
                tr("LEFT↔RIGHT: %1 · LEFT↔BASE: %2 · RIGHT↔BASE: %3").arg(item["leftRight"].toString(), item["leftBase"].toString(), item["rightBase"].toString());
        if (Role != Qt::DisplayRole && Role != Qt::AccessibleTextRole) return {};
        if (Index.column() == 0) return item["path"].toString();
        if (Index.column() == 1) return statusName(item["status"].toString());
        const auto side = item[QStringList{"left", "right", "base"}[Index.column() - 2]].toObject();
        if (!side["present"].toBool()) return tr("없음");
        return side["directory"].toBool() ? tr("폴더") : tr("%1 B").arg(QLocale().toString(side["size"].toString().toULongLong()));
    }
    QVariant DirectoryModel::headerData(int Section, Qt::Orientation Orientation, int Role) const
    {
        if (Role != Qt::DisplayRole) return {};
        if (Orientation == Qt::Vertical) return Section + 1;
        return Section >= 0 && Section < 5 ? QStringList{tr("경로"), tr("상태"), "LEFT", "RIGHT", "BASE"}[Section] : QVariant{};
    }
    void DirectoryFilter::SetQuery(QString Query, int Status)
    { beginFilterChange(); query_ = std::move(Query); status_ = Status; endFilterChange(Direction::Rows); }
    bool DirectoryFilter::filterAcceptsRow(int Row, const QModelIndex& Parent) const
    {
        const auto item = sourceModel()->data(sourceModel()->index(Row, 0, Parent), Qt::UserRole).toJsonObject();
        return DirectoryEntryMatches(item, query_, status_);
    }
    DirectoryView::DirectoryView(QWidget* Parent) : QWidget(Parent)
    {
        setObjectName("directoryView"); auto* box = new QVBoxLayout(this);
        roots_ = new QLabel(this); roots_->setTextFormat(Qt::PlainText); roots_->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
        roots_->setWordWrap(true); box->addWidget(roots_);
        auto* searchRow = new QHBoxLayout(); auto* query = new QLineEdit(this); query->setObjectName("directoryQuery");
        query->setPlaceholderText(tr("경로에서 찾기")); query->setAccessibleName(tr("디렉터리 경로 검색")); query->setClearButtonEnabled(true);
        auto* status = new QComboBox(this); status->setObjectName("directoryStatusFilter"); status->setAccessibleName(tr("디렉터리 상태 필터"));
        status->addItems({tr("모든 항목"), tr("차이만"), tr("충돌만")});
        openButton_ = new QPushButton(tr("파일 비교 열기"), this); openButton_->setObjectName("directoryOpenFile");
        openButton_->setToolTip(tr("양쪽에 있는 파일을 현재 디스크 내용으로 비교합니다. BASE 파일이 있으면 함께 엽니다."));
        auto* merge = new QPushButton(tr("병합 계획…"), this); merge->setObjectName("directoryMergePlan");
        connect(merge, &QPushButton::clicked, this, [this] {
            auto* dialog = new DirectoryMergeDialog(request_, comparison_, this); dialog->setAttribute(Qt::WA_DeleteOnClose); dialog->show();
        });
        searchRow->addWidget(merge);
        viewMode_ = new QComboBox(this); viewMode_->setObjectName("directoryViewMode");
        viewMode_->setAccessibleName(tr("디렉터리 표시 방식")); viewMode_->addItems({tr("좌우 폴더 트리"), tr("경로 목록")});
        searchRow->addWidget(viewMode_);
        searchRow->addWidget(query, 1); searchRow->addWidget(status); searchRow->addWidget(openButton_); box->addLayout(searchRow);
        model_ = new DirectoryModel(this); model_->setObjectName("directoryModel"); filter_ = new DirectoryFilter(this); filter_->setSourceModel(model_);
        filter_->setSortCaseSensitivity(Qt::CaseInsensitive);
        table_ = new QTableView(this); table_->setObjectName("directoryTable"); table_->setAccessibleName(tr("디렉터리 비교 결과"));
        table_->setModel(filter_); table_->setSelectionBehavior(QAbstractItemView::SelectRows); table_->setSelectionMode(QAbstractItemView::SingleSelection);
        table_->setAlternatingRowColors(true); table_->setSortingEnabled(true); table_->sortByColumn(0, Qt::AscendingOrder);
        table_->verticalHeader()->setVisible(false); table_->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
        table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
        for (int column = 1; column < 5; ++column) table_->setColumnWidth(column, 140);
        box->addWidget(table_, 1); table_->hide();
        treeModel_ = new DirectoryTreeModel(this); treeModel_->setObjectName("directoryTreeModel");
        treeFilter_ = new DirectoryTreeFilter(this); treeFilter_->setSourceModel(treeModel_);
        treeBox_ = new QSplitter(Qt::Horizontal, this); treeBox_->setObjectName("directoryTreeSplitter"); treeBox_->setChildrenCollapsible(false);
        for (int side = 0; side < 2; ++side)
        {
            auto* pane = new QWidget(treeBox_); auto* column = new QVBoxLayout(pane); column->setContentsMargins(0, 0, 0, 0);
            auto* label = new QLabel(pane); treeRoots_[side] = label; label->setTextFormat(Qt::PlainText);
            label->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
            label->setMinimumWidth(0); label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed); column->addWidget(label);
            auto* tree = new QTreeView(pane); trees_[side] = tree;
            tree->setObjectName(side ? "directoryRightTree" : "directoryLeftTree");
            tree->setAccessibleName(side ? tr("오른쪽 폴더 트리") : tr("왼쪽 폴더 트리")); tree->setModel(treeFilter_);
            tree->setUniformRowHeights(true); tree->setItemDelegate(new TreeDelegate(tree));
            tree->setSelectionMode(QAbstractItemView::SingleSelection); tree->setSelectionBehavior(QAbstractItemView::SelectRows);
            tree->setEditTriggers(QAbstractItemView::NoEditTriggers); tree->setAlternatingRowColors(true);
            tree->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel); tree->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
            tree->setTreePosition(side * 3); tree->header()->setStretchLastSection(false);
            for (int i = 0; i < 6; ++i) tree->setColumnHidden(i, i / 3 != side);
            tree->header()->setSectionResizeMode(side * 3, QHeaderView::Stretch);
            tree->setColumnWidth(side * 3 + 1, 140); tree->setColumnWidth(side * 3 + 2, 100);
            column->addWidget(tree, 1);
            connect(tree, &QTreeView::activated, this, [this] { OpenCurrent(); });
        }
        for (int side = 0; side < 2; ++side)
        {
            auto* tree = trees_[side]; auto* other = trees_[1 - side];
            const auto expansion = [this, other](const QModelIndex& Index, bool Expanded) {
                if (syncing_) return; QScopedValueRollback guard(syncing_, true); other->setExpanded(Index, Expanded);
            };
            connect(tree, &QTreeView::expanded, this, [expansion](const QModelIndex& Index) { expansion(Index, true); });
            connect(tree, &QTreeView::collapsed, this, [expansion](const QModelIndex& Index) { expansion(Index, false); });
            connect(tree->verticalScrollBar(), &QScrollBar::valueChanged, other->verticalScrollBar(), &QScrollBar::setValue);
            connect(tree->selectionModel(), &QItemSelectionModel::currentChanged, this, [this, other, side](const QModelIndex& Index) {
                if (syncing_) return;
                { QScopedValueRollback guard(syncing_, true); other->setCurrentIndex(Index.isValid() ? Index.siblingAtColumn((1 - side) * 3) : QModelIndex{}); }
                updateSelection();
            });
        }
        connect(viewMode_, &QComboBox::currentIndexChanged, this, [this](int Index) { switchView(Index == 0); });
        box->addWidget(treeBox_, 1); summary_ = new QLabel(this); summary_->setTextFormat(Qt::PlainText); box->addWidget(summary_);
        const auto change = [this, query, status] {
            filter_->SetQuery(query->text(), status->currentIndex()); treeFilter_->SetQuery(query->text(), status->currentIndex());
            if ((!query->text().isEmpty() || status->currentIndex()) && treeModel_->VisibleEntries() <= 2000)
                for (auto* tree : trees_) tree->expandAll();
            updateSelection();
        };
        connect(query, &QLineEdit::textChanged, this, change); connect(status, &QComboBox::currentIndexChanged, this, change);
        connect(table_->selectionModel(), &QItemSelectionModel::currentChanged, this, [this] { updateSelection(); });
        connect(table_, &QTableView::doubleClicked, this, [this] { OpenCurrent(); });
        connect(openButton_, &QPushButton::clicked, this, [this] { OpenCurrent(); });
        updateSelection();
    }
    void DirectoryView::SetComparison(const Request& Input, const Result& Value)
    {
        request_ = Input; comparison_ = Value; model_->SetEntries(Value.entries_); table_->setColumnHidden(4, Input.base_.isEmpty());
        treeModel_->SetEntries(Value.entries_);
        treeFilter_->SetQuery(findChild<QLineEdit*>("directoryQuery")->text(), findChild<QComboBox*>("directoryStatusFilter")->currentIndex());
        for (int side = 0; side < 2; ++side)
        {
            const auto path = side ? Input.file2_ : Input.file1_;
            treeRoots_[side]->setText((side ? tr("RIGHT  ") : tr("LEFT  ")) + path); treeRoots_[side]->setToolTip(path);
            trees_[side]->expandToDepth(0);
        }
        updateRoots(); updateSelection();
    }
    void DirectoryView::updateRoots()
    {
        QStringList lines;
        if (!treeMode_) lines << tr("LEFT  %1").arg(request_.file1_) << tr("RIGHT  %1").arg(request_.file2_);
        if (!request_.base_.isEmpty()) lines << tr("BASE  %1").arg(request_.base_);
        lines << ((comparison_.directoryLevel_ == "metadata" ? tr("메타데이터 비교 — 크기·수정 시각만 검사, 내용 동일을 보장하지 않음") : tr("파일 내용 비교")) +
            (comparison_.directoryFiltered_ ? tr(" · 포함/제외 필터 적용") : QString{}));
        roots_->setText(lines.join('\n'));
    }
    void DirectoryView::SetListFont(const QFont& Font)
    {
        table_->setFont(Font); table_->verticalHeader()->setDefaultSectionSize(std::max(28, QFontMetrics(Font).height() + 10));
        for (auto* tree : trees_) { tree->setFont(Font); tree->doItemsLayout(); }
    }
    QJsonObject DirectoryView::currentEntry() const
    {
        return treeMode_ ? treeModel_->Entry(treeFilter_->mapToSource(trees_[0]->currentIndex())) :
            model_->Entry(filter_->mapToSource(table_->currentIndex()).row());
    }
    void DirectoryView::selectTree(const QModelIndex& Index)
    {
        if (!Index.isValid()) return;
        for (auto parent = Index.parent(); parent.isValid(); parent = parent.parent())
            for (auto* tree : trees_) tree->setExpanded(parent, true);
        trees_[0]->setCurrentIndex(Index);
        for (int side = 0; side < 2; ++side) trees_[side]->scrollTo(Index.siblingAtColumn(side * 3), QAbstractItemView::EnsureVisible);
    }
    void DirectoryView::switchView(bool Tree)
    {
        if (Tree == treeMode_) return;
        const int row = treeMode_ ? trees_[0]->currentIndex().data(DirectoryTreeModel::EntryRole).toInt() : filter_->mapToSource(table_->currentIndex()).row();
        const bool selected = treeMode_ ? trees_[0]->currentIndex().isValid() : table_->currentIndex().isValid();
        treeMode_ = Tree; treeBox_->setVisible(Tree); table_->setVisible(!Tree); updateRoots();
        if (selected)
        {
            if (Tree) selectTree(treeFilter_->mapFromSource(treeModel_->ForEntry(row)));
            else { const auto index = filter_->mapFromSource(model_->index(row, 0)); table_->setCurrentIndex(index); table_->scrollTo(index); }
        }
        updateSelection();
    }
    void DirectoryView::updateSelection()
    {
        const auto item = currentEntry();
        openButton_->setEnabled(file(item["left"].toObject()) && file(item["right"].toObject()));
        summary_->setText((treeMode_ ? tr("일치 %1 / 전체 %2개 · F7 다음 차이 · Shift+F7 이전 차이") : tr("표시 %1 / 전체 %2개 · F7 다음 차이 · Shift+F7 이전 차이")).arg(treeMode_ ? treeModel_->VisibleEntries() : filter_->rowCount()).arg(model_->rowCount()));
    }
    void DirectoryView::OpenCurrent()
    {
        const auto item = currentEntry();
        if (!file(item["left"].toObject()) || !file(item["right"].toObject()) || !open_) return;
        Request next; next.mode_ = "compare"; next.encoding_ = request_.encoding_; next.theme_ = request_.theme_;
        next.file1_ = QDir(request_.file1_).filePath(item["left"].toObject()["path"].toString());
        next.file2_ = QDir(request_.file2_).filePath(item["right"].toObject()["path"].toString());
        if (file(item["base"].toObject())) next.base_ = QDir(request_.base_).filePath(item["base"].toObject()["path"].toString());
        open_(std::move(next));
    }
    void DirectoryView::NavigateDifference(int Direction)
    {
        if (treeMode_)
        {
            selectTree(treeFilter_->mapFromSource(treeModel_->NextDifference(treeFilter_->mapToSource(trees_[0]->currentIndex()), Direction))); return;
        }
        const int count = filter_->rowCount(); if (!count || !Direction) return;
        const auto current = table_->currentIndex().row();
        for (int offset = 1; offset <= count; ++offset)
        {
            const int row = current < 0 ? (Direction > 0 ? offset - 1 : count - offset) : (current + (Direction > 0 ? offset : -offset) + count) % count;
            const auto index = filter_->index(row, 0); const auto entry = model_->Entry(filter_->mapToSource(index).row());
            if (entry["status"] != "Equal") { table_->setCurrentIndex(index); table_->scrollTo(index); return; }
        }
    }
}
