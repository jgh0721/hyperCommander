#pragma once

#include "capture/CaptureTypes.h"

#include <QDialog>
#include <QImage>
#include <QList>
#include <QPainterPath>
#include <QPoint>
#include <QPixmap>
#include <QRect>
#include <QRegion>

class QCheckBox;
class QPainter;
class QShortcut;
class QTimer;
class QToolButton;

namespace Capture {

class CaptureOverlay : public QDialog
{
    Q_OBJECT

public:
    explicit CaptureOverlay(QWidget* parent = nullptr);
    ~CaptureOverlay() override;

    static CaptureResult run(QWidget* parent = nullptr);
    CaptureResult result() const;
    void cancelCapture();
    void paintScreenOverlay(QPainter& painter, const QRect& screenGeometry, const QRect& localRect) const;
    void handleExternalMousePress(QMouseEvent* event);
    void handleExternalMouseMove(QMouseEvent* event);
    void handleExternalMouseRelease(QMouseEvent* event);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    void buildToolBar();
    void setTargetMode(TargetMode mode);
    void finishCapture(const QRect& logicalRect, const QPainterPath& logicalMask = {});
    QImage imageForSelection(const QRect& logicalRect, const QPainterPath& logicalMask) const;
    void hideOverlaysForCapture();
    void drawDesktopBackground(QPainter& painter, const QRect& targetRect, bool dimmed) const;
    void drawDesktopBackgroundForScreen(QPainter& painter, const QRect& targetRect, const QRect& screenGeometry, bool dimmed) const;
    void refreshOverlayWindows();
    QRect normalizedSelection() const;
    QRect logicalRectFromLocal(const QRect& localRect) const;
    QPoint localPointFromEvent(const QPoint& pos) const;
    QRect windowRectAt(const QPoint& globalPos) const;
    void updateHoveredWindow(const QPoint& globalPos);
    QRect activeScreenLocalGeometry() const;
    void updateToolChecks();
    void placeToolBar();
    static QImage blurredImage(const QImage& source);

    QWidget* m_toolBar = nullptr;
    QToolButton* m_fullScreenButton = nullptr;
    QToolButton* m_windowButton = nullptr;
    QToolButton* m_rectButton = nullptr;
    QToolButton* m_ellipseButton = nullptr;
    QToolButton* m_freeformButton = nullptr;
    QToolButton* m_markupButton = nullptr;
    QShortcut* m_escapeShortcut = nullptr;
    QTimer* m_escapePollTimer = nullptr;

    QRect m_virtualGeometry;
    QRect m_overlayGeometry;
    QRect m_activeScreenLocalGeometry;
    QRegion m_screenRegion;
    QImage m_desktopImage;
    QImage m_blurredImage;
    QList<QWidget*> m_passiveOverlays;
    CaptureOptions m_options;
    CaptureResult m_result;
    bool m_dragging = false;
    QPoint m_dragStart;
    QPoint m_dragCurrent;
    QPainterPath m_freeformPath;
    QRect m_hoverWindowLogicalRect;
    QRect m_hoverWindowLocalRect;
};

} // namespace Capture








