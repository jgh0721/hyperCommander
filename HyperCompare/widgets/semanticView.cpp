#include "stdafx.h"
#include "semanticView.hpp"
#include <QComboBox>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSortFilterProxyModel>
#include <QTableView>
#include <QVBoxLayout>
#include <QHBoxLayout>

namespace hc::compare
{
    namespace
    {
        QString statusName(const QString& Status)
        {
            if (Status == "RenamedModified") return QObject::tr("이름 변경 추정 + 본문 수정");
            if (Status == "MovedRenamedModified") return QObject::tr("이동 + 이름 변경 추정 + 본문 수정");
            if (Status == "Renamed") return QObject::tr("이름 변경 추정");
            if (Status == "MovedRenamed") return QObject::tr("이동 + 이름 변경 추정");
            if (Status == "Moved") return QObject::tr("이동");
            if (Status == "MovedModified") return QObject::tr("이동 + 수정");
            if (Status == "Modified") return QObject::tr("수정");
            if (Status == "Added") return QObject::tr("추가");
            if (Status == "Removed") return QObject::tr("삭제");
            if (Status == "Ambiguous") return QObject::tr("대응 모호");
            if (Status == "Unmatched") return QObject::tr("대응 미확정");
            return QObject::tr("동일");
        }
    }
    QVariant SemanticModel::data(const QModelIndex& Index, int Role) const
    {
        if (!Index.isValid() || Index.row() < 0 || Index.row() >= rows_.size() || Index.column() < 0 || Index.column() >= 4) return {};
        const auto row = rows_[Index.row()].toObject();
        if (Role == Qt::UserRole) return row;
        if (Role == Qt::ToolTipRole && row["match"] == "UniqueTokenEdit")
            return tr("같은 scope·선언에서 본문 토큰의 90% 이상이 일치하고, 한 구간에서 각 3토큰 이내 차이가 나는 상호 유일 후보입니다. 구문 기반 추정이며 병합 결정은 변경하지 않습니다.");
        if (Role == Qt::ToolTipRole && row["match"] == "UniqueNumericLiteralShape")
            return tr("같은 scope와 선언 구조에서 숫자 리터럴을 제외한 본문 토큰이 유일하게 일치하는 이름 변경 후보입니다. 의미적 동일성을 보장하거나 병합 결정을 바꾸지 않습니다.");
        if (Role == Qt::ToolTipRole) return tr("함수의 구문 구조와 상대 순서를 비교합니다. 매크로·타입 해석 및 자동 충돌 해결은 수행하지 않습니다.");
        if (Role != Qt::DisplayRole) return {};
        if (Index.column() == 0) return row["name"].toString() + (row["newName"].isString() ? " → " + row["newName"].toString() : QString{});
        if (Index.column() == 1) return statusName(row["status"].toString()) + (row["match"] == "UniqueName" ? tr(" · 이름 대응") : QString{});
        const auto function = Function(Index.row(), Index.column() - 2);
        return function.isEmpty() ? QVariant(QString("—")) : QVariant(function["line"].toInt() + 1);
    }
    QVariant SemanticModel::headerData(int Section, Qt::Orientation Direction, int Role) const
    {
        if (Direction == Qt::Horizontal && Role == Qt::DisplayRole && Section >= 0 && Section < 4)
            return QStringList{tr("함수 / scope"), tr("변경"), tr("첫 번째 줄"), tr("두 번째 줄")}[Section];
        return {};
    }
    void SemanticModel::SetPair(const QJsonObject& Pair, const QJsonArray& Documents)
    {
        beginResetModel(); rows_ = {}; functions_[0] = {}; functions_[1] = {};
        const auto inputs = Pair["inputs"].toArray();
        if (inputs.size() == 2)
            for (int side = 0; side < 2; ++side)
            { const auto i = inputs[side].toInt(-1); if (i >= 0 && i < Documents.size()) functions_[side] = Documents[i].toObject()["functions"].toArray(); }
        for (const auto& value : Pair["relations"].toArray()) if (value.toObject()["status"] != "Unchanged") rows_.append(value);
        endResetModel();
    }
    QJsonObject SemanticModel::Function(int Row, int Side) const
    {
        if (Row < 0 || Row >= rows_.size() || Side < 0 || Side > 1) return {};
        const auto index = rows_[Row].toObject()[Side ? "right" : "left"].toInt(-1);
        return index < 0 || index >= functions_[Side].size() ? QJsonObject{} : functions_[Side][index].toObject();
    }
    SemanticView::SemanticView(QWidget* Parent) : QWidget(Parent)
    {
        setObjectName("semanticView"); auto* layout = new QVBoxLayout(this); layout->setContentsMargins(4, 4, 4, 4);
        auto* row = new QHBoxLayout; pairs_ = new QComboBox(this); pairs_->setObjectName("semanticPairs"); pairs_->setAccessibleName(tr("함수 분석 비교 쌍"));
        auto* search = new QLineEdit(this); search->setPlaceholderText(tr("함수 이름 검색")); search->setAccessibleName(tr("함수 이름 검색"));
        auto* reveal = new QPushButton(tr("함수로 이동"), this); reveal->setObjectName("semanticReveal");
        row->addWidget(pairs_); row->addWidget(search, 1); row->addWidget(reveal); layout->addLayout(row);
        coverage_ = new QLabel(this); coverage_->setObjectName("semanticCoverage"); coverage_->setWordWrap(true); layout->addWidget(coverage_);
        model_ = new SemanticModel(this); filter_ = new QSortFilterProxyModel(this); filter_->setSourceModel(model_); filter_->setFilterKeyColumn(0); filter_->setFilterCaseSensitivity(Qt::CaseInsensitive);
        table_ = new QTableView(this); table_->setObjectName("semanticTable"); table_->setModel(filter_); table_->setSelectionBehavior(QAbstractItemView::SelectRows);
        table_->setSelectionMode(QAbstractItemView::SingleSelection); table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
        table_->verticalHeader()->hide(); table_->verticalHeader()->setDefaultSectionSize(fontMetrics().height() + 10);
        table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
        table_->setColumnWidth(1, 180); table_->setColumnWidth(2, 100); table_->setColumnWidth(3, 100); layout->addWidget(table_, 1);
        connect(search, &QLineEdit::textChanged, filter_, &QSortFilterProxyModel::setFilterFixedString);
        connect(pairs_, &QComboBox::currentIndexChanged, this, [this](int Index) { updatePair(); if (pairChanged_) pairChanged_(Index); });
        connect(reveal, &QPushButton::clicked, this, &SemanticView::RevealCurrent);
        connect(table_, &QTableView::activated, this, [this] { RevealCurrent(); });
    }
    void SemanticView::SetResult(const QJsonObject& Analysis, bool HasBase)
    {
        analysis_ = Analysis; const QSignalBlocker blocker(pairs_); pairs_->clear();
        for (const auto& value : analysis_["pairs"].toArray())
        {
            const auto inputs = value.toObject()["inputs"].toArray();
            if (inputs.size() != 2) continue;
            const auto name = [&](int Index) { return HasBase ? QStringList{"LEFT", "RIGHT", "BASE"}.value(Index) : QString("FILE %1").arg(Index + 1); };
            pairs_->addItem(name(inputs[0].toInt()) + " ↔ " + name(inputs[1].toInt()));
        }
        updatePair();
    }
    void SemanticView::SetPair(int Index) { if (Index >= 0 && Index < pairs_->count()) pairs_->setCurrentIndex(Index); }
    void SemanticView::SetListFont(const QFont& Font)
    { table_->setFont(Font); table_->verticalHeader()->setDefaultSectionSize(QFontMetrics(Font).height() + 10); }
    void SemanticView::updatePair()
    {
        const auto values = analysis_["pairs"].toArray(); const int index = pairs_->currentIndex();
        const auto pair = index < 0 || index >= values.size() ? QJsonObject{} : values[index].toObject();
        const auto documents = analysis_["documents"].toArray(); model_->SetPair(pair, documents);
        QStringList partial;
        for (const auto& value : pair["inputs"].toArray())
        {
            const int at = value.toInt(-1); if (at < 0 || at >= documents.size()) continue;
            const auto document = documents[at].toObject();
            if (document["status"] == "Partial") partial << tr("%1: 문법 오류 또는 분석 제외 함수").arg(at + 1);
            else if (document["status"] != "Complete") partial << tr("%1: 분석 불가 (%2)").arg(at + 1).arg(document["reason"].toString());
        }
        if (pair["ambiguous"].toInt()) partial << tr("대응 모호 %1개").arg(pair["ambiguous"].toInt());
        coverage_->setText(tr("이동 %1 · 수정 %2 · 이름 변경 추정 %3 · 구문 구조 분석 (매크로·타입 해석 제외)%4")
            .arg(pair["moved"].toInt()).arg(pair["modified"].toInt()).arg(pair["renamed"].toInt()).arg(partial.isEmpty() ? QString{} : " · " + partial.join(" / ")));
    }
    void SemanticView::RevealCurrent()
    {
        const auto index = filter_->mapToSource(table_->currentIndex()); if (!index.isValid() || !reveal_) return;
        const auto values = analysis_["pairs"].toArray(); const auto at = pairs_->currentIndex(); if (at < 0 || at >= values.size()) return;
        const auto inputs = values[at].toObject()["inputs"].toArray(); if (inputs.size() != 2) return;
        reveal_(inputs[0].toInt(), model_->Function(index.row(), 0), inputs[1].toInt(), model_->Function(index.row(), 1));
    }
}
