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
    void SetLineCount(int Lines) { Lines = qMax(1, Lines); if (lineCount_ == Lines) return; lineCount_ = Lines; updateGeometry(); update(); }
    void SetElideMode(Qt::TextElideMode Mode) { elideMode_ = Mode; update(); }
    QSize sizeHint() const override {
        const auto Margins = contentsMargins();
        return {0, (lineCount_ == 1 ? fontMetrics().height() : fontMetrics().lineSpacing() * lineCount_) + Margins.top() + Margins.bottom()};
    }
    QSize minimumSizeHint() const override { return sizeHint(); }
    bool hasHeightForWidth() const override { return false; }
    int heightForWidth(int) const override { return sizeHint().height(); }
protected:
    void paintEvent(QPaintEvent*) override {
        if (lineCount_ == 1) { PaintElidedLabel(*this, elideMode_); return; }
        QPainter Painter(this);
        const auto Lines = text().split('\n');
        const auto Bounds = contentsRect();
        const int Height = fontMetrics().lineSpacing();
        const int Top = Bounds.top() + qMax(0, (Bounds.height() - Height * lineCount_) / 2);
        for (int I = 0; I < lineCount_; ++I) {
            const auto Line = I == lineCount_ - 1 ? Lines.mid(I).join(' ') : Lines.value(I);
            const auto Elided = fontMetrics().elidedText(Line, elideMode_, qMax(0, Bounds.width()));
            style()->drawItemText(&Painter, QRect(Bounds.left(), Top + I * Height, Bounds.width(), Height),
                Qt::AlignLeft | Qt::AlignVCenter, palette(), isEnabled(), Elided, QPalette::WindowText);
        }
    }
private:
    int lineCount_ = 1;
    Qt::TextElideMode elideMode_ = Qt::ElideRight;
};
}
