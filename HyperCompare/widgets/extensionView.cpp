#include "stdafx.h"
#include "extensionView.hpp"
#include "imageCompareView.hpp"
#include "archiveCompareView.hpp"
#include "docxCompareView.hpp"
#include "core/compare.hpp"
#include <QVBoxLayout>
#include <QLabel>
#include <QTableWidget>
#include <QHeaderView>
#include <QJsonArray>

namespace hc::compare
{
    namespace {
    class BinaryCompareView final : public ExtensionView {
    public:
        BinaryCompareView(const QJsonObject& Comparison, const QStringList& Paths, QWidget* Parent) : ExtensionView(Parent) {
            setObjectName("hc.compare.binary"); auto* Layout = new QVBoxLayout(this);
            const auto Documents = Comparison["documents"].toArray();
            if (Documents.size() != Paths.size()) throw Failure("BinaryViewInputs");
            for (int Side = 0; Side < Paths.size(); ++Side) {
                auto* Label = new QLabel(this); Label->setTextFormat(Qt::PlainText); Label->setWordWrap(true);
                Label->setText(tr("%1 · %2 byte").arg(Paths[Side], Documents[Side].toObject()["size"].toString())); Layout->addWidget(Label);
            }
            auto* Summary = new QLabel(this);
            Summary->setText(tr("바이너리 비교 · 다른 바이트 %1개 · 차이 구간 %2개").arg(Comparison["differentBytes"].toString(), Comparison["rangeCount"].toString()));
            Layout->addWidget(Summary);
            if (!Comparison["rangesComplete"].toBool()) Layout->addWidget(new QLabel(tr("전체 파일 비교를 완료했습니다. 차이 구간은 처음 4096개만 표시합니다."), this));
            table_ = new QTableWidget(this); table_->setColumnCount(4);
            table_->setHorizontalHeaderLabels({tr("오프셋 (16진수)"), tr("길이 (byte)"), tr("왼쪽 (최대 32 byte)"), tr("오른쪽 (최대 32 byte)")});
            table_->setEditTriggers(QAbstractItemView::NoEditTriggers); table_->setSelectionBehavior(QAbstractItemView::SelectRows);
            const auto Ranges = Comparison["ranges"].toArray(); table_->setRowCount(Ranges.size());
            for (int Row = 0; Row < Ranges.size(); ++Row) {
                const auto Range = Ranges[Row].toObject();
                const QStringList Cells{QString::number(Range["offset"].toString().toULongLong(), 16).toUpper(), Range["length"].toString(), Range["leftHex"].toString(), Range["rightHex"].toString()};
                for (int Column = 0; Column < Cells.size(); ++Column) table_->setItem(Row, Column, new QTableWidgetItem(Cells[Column]));
            }
            table_->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
            table_->horizontalHeader()->setStretchLastSection(true); Layout->addWidget(table_);
        }
        void SetListFont(const QFont& Font) override { table_->setFont(Font); }
    private:
        QTableWidget* table_ = nullptr;
    };
    }
    ExtensionView* CreateExtensionView(const QJsonObject& Comparison, const QStringList& Paths, QWidget* Parent)
    {
        const auto schema = Comparison["payloadSchema"].toString();
        if (schema == "binary.offset.v1") return new BinaryCompareView(Comparison, Paths, Parent);
        if (schema == "image.rgba.v1") return new ImageCompareView(Comparison, Paths, Parent);
        if (schema == "archive.entries.v1") return new ArchiveCompareView(Comparison, Paths, Parent);
        if (schema == "office.paragraphs.v1") return new DocxCompareView(Comparison, Paths, Parent);
        throw Failure("ExtensionViewUnsupported");
    }
}
