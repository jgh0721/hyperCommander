#include "stdafx.h"
#include "docxCompareView.hpp"
#include "archiveCompareView.hpp"
#include <QAbstractTableModel>
#include <QHeaderView>
#include <QLabel>
#include <QPlainTextEdit>
#include <QSplitter>
#include <QTableView>
#include <QTabWidget>
#include <QVBoxLayout>

namespace hc::compare
{
    namespace
    {
        class ParagraphModel final : public QAbstractTableModel
        {
        public:
            ParagraphModel(const QJsonArray& Changes, const QJsonArray& Left, const QJsonArray& Right, QObject* Parent) : QAbstractTableModel(Parent), left_(Left), right_(Right)
            {
                for (const auto& value : Changes)
                {
                    const auto edit = value.toObject(); const int begin = edit["begin"].toInt(), end = edit["end"].toInt(), otherBegin = edit["otherBegin"].toInt(), otherEnd = edit["otherEnd"].toInt();
                    for (int i = 0; i < std::max(end - begin, otherEnd - otherBegin); ++i) rows_.append({begin + i < end ? begin + i : -1, otherBegin + i < otherEnd ? otherBegin + i : -1});
                }
            }
            int rowCount(const QModelIndex& Parent = {}) const override { return Parent.isValid() ? 0 : rows_.size(); }
            int columnCount(const QModelIndex& Parent = {}) const override { return Parent.isValid() ? 0 : 5; }
            QVariant data(const QModelIndex& Index, int Role) const override
            {
                if (!Index.isValid() || Index.row() >= rows_.size() || Index.column() >= 5) return {};
                const auto [left, right] = rows_[Index.row()];
                if (Role == Qt::UserRole) return Text(Index.row(), Index.column() >= 3);
                if (Role != Qt::DisplayRole && Role != Qt::AccessibleTextRole) return {};
                switch (Index.column())
                {
                case 0: return left < 0 ? tr("추가") : right < 0 ? tr("삭제") : tr("변경");
                case 1: return left < 0 ? QString{} : QString::number(left + 1);
                case 3: return right < 0 ? QString{} : QString::number(right + 1);
                default: { auto text = Text(Index.row(), Index.column() >= 3); if (text.size() > 180) text = text.left(180) + QChar(0x2026); return text.replace('\n', QChar(0x21b5)); }
                }
            }
            QVariant headerData(int Section, Qt::Orientation Orientation, int Role) const override
            { if (Orientation == Qt::Horizontal && Role == Qt::DisplayRole && Section >= 0 && Section < 5) return QStringList{tr("상태"), tr("문단"), "LEFT", tr("문단"), "RIGHT"}[Section]; return {}; }
            QString Text(int Row, bool Right) const
            {
                if (Row < 0 || Row >= rows_.size()) return {}; const int ordinal = Right ? rows_[Row].second : rows_[Row].first;
                return ordinal < 0 ? QString{} : (Right ? right_ : left_)[ordinal].toObject()["text"].toString();
            }
        private:
            QJsonArray left_, right_; QVector<QPair<int, int>> rows_;
        };
    }
    DocxCompareView::DocxCompareView(const QJsonObject& Comparison, const QStringList& Paths, QWidget* Parent) : ExtensionView(Parent)
    {
        setObjectName("docxCompareView"); auto* layout = new QVBoxLayout(this);
        auto* roots = new QLabel(Paths.join("\n"), this); roots->setTextFormat(Qt::PlainText); roots->setWordWrap(true); layout->addWidget(roots);
        auto* scope = new QLabel(tr("DOCX 본문 문단 텍스트 · 읽기 전용\n서식·페이지 배치·머리글·각주·그림·삭제 이력은 텍스트 비교 범위에서 제외됩니다. 내부 파일 탭에서 모든 구성 파일의 바이트 차이를 확인할 수 있습니다."), this);
        scope->setObjectName("docxCoverage"); scope->setWordWrap(true); layout->addWidget(scope);
        auto* summary = new QLabel(Comparison["textEqual"].toBool() ? (Comparison["rawEqual"].toBool() ? tr("파일과 본문 텍스트 동일") : tr("본문 텍스트 동일 · 패키지 바이트 다름")) : tr("본문 텍스트 다름"), this);
        summary->setObjectName("docxSummary"); layout->addWidget(summary);
        auto* tabs = new QTabWidget(this); tabs->setObjectName("docxTabs"); layout->addWidget(tabs, 1);
        auto* splitter = new QSplitter(Qt::Vertical, tabs); tabs->addTab(splitter, tr("본문 문단 차이"));
        table_ = new QTableView(splitter); table_->setObjectName("docxParagraphTable"); const auto docs = Comparison["documents"].toArray();
        auto* model = new ParagraphModel(Comparison["changes"].toArray(), docs[0].toObject()["paragraphs"].toArray(), docs[1].toObject()["paragraphs"].toArray(), this); table_->setModel(model);
        table_->setSelectionBehavior(QAbstractItemView::SelectRows); table_->setSelectionMode(QAbstractItemView::SingleSelection); table_->setEditTriggers(QAbstractItemView::NoEditTriggers); table_->verticalHeader()->hide();
        table_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch); table_->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Stretch);
        table_->setColumnWidth(0, 80); table_->setColumnWidth(1, 70); table_->setColumnWidth(3, 70);
        auto* details = new QSplitter(Qt::Horizontal, splitter); left_ = new QPlainTextEdit(details); right_ = new QPlainTextEdit(details);
        left_->setObjectName("docxLeftText"); right_->setObjectName("docxRightText"); left_->setAccessibleName(tr("왼쪽 선택 문단")); right_->setAccessibleName(tr("오른쪽 선택 문단"));
        left_->setReadOnly(true); right_->setReadOnly(true); splitter->setStretchFactor(0, 2); splitter->setStretchFactor(1, 1);
        parts_ = new ArchiveCompareView(Comparison, {}, tabs); tabs->addTab(parts_, tr("내부 파일"));
        connect(table_->selectionModel(), &QItemSelectionModel::currentRowChanged, this, [this, model](const QModelIndex& Index) { left_->setPlainText(model->Text(Index.row(), false)); right_->setPlainText(model->Text(Index.row(), true)); });
        if (model->rowCount()) table_->setCurrentIndex(model->index(0, 0));
    }
    void DocxCompareView::SetListFont(const QFont& Font)
    { table_->setFont(Font); table_->verticalHeader()->setDefaultSectionSize(QFontMetrics(Font).height() + 10); parts_->SetListFont(Font); }
    void DocxCompareView::SetTextFont(const QFont& Font) { left_->setFont(Font); right_->setFont(Font); }
}
