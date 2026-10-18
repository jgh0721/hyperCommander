#pragma once

#include <QCache>
#include <QDataStream>
#include <QFontMetricsF>
#include <QImage>
#include <QIODevice>
#include <QPainter>
#include <QtMath>
#include <array>
#include <cmath>

namespace mrst::scintilla {
// GUI-thread only, shared by all Scintilla surfaces (including buffered drawing).
struct TextMaskState
{
    bool enabled = false;
    double gamma = 1.2;
    double strength = 1.1;
    std::array<int, 256> coverage{};
    QCache<QByteArray, QImage> cache{4096}; // KiB: at most 4 MiB of image data
};

inline TextMaskState& textMaskState()
{
    static TextMaskState state;
    return state;
}

inline void setTextMask(bool enabled, double gamma, double strength)
{
    gamma = std::isfinite(gamma) ? qBound(0.5, gamma, 3.0) : 1.2;
    strength = std::isfinite(strength) ? qBound(0.5, strength, 2.0) : 1.1;
    auto& state = textMaskState();
    if (state.enabled == enabled && state.gamma == gamma && state.strength == strength)
        return;
    state.enabled = enabled;
    state.gamma = gamma;
    state.strength = strength;
    state.cache.clear();
    for (int i = 0; i < 256; ++i)
        state.coverage[i] = qBound(0, qRound(qPow(i / 255.0, 1.0 / gamma) * strength * 255.0), 255);
}

inline void drawTextWithMask(QPainter& painter, const QPointF& baseline, const QString& text)
{
    auto& state = textMaskState();
    if (!state.enabled || text.isEmpty()) {
        painter.drawText(baseline, text);
        return;
    }
    const qreal dpr = painter.device()->devicePixelRatioF();
    const QFontMetricsF metrics(painter.font(), painter.device());
    // Include italic bearings, combining marks, and blank advances; preserve the
    // original baseline phase at fractional DPI. Destination clipping is retained.
    const QRectF bounds = metrics.boundingRect(text).united(
        QRectF(0, -metrics.ascent(), metrics.horizontalAdvance(text), metrics.height()))
        .translated(baseline).adjusted(-2, -2, 2, 2);
    const QPointF origin(qFloor(bounds.left() * dpr) / dpr, qFloor(bounds.top() * dpr) / dpr);
    const qreal width = qCeil((bounds.right() - origin.x()) * dpr);
    const qreal height = qCeil((bounds.bottom() - origin.y()) * dpr);
    // Long unwrapped lines must not allocate unbounded temporary images.
    if (width <= 0 || height <= 0 || width * height > 1024 * 1024) {
        painter.drawText(baseline, text);
        return;
    }
    const QSize pixels{int(width), int(height)};
    const QPointF local = baseline - origin;
    QByteArray key;
    QDataStream stream(&key, QIODevice::WriteOnly);
    stream << painter.font() << text << painter.pen().color().rgba()
           << dpr << local << pixels << int(painter.renderHints())
           << painter.device()->logicalDpiX() << painter.device()->logicalDpiY();
    const QImage* mask = state.cache.object(key);
    QImage generated;
    if (!mask) {
        generated = QImage(pixels, QImage::Format_ARGB32_Premultiplied);
        if (generated.isNull()) {
            painter.drawText(baseline, text);
            return;
        }
        generated.setDevicePixelRatio(dpr);
        generated.setDotsPerMeterX(qRound(painter.device()->logicalDpiX() / 0.0254));
        generated.setDotsPerMeterY(qRound(painter.device()->logicalDpiY() / 0.0254));
        generated.fill(Qt::transparent);
        QPainter glyphs(&generated);
        glyphs.setFont(painter.font());
        glyphs.setPen(painter.pen());
        glyphs.setRenderHints(painter.renderHints());
        glyphs.drawText(local, text);
        glyphs.end();
        for (int y = 0; y < generated.height(); ++y) {
            auto* row = reinterpret_cast<QRgb*>(generated.scanLine(y));
            for (int x = 0; x < generated.width(); ++x) {
                const QRgb rgba = qUnpremultiply(row[x]);
                row[x] = qPremultiply(qRgba(qRed(rgba), qGreen(rgba), qBlue(rgba), state.coverage[qAlpha(rgba)]));
            }
        }
        const int cost = int((generated.sizeInBytes() + key.size() + 1023) / 1024);
        auto* owned = new QImage(generated);
        if (state.cache.insert(key, owned, cost)) mask = owned;
        else mask = &generated;
    }
    if (painter.backgroundMode() == Qt::OpaqueMode)
        painter.fillRect(metrics.boundingRect(text).translated(baseline), painter.background());
    painter.drawImage(origin, *mask);
}
} // namespace mrst::scintilla
