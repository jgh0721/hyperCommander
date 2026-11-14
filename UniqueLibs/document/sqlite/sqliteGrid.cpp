#include "stdafx.h"
#include "sqliteGrid.hpp"

/// 모듈

/// UIs

/// 기타 헤더파일

/// 고유 라이브러리
#include "sqliteCellDelegate.hpp"
#include "sqliteGlyphs.hpp"
#include "sqliteResultModel.hpp"

/// 공통 라이브러리

/// 외부 라이브러리
#include <fmstyle/ThemeColors.h>
#include <fmstyle/ThemeManager.h>

/// 선언 헤더파일들

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QApplication>
#include <QClipboard>
#include <QFontDatabase>
#include <QKeyEvent>
#include <QMap>
#include <QPainter>

namespace hc::sqlite
{
    GridHeader::GridHeader(QWidget* Parent) : QHeaderView(Qt::Horizontal, Parent)
    {
        setSectionsClickable(true);
        setHighlightSections(false);
        setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        setMinimumSectionSize(40);
    }

    QSize GridHeader::sizeHint() const
    {
        auto Size = QHeaderView::sizeHint();
        Size.setHeight(qMax(Size.height(), fontMetrics().height() * 2 + 10));
        return Size;
    }

    void GridHeader::paintSection(QPainter* Painter, const QRect& Rect, int Section) const
    {
        if (!Rect.isValid()) return;
        QStyleOptionHeader Opt;
        initStyleOption(&Opt);
        Opt.rect = Rect;
        Opt.section = Section;
        Opt.text.clear();
        Opt.sortIndicator = isSortIndicatorShown() && sortIndicatorSection() == Section
            ? (sortIndicatorOrder() == Qt::AscendingOrder ? QStyleOptionHeader::SortUp : QStyleOptionHeader::SortDown) : QStyleOptionHeader::None;
        Painter->save();
        style()->drawControl(QStyle::CE_Header, &Opt, Painter, this);
        Painter->restore();
        if (!model()) return;
        const auto& Colors = fm::style::themeColorsFor(this);
        using T = fm::style::Token;
        const auto Name = model()->headerData(Section, Qt::Horizontal, Qt::DisplayRole).toString();
        const auto Type = model()->headerData(Section, Qt::Horizontal, DeclaredTypeRole).toString();
        const bool Numeric = model()->headerData(Section, Qt::Horizontal, NumericRole).toBool();
        const bool Key = model()->headerData(Section, Qt::Horizontal, Qt::ToolTipRole).toString().contains(tr("기본 키"));
        QRect Text = Rect.adjusted(8, 3, Opt.sortIndicator != QStyleOptionHeader::None ? -18 : -8, -3);
        QFont NameFont = font();
        NameFont.setWeight(QFont::DemiBold);
        QFont TypeFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
        TypeFont = SizedLike(TypeFont, font(), -2.0, 6.5);
        const int Align = Qt::AlignVCenter | (Numeric ? Qt::AlignRight : Qt::AlignLeft);
        const int Half = Text.height() / 2 + 1;
        Painter->save();
        Painter->setFont(NameFont);
        Painter->setPen(Key ? Colors[T::Warn] : Colors[T::Fg]);
        const QRect Top(Text.left(), Text.top(), Text.width(), Type.isEmpty() ? Text.height() : Half);
        Painter->drawText(Top, Align, QFontMetrics(NameFont).elidedText(Name, Qt::ElideRight, Top.width()));
        if (!Type.isEmpty()) {
            Painter->setFont(TypeFont);
            Painter->setPen(Colors[T::Fg3]);
            const QRect Bottom(Text.left(), Text.top() + Half, Text.width(), Text.height() - Half);
            Painter->drawText(Bottom, Align, QFontMetrics(TypeFont).elidedText(Type, Qt::ElideRight, Bottom.width()));
        }
        Painter->restore();
    }

    GridView::GridView(QWidget* Parent) : QTableView(Parent)
    {
        setObjectName(QStringLiteral("sqliteGrid"));
        auto* Header = new GridHeader(this);
        setHorizontalHeader(Header);
        delegate_ = new CellDelegate(this);
        setItemDelegate(delegate_);
        setSelectionBehavior(QAbstractItemView::SelectItems);
        setSelectionMode(QAbstractItemView::ExtendedSelection);
        setEditTriggers(QAbstractItemView::NoEditTriggers);
        setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
        setVerticalScrollMode(QAbstractItemView::ScrollPerItem);
        setWordWrap(false);
        setTextElideMode(Qt::ElideRight);
        setAlternatingRowColors(true);
        setFrameShape(QFrame::NoFrame);
        verticalHeader()->setMinimumSectionSize(16);
        verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
        updateRowHeight();
        verticalHeader()->setDefaultAlignment(Qt::AlignRight | Qt::AlignVCenter);
        Header->setSortIndicatorShown(false);
        // 정렬은 모델이 ORDER BY 로 다시 읽는다(QTableView 의 setSortingEnabled 는 켜는 순간 정렬해 쓰지 않는다).
        connect(Header, &QHeaderView::sectionClicked, this, [this, Header](int Section) {
            if (!model()) return;
            sortOrder_ = Section == sortColumn_ && sortOrder_ == Qt::AscendingOrder ? Qt::DescendingOrder : Qt::AscendingOrder;
            sortColumn_ = Section;
            Header->setSortIndicatorShown(true);
            Header->setSortIndicator(Section, sortOrder_);
            model()->sort(Section, sortOrder_);
        });
    }

    QString GridView::SelectionText() const
    {
        if (!model()) return {};
        auto Indexes = selectionModel() ? selectionModel()->selectedIndexes() : QModelIndexList{};
        if (Indexes.isEmpty() && currentIndex().isValid()) Indexes.append(currentIndex());
        if (Indexes.isEmpty()) return {};
        QMap<int, QMap<int, QString>> Rows;
        int MinColumn = INT_MAX, MaxColumn = -1;
        for (const auto& Index : Indexes) {
            const auto Value = Index.data(ValueRole);
            Rows[Index.row()][Index.column()] = Value.isValid() ? (Value.typeId() == QMetaType::QByteArray ? QString::fromLatin1(Value.toByteArray().toHex()) : Value.toString()) : QString();
            MinColumn = qMin(MinColumn, Index.column());
            MaxColumn = qMax(MaxColumn, Index.column());
        }
        QStringList Lines;
        for (auto Row = Rows.cbegin(); Row != Rows.cend(); ++Row) {
            QStringList Cells;
            for (int Column = MinColumn; Column <= MaxColumn; ++Column) Cells.append(Row.value().value(Column));
            Lines.append(Cells.join(QLatin1Char('\t')));
        }
        return Lines.join(QStringLiteral("\r\n"));
    }

    void GridView::FitColumns(int MaxWidth)
    {
        if (!model()) return;
        const int Rows = qMin(model()->rowCount(), 64);
        QFontMetrics Metrics(font());
        QFontMetrics Bold([this] { auto Font = font(); Font.setWeight(QFont::DemiBold); return Font; }());
        for (int Column = 0; Column < model()->columnCount(); ++Column) {
            int Width = Bold.horizontalAdvance(model()->headerData(Column, Qt::Horizontal).toString()) + 28;
            Width = qMax(Width, Metrics.horizontalAdvance(model()->headerData(Column, Qt::Horizontal, DeclaredTypeRole).toString()) + 24);
            for (int Row = 0; Row < Rows; ++Row)
                Width = qMax(Width, Metrics.horizontalAdvance(model()->index(Row, Column).data().toString()) + 20);
            setColumnWidth(Column, qBound(56, Width, MaxWidth));
        }
    }

    void GridView::updateRowHeight()
    {
        verticalHeader()->setDefaultSectionSize(qMax(20, fontMetrics().height() + 6));
    }

    void GridView::changeEvent(QEvent* Event)
    {
        QTableView::changeEvent(Event);
        if (Event->type() == QEvent::FontChange || Event->type() == QEvent::StyleChange) updateRowHeight();
    }

    void GridView::showEvent(QShowEvent* Event)
    {
        updateRowHeight();
        QTableView::showEvent(Event);
    }

    void GridView::keyPressEvent(QKeyEvent* Event)
    {
        if (Event->matches(QKeySequence::Copy)) {
            if (brokered_) emit CopyRequested();
            else QApplication::clipboard()->setText(SelectionText());
            Event->accept();
            return;
        }
        QTableView::keyPressEvent(Event);
    }
}
