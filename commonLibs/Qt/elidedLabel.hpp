#pragma once

#include <QLabel>
#include <QPainter>
#include <QStyle>

namespace hc::qt {
inline void PaintElidedLabel(QLabel& Label, Qt::TextElideMode Mode) {
    QPainter Painter(&Label);
    const auto Text = Label.fontMetrics().elidedText(QString(Label.text()).replace('\n', ' '), Mode, qMax(0, Label.contentsRect().width()));
    Label.style()->drawItemText(&Painter, Label.contentsRect(), QStyle::visualAlignment(Label.layoutDirection(), Label.alignment()),
        Label.palette(), Label.isEnabled(), Text, QPalette::WindowText);
}
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
    void SetElideMode(Qt::TextElideMode Mode) { elideMode_ = Mode; update(); }
    QSize sizeHint() const override {
        const auto Margins = contentsMargins();
        return {0, fontMetrics().height() + Margins.top() + Margins.bottom()};
    }
    QSize minimumSizeHint() const override { return sizeHint(); }
protected:
    void paintEvent(QPaintEvent*) override {
        PaintElidedLabel(*this, elideMode_);
    }
private:
    Qt::TextElideMode elideMode_ = Qt::ElideRight;
};
}
