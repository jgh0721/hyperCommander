#pragma once

#include <QLabel>
#include <QPainter>
#include <QStyle>

namespace hc::qt {
// A single text row whose contents never change the surrounding layout.
// QLabel retains the full plain text for accessibility; the tooltip exposes it
// to mouse users when the available width requires an ellipsis.
class ElidedLabel : public QLabel {
public:
    explicit ElidedLabel(QWidget* Parent = nullptr) : QLabel(Parent) {
        setTextFormat(Qt::PlainText);
        setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    }
    void SetText(const QString& Text) { setText(Text); setToolTip(Text); }
    QSize sizeHint() const override {
        const auto Margins = contentsMargins();
        return {0, fontMetrics().height() + Margins.top() + Margins.bottom()};
    }
    QSize minimumSizeHint() const override { return sizeHint(); }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter Painter(this);
        const auto Text = fontMetrics().elidedText(QString(text()).replace('\n', ' '), Qt::ElideRight, qMax(0, contentsRect().width()));
        style()->drawItemText(&Painter, contentsRect(), QStyle::visualAlignment(layoutDirection(), alignment()), palette(), isEnabled(), Text, QPalette::WindowText);
    }
};
}
