#include "capture/CaptureMarkupDialog.h"

#include "capture/WindowsGraphicsCapture.h"

#include <QApplication>
#include <fmstyle/ThemeManager.h>
#include <QButtonGroup>
#include <QCheckBox>
#include <QColorDialog>
#include <QCursor>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScreen>
#include <QSpinBox>
#include <QStyle>
#include <QToolButton>
#include <QLabel>
#include <QSlider>
#include <QGridLayout>
#include <QVBoxLayout>
#include <QPushButton>
#include <QMenu>
#include <QAction>

#include <functional>
#include <cmath>

#include "core/ThemeManager.h"

namespace Capture {
namespace {

class StrokePreview : public QWidget {
public:
    QColor color;
    int weight;
    StrokePreview(QWidget* parent = nullptr) : QWidget(parent), color(Qt::black), weight(4) {
        setMinimumHeight(30);
    }
    void updateStyle(const QColor& c, int w) {
        color = c;
        weight = w;
        update();
    }
protected:
    void paintEvent(QPaintEvent* event) override {
        Q_UNUSED(event);
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        QPen pen(color, weight, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        p.setPen(pen);

        QPainterPath path;
        path.moveTo(10, height() / 2);
        path.cubicTo((width() * 0.3), height() / 2 - 20, (width() * 0.7), height() / 2 + 20, width() - 10, height() / 2);
        p.drawPath(path);
    }
};

void showPenPopup(QWidget* button, QColor current_color, int current_width, std::function<void(QColor, int)> callback) {
    if (!button) return;
    auto* popup = new QWidget(button->window(), Qt::Popup | Qt::FramelessWindowHint);
    popup->setAttribute(Qt::WA_DeleteOnClose);

    QColor bgColor = ThemeManager::instance().toolBarColor();
    QColor fgColor = ThemeManager::instance().foregroundColor();
    QColor borderColor = fgColor;
    borderColor.setAlpha(70);
    QString bgStr = QString("rgba(%1, %2, %3, 255)").arg(bgColor.red()).arg(bgColor.green()).arg(bgColor.blue());
    QString fgStr = QString("rgba(%1, %2, %3, 255)").arg(fgColor.red()).arg(fgColor.green()).arg(fgColor.blue());
    QString borderStr = QString("rgba(%1, %2, %3, %4)").arg(borderColor.red()).arg(borderColor.green()).arg(borderColor.blue()).arg(borderColor.alpha());
    const auto& Tokens = fm::style::ThemeManager::instance().colors();

    popup->setStyleSheet(QString(
        "QWidget { background: %1; color: %2; border: 1px solid %3; border-radius: 8px; }"
        "QLabel { border: none; font-weight: bold; background: none; color: %2; }"
        "QToolButton { border-radius: 12px; border: 2px solid transparent; background: none; }"
        "QToolButton:checked { border-color: %2; }"
        "QSlider::groove:horizontal { border: 1px solid %3; height: 4px; background: %3; margin: 2px 0; border-radius: 2px; }"
        "QSlider::handle:horizontal { background: %4; border: 1px solid %5; width: 14px; margin: -5px 0; border-radius: 7px; }"
    ).arg(bgStr, fgStr, borderStr, Tokens[fm::style::Token::Accent].name(), Tokens[fm::style::Token::Accent].darker(125).name()));

    auto* mainLayout = new QVBoxLayout(popup);
    mainLayout->setContentsMargins(16, 16, 16, 16);
    mainLayout->setSpacing(10);

    auto* colorLabel = new QLabel("Colors", popup);
    mainLayout->addWidget(colorLabel);

    auto* colorGrid = new QGridLayout();
    colorGrid->setSpacing(6);

    QList<QColor> colors = {
        QColor(0,0,0), QColor(255,255,255), QColor(220,220,220), QColor(160,160,160), QColor(100,100,100), QColor(64,64,64),
        QColor(176,0,86), QColor(220,40,40), QColor(255,102,0), QColor(255,160,0), QColor(250,210,0), QColor(255,255,0),
        QColor(172,230,0), QColor(0,230,0), QColor(0,128,64), QColor(0,172,230), QColor(0,102,255), QColor(0,0,255),
        QColor(102,0,255), QColor(128,0,128), QColor(255,204,204), QColor(204,153,102), QColor(128,64,0), QColor(80,40,0),
        QColor(255,102,255), QColor(255,204,153), QColor(255,255,153), QColor(153,255,153), QColor(153,204,255), QColor(200,180,255)
    };

    int row = 0; int col = 0;
    auto* colorGroup = new QButtonGroup(popup);
    for (int i = 0; i < colors.size(); ++i) {
        QColor c = colors[i];
        auto* cb = new QToolButton(popup);
        cb->setFixedSize(24, 24);
        cb->setCheckable(true);
        cb->setCursor(Qt::PointingHandCursor);
        // Light swatches get the theme line so they stay visible; the checked ring uses the text color of the theme.
        QString borderBase = (c == QColor(255,255,255)) ? QString("border: 1px solid %1;").arg(Tokens[fm::style::Token::Line].name()) : QString();
        cb->setStyleSheet(QString("QToolButton { background-color: %1; %2 } QToolButton:checked { border: 2px solid %3; }").arg(c.name(), borderBase, Tokens[fm::style::Token::Fg].name()));

        if (c == current_color) cb->setChecked(true);
        colorGroup->addButton(cb, i);
        colorGrid->addWidget(cb, row, col);
        col++;
        if (col >= 6) { col = 0; row++; }
    }
    mainLayout->addLayout(colorGrid);

    auto* sizeLabel = new QLabel("Size", popup);
    sizeLabel->setStyleSheet("margin-top: 10px; border: none; font-weight: bold; background: none;");
    mainLayout->addWidget(sizeLabel);

    auto* preview = new StrokePreview(popup);
    preview->updateStyle(current_color, current_width);
    mainLayout->addWidget(preview);

    auto* slider = new QSlider(Qt::Horizontal, popup);
    slider->setRange(1, 40);
    slider->setValue(current_width);
    slider->setCursor(Qt::PointingHandCursor);
    mainLayout->addWidget(slider);

    auto updatePreviewAndCallback = [=]() {
        int id = colorGroup->checkedId();
        QColor c = id >= 0 ? colors[id] : current_color;
        int w = slider->value();
        preview->updateStyle(c, w);
        callback(c, w);
    };

    QObject::connect(colorGroup, &QButtonGroup::idClicked, slider, updatePreviewAndCallback);
    QObject::connect(slider, &QSlider::valueChanged, preview, updatePreviewAndCallback);

    popup->adjustSize();
    QPoint pos = button->mapToGlobal(QPoint(button->width() / 2, button->height() + 5));
    pos.setX(pos.x() - popup->width() / 2); // Center below button

    // Attempt to handle screen bounds
    QScreen* screen = QGuiApplication::screenAt(pos);
    if (!screen) screen = QGuiApplication::primaryScreen();
    if (screen) {
        QRect screenGeometry = screen->geometry();
        if (pos.x() < screenGeometry.left()) pos.setX(screenGeometry.left() + 5);
        if (pos.x() + popup->width() > screenGeometry.right()) pos.setX(screenGeometry.right() - popup->width() - 5);
        if (pos.y() + popup->height() > screenGeometry.bottom()) pos.setY(button->mapToGlobal(QPoint(0,0)).y() - popup->height() - 5);
    }

    popup->move(pos);
    popup->show();
}

void showShapePopup(QWidget* button, Capture::MarkupShape current_shape, QColor current_outline_color, QColor current_fill_color, bool has_fill, int current_width, const QString& current_emoji, std::function<void(Capture::MarkupShape, QColor, QColor, bool, int, QString)> callback) {
    if (!button) return;

    struct ShapeState {
        Capture::MarkupShape shape;
        QColor outline;
        QColor fill;
        bool hasFill;
        int width;
        QString emoji;
    };
    auto state = std::make_shared<ShapeState>();
    state->shape = current_shape;
    state->outline = current_outline_color;
    state->fill = current_fill_color;
    state->hasFill = has_fill;
    state->width = current_width;
    state->emoji = current_emoji;

    auto* popup = new QWidget(button->window(), Qt::Popup | Qt::FramelessWindowHint);
    popup->setAttribute(Qt::WA_DeleteOnClose);

    QColor bgColor = ThemeManager::instance().toolBarColor();
    QColor fgColor = ThemeManager::instance().foregroundColor();
    QColor borderColor = fgColor;
    borderColor.setAlpha(70);
    QString bgStr = QString("rgba(%1, %2, %3, 255)").arg(bgColor.red()).arg(bgColor.green()).arg(bgColor.blue());
    QString fgStr = QString("rgba(%1, %2, %3, 255)").arg(fgColor.red()).arg(fgColor.green()).arg(fgColor.blue());
    QString borderStr = QString("rgba(%1, %2, %3, %4)").arg(borderColor.red()).arg(borderColor.green()).arg(borderColor.blue()).arg(borderColor.alpha());

    QString styleHelper = QString(
        "QWidget { background: %1; color: %2; border: 1px solid %3; border-radius: 8px; }"
        "QLabel { border: none; font-weight: bold; background: none; color: %2; }"
        "QToolButton, QPushButton { border-radius: 6px; border: 2px solid transparent; background: none; padding: 4px; }"
        "QToolButton:checked, QPushButton:checked { border-color: %4; background: %5; }"
        "QToolButton:hover, QPushButton:hover { background: %6; }"
    ).arg(bgStr, fgStr, borderStr, fm::style::ThemeManager::instance().colors()[fm::style::Token::Accent].name(),
          fm::style::ThemeManager::instance().colors()[fm::style::Token::AccentSoft].name(), fm::style::ThemeManager::instance().colors()[fm::style::Token::Alt].name());
    popup->setStyleSheet(styleHelper);

    auto* mainLayout = new QVBoxLayout(popup);
    mainLayout->setContentsMargins(10, 10, 10, 10);
    mainLayout->setSpacing(10);

    auto* shapeLayout = new QHBoxLayout();
    auto* shapeGroup = new QButtonGroup(popup);

    auto addShapeBtn = [&](const QString& text, Capture::MarkupShape shape) {
        auto* btn = new QToolButton(popup);
        btn->setText(text);
        btn->setCheckable(true);
        if (shape == state->shape) btn->setChecked(true);
        shapeGroup->addButton(btn, static_cast<int>(shape));
        shapeLayout->addWidget(btn);
        return btn;
    };

    auto* emojiBtn = addShapeBtn("😀", Capture::MarkupShape::Emoji);
    addShapeBtn("▭", Capture::MarkupShape::Rectangle);
    addShapeBtn("◯", Capture::MarkupShape::Ellipse);
    addShapeBtn("／", Capture::MarkupShape::Line);
    addShapeBtn("↗", Capture::MarkupShape::Arrow);
    mainLayout->addLayout(shapeLayout);

    auto* propLayout = new QHBoxLayout();

    auto* fillBtn = new QToolButton(popup);
    fillBtn->setText("채우기");
    if (state->hasFill) fillBtn->setStyleSheet(QString("color: %1; border-bottom: 4px solid %1; border-bottom-left-radius: 2px; border-bottom-right-radius: 2px;").arg(state->fill.name()));

    auto* outlineBtn = new QToolButton(popup);
    outlineBtn->setText("윤곽선");
    outlineBtn->setStyleSheet(QString("color: %1; border-bottom: %2px solid %1; border-bottom-left-radius: 2px; border-bottom-right-radius: 2px;").arg(state->outline.name()).arg(qMin(state->width, 12)));

    propLayout->addWidget(fillBtn);
    propLayout->addWidget(outlineBtn);
    mainLayout->addLayout(propLayout);

    auto triggerCallback = [state, callback]() {
        callback(state->shape, state->outline, state->fill, state->hasFill, state->width, state->emoji);
    };

    QObject::connect(fillBtn, &QToolButton::clicked, popup, [=]() {
        QMenu menu(popup);
        QAction* noFill = menu.addAction("채우기 없음");
        QAction* chooseColorAction = menu.addAction("색상 선택...");
        QAction* res = menu.exec(fillBtn->mapToGlobal(QPoint(0, fillBtn->height())));
        if (res == noFill) {
            state->hasFill = false;
        } else if (res == chooseColorAction) {
            auto color = QColorDialog::getColor(state->hasFill ? state->fill : QColor(255, 255, 255), popup, "채우기 색상", QColorDialog::ShowAlphaChannel);
            if (color.isValid()) {
                state->fill = color;
                state->hasFill = true;
            } else {
                return;
            }
        } else {
            return;
        }
        if (state->hasFill) fillBtn->setStyleSheet(QString("color: %1; border-bottom: 4px solid %1; border-bottom-left-radius: 2px; border-bottom-right-radius: 2px;").arg(state->fill.name()));
        else fillBtn->setStyleSheet("");
        triggerCallback();
    });

    QObject::connect(outlineBtn, &QToolButton::clicked, popup, [=]() {
        showPenPopup(outlineBtn, state->outline, state->width, [=](QColor c, int w) {
            state->outline = c;
            state->width = w;
            outlineBtn->setStyleSheet(QString("color: %1; border-bottom: %2px solid %1; border-bottom-left-radius: 2px; border-bottom-right-radius: 2px;").arg(state->outline.name()).arg(qMin(state->width, 12)));
            triggerCallback();
        });
    });

    QObject::connect(shapeGroup, &QButtonGroup::idClicked, popup, [=](int id) {
        state->shape = static_cast<Capture::MarkupShape>(id);
        if (state->shape == Capture::MarkupShape::Emoji) {
            auto* emojiPopup = new QWidget(popup->window(), Qt::Popup | Qt::FramelessWindowHint);
            emojiPopup->setAttribute(Qt::WA_DeleteOnClose);
            emojiPopup->setStyleSheet(styleHelper);
            auto* emojiLayout = new QGridLayout(emojiPopup);
            emojiLayout->setContentsMargins(8, 8, 8, 8);
            emojiLayout->setSpacing(4);
            QStringList emojis = {
                "😀", "😃", "😄", "😁", "😆", "😅", "😂", "🤣", "😊", "😇", "🙂", "🙃", "😉", "😌", "😍", "🥰",
                "😘", "😗", "😙", "😚", "😋", "😛", "😝", "😜", "🤪", "🤨", "🧐", "🤓", "😎", "🤩", "🥳", "😏",
                "😒", "😞", "😔", "😟", "😕", "🙁", "☹️", "😣", "😖", "😫", "😩", "🥺", "😢", "😭", "😤", "😠",
                "😡", "🤬", "🤯", "😳", "🥵", "🥶", "😱", "😨", "😰", "😥", "😓", "🤔", "🤭", "🤫", "🤥", "😶",
                "👍", "👎", "👏", "🙌", "❤️", "💔", "🔥", "✨", "⭐", "🎉"
            };
            int r = 0, c = 0;
            for (const QString& e : emojis) {
                auto* btn = new QPushButton(emojiPopup);
                btn->setText(e);
                btn->setFixedSize(48, 48);
                btn->setFlat(true);
                QFont f = btn->font();
#ifdef Q_OS_WIN
                f.setFamily("Segoe UI Emoji");
#endif
                f.setPixelSize(22);
                btn->setFont(f);
                QObject::connect(btn, &QPushButton::clicked, emojiPopup, [=]() {
                    state->emoji = e;
                    triggerCallback();
                    emojiPopup->close();
                    popup->close();
                });
                emojiLayout->addWidget(btn, r, c);
                c++;
                if (c >= 8) { c = 0; r++; }
            }
            emojiPopup->adjustSize();
            QPoint ePos = QCursor::pos();
            ePos.setX(ePos.x() - emojiPopup->width() / 2);
            ePos.setY(ePos.y() - emojiPopup->height() / 2);

            QScreen* screen = QGuiApplication::screenAt(ePos);
            if (!screen) screen = QGuiApplication::primaryScreen();
            if (screen) {
                QRect screenGeometry = screen->geometry();
                if (ePos.x() < screenGeometry.left()) ePos.setX(screenGeometry.left() + 5);
                if (ePos.x() + emojiPopup->width() > screenGeometry.right()) ePos.setX(screenGeometry.right() - emojiPopup->width() - 5);
                if (ePos.y() + emojiPopup->height() > screenGeometry.bottom()) ePos.setY(screenGeometry.bottom() - emojiPopup->height() - 5);
                if (ePos.y() < screenGeometry.top()) ePos.setY(screenGeometry.top() + 5);
            }

            emojiPopup->move(ePos);
            emojiPopup->show();
        } else {
            triggerCallback();
        }
    });

    popup->adjustSize();
    QPoint pos = button->mapToGlobal(QPoint(button->width() / 2, button->height() + 5));
    pos.setX(pos.x() - popup->width() / 2); // Center below button

    // Attempt to handle screen bounds
    QScreen* screen = QGuiApplication::screenAt(pos);
    if (!screen) screen = QGuiApplication::primaryScreen();
    if (screen) {
        QRect screenGeometry = screen->geometry();
        if (pos.x() < screenGeometry.left()) pos.setX(screenGeometry.left() + 5);
        if (pos.x() + popup->width() > screenGeometry.right()) pos.setX(screenGeometry.right() - popup->width() - 5);
        if (pos.y() + popup->height() > screenGeometry.bottom()) pos.setY(button->mapToGlobal(QPoint(0,0)).y() - popup->height() - 5);
    }

    popup->move(pos);
    popup->show();
}

QToolButton* makeToolButton(QWidget* parent, const QString& text, const QString& toolTip, bool checkable = false)
{
    auto* button = new QToolButton(parent);
    button->setText(text);
    button->setToolTip(toolTip);
    button->setCheckable(checkable);
    button->setAutoRaise(false);
    button->setMinimumHeight(34);
    return button;
}

void drawResizeHandles(QPainter& painter, const QRect& target)
{
    if (target.isEmpty())
        return;

    constexpr int kHandleSize = 9;
    const QList<QPoint> centers = {
        target.topLeft(), QPoint(target.center().x(), target.top()), target.topRight(),
        QPoint(target.left(), target.center().y()), QPoint(target.right(), target.center().y()),
        target.bottomLeft(), QPoint(target.center().x(), target.bottom()), target.bottomRight()
    };

    painter.save();
    painter.setPen(QPen(QColor(45, 125, 255), 1));
    painter.setBrush(QColor(255, 255, 255));
    for (const QPoint& center : centers) {
        painter.drawRect(QRect(center.x() - kHandleSize / 2,
                               center.y() - kHandleSize / 2,
                               kHandleSize,
                               kHandleSize));
    }
    painter.restore();
}

int hitTestResizeHandles(const QPoint& pos, const QRect& target)
{
    if (target.isEmpty())
        return -1;
    constexpr int kHitTestMargin = 12;
    const QList<QPoint> centers = {
        target.topLeft(), QPoint(target.center().x(), target.top()), target.topRight(),
        QPoint(target.left(), target.center().y()), QPoint(target.right(), target.center().y()),
        target.bottomLeft(), QPoint(target.center().x(), target.bottom()), target.bottomRight()
    };
    for (int i = 0; i < centers.size(); ++i) {
        if (qAbs(pos.x() - centers[i].x()) <= kHitTestMargin && qAbs(pos.y() - centers[i].y()) <= kHitTestMargin)
            return i;
    }
    return -1;
}

Qt::CursorShape cursorForHandle(int index)
{
    switch (index) {
    case 0:
    case 7: return Qt::SizeFDiagCursor;
    case 1:
    case 6: return Qt::SizeVerCursor;
    case 2:
    case 5: return Qt::SizeBDiagCursor;
    case 3:
    case 4: return Qt::SizeHorCursor;
    default: return Qt::ArrowCursor;
    }
}

} // namespace

CaptureMarkupDialog::CaptureMarkupDialog(const QImage& image, const QRect& sourceLogicalRect, QWidget* parent)
    : QDialog(parent)
    , m_originalImage(image.convertToFormat(QImage::Format_ARGB32_Premultiplied))
    , m_image(m_originalImage)
    , m_previewImage(m_originalImage)
    , m_sourceLogicalRect(sourceLogicalRect)
{
    setWindowTitle(tr("캡쳐 마크업"));
    setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Dialog);
    setModal(true);
    setMouseTracking(true);
    setAttribute(Qt::WA_DeleteOnClose, false);

    const qreal imageDpr = qMax<qreal>(1.0, image.devicePixelRatio());
    m_originalImage.setDevicePixelRatio(imageDpr);
    m_image.setDevicePixelRatio(imageDpr);
    m_previewImage.setDevicePixelRatio(imageDpr);

    QRect desktopGeometry;
    for (QScreen* screen : QApplication::screens()) {
        if (screen)
            desktopGeometry = desktopGeometry.united(screen->geometry());
    }
    if (desktopGeometry.isEmpty() && QApplication::primaryScreen())
        desktopGeometry = QApplication::primaryScreen()->geometry();

    QScreen* targetScreen = m_sourceLogicalRect.isValid()
        ? QApplication::screenAt(m_sourceLogicalRect.center())
        : QApplication::screenAt(QCursor::pos());
    if (!targetScreen)
        targetScreen = QApplication::primaryScreen();
    m_virtualGeometry = targetScreen ? targetScreen->geometry() : desktopGeometry;
    if (m_virtualGeometry.isEmpty())
        m_virtualGeometry = desktopGeometry;

    m_cropLogicalRect = QRect(0, 0, m_sourceLogicalRect.width(), m_sourceLogicalRect.height());

    for (QScreen* screen : QApplication::screens()) {
        if (screen && screen->geometry().intersects(m_virtualGeometry))
            m_screenRegion += QRegion(screen->geometry().translated(-m_virtualGeometry.topLeft()));
    }
    setGeometry(m_virtualGeometry);

    rebuildToolBar();
    placeToolBar();

    m_backgroundImage = WindowsGraphicsCapture::captureVirtualDesktop();
    setMouseTracking(true);
}

QImage CaptureMarkupDialog::markedImage() const
{
    return m_image;
}

void CaptureMarkupDialog::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    if (!m_screenRegion.isEmpty())
        painter.setClipRegion(m_screenRegion);

    if (!m_backgroundImage.isNull()) {
        QRect desktopGeometry;
        for (QScreen* screen : QApplication::screens()) {
            if (screen)
                desktopGeometry = desktopGeometry.united(screen->geometry());
        }
        for (QScreen* screen : QApplication::screens()) {
            if (!screen)
                continue;
            const QRect screenLocalRect = screen->geometry().translated(-m_virtualGeometry.topLeft());
            const QRect paintRect = rect().intersected(screenLocalRect);
            if (paintRect.isEmpty())
                continue;
            const QRect sourceRect(paintRect.topLeft() + m_virtualGeometry.topLeft() - desktopGeometry.topLeft(),
                                   paintRect.size());
            painter.drawImage(paintRect, m_backgroundImage, sourceRect.intersected(m_backgroundImage.rect()));
        }
    } else {
        painter.fillRect(rect(), QColor(20, 20, 20));
    }

    const QRect target = imageTargetRect();

    // Dim the background EXCEPT inside the target
    QRegion dimRegion = QRegion(rect()).subtracted(target);
    painter.save();
    painter.setClipRegion(dimRegion);
    painter.fillRect(rect(), QColor(0, 0, 0, 92));
    painter.restore();

    // Draw the image inside the target
    painter.save();
    painter.setClipRect(target);
    QRect oldImageTarget(target.left() - m_cropLogicalRect.left(),
                         target.top() - m_cropLogicalRect.top(),
                         m_sourceLogicalRect.width(),
                         m_sourceLogicalRect.height());
    painter.drawImage(oldImageTarget, m_drawing ? m_previewImage : m_image);
    painter.restore();

    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(QColor(45, 125, 255), 2));
    painter.drawRect(target.adjusted(0, 0, -1, -1));
    drawResizeHandles(painter, target);
}

void CaptureMarkupDialog::mousePressEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton) {
        QDialog::mousePressEvent(event);
        return;
    }

    const QRect target = imageTargetRect();
    int handle = hitTestResizeHandles(event->pos(), target);
    if (handle >= 0) {
        m_resizing = true;
        m_resizingHandleIndex = handle;
        return;
    }

    if (!target.contains(event->pos())) {
        QDialog::mousePressEvent(event);
        return;
    }

    m_drawing = true;
    m_startPoint = imagePointFromWidget(event->pos());
    m_lastPoint = m_startPoint;
    m_previewImage = m_image;
    pushUndoState();

    if (m_tool == MarkupTool::Pen || m_tool == MarkupTool::Highlighter || m_tool == MarkupTool::Eraser)
        drawLineTo(m_startPoint);

    update();
}

void CaptureMarkupDialog::mouseMoveEvent(QMouseEvent* event)
{
    const QRect target = imageTargetRect();
    if (!m_drawing && !m_resizing) {
        int handle = hitTestResizeHandles(event->pos(), target);
        if (handle >= 0)
            setCursor(cursorForHandle(handle));
        else if (target.contains(event->pos()))
            setCursor(Qt::CrossCursor);
        else
            setCursor(Qt::ArrowCursor);
        QDialog::mouseMoveEvent(event);
        return;
    }

    if (m_resizing) {
        const QPoint pos = event->pos();
        const QRect baseTarget = m_sourceLogicalRect.translated(-m_virtualGeometry.topLeft());
        int newLeft = m_cropLogicalRect.left();
        int newTop = m_cropLogicalRect.top();
        int newRight = m_cropLogicalRect.right();
        int newBottom = m_cropLogicalRect.bottom();

        int absoluteLeft = baseTarget.left() + newLeft;
        int absoluteRight = baseTarget.left() + newRight;
        int absoluteTop = baseTarget.top() + newTop;
        int absoluteBottom = baseTarget.top() + newBottom;

        const int logicalX = pos.x();
        const int logicalY = pos.y();

        switch (m_resizingHandleIndex) {
        case 0: absoluteLeft = logicalX; absoluteTop = logicalY; break;   // Top-Left
        case 1: absoluteTop = logicalY; break;                       // Top
        case 2: absoluteRight = logicalX; absoluteTop = logicalY; break;  // Top-Right
        case 3: absoluteLeft = logicalX; break;                      // Left
        case 4: absoluteRight = logicalX; break;                     // Right
        case 5: absoluteLeft = logicalX; absoluteBottom = logicalY; break;// Bottom-Left
        case 6: absoluteBottom = logicalY; break;                    // Bottom
        case 7: absoluteRight = logicalX; absoluteBottom = logicalY; break;// Bottom-Right
        }

        absoluteLeft = qBound(0, absoluteLeft, absoluteRight - 10);
        absoluteRight = qBound(absoluteLeft + 10, absoluteRight, m_virtualGeometry.width() - 1);
        absoluteTop = qBound(0, absoluteTop, absoluteBottom - 10);
        absoluteBottom = qBound(absoluteTop + 10, absoluteBottom, m_virtualGeometry.height() - 1);

        m_cropLogicalRect = QRect(QPoint(absoluteLeft - baseTarget.left(), absoluteTop - baseTarget.top()),
                                  QPoint(absoluteRight - baseTarget.left(), absoluteBottom - baseTarget.top()));
        update();
        placeToolBar();
        return;
    }

    const QPoint imagePoint = imagePointFromWidget(event->pos());
    if (m_tool == MarkupTool::Pen || m_tool == MarkupTool::Highlighter || m_tool == MarkupTool::Eraser) {
        drawLineTo(imagePoint);
    } else if (m_tool == MarkupTool::Shape) {
        m_previewImage = m_image;
        QPainter painter(&m_previewImage);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(m_shapeOutlineColor, m_width, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.setBrush(m_shapeHasFill ? QBrush(m_shapeFillColor) : Qt::NoBrush);
        const QRect shapeRect(m_startPoint, imagePoint);

        if (m_shape == MarkupShape::Rectangle) {
            painter.drawRect(shapeRect.normalized());
        } else if (m_shape == MarkupShape::Ellipse) {
            painter.drawEllipse(shapeRect.normalized());
        } else if (m_shape == MarkupShape::Line) {
            painter.drawLine(m_startPoint, imagePoint);
        } else if (m_shape == MarkupShape::Arrow) {
            painter.drawLine(m_startPoint, imagePoint);
            // arrow head
            QPointF p1 = m_startPoint;
            QPointF p2 = imagePoint;
            QLineF line(p1, p2);
            if (line.length() > 5.0) {
                double angle = std::atan2(-line.dy(), line.dx());
                double arrowSize = m_width * 3.0 + 5.0;
                QPointF arrowP1 = p2 - QPointF(sin(angle + M_PI / 3) * arrowSize, cos(angle + M_PI / 3) * arrowSize);
                QPointF arrowP2 = p2 - QPointF(sin(angle + M_PI - M_PI / 3) * arrowSize, cos(angle + M_PI - M_PI / 3) * arrowSize);

                QPolygonF arrowHead;
                arrowHead << p2 << arrowP1 << arrowP2;
                painter.setBrush(m_shapeOutlineColor);
                painter.drawPolygon(arrowHead);
            }
        } else if (m_shape == MarkupShape::Emoji) {
            QFont f = painter.font();
#ifdef Q_OS_WIN
            f.setFamily("Segoe UI Emoji");
#endif
            f.setPixelSize(qMax(10, qMin(qAbs(shapeRect.width()), qAbs(shapeRect.height()))));
            painter.setFont(f);
            painter.drawText(shapeRect.normalized(), Qt::AlignCenter, m_emojiString);
        }
    }

    update();
}

void CaptureMarkupDialog::mouseReleaseEvent(QMouseEvent* event)
{
    if (m_resizing && event->button() == Qt::LeftButton) {
        m_resizing = false;
        m_resizingHandleIndex = -1;
        commitCrop();
        update();
        return;
    }

    if (!m_drawing || event->button() != Qt::LeftButton) {
        QDialog::mouseReleaseEvent(event);
        return;
    }

    const QPoint imagePoint = imagePointFromWidget(event->pos());
    if (m_tool == MarkupTool::Shape)
        commitShape(imagePoint);

    m_previewImage = m_image;
    m_drawing = false;
}

void CaptureMarkupDialog::keyPressEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Escape) {
        reject();
        return;
    }
    if (event->matches(QKeySequence::Undo)) {
        undo();
        return;
    }
    if (event->matches(QKeySequence::Redo)) {
        redo();
        return;
    }
    QDialog::keyPressEvent(event);
}

void CaptureMarkupDialog::resizeEvent(QResizeEvent* event)
{
    QDialog::resizeEvent(event);
    placeToolBar();
}

QRect CaptureMarkupDialog::imageTargetRect() const
{
    return m_cropLogicalRect.translated(m_sourceLogicalRect.topLeft() - m_virtualGeometry.topLeft());
}

QPoint CaptureMarkupDialog::imagePointFromWidget(const QPoint& pos) const
{
    const QRect target = imageTargetRect();
    return QPoint(pos.x() - target.left() + m_cropLogicalRect.left(),
                  pos.y() - target.top() + m_cropLogicalRect.top());
}

void CaptureMarkupDialog::rebuildToolBar()
{
    m_toolBar = new QWidget(this);
    m_toolBar->setObjectName(QStringLiteral("captureMarkupToolBar"));

    m_penButton = makeToolButton(m_toolBar, tr("볼펜"), tr("볼펜"), true);
    m_penButton->setObjectName("penBtn");
    m_highlighterButton = makeToolButton(m_toolBar, tr("형광펜"), tr("형광펜"), true);
    m_highlighterButton->setObjectName("hlBtn");
    m_eraserButton = makeToolButton(m_toolBar, tr("지우개"), tr("지우개"), true);
    m_shapeButton = makeToolButton(m_toolBar, tr("도형"), tr("도형"), true);

    const auto& themeManager = ThemeManager::instance();
    QColor bgColor = themeManager.toolBarColor(); // Use toolbar color
    bgColor.setAlpha(235);
    QColor fgColor = themeManager.foregroundColor();
    // The legacy palette has no accent key (it answered magenta); checked tools use the product accent.
    QColor highlight = fm::style::ThemeManager::instance().colors()[fm::style::Token::Accent];
    if (!highlight.isValid()) highlight = QColor(68, 137, 255);
    QColor border = fgColor; border.setAlpha(70);
    QColor hover = fgColor; hover.setAlpha(35);
    QColor checkedBg = highlight; checkedBg.setAlpha(170);
    QColor checkedBorder = fgColor; checkedBorder.setAlpha(120);

    m_toolBar->setStyleSheet(QString(
        "#captureMarkupToolBar { background: rgba(%1, %2, %3, %4); border: 1px solid rgba(%5, %6, %7, %8); border-radius: 10px; }"
        "QToolButton { color: rgb(%9, %10, %11); padding: 6px 10px; border: 1px solid transparent; border-radius: 6px; }"
        "QToolButton:checked { background: rgba(%12, %13, %14, %15); border-color: rgba(%16, %17, %18, %19); }"
        "QToolButton:hover { background: rgba(%20, %21, %22, %23); }"
        "QLabel, QCheckBox, QSpinBox { color: rgb(%9, %10, %11); }"
    ).arg(bgColor.red()).arg(bgColor.green()).arg(bgColor.blue()).arg(bgColor.alpha())
     .arg(border.red()).arg(border.green()).arg(border.blue()).arg(border.alpha())
     .arg(fgColor.red()).arg(fgColor.green()).arg(fgColor.blue())
     .arg(checkedBg.red()).arg(checkedBg.green()).arg(checkedBg.blue()).arg(checkedBg.alpha())
     .arg(checkedBorder.red()).arg(checkedBorder.green()).arg(checkedBorder.blue()).arg(checkedBorder.alpha())
     .arg(hover.red()).arg(hover.green()).arg(hover.blue()).arg(hover.alpha()));

    auto* layout = new QHBoxLayout(m_toolBar);
    layout->setContentsMargins(10, 8, 10, 8);
    layout->setSpacing(6);

    auto* group = new QButtonGroup(m_toolBar);
    group->setExclusive(true);

    group->addButton(m_penButton);
    group->addButton(m_highlighterButton);
    group->addButton(m_eraserButton);
    group->addButton(m_shapeButton);

    layout->addWidget(m_penButton);
    layout->addWidget(m_highlighterButton);
    layout->addWidget(m_eraserButton);
    layout->addWidget(m_shapeButton);

    // 굵기, 채우기, 색상 선택은 도형/펜 팝업 내부 기능으로 대체하여 메인 툴바에서 제거


    m_undoButton = makeToolButton(m_toolBar, tr("Undo"), tr("실행 취소"));
    m_redoButton = makeToolButton(m_toolBar, tr("Redo"), tr("다시 실행"));
    layout->addWidget(m_undoButton);
    layout->addWidget(m_redoButton);

    auto* doneButton = makeToolButton(m_toolBar, tr("완료"), tr("마크업 완료"));
    auto* cancelButton = makeToolButton(m_toolBar, tr("취소"), tr("캡쳐 취소"));
    layout->addWidget(doneButton);
    layout->addWidget(cancelButton);

    auto updateButtonStyles = [this, checkedBorder]() {
        QString penStyle = QString(
            "QToolButton { border-bottom: 4px solid %1; border-bottom-left-radius: 2px; border-bottom-right-radius: 2px; }"
            "QToolButton:checked { border-bottom: 4px solid %1; border-color: rgba(%2, %3, %4, %5) rgba(%2, %3, %4, %5) %1 rgba(%2, %3, %4, %5); }"
        ).arg(m_penColor.name())
         .arg(checkedBorder.red()).arg(checkedBorder.green()).arg(checkedBorder.blue()).arg(checkedBorder.alpha());

        QString hlStyle = QString(
            "QToolButton { border-bottom: 4px solid %1; border-bottom-left-radius: 2px; border-bottom-right-radius: 2px; }"
            "QToolButton:checked { border-bottom: 4px solid %1; border-color: rgba(%2, %3, %4, %5) rgba(%2, %3, %4, %5) %1 rgba(%2, %3, %4, %5); }"
        ).arg(m_highlighterColor.name())
         .arg(checkedBorder.red()).arg(checkedBorder.green()).arg(checkedBorder.blue()).arg(checkedBorder.alpha());

        m_penButton->setStyleSheet(penStyle);
        m_highlighterButton->setStyleSheet(hlStyle);
    };

    connect(m_penButton, &QToolButton::clicked, this, [this, updateButtonStyles] {
        if (m_tool != MarkupTool::Pen) {
            setTool(MarkupTool::Pen);
            if (m_widthSpin) m_widthSpin->setValue(m_penWidth);
        } else {
            showPenPopup(m_penButton, m_penColor, m_penWidth, [this, updateButtonStyles](QColor c, int w) {
                m_penColor = c;
                m_penWidth = w;
                if (m_widthSpin) {
                    m_widthSpin->blockSignals(true);
                    m_widthSpin->setValue(w);
                    m_widthSpin->blockSignals(false);
                }
                updateButtonStyles();
            });
        }
    });

    connect(m_highlighterButton, &QToolButton::clicked, this, [this, updateButtonStyles] {
        if (m_tool != MarkupTool::Highlighter) {
            setTool(MarkupTool::Highlighter);
            if (m_widthSpin) m_widthSpin->setValue(m_highlighterWidth);
        } else {
            showPenPopup(m_highlighterButton, m_highlighterColor, m_highlighterWidth, [this, updateButtonStyles](QColor c, int w) {
                m_highlighterColor = c;
                m_highlighterWidth = w;
                if (m_widthSpin) {
                    m_widthSpin->blockSignals(true);
                    m_widthSpin->setValue(w);
                    m_widthSpin->blockSignals(false);
                }
                updateButtonStyles();
            });
        }
    });

    connect(m_eraserButton, &QToolButton::clicked, this, [this] { setTool(MarkupTool::Eraser); });
    connect(m_shapeButton, &QToolButton::clicked, this, [this] {
        if (m_tool != MarkupTool::Shape) {
            setTool(MarkupTool::Shape);
        } else {
            showShapePopup(m_shapeButton, m_shape, m_shapeOutlineColor, m_shapeFillColor, m_shapeHasFill, m_width, m_emojiString, [this](MarkupShape s, QColor outline, QColor fill, bool has_fill, int w, QString emoji) {
                m_shape = s;
                m_shapeOutlineColor = outline;
                m_shapeFillColor = fill;
                m_shapeHasFill = has_fill;
                m_width = w;
                m_emojiString = emoji;
                if (m_widthSpin) {
                    m_widthSpin->blockSignals(true);
                    m_widthSpin->setValue(w);
                    m_widthSpin->blockSignals(false);
                }
            });
        }
    });

    // 굵기, 채우기 버튼 등의 이벤트 연결 제거됨

    connect(m_undoButton, &QToolButton::clicked, this, &CaptureMarkupDialog::undo);
    connect(m_redoButton, &QToolButton::clicked, this, &CaptureMarkupDialog::redo);
    connect(doneButton, &QToolButton::clicked, this, &QDialog::accept);
    connect(cancelButton, &QToolButton::clicked, this, &QDialog::reject);

    updateButtonStyles();
    updateToolChecks();
    updateColorButtonStatus();
    placeToolBar();
}

void CaptureMarkupDialog::placeToolBar()
{
    if (!m_toolBar)
        return;

    const QSize hint = m_toolBar->sizeHint();
    const QRect target = imageTargetRect();
    QRect safeArea = rect().adjusted(12, 12, -12, -12);
    if (safeArea.isEmpty())
        safeArea = rect();

    int x = target.isEmpty()
        ? (width() - hint.width()) / 2
        : target.center().x() - hint.width() / 2;
    x = qBound(safeArea.left(), x, qMax(safeArea.left(), safeArea.right() - hint.width() + 1));

    int y = target.isEmpty() ? safeArea.bottom() - hint.height() + 1 : target.bottom() + 12;
    if (y + hint.height() > safeArea.bottom() + 1)
        y = target.top() - hint.height() - 12;
    if (y < safeArea.top())
        y = safeArea.bottom() - hint.height() + 1;

    m_toolBar->setGeometry(x, y, hint.width(), hint.height());
    m_toolBar->show();
    m_toolBar->raise();
}

void CaptureMarkupDialog::setTool(MarkupTool tool)
{
    m_tool = tool;
    updateToolChecks();
}

void CaptureMarkupDialog::chooseColor()
{
    const QColor color = QColorDialog::getColor(m_color, this, tr("마크업 색상"), QColorDialog::ShowAlphaChannel);
    if (color.isValid())
        m_color = color;
}

void CaptureMarkupDialog::pushUndoState()
{
    m_undoStack.push_back({m_image, m_sourceLogicalRect});
    if (m_undoStack.size() > 50)
        m_undoStack.removeFirst();
    m_redoStack.clear();
}

void CaptureMarkupDialog::undo()
{
    if (m_undoStack.isEmpty())
        return;
    m_redoStack.push_back({m_image, m_sourceLogicalRect});
    MarkupState state = m_undoStack.takeLast();
    m_image = state.image;
    m_sourceLogicalRect = state.sourceLogicalRect;
    m_cropLogicalRect = QRect(0, 0, m_sourceLogicalRect.width(), m_sourceLogicalRect.height());
    m_previewImage = m_image;
    update();
    placeToolBar();
}

void CaptureMarkupDialog::redo()
{
    if (m_redoStack.isEmpty())
        return;
    m_undoStack.push_back({m_image, m_sourceLogicalRect});
    MarkupState state = m_redoStack.takeLast();
    m_image = state.image;
    m_sourceLogicalRect = state.sourceLogicalRect;
    m_cropLogicalRect = QRect(0, 0, m_sourceLogicalRect.width(), m_sourceLogicalRect.height());
    m_previewImage = m_image;
    update();
    placeToolBar();
}

void CaptureMarkupDialog::commitCrop()
{
    if (m_cropLogicalRect == QRect(0, 0, m_sourceLogicalRect.width(), m_sourceLogicalRect.height()))
        return;

    QRect absoluteNewRect = m_cropLogicalRect.translated(m_sourceLogicalRect.topLeft()).intersected(m_virtualGeometry);
    if (absoluteNewRect.isEmpty()) {
        m_cropLogicalRect = QRect(0, 0, m_sourceLogicalRect.width(), m_sourceLogicalRect.height());
        return;
    }

    const qreal dpr = m_image.devicePixelRatio();
    const QSize newPixelSize(qMax(1, qRound(absoluteNewRect.width() * dpr)),
                             qMax(1, qRound(absoluteNewRect.height() * dpr)));

    QImage newImage(newPixelSize, QImage::Format_ARGB32_Premultiplied);
    newImage.setDevicePixelRatio(dpr);
    newImage.fill(Qt::transparent);

    QPainter painter(&newImage);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);

    if (!m_backgroundImage.isNull()) {
        QRect bgSourceRect = absoluteNewRect.translated(-m_virtualGeometry.topLeft());
        const qreal bgDpr = m_backgroundImage.devicePixelRatio();
        const QRect bgSourcePixelRect(qRound(bgSourceRect.left() * bgDpr),
                                      qRound(bgSourceRect.top() * bgDpr),
                                      qRound(bgSourceRect.width() * bgDpr),
                                      qRound(bgSourceRect.height() * bgDpr));
        painter.drawImage(newImage.rect(), m_backgroundImage, bgSourcePixelRect);
    }

    QPoint logicalOffset = m_sourceLogicalRect.topLeft() - absoluteNewRect.topLeft();
    QPoint pixelOffset(qRound(logicalOffset.x() * dpr), qRound(logicalOffset.y() * dpr));

    painter.drawImage(pixelOffset, m_image);
    painter.end();

    m_sourceLogicalRect = absoluteNewRect;
    m_cropLogicalRect = QRect(0, 0, m_sourceLogicalRect.width(), m_sourceLogicalRect.height());
    m_image = newImage;
    m_originalImage = newImage;
    m_previewImage = newImage;

    pushUndoState();
    placeToolBar();
}

void CaptureMarkupDialog::drawLineTo(const QPoint& imagePoint)
{
    QPainter painter(&m_image);
    painter.setRenderHint(QPainter::Antialiasing);

    if (m_tool == MarkupTool::Eraser) {
        QPainterPath path;
        path.moveTo(m_lastPoint);
        path.lineTo(imagePoint);
        QPainterPathStroker stroker;
        stroker.setWidth(m_width);
        stroker.setCapStyle(Qt::RoundCap);
        stroker.setJoinStyle(Qt::RoundJoin);
        painter.setClipPath(stroker.createStroke(path));
        painter.drawImage(QPoint(0, 0), m_originalImage);
    } else {
        painter.setPen(currentPen());
        painter.drawLine(m_lastPoint, imagePoint);
    }

    m_lastPoint = imagePoint;
    m_previewImage = m_image;
}

void CaptureMarkupDialog::commitShape(const QPoint& imagePoint)
{
    QPainter painter(&m_image);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(m_shapeOutlineColor, m_width, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.setBrush(m_shapeHasFill ? QBrush(m_shapeFillColor) : Qt::NoBrush);
    const QRect shapeRect(m_startPoint, imagePoint);

    if (m_shape == MarkupShape::Rectangle) {
        painter.drawRect(shapeRect.normalized());
    } else if (m_shape == MarkupShape::Ellipse) {
        painter.drawEllipse(shapeRect.normalized());
    } else if (m_shape == MarkupShape::Line) {
        painter.drawLine(m_startPoint, imagePoint);
    } else if (m_shape == MarkupShape::Arrow) {
        painter.drawLine(m_startPoint, imagePoint);
        // arrow head
        QPointF p1 = m_startPoint;
        QPointF p2 = imagePoint;
        QLineF line(p1, p2);
        if (line.length() > 5.0) {
            double angle = std::atan2(-line.dy(), line.dx());
            double arrowSize = m_width * 3.0 + 5.0;
            QPointF arrowP1 = p2 - QPointF(sin(angle + M_PI / 3) * arrowSize, cos(angle + M_PI / 3) * arrowSize);
            QPointF arrowP2 = p2 - QPointF(sin(angle + M_PI - M_PI / 3) * arrowSize, cos(angle + M_PI - M_PI / 3) * arrowSize);

            QPolygonF arrowHead;
            arrowHead << p2 << arrowP1 << arrowP2;
            painter.setBrush(m_shapeOutlineColor);
            painter.drawPolygon(arrowHead);
        }
    } else if (m_shape == MarkupShape::Emoji) {
        QFont f = painter.font();
#ifdef Q_OS_WIN
        f.setFamily("Segoe UI Emoji");
#endif
        f.setPixelSize(qMax(10, qMin(qAbs(shapeRect.width()), qAbs(shapeRect.height()))));
        painter.setFont(f);
        painter.drawText(shapeRect.normalized(), Qt::AlignCenter, m_emojiString);
    }
}

QPen CaptureMarkupDialog::currentPen() const
{
    QColor color = effectiveColor();
    int width = m_width;
    if (m_tool == MarkupTool::Pen) width = m_penWidth;
    else if (m_tool == MarkupTool::Highlighter) width = m_highlighterWidth;

    QPen pen(color, width, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    return pen;
}

QColor CaptureMarkupDialog::effectiveColor() const
{
    if (m_tool == MarkupTool::Pen) {
        return m_penColor;
    }
    if (m_tool == MarkupTool::Highlighter) {
        QColor color = m_highlighterColor;
        color.setAlpha(95);
        return color;
    }
    if (m_tool == MarkupTool::Eraser)
        return Qt::transparent;
    return m_color;
}

void CaptureMarkupDialog::updateToolChecks()
{
    if (m_penButton) m_penButton->setChecked(m_tool == MarkupTool::Pen);
    if (m_highlighterButton) m_highlighterButton->setChecked(m_tool == MarkupTool::Highlighter);
    if (m_eraserButton) m_eraserButton->setChecked(m_tool == MarkupTool::Eraser);
    if (m_shapeButton) m_shapeButton->setChecked(m_tool == MarkupTool::Shape);
}

void CaptureMarkupDialog::updateColorButtonStatus()
{
    if (m_colorButton) {
        m_colorButton->setStyleSheet(QString(
            "QToolButton { background-color: %1; border: 1px solid %3; border-radius: 4px; }"
            "QToolButton:hover { background-color: %2; }"
        ).arg(m_color.name()).arg(m_color.lighter(110).name()).arg(fm::style::ThemeManager::instance().colors()[fm::style::Token::Line].name()));
    }
}

} // namespace Capture
