#pragma once

/// 모듈

/// UIs

/// 기타 헤더파일
#include "text/TextMinimapModel.h"

/// 고유 라이브러리

/// 공통 라이브러리

/// 외부 라이브러리

/// 선언 헤더파일들
class TextMinimap;

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QElapsedTimer>
#include <QPointer>
#include <QTimer>
#include <QWidget>

// Projection of the last paint; shared by painting, input and tests.
struct TextMinimapMapping
{
    bool document_ = true;
    TextMinimapDocumentGeometry documentGeometry_;
    TextMinimapFitGeometry fitGeometry_;
    double RowAt(double Y) const { return document_ ? documentGeometry_.RowAt(Y) : fitGeometry_.RowAt(Y); }
    double YOfRow(double Row) const { return document_ ? documentGeometry_.YOfRow(Row) : fitGeometry_.YOfRow(Row); }
    double SliderTop() const { return document_ ? documentGeometry_.sliderTop_ : fitGeometry_.sliderTop_; }
    double SliderHeight() const { return document_ ? documentGeometry_.sliderHeight_ : fitGeometry_.sliderHeight_; }
    double RowHeight() const { return document_ ? documentGeometry_.rowHeight_ : fitGeometry_.perRow_; }
};

// The minimap column. A child of the editor's scroll area, placed in the strip the viewport leaves free.
class TextMinimapWidget final : public QWidget
{
    Q_OBJECT

public:
    TextMinimapWidget(TextMinimap* Owner, QWidget* Parent);
    TextMinimapMapping Mapping() const;

protected:
    void paintEvent(QPaintEvent* Event) override;
    void mousePressEvent(QMouseEvent* Event) override;
    void mouseMoveEvent(QMouseEvent* Event) override;
    void mouseReleaseEvent(QMouseEvent* Event) override;
    void mouseDoubleClickEvent(QMouseEvent* Event) override;
    void wheelEvent(QWheelEvent* Event) override;
    void leaveEvent(QEvent* Event) override;
    void contextMenuEvent(QContextMenuEvent* Event) override;

private:
    void dragTo(double Y);
    void paintLabels(QPainter& Painter, const TextMinimapMapping& Map) const;
    void paintMarks(QPainter& Painter, const TextMinimapMapping& Map) const;
    void paintViewport(QPainter& Painter, const TextMinimapMapping& Map) const;

    TextMinimap* owner_;
    bool hover_ = false;
    bool dragging_ = false;
    double dragOffset_ = 0;
    // The double-click target is the row under the first press: that press already moved the thumbnail.
    double firstPressRow_ = -1;
    double firstPressY_ = -1;
    QElapsedTimer firstPressClock_;
    QTimer peekTimer_;
    QPoint peekPosition_;
};
