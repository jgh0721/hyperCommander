#pragma once
#include <QPainter>
#include <QJsonObject>

namespace hc::ui {
inline Qt::PenStyle FileRuleBorderStyle(const QString& Style)
{
    if (Style == "solid") return Qt::SolidLine;
    if (Style == "dash") return Qt::DashLine;
    if (Style == "dot") return Qt::DotLine;
    if (Style == "dashDot") return Qt::DashDotLine;
    if (Style == "dashDotDot") return Qt::DashDotDotLine;
    return Qt::NoPen;
}
inline void PaintFileRuleBorder(QPainter* Painter, const QRectF& Rect, const QColor& Color, const QJsonObject& Effects)
{
    const auto Style = FileRuleBorderStyle(Effects["border"].toString());
    if (Style == Qt::NoPen || !Color.isValid() || Rect.width() < 4 || Rect.height() < 4) return;
    Painter->save(); Painter->setBrush(Qt::NoBrush); Painter->setRenderHint(QPainter::Antialiasing, false);
    Painter->setPen(QPen(Color, 1.0, Style));
    Painter->drawRect(Rect.adjusted(1.5, 1.5, -1.5, -1.5)); Painter->restore();
}
// A row whose field conditions are still being read: a 3 px bar on the leading edge. It stays on
// selected rows, where the pending background gives way to the selection color.
inline void PaintFileRulePending(QPainter* Painter, const QRectF& Rect, const QJsonObject& Effects)
{
    const QColor Color(Effects["pendingMarker"].toString());
    if (!Color.isValid() || Rect.width() < 4 || Rect.height() < 4) return;
    Painter->save(); Painter->setRenderHint(QPainter::Antialiasing, false);
    Painter->fillRect(QRectF(Rect.left(), Rect.top(), 3, Rect.height()), Color); Painter->restore();
}
}
