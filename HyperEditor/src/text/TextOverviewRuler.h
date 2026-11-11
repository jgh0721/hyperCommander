#pragma once

/// 모듈

/// UIs

/// 기타 헤더파일

/// 고유 라이브러리

/// 공통 라이브러리

/// 외부 라이브러리

/// 선언 헤더파일들
class TextMinimap;

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QPointer>
#include <QScrollBar>
#include <QTimer>
#include <QWidget>

// Marks over the vertical scroll bar. Paints only; mouse input reaches the scroll bar through an event filter so
// thumb dragging and paging behave as before.
class TextOverviewRuler final : public QWidget
{
    Q_OBJECT

public:
    TextOverviewRuler(TextMinimap* Owner, QScrollBar* Bar);
    QRect Groove() const;
    double RowAt(int Y) const;
    double YOfRow(double Row) const;
    // Row of the nearest mark within Tolerance pixels of Y, or -1.
    int MarkRowNear(int Y, int Tolerance) const;

protected:
    void paintEvent(QPaintEvent* Event) override;
    bool eventFilter(QObject* Watched, QEvent* Event) override;

private:
    void reposition();
    QRect sliderRect() const;

    TextMinimap* owner_;
    QPointer<QScrollBar> bar_;
    QTimer peekTimer_;
    QPoint peekPosition_;
};
