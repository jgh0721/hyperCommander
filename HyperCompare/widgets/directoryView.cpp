#include "stdafx.h"
#include "directoryView.hpp"
#include <QDir>
#include <QLocale>
#include <QFontMetrics>
#include <QTableView>
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
        QString statusName(const QString& Status)
        {
            static const QMap<QString, QString> labels{{"Equal", QObject::tr("동일")}, {"Different", QObject::tr("다름")},
                {"LeftOnly", QObject::tr("왼쪽만 존재")}, {"RightOnly", QObject::tr("오른쪽만 존재")},
                {"TypeConflict", QObject::tr("파일/폴더 충돌")}, {"NameDifferent", QObject::tr("이름 다름")},
                {"LeftChanged", QObject::tr("왼쪽 수정")}, {"RightChanged", QObject::tr("오른쪽 수정")},
                {"LeftAdded", QObject::tr("왼쪽 추가")}, {"RightAdded", QObject::tr("오른쪽 추가")},
                {"LeftDeleted", QObject::tr("왼쪽 삭제")}, {"RightDeleted", QObject::tr("오른쪽 삭제")},
                {"BothDeleted", QObject::tr("양쪽 삭제")}, {"BothSame", QObject::tr("양쪽 동일 변경")}, {"Conflict", QObject::tr("충돌")}};
            return labels.value(Status, Status);
        }
        bool file(const QJsonObject& Entry) { return Entry["present"].toBool() && !Entry["directory"].toBool(); }
    }
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
        const auto status = item["status"].toString();
        return item["path"].toString().contains(query_, Qt::CaseInsensitive) &&
            (status_ == 0 || (status_ == 1 && status != "Equal") || (status_ == 2 && (status == "Conflict" || status == "TypeConflict")));
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
        searchRow->addWidget(query, 1); searchRow->addWidget(status); searchRow->addWidget(openButton_); box->addLayout(searchRow);
        model_ = new DirectoryModel(this); model_->setObjectName("directoryModel"); filter_ = new DirectoryFilter(this); filter_->setSourceModel(model_);
        filter_->setSortCaseSensitivity(Qt::CaseInsensitive);
        table_ = new QTableView(this); table_->setObjectName("directoryTable"); table_->setAccessibleName(tr("디렉터리 비교 결과"));
        table_->setModel(filter_); table_->setSelectionBehavior(QAbstractItemView::SelectRows); table_->setSelectionMode(QAbstractItemView::SingleSelection);
        table_->setAlternatingRowColors(true); table_->setSortingEnabled(true); table_->sortByColumn(0, Qt::AscendingOrder);
        table_->verticalHeader()->setVisible(false); table_->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
        table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
        for (int column = 1; column < 5; ++column) table_->setColumnWidth(column, 140);
        box->addWidget(table_, 1); summary_ = new QLabel(this); summary_->setTextFormat(Qt::PlainText); box->addWidget(summary_);
        const auto change = [this, query, status] { filter_->SetQuery(query->text(), status->currentIndex()); updateSelection(); };
        connect(query, &QLineEdit::textChanged, this, change); connect(status, &QComboBox::currentIndexChanged, this, change);
        connect(table_->selectionModel(), &QItemSelectionModel::currentChanged, this, [this] { updateSelection(); });
        connect(table_, &QTableView::doubleClicked, this, [this] { OpenCurrent(); });
        connect(openButton_, &QPushButton::clicked, this, [this] { OpenCurrent(); });
        updateSelection();
    }
    void DirectoryView::SetComparison(const Request& Input, const Result& Value)
    {
        request_ = Input; model_->SetEntries(Value.entries_); table_->setColumnHidden(4, Input.base_.isEmpty());
        roots_->setText(tr("LEFT  %1\nRIGHT  %2%3\n%4%5").arg(Input.file1_, Input.file2_,
            Input.base_.isEmpty() ? QString{} : "\nBASE  " + Input.base_,
            Value.directoryLevel_ == "metadata" ? tr("메타데이터 비교 — 크기·수정 시각만 검사, 내용 동일을 보장하지 않음") : tr("파일 내용 비교"),
            Value.directoryFiltered_ ? tr(" · 포함/제외 필터 적용") : QString{}));
        updateSelection();
    }
    void DirectoryView::SetListFont(const QFont& Font)
    { table_->setFont(Font); table_->verticalHeader()->setDefaultSectionSize(std::max(28, QFontMetrics(Font).height() + 10)); }
    void DirectoryView::updateSelection()
    {
        const auto item = model_->Entry(filter_->mapToSource(table_->currentIndex()).row());
        openButton_->setEnabled(file(item["left"].toObject()) && file(item["right"].toObject()));
        summary_->setText(tr("표시 %1 / 전체 %2개 · F7 다음 차이 · Shift+F7 이전 차이").arg(filter_->rowCount()).arg(model_->rowCount()));
    }
    void DirectoryView::OpenCurrent()
    {
        const auto item = model_->Entry(filter_->mapToSource(table_->currentIndex()).row());
        if (!file(item["left"].toObject()) || !file(item["right"].toObject()) || !open_) return;
        Request next; next.mode_ = "compare"; next.encoding_ = request_.encoding_; next.theme_ = request_.theme_;
        next.file1_ = QDir(request_.file1_).filePath(item["left"].toObject()["path"].toString());
        next.file2_ = QDir(request_.file2_).filePath(item["right"].toObject()["path"].toString());
        if (file(item["base"].toObject())) next.base_ = QDir(request_.base_).filePath(item["base"].toObject()["path"].toString());
        open_(std::move(next));
    }
    void DirectoryView::NavigateDifference(int Direction)
    {
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
