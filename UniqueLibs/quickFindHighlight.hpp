#pragma once
#include "quickFindSettings.hpp"
#include <QPainter>
#include <QTextBoundaryFinder>
#include <QTextLayout>
#include <QStyledItemDelegate>
#include <QApplication>
#include <fmstyle/ThemeManager.h>

namespace hc::quickfind {
// Map normalized, case-folded matches back to complete source graphemes.
inline QList<QPair<int, int>> HighlightRanges(const QString& Text, const QString& Query, const QString& Match = "contains")
{
    QList<QPair<int, int>> Ranges;
    if (Query.isEmpty()) return Ranges;
    QString Folded; QList<QPair<int, int>> Positions;
    QTextBoundaryFinder Boundary(QTextBoundaryFinder::Grapheme, Text);
    int Begin = 0;
    while (true) {
        const int End = Boundary.toNextBoundary(); if (End < 0) break;
        const auto Part = Text.mid(Begin, End - Begin).normalized(QString::NormalizationForm_C).toCaseFolded();
        Folded += Part; for (int I = 0; I < Part.size(); ++I) Positions.append({Begin, End}); Begin = End;
    }
    const auto Term = Query.normalized(QString::NormalizationForm_C).toCaseFolded();
    for (int From = 0; From <= Folded.size() - Term.size();) {
        const int At = Folded.indexOf(Term, From); if (At < 0) break;
        if ((Match != "starts" || At == 0) && (Match != "ends" || At + Term.size() == Folded.size())) {
            const int Start = Positions[At].first, End = Positions[At + Term.size() - 1].second;
            if (!Ranges.isEmpty() && Start <= Ranges.last().first + Ranges.last().second)
                Ranges.last().second = End - Ranges.last().first;
            else Ranges.append({Start, End - Start});
        }
        From = At + int(Term.size());
    }
    return Ranges;
}
// Match marks follow the theme: the warning outline tone behind body text reads in light and dark.
inline QColor HighlightBackground() { return fm::style::ThemeManager::instance().colors()[fm::style::Token::WarnLine]; }
inline QColor HighlightForeground() { return fm::style::ThemeManager::instance().colors()[fm::style::Token::Fg]; }
inline void PaintHighlightedText(QPainter* Painter, const QRect& Rect, const QString& Text,
    const QString& Query, Qt::Alignment Alignment = Qt::AlignLeft)
{
    QTextLayout Layout(Text, Painter->font());
    QList<QTextLayout::FormatRange> Formats;
    for (const auto& Range : HighlightRanges(Text, Query)) {
        QTextLayout::FormatRange Format; Format.start = Range.first; Format.length = Range.second;
        Format.format.setBackground(HighlightBackground()); Format.format.setForeground(HighlightForeground()); Formats.append(Format);
    }
    Layout.setFormats(Formats); Layout.beginLayout(); auto Line = Layout.createLine();
    if (Line.isValid()) Line.setLineWidth(qMax(0, Rect.width())); Layout.endLayout();
    if (!Line.isValid()) return;
    qreal X = Rect.left();
    if (Alignment.testFlag(Qt::AlignHCenter)) X += (Rect.width() - Line.naturalTextWidth()) / 2;
    else if (Alignment.testFlag(Qt::AlignRight)) X += Rect.width() - Line.naturalTextWidth();
    Painter->save(); Painter->setClipRect(Rect, Qt::IntersectClip);
    Layout.draw(Painter, QPointF(X, Rect.top() + (Rect.height() - Line.height()) / 2)); Painter->restore();
}
class ResultHighlightDelegate final : public QStyledItemDelegate {
public:
    explicit ResultHighlightDelegate(QObject* Parent) : QStyledItemDelegate(Parent) {}
    void paint(QPainter* Painter, const QStyleOptionViewItem& Option, const QModelIndex& Index) const override
    {
        const auto Query = parent()->property("hc.quickFind.text").toString();
        if (Index.column() || Query.isEmpty() || !parent()->property("hc.quickFind.highlight").toBool()) {
            QStyledItemDelegate::paint(Painter, Option, Index); return;
        }
        QStyleOptionViewItem Item(Option); initStyleOption(&Item, Index);
        const auto* Style = Item.widget ? Item.widget->style() : QApplication::style();
        const auto Rect = Style->subElementRect(QStyle::SE_ItemViewItemText, &Item, Item.widget);
        const auto Text = Item.fontMetrics.elidedText(Item.text, Item.textElideMode, Rect.width()); Item.text.clear();
        Style->drawControl(QStyle::CE_ItemViewItem, &Item, Painter, Item.widget);
        Painter->save(); Painter->setFont(Item.font);
        Painter->setPen(Item.palette.color(Item.state.testFlag(QStyle::State_Selected) ? QPalette::HighlightedText : QPalette::Text));
        PaintHighlightedText(Painter, Rect, Text, Query); Painter->restore();
    }
};
}
