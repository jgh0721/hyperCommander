#pragma once

#include "capture/CaptureTypes.h"

#include <QColor>
#include <QDialog>
#include <QImage>
#include <QPoint>
#include <QRegion>
#include <QVector>

class QCheckBox;
class QSpinBox;
class QToolButton;

namespace Capture {

struct MarkupState {
    QImage image;
    QRect sourceLogicalRect;
};

class CaptureMarkupDialog : public QDialog
{
    Q_OBJECT

public:
    explicit CaptureMarkupDialog(const QImage& image, const QRect& sourceLogicalRect = {}, QWidget* parent = nullptr);

    QImage markedImage() const;

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    QRect imageTargetRect() const;
    QPoint imagePointFromWidget(const QPoint& pos) const;
    void rebuildToolBar();
    void placeToolBar();
    void setTool(MarkupTool tool);
    void chooseColor();
    void pushUndoState();
    void undo();
    void redo();
    void drawLineTo(const QPoint& imagePoint);
    void commitShape(const QPoint& imagePoint);
    void commitCrop();
    QPen currentPen() const;
    QColor effectiveColor() const;
    void updateToolChecks();
    void updateColorButtonStatus();

    QWidget* m_toolBar = nullptr;
    QToolButton* m_penButton = nullptr;
    QToolButton* m_highlighterButton = nullptr;
    QToolButton* m_eraserButton = nullptr;
    QToolButton* m_shapeButton = nullptr;
    QToolButton* m_colorButton = nullptr;
    QToolButton* m_undoButton = nullptr;
    QToolButton* m_redoButton = nullptr;
    QSpinBox* m_widthSpin = nullptr;
    QCheckBox* m_fillCheck = nullptr;

    QImage m_originalImage;
    QImage m_image;
    QImage m_previewImage;
    QImage m_backgroundImage;
    QRect m_sourceLogicalRect;
    QRect m_virtualGeometry;
    QRegion m_screenRegion;
    QVector<MarkupState> m_undoStack;
    QVector<MarkupState> m_redoStack;
    MarkupTool m_tool = MarkupTool::Pen;
    MarkupShape m_shape = MarkupShape::Rectangle;
    QString m_emojiString = QString::fromUtf8("😀");
    QColor m_color = QColor(220, 40, 40); // Global color/fallback
    QColor m_penColor = QColor(220, 40, 40);
    int m_penWidth = 4;
    QColor m_highlighterColor = QColor(255, 255, 0);
    int m_highlighterWidth = 20;
    QColor m_shapeOutlineColor = QColor(220, 40, 40);
    QColor m_shapeFillColor = QColor(255, 255, 255);
    bool m_shapeHasFill = false;
    int m_width = 4;
    bool m_fillShapes = false;
    bool m_drawing = false;
    QPoint m_startPoint;
    QPoint m_lastPoint;

    QRect m_cropLogicalRect;
    int m_resizingHandleIndex = -1;
    bool m_resizing = false;
};

} // namespace Capture
