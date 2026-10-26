#pragma once

#include <QWidget>
#include <QPainter>
#include <QFont>
#include <QFontMetrics>
#include <QAbstractScrollArea>
#include <QScrollBar>
#include <fmstyle/ThemeManager.h>

/// 텍스트 에디터 상단에 표시되는 열 번호 눈금자
class ColumnRulerWidget : public QWidget
{
public:
    explicit ColumnRulerWidget(QWidget* parent = nullptr)
        : QWidget(parent)
    {
        setFixedHeight(20);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }

    void setEditorFont(const QFont& font)
    {
        m_editorFont = font;
        m_charWidth = QFontMetricsF(m_editorFont).horizontalAdvance(QLatin1Char('0'));
        update();
    }

    void SetLabelFont(const QFont& Font)
    { setFont(Font); setFixedHeight(qMax(20, QFontMetrics(Font).height() + 10)); update(); }

    void setScrollOffset(int pixelOffset)
    {
        if (m_scrollOffset != pixelOffset) {
            m_scrollOffset = pixelOffset;
            update();
        }
    }

    void setLeftMarginWidth(int width)
    {
        if (m_leftMargin != width) {
            m_leftMargin = width;
            update();
        }
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        if (m_charWidth <= 0.0)
            return;

        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, false);

        // 배경
        // Product tokens: the ruler reads as the editor margin (Win) with muted numbers and grid ticks.
        const auto& Colors = fm::style::themeColorsFor(this);
        const QColor bgColor = Colors[fm::style::Token::Win];
        const QColor fgColor = Colors[fm::style::Token::Fg3];
        const QColor tickColor = Colors[fm::style::Token::Line];

        painter.fillRect(rect(), bgColor);

        // 하단 구분선
        painter.setPen(tickColor);
        painter.drawLine(0, height() - 1, width(), height() - 1);

        // 눈금 그리기
        painter.setFont(font());

        const int viewWidth = width();
        const double startCol = m_scrollOffset / m_charWidth;
        const int firstCol = qMax(0, static_cast<int>(startCol));
        const int maxCols = static_cast<int>((viewWidth + m_scrollOffset) / m_charWidth) + 2;

        for (int col = firstCol; col <= maxCols; ++col) {
            const double xPos = m_leftMargin + (col * m_charWidth) - m_scrollOffset;
            if (xPos < m_leftMargin - 10 || xPos > viewWidth + 10)
                continue;

            const int ix = static_cast<int>(xPos);
            if (col % 10 == 0) {
                // 10단위: 긴 눈금 + 숫자
                painter.setPen(fgColor);
                painter.drawLine(ix, height() - 6, ix, height() - 1);
                const QString label = QString::number(col);
                const int labelWidth = painter.fontMetrics().horizontalAdvance(label);
                painter.drawText(ix - labelWidth / 2, height() - 8, label);
            } else if (col % 5 == 0) {
                // 5단위: 중간 눈금
                painter.setPen(tickColor);
                painter.drawLine(ix, height() - 5, ix, height() - 1);
            } else {
                // 1단위: 짧은 눈금
                painter.setPen(tickColor);
                painter.drawLine(ix, height() - 3, ix, height() - 1);
            }
        }
    }

private:
    QFont   m_editorFont;
    double  m_charWidth = 8.0;
    int     m_scrollOffset = 0;
    int     m_leftMargin = 0;
};




