#include "capture/CaptureOverlay.h"

#include "capture/CaptureMarkupDialog.h"
#include "capture/WindowsGraphicsCapture.h"

#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QCursor>
#include <QEventLoop>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QScreen>
#include <QShortcut>
#include <QThread>
#include <QTimer>
#include <QToolButton>

#include <utility>

#include "core/ThemeManager.h"

#ifdef Q_OS_WIN
#include <QtCore/qt_windows.h>
#include <dwmapi.h>
#pragma comment(lib, "dwmapi.lib")
#endif

namespace Capture {
namespace {

QToolButton* makeCaptureButton(QWidget* parent, const QString& text, const QString& toolTip, bool checkable = true)
{
    auto* button = new QToolButton(parent);
    button->setText(text);
    button->setToolTip(toolTip);
    button->setCheckable(checkable);
    button->setMinimumHeight(36);
    button->setAutoRaise(false);
    return button;
}

constexpr int kOverlayDimAlpha = 92;

class PassiveScreenOverlay final : public QWidget
{
public:
    PassiveScreenOverlay(CaptureOverlay* owner, const QRect& screenGeometry)
        : QWidget(owner)
        , m_owner(owner)
        , m_screenGeometry(screenGeometry)
    {
        setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool);
        setAttribute(Qt::WA_DeleteOnClose, false);
        setAttribute(Qt::WA_ShowWithoutActivating, true);
        setFocusPolicy(Qt::StrongFocus);
        setMouseTracking(true);
        setCursor(Qt::CrossCursor);
        setGeometry(screenGeometry);
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        if (m_owner)
            m_owner->paintScreenOverlay(painter, m_screenGeometry, rect());
    }

    void mousePressEvent(QMouseEvent* event) override
    {
        if (m_owner)
            m_owner->handleExternalMousePress(event);
    }

    void mouseMoveEvent(QMouseEvent* event) override
    {
        if (m_owner)
            m_owner->handleExternalMouseMove(event);
    }

    void mouseReleaseEvent(QMouseEvent* event) override
    {
        if (m_owner)
            m_owner->handleExternalMouseRelease(event);
    }

    void keyPressEvent(QKeyEvent* event) override
    {
        if (event->key() == Qt::Key_Escape && m_owner) {
            m_owner->cancelCapture();
            return;
        }
        QWidget::keyPressEvent(event);
    }

private:
    CaptureOverlay* m_owner = nullptr;
    QRect m_screenGeometry;
};

#ifdef Q_OS_WIN
struct WindowSearchContext {
    POINT physicalPoint;
    QSet<HWND> ignores;
    HWND found = nullptr;
    RECT foundRect{};
};

BOOL CALLBACK enumWindowsForPoint(HWND hwnd, LPARAM lParam)
{
    auto* context = reinterpret_cast<WindowSearchContext*>(lParam);
    if (!context || !hwnd || context->ignores.contains(hwnd) || !IsWindowVisible(hwnd))
        return TRUE;

    HWND root = GetAncestor(hwnd, GA_ROOT);
    if (root != hwnd || context->ignores.contains(hwnd))
        return TRUE;

    DWORD exStyle = GetWindowLong(hwnd, GWL_EXSTYLE);
    if (exStyle & WS_EX_TRANSPARENT)
        return TRUE;

    RECT rect{};
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &rect, sizeof(rect)))) {
        // Use extended frame bounds if available to omit invisible shadows
    } else if (!GetWindowRect(hwnd, &rect)) {
        return TRUE;
    }

    if (!PtInRect(&rect, context->physicalPoint))
        return TRUE;

    BOOL cloaked = FALSE;
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked)
        return TRUE;

    wchar_t className[256];
    if (GetClassNameW(hwnd, className, 256)) {
        QString cls = QString::fromWCharArray(className);
        if (cls == "EdgeUiInputTopWnd" || cls == "DummyDWMListenerWindow" || cls == "SysShadow" || cls == "Progman")
            return TRUE;
    }

    context->found = hwnd;
    context->foundRect = rect;
    return FALSE;
}
#endif

} // namespace

CaptureOverlay::CaptureOverlay(QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(tr("화면 캡쳐"));
    setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool);
    setModal(true);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setCursor(Qt::CrossCursor);
    setAttribute(Qt::WA_DeleteOnClose, false);

    for (QScreen* screen : QGuiApplication::screens()) {
        if (screen)
            m_virtualGeometry = m_virtualGeometry.united(screen->geometry());
    }
    if (m_virtualGeometry.isEmpty() && QGuiApplication::primaryScreen())
        m_virtualGeometry = QGuiApplication::primaryScreen()->geometry();

    QScreen* activeScreen = QGuiApplication::screenAt(QCursor::pos());
    if (!activeScreen && parentWidget())
        activeScreen = QGuiApplication::screenAt(parentWidget()->mapToGlobal(parentWidget()->rect().center()));
    if (!activeScreen)
        activeScreen = QGuiApplication::primaryScreen();

    m_overlayGeometry = activeScreen ? activeScreen->geometry() : m_virtualGeometry;
    if (m_overlayGeometry.isEmpty())
        m_overlayGeometry = m_virtualGeometry;

    m_activeScreenLocalGeometry = QRect(QPoint(0, 0), m_overlayGeometry.size());
    if (m_activeScreenLocalGeometry.isEmpty())
        m_activeScreenLocalGeometry = QRect(QPoint(0, 0), m_overlayGeometry.size());

    m_screenRegion = QRegion(QRect(QPoint(0, 0), m_overlayGeometry.size()));

    m_desktopImage = WindowsGraphicsCapture::captureVirtualDesktop();
    m_blurredImage = blurredImage(m_desktopImage);

    setGeometry(m_overlayGeometry);
    if (auto* app = QCoreApplication::instance())
        app->installEventFilter(this);
    m_escapeShortcut = new QShortcut(QKeySequence(Qt::Key_Escape), this);
    m_escapeShortcut->setContext(Qt::ApplicationShortcut);
    connect(m_escapeShortcut, &QShortcut::activated, this, [this] { cancelCapture(); });
    m_escapePollTimer = new QTimer(this);
    m_escapePollTimer->setInterval(30);
    connect(m_escapePollTimer, &QTimer::timeout, this, [this] {
#ifdef Q_OS_WIN
        if ((GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0)
            cancelCapture();
#endif
    });
    m_escapePollTimer->start();
    buildToolBar();

    for (QScreen* screen : QGuiApplication::screens()) {
        if (!screen)
            continue;
        const QRect screenGeometry = screen->geometry();
        if (screenGeometry == m_overlayGeometry)
            continue;
        auto* overlay = new PassiveScreenOverlay(this, screenGeometry);
        overlay->show();
        m_passiveOverlays.append(overlay);
    }
    raise();
    activateWindow();
    QTimer::singleShot(0, this, [this] {
        raise();
        activateWindow();
        setFocus(Qt::ActiveWindowFocusReason);
        grabKeyboard();
        if (m_options.targetMode == TargetMode::Window)
            updateHoveredWindow(QCursor::pos());
    });
}

CaptureOverlay::~CaptureOverlay()
{
    if (auto* app = QCoreApplication::instance())
        app->removeEventFilter(this);
    if (QWidget::keyboardGrabber() == this)
        releaseKeyboard();

    for (QWidget* overlay : std::as_const(m_passiveOverlays)) {
        if (overlay) {
            overlay->close();
            delete overlay;
        }
    }
    m_passiveOverlays.clear();
}

CaptureResult CaptureOverlay::run(QWidget* parent)
{
    CaptureOverlay overlay(parent);
    if (overlay.exec() == QDialog::Accepted)
        return overlay.result();
    CaptureResult result = overlay.result();
    result.accepted = false;
    result.canceled = true;
    return result;
}

CaptureResult CaptureOverlay::result() const
{
    return m_result;
}

void CaptureOverlay::cancelCapture()
{
    m_result.accepted = false;
    m_result.canceled = true;
    if (QWidget::keyboardGrabber() == this)
        releaseKeyboard();
    reject();
}

bool CaptureOverlay::eventFilter(QObject* watched, QEvent* event)
{
    Q_UNUSED(watched)

    if (event && (event->type() == QEvent::KeyPress || event->type() == QEvent::ShortcutOverride)) {
        auto* keyEvent = static_cast<QKeyEvent*>(event);
        if (keyEvent->key() == Qt::Key_Escape) {
            cancelCapture();
            return true;
        }
    }

    return false;
}

void CaptureOverlay::showEvent(QShowEvent* event)
{
    QDialog::showEvent(event);
    raise();
    activateWindow();
    setFocus(Qt::ActiveWindowFocusReason);
    grabKeyboard();
}

void CaptureOverlay::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    paintScreenOverlay(painter, m_overlayGeometry, rect());

    painter.setPen(QColor(255, 255, 255, 230));
    const QRect guideRect = activeScreenLocalGeometry().adjusted(0, 82, 0, 0);
    painter.drawText(guideRect, Qt::AlignHCenter | Qt::AlignTop,
                     m_options.targetMode == TargetMode::Window
                         ? tr("캡쳐할 창 위로 마우스를 이동한 뒤 클릭하세요. Esc 키로 취소")
                         : tr("캡쳐할 영역을 선택하세요. Esc 키로 취소"));
}

void CaptureOverlay::paintScreenOverlay(QPainter& painter, const QRect& screenGeometry, const QRect& localRect) const
{
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    drawDesktopBackgroundForScreen(painter, localRect, screenGeometry, true);

    if (m_options.targetMode == TargetMode::Window && !m_hoverWindowLogicalRect.isEmpty()) {
        const QRect hoverLocalRect = m_hoverWindowLogicalRect.translated(-screenGeometry.topLeft()).intersected(localRect);
        if (!hoverLocalRect.isEmpty()) {
            painter.save();
            painter.setClipRect(hoverLocalRect);
            drawDesktopBackgroundForScreen(painter, localRect, screenGeometry, false);
            painter.restore();

            painter.fillRect(hoverLocalRect, QColor(70, 150, 255, 30));
            QPen hoverPen(QColor(70, 150, 255), 3.0);
            hoverPen.setCosmetic(true);
            painter.setPen(hoverPen);
            painter.setBrush(Qt::NoBrush);
            painter.drawRect(hoverLocalRect.adjusted(1, 1, -2, -2));
        }
    }

    const QRect logicalSelection = normalizedSelection();
    const QRect localSelection = logicalSelection.translated(-screenGeometry.topLeft()).intersected(localRect);
    if (!localSelection.isEmpty()) {
        painter.save();
        if (m_options.targetMode == TargetMode::Ellipse) {
            QPainterPath path;
            path.addEllipse(logicalSelection);
            painter.setClipPath(QTransform::fromTranslate(-screenGeometry.left(), -screenGeometry.top()).map(path));
        } else if (m_options.targetMode == TargetMode::Freeform && !m_freeformPath.isEmpty()) {
            painter.setClipPath(QTransform::fromTranslate(-screenGeometry.left(), -screenGeometry.top()).map(m_freeformPath));
        } else {
            painter.setClipRect(localSelection);
        }
        drawDesktopBackgroundForScreen(painter, localRect, screenGeometry, false);
        painter.restore();

        QPen border(QColor(85, 165, 255), 2.0);
        border.setCosmetic(true);
        painter.setPen(border);
        painter.setBrush(Qt::NoBrush);
        if (m_options.targetMode == TargetMode::Ellipse)
            painter.drawEllipse(localSelection);
        else if (m_options.targetMode == TargetMode::Freeform && !m_freeformPath.isEmpty())
            painter.drawPath(QTransform::fromTranslate(-screenGeometry.left(), -screenGeometry.top()).map(m_freeformPath));
        else
            painter.drawRect(localSelection.adjusted(0, 0, -1, -1));
    }
}

void CaptureOverlay::mousePressEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton) {
        QDialog::mousePressEvent(event);
        return;
    }

    if (m_toolBar && m_toolBar->geometry().contains(event->pos())) {
        QDialog::mousePressEvent(event);
        return;
    }

    const QPoint globalPos = event->globalPosition().toPoint();
    if (m_options.targetMode == TargetMode::Window) {
        const QRect windowRect = windowRectAt(globalPos);
        if (!windowRect.isEmpty())
            finishCapture(windowRect);
        return;
    }

    m_dragging = true;
    m_dragStart = globalPos;
    m_dragCurrent = globalPos;
    m_freeformPath = {};
    if (m_options.targetMode == TargetMode::Freeform)
        m_freeformPath.moveTo(globalPos);
    refreshOverlayWindows();
}

void CaptureOverlay::mouseMoveEvent(QMouseEvent* event)
{
    if (!m_dragging && m_options.targetMode == TargetMode::Window) {
        updateHoveredWindow(event->globalPosition().toPoint());
        return;
    }

    if (!m_dragging) {
        QDialog::mouseMoveEvent(event);
        return;
    }

    m_dragCurrent = event->globalPosition().toPoint();
    if (m_options.targetMode == TargetMode::Freeform)
        m_freeformPath.lineTo(m_dragCurrent);
    refreshOverlayWindows();
}

void CaptureOverlay::mouseReleaseEvent(QMouseEvent* event)
{
    if (!m_dragging || event->button() != Qt::LeftButton) {
        QDialog::mouseReleaseEvent(event);
        return;
    }

    m_dragCurrent = event->globalPosition().toPoint();
    if (m_options.targetMode == TargetMode::Freeform)
        m_freeformPath.lineTo(m_dragCurrent);
    m_dragging = false;

    const QRect selection = normalizedSelection();
    if (selection.width() < 4 || selection.height() < 4) {
        refreshOverlayWindows();
        return;
    }

    QPainterPath logicalMask;
    if (m_options.targetMode == TargetMode::Ellipse) {
        logicalMask.addEllipse(selection);
    } else if (m_options.targetMode == TargetMode::Freeform && !m_freeformPath.isEmpty()) {
        logicalMask = m_freeformPath;
    }

    finishCapture(selection, logicalMask);
}

void CaptureOverlay::handleExternalMousePress(QMouseEvent* event)
{
    if (!event)
        return;
    if (event->button() != Qt::LeftButton) {
        event->ignore();
        return;
    }

    const QPoint globalPos = event->globalPosition().toPoint();
    if (m_options.targetMode == TargetMode::Window) {
        const QRect windowRect = windowRectAt(globalPos);
        if (!windowRect.isEmpty())
            finishCapture(windowRect);
        return;
    }

    m_dragging = true;
    m_dragStart = globalPos;
    m_dragCurrent = globalPos;
    m_freeformPath = {};
    if (m_options.targetMode == TargetMode::Freeform)
        m_freeformPath.moveTo(globalPos);
    refreshOverlayWindows();
}

void CaptureOverlay::handleExternalMouseMove(QMouseEvent* event)
{
    if (!event)
        return;

    if (!m_dragging && m_options.targetMode == TargetMode::Window) {
        updateHoveredWindow(event->globalPosition().toPoint());
        return;
    }
    if (!m_dragging)
        return;

    m_dragCurrent = event->globalPosition().toPoint();
    if (m_options.targetMode == TargetMode::Freeform)
        m_freeformPath.lineTo(m_dragCurrent);
    refreshOverlayWindows();
}

void CaptureOverlay::handleExternalMouseRelease(QMouseEvent* event)
{
    if (!event)
        return;
    if (!m_dragging || event->button() != Qt::LeftButton)
        return;

    m_dragCurrent = event->globalPosition().toPoint();
    if (m_options.targetMode == TargetMode::Freeform)
        m_freeformPath.lineTo(m_dragCurrent);
    m_dragging = false;

    const QRect selection = normalizedSelection();
    if (selection.width() < 4 || selection.height() < 4) {
        refreshOverlayWindows();
        return;
    }

    QPainterPath logicalMask;
    if (m_options.targetMode == TargetMode::Ellipse)
        logicalMask.addEllipse(selection);
    else if (m_options.targetMode == TargetMode::Freeform && !m_freeformPath.isEmpty())
        logicalMask = m_freeformPath;

    finishCapture(selection, logicalMask);
}

void CaptureOverlay::keyPressEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Escape) {
        cancelCapture();
        return;
    }
    QDialog::keyPressEvent(event);
}

void CaptureOverlay::resizeEvent(QResizeEvent* event)
{
    QDialog::resizeEvent(event);
    placeToolBar();
}

void CaptureOverlay::buildToolBar()
{
    m_toolBar = new QWidget(this);
    m_toolBar->setObjectName(QStringLiteral("captureOverlayToolBar"));

    const auto& themeManager = ThemeManager::instance();
    QColor bgColor = themeManager.toolBarColor(); // Use toolbar color
    bgColor.setAlpha(235);
    QColor fgColor = themeManager.foregroundColor();
    QColor highlight = themeManager.color("Theme/AccentColor");
    if (!highlight.isValid()) highlight = QColor(68, 137, 255);
    QColor border = fgColor; border.setAlpha(70);
    QColor hover = fgColor; hover.setAlpha(35);
    QColor checkedBg = highlight; checkedBg.setAlpha(170);
    QColor checkedBorder = fgColor; checkedBorder.setAlpha(120);

    m_toolBar->setStyleSheet(QString(
        "#captureOverlayToolBar { background: rgba(%1, %2, %3, %4); border: 1px solid rgba(%5, %6, %7, %8); border-radius: 12px; }"
        "QToolButton { color: rgb(%9, %10, %11); padding: 7px 11px; border: 1px solid transparent; border-radius: 7px; }"
        "QToolButton:checked { background: rgba(%12, %13, %14, %15); border-color: rgba(%16, %17, %18, %19); }"
        "QToolButton:hover { background: rgba(%20, %21, %22, %23); }"
    ).arg(bgColor.red()).arg(bgColor.green()).arg(bgColor.blue()).arg(bgColor.alpha())
     .arg(border.red()).arg(border.green()).arg(border.blue()).arg(border.alpha())
     .arg(fgColor.red()).arg(fgColor.green()).arg(fgColor.blue())
     .arg(checkedBg.red()).arg(checkedBg.green()).arg(checkedBg.blue()).arg(checkedBg.alpha())
     .arg(checkedBorder.red()).arg(checkedBorder.green()).arg(checkedBorder.blue()).arg(checkedBorder.alpha())
     .arg(hover.red()).arg(hover.green()).arg(hover.blue()).arg(hover.alpha()));

    auto* layout = new QHBoxLayout(m_toolBar);
    layout->setContentsMargins(12, 9, 12, 9);
    layout->setSpacing(7);

    auto* group = new QButtonGroup(m_toolBar);
    group->setExclusive(true);

    m_fullScreenButton = makeCaptureButton(m_toolBar, tr("전체"), tr("전체 화면 캡쳐"), false);
    m_windowButton = makeCaptureButton(m_toolBar, tr("창"), tr("창 단위 캡쳐"));
    m_rectButton = makeCaptureButton(m_toolBar, tr("사각형"), tr("직사각형 영역 캡쳐"));
    m_ellipseButton = makeCaptureButton(m_toolBar, tr("원형"), tr("원형/타원 영역 캡쳐"));
    m_freeformButton = makeCaptureButton(m_toolBar, tr("자유형"), tr("자유형 영역 캡쳐"));

    group->addButton(m_windowButton);
    group->addButton(m_rectButton);
    group->addButton(m_ellipseButton);
    group->addButton(m_freeformButton);

    layout->addWidget(m_fullScreenButton);
    layout->addWidget(m_windowButton);
    layout->addWidget(m_rectButton);
    layout->addWidget(m_ellipseButton);
    layout->addWidget(m_freeformButton);

    m_markupButton = makeCaptureButton(m_toolBar, tr("마크업"), tr("캡쳐 후 바로 복사하지 않고 캡쳐 전용 마크업 편집을 표시합니다."), true);
    layout->addWidget(m_markupButton);

    auto* cancelButton = makeCaptureButton(m_toolBar, tr("취소"), tr("캡쳐 취소"), false);
    layout->addWidget(cancelButton);

    connect(m_fullScreenButton, &QToolButton::clicked, this, [this] {
        finishCapture(m_virtualGeometry);
    });
    connect(m_windowButton, &QToolButton::clicked, this, [this] { setTargetMode(TargetMode::Window); });
    connect(m_rectButton, &QToolButton::clicked, this, [this] { setTargetMode(TargetMode::Rectangle); });
    connect(m_ellipseButton, &QToolButton::clicked, this, [this] { setTargetMode(TargetMode::Ellipse); });
    connect(m_freeformButton, &QToolButton::clicked, this, [this] { setTargetMode(TargetMode::Freeform); });
    connect(m_markupButton, &QToolButton::toggled, this, [this](bool checked) { m_options.markupEnabled = checked; });
    connect(cancelButton, &QToolButton::clicked, this, [this] { cancelCapture(); });

    updateToolChecks();
    placeToolBar();
    m_toolBar->raise();
}

void CaptureOverlay::setTargetMode(TargetMode mode)
{
    m_options.targetMode = mode;
    m_dragging = false;
    m_dragStart = {};
    m_dragCurrent = {};
    m_freeformPath = {};
    m_hoverWindowLogicalRect = {};
    m_hoverWindowLocalRect = {};
    if (mode == TargetMode::Window)
        updateHoveredWindow(QCursor::pos());
    updateToolChecks();
    refreshOverlayWindows();
}

void CaptureOverlay::finishCapture(const QRect& logicalRect, const QPainterPath& logicalMask)
{
    hideOverlaysForCapture();

    QImage image = imageForSelection(logicalRect, logicalMask);
    if (image.isNull()) {
        m_result.accepted = false;
        m_result.canceled = true;
        reject();
        return;
    }

    if (m_options.markupEnabled) {
        if (QWidget::keyboardGrabber() == this)
            releaseKeyboard();
        if (auto* app = QCoreApplication::instance())
            app->removeEventFilter(this);
        CaptureMarkupDialog dialog(image, logicalRect, parentWidget());
        if (dialog.exec() != QDialog::Accepted) {
            m_result.accepted = false;
            m_result.canceled = true;
            reject();
            return;
        }
        image = dialog.markedImage();
    }

    m_result.accepted = true;
    m_result.canceled = false;
    m_result.options = m_options;
    m_result.logicalRect = logicalRect;
    m_result.image = image;
    if (QWidget::keyboardGrabber() == this)
        releaseKeyboard();
    accept();
}

void CaptureOverlay::hideOverlaysForCapture()
{
    if (m_toolBar)
        m_toolBar->hide();
    hide();
    for (QWidget* overlay : std::as_const(m_passiveOverlays)) {
        if (overlay)
            overlay->hide();
    }
    if (QWidget::keyboardGrabber() == this)
        releaseKeyboard();
    QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
    QThread::msleep(80);
    QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
}

void CaptureOverlay::drawDesktopBackground(QPainter& painter, const QRect& targetRect, bool dimmed) const
{
    drawDesktopBackgroundForScreen(painter, targetRect, m_overlayGeometry, dimmed);
}

void CaptureOverlay::drawDesktopBackgroundForScreen(QPainter& painter, const QRect& targetRect, const QRect& screenGeometry, bool dimmed) const
{
    const QImage& image = dimmed && !m_blurredImage.isNull() ? m_blurredImage : m_desktopImage;
    if (!image.isNull()) {
        const QRect sourceRect(targetRect.topLeft() + screenGeometry.topLeft() - m_virtualGeometry.topLeft(),
                               targetRect.size());
        painter.drawImage(targetRect, image, sourceRect.intersected(image.rect()));
    } else {
        painter.fillRect(targetRect, QColor(25, 25, 25));
    }

    if (dimmed)
        painter.fillRect(targetRect, QColor(0, 0, 0, kOverlayDimAlpha));
}

void CaptureOverlay::refreshOverlayWindows()
{
    update();
    for (QWidget* overlay : std::as_const(m_passiveOverlays)) {
        if (overlay)
            overlay->update();
    }
}

QImage CaptureOverlay::imageForSelection(const QRect& logicalRect, const QPainterPath& logicalMask) const
{
    if (logicalRect.isEmpty())
        return {};

    QImage capturedImage = WindowsGraphicsCapture::captureLogicalRect(logicalRect);
    const qreal imageDpr = qMax<qreal>(1.0, capturedImage.devicePixelRatio());
    QImage image = capturedImage.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    image.setDevicePixelRatio(imageDpr);
    if (image.isNull())
        return {};

    if (logicalMask.isEmpty())
        return image;

    QImage masked(image.size(), QImage::Format_ARGB32_Premultiplied);
    masked.setDevicePixelRatio(imageDpr);
    masked.fill(Qt::transparent);
    QPainter painter(&masked);
    painter.setRenderHint(QPainter::Antialiasing);
    const qreal xScale = static_cast<qreal>(image.width()) / logicalRect.width();
    const qreal yScale = static_cast<qreal>(image.height()) / logicalRect.height();
    const QTransform maskTransform(xScale, 0.0,
                                   0.0, yScale,
                                   -logicalRect.left() * xScale,
                                   -logicalRect.top() * yScale);
    QPainterPath localMask = maskTransform.map(logicalMask);
    painter.setClipPath(localMask);
    painter.drawImage(QPoint(0, 0), image);
    return masked;
}

QRect CaptureOverlay::normalizedSelection() const
{
    if (m_options.targetMode == TargetMode::Freeform && !m_freeformPath.isEmpty())
        return m_freeformPath.boundingRect().toAlignedRect().intersected(m_virtualGeometry);
    return QRect(m_dragStart, m_dragCurrent).normalized().intersected(m_virtualGeometry);
}

QRect CaptureOverlay::logicalRectFromLocal(const QRect& localRect) const
{
    return localRect.translated(m_overlayGeometry.topLeft());
}

QPoint CaptureOverlay::localPointFromEvent(const QPoint& pos) const
{
    return QPoint(qBound(0, pos.x(), width() - 1), qBound(0, pos.y(), height() - 1));
}

QRect CaptureOverlay::windowRectAt(const QPoint& globalPos) const
{
#ifdef Q_OS_WIN
    WindowSearchContext context;
    POINT physicalPt;
    if (!GetCursorPos(&physicalPt)) {
        physicalPt.x = globalPos.x();
        physicalPt.y = globalPos.y();
    }
    context.physicalPoint = physicalPt;

    // Ignore self overlay windows
    context.ignores.insert(reinterpret_cast<HWND>(effectiveWinId()));
    if (m_toolBar)
        context.ignores.insert(reinterpret_cast<HWND>(m_toolBar->effectiveWinId()));
    for (QWidget* passive : std::as_const(m_passiveOverlays)) {
        if (passive)
            context.ignores.insert(reinterpret_cast<HWND>(passive->effectiveWinId()));
    }

    EnumWindows(enumWindowsForPoint, reinterpret_cast<LPARAM>(&context));
    if (context.found) {
        // Map physical foundRect back to logical coordinates.
        // We find the monitor containing the center of foundRect to determine its DPR.
        POINT centerPt;
        centerPt.x = context.foundRect.left + (context.foundRect.right - context.foundRect.left) / 2;
        centerPt.y = context.foundRect.top + (context.foundRect.bottom - context.foundRect.top) / 2;
        HMONITOR hMonitor = MonitorFromPoint(centerPt, MONITOR_DEFAULTTONEAREST);
        UINT dpiX = 96, dpiY = 96;
        using GetDpiForMonitorFn = HRESULT(STDAPICALLTYPE*)(HMONITOR, int, UINT*, UINT*);
        if (HMODULE shcore = LoadLibraryW(L"shcore.dll")) {
            if (auto getDpi = reinterpret_cast<GetDpiForMonitorFn>(GetProcAddress(shcore, "GetDpiForMonitor"))) {
                getDpi(hMonitor, 0 /*MDT_EFFECTIVE_DPI*/, &dpiX, &dpiY);
            }
            FreeLibrary(shcore);
        }

        const qreal dpr = dpiX / 96.0;

        // Rather than absolute mapping, we align logical rect by mapping physical size and center
        // to Qt's logical representation space.
        // Simple scale usually works if Qt uses the same origin logic:
        int lLeft = qRound(context.foundRect.left / dpr);
        int lTop = qRound(context.foundRect.top / dpr);
        int lWidth = qRound((context.foundRect.right - context.foundRect.left) / dpr);
        int lHeight = qRound((context.foundRect.bottom - context.foundRect.top) / dpr);

        // However, multi-monitor with different DPI can offset the origin physically vs logically.
        // Let's rely on QScreen's physical-to-logical mapping for the top-left offset if possible:
        // Actually, we can just find which QScreen matches this monitor and translate from physical origin to logical origin.
        MONITORINFOEXW mi;
        mi.cbSize = sizeof(mi);
        GetMonitorInfoW(hMonitor, &mi);
        for (QScreen* screen : QGuiApplication::screens()) {
            if (screen->name() == QString::fromWCharArray(mi.szDevice)) {
                lLeft = screen->geometry().left() + qRound((context.foundRect.left - mi.rcMonitor.left) / dpr);
                lTop = screen->geometry().top() + qRound((context.foundRect.top - mi.rcMonitor.top) / dpr);
                break;
            }
        }

        QRect rect(lLeft, lTop, lWidth, lHeight);
        return rect.intersected(m_virtualGeometry);
    }
#else
    Q_UNUSED(globalPos)
#endif
    return {};
}

void CaptureOverlay::updateHoveredWindow(const QPoint& globalPos)
{
    const QRect logicalRect = windowRectAt(globalPos);
    const QRect localRect = logicalRect.translated(-m_overlayGeometry.topLeft()).intersected(rect());
    if (logicalRect == m_hoverWindowLogicalRect && localRect == m_hoverWindowLocalRect)
        return;

    m_hoverWindowLogicalRect = logicalRect;
    m_hoverWindowLocalRect = localRect;
    refreshOverlayWindows();
}

QRect CaptureOverlay::activeScreenLocalGeometry() const
{
    return m_activeScreenLocalGeometry.isEmpty() ? rect() : m_activeScreenLocalGeometry;
}

void CaptureOverlay::updateToolChecks()
{
    if (m_windowButton) m_windowButton->setChecked(m_options.targetMode == TargetMode::Window);
    if (m_rectButton) m_rectButton->setChecked(m_options.targetMode == TargetMode::Rectangle);
    if (m_ellipseButton) m_ellipseButton->setChecked(m_options.targetMode == TargetMode::Ellipse);
    if (m_freeformButton) m_freeformButton->setChecked(m_options.targetMode == TargetMode::Freeform);
    if (m_markupButton) m_markupButton->setChecked(m_options.markupEnabled);
}

void CaptureOverlay::placeToolBar()
{
    if (!m_toolBar)
        return;

    const QSize hint = m_toolBar->sizeHint();
    const QRect screenRect = activeScreenLocalGeometry();
    const int x = qBound(screenRect.left() + 8,
                         screenRect.left() + (screenRect.width() - hint.width()) / 2,
                         qMax(screenRect.left() + 8, screenRect.right() - hint.width() - 8));
    const int y = screenRect.top() + 16;
    m_toolBar->setGeometry(x, y, hint.width(), hint.height());
    m_toolBar->raise();
}

QImage CaptureOverlay::blurredImage(const QImage& source)
{
    if (source.isNull())
        return {};

    const QSize smallSize(qMax(1, source.width() / 12), qMax(1, source.height() / 12));
    QImage blurred = source.scaled(smallSize, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
        .scaled(source.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
        .convertToFormat(source.format());
    blurred.setDevicePixelRatio(source.devicePixelRatio());
    return blurred;
}

} // namespace Capture

















