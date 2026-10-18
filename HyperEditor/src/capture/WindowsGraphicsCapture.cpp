#include "capture/WindowsGraphicsCapture.h"

#include <QGuiApplication>
#include <QPainter>
#include <QPixmap>
#include <QScreen>

#include <utility>

#ifdef Q_OS_WIN
#include <QtCore/qt_windows.h>
#include <inspectable.h>
#include <roapi.h>
#include <winstring.h>
#endif

namespace Capture {

bool WindowsGraphicsCapture::isSupported()
{
#ifdef Q_OS_WIN
    HSTRING className = nullptr;
    static constexpr wchar_t kGraphicsCaptureSessionClass[] = L"Windows.Graphics.Capture.GraphicsCaptureSession";
    const HRESULT createStringResult = ::WindowsCreateString(
        kGraphicsCaptureSessionClass,
        static_cast<UINT32>((sizeof(kGraphicsCaptureSessionClass) / sizeof(wchar_t)) - 1),
        &className);
    if (FAILED(createStringResult))
        return false;

    IInspectable* factory = nullptr;
    const HRESULT factoryResult = ::RoGetActivationFactory(
        className,
        IID_IInspectable,
        reinterpret_cast<void**>(&factory));
    ::WindowsDeleteString(className);

    if (factory)
        factory->Release();

    if (SUCCEEDED(factoryResult))
        return true;

    const HRESULT initResult = ::RoInitialize(RO_INIT_MULTITHREADED);
    if (FAILED(initResult) && initResult != RPC_E_CHANGED_MODE)
        return false;

    className = nullptr;
    if (FAILED(::WindowsCreateString(
            kGraphicsCaptureSessionClass,
            static_cast<UINT32>((sizeof(kGraphicsCaptureSessionClass) / sizeof(wchar_t)) - 1),
            &className))) {
        if (SUCCEEDED(initResult))
            ::RoUninitialize();
        return false;
    }

    factory = nullptr;
    const HRESULT initializedFactoryResult = ::RoGetActivationFactory(
        className,
        IID_IInspectable,
        reinterpret_cast<void**>(&factory));
    ::WindowsDeleteString(className);
    if (factory)
        factory->Release();
    if (SUCCEEDED(initResult))
        ::RoUninitialize();

    return SUCCEEDED(initializedFactoryResult);
#else
    return false;
#endif
}

QImage WindowsGraphicsCapture::captureVirtualDesktop()
{
    const QList<QScreen*> screens = QGuiApplication::screens();
    if (screens.isEmpty())
        return {};

    QRect virtualGeometry;
    for (QScreen* screen : screens) {
        if (screen)
            virtualGeometry = virtualGeometry.united(screen->geometry());
    }

    if (virtualGeometry.isEmpty())
        return {};

    QImage result(virtualGeometry.size(), QImage::Format_ARGB32_Premultiplied);
    if (result.isNull())
        return {};

    result.fill(Qt::transparent);
    QPainter painter(&result);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);

    for (QScreen* screen : screens) {
        if (!screen)
            continue;

        const QRect geometry = screen->geometry();
        QPixmap pixmap = screen->grabWindow(0);
        if (pixmap.isNull())
            continue;

        QImage screenImage = pixmap.toImage();
        if (screenImage.isNull())
            continue;

        if (screenImage.size() != geometry.size()) {
            screenImage = screenImage.scaled(geometry.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        }

        const QRect targetRect(geometry.topLeft() - virtualGeometry.topLeft(), geometry.size());
        painter.drawImage(targetRect, screenImage);
    }

    return result;
}

QImage WindowsGraphicsCapture::captureLogicalRect(const QRect& logicalRect)
{
    if (logicalRect.isEmpty())
        return {};

    QList<QScreen*> intersectingScreens;
    for (QScreen* screen : QGuiApplication::screens()) {
        if (screen && screen->geometry().intersects(logicalRect))
            intersectingScreens.append(screen);
    }

    if (intersectingScreens.isEmpty())
        return {};

    qreal targetDpr = 1.0;
    if (QScreen* centerScreen = QGuiApplication::screenAt(logicalRect.center())) {
        targetDpr = qMax<qreal>(1.0, centerScreen->devicePixelRatio());
    } else {
        for (QScreen* screen : std::as_const(intersectingScreens))
            targetDpr = qMax(targetDpr, screen->devicePixelRatio());
    }

    const QSize targetSize(qMax(1, qRound(logicalRect.width() * targetDpr)),
                           qMax(1, qRound(logicalRect.height() * targetDpr)));
    QImage result(targetSize, QImage::Format_ARGB32_Premultiplied);
    if (result.isNull())
        return {};
    result.setDevicePixelRatio(targetDpr);
    result.fill(Qt::transparent);

    QPainter painter(&result);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);

    for (QScreen* screen : std::as_const(intersectingScreens)) {
        if (!screen)
            continue;

        const QRect screenGeometry = screen->geometry();
        const QRect intersection = logicalRect.intersected(screenGeometry);
        if (intersection.isEmpty())
            continue;

        const QPixmap pixmap = screen->grabWindow(0);
        if (pixmap.isNull())
            continue;

        const qreal sourceDpr = qMax<qreal>(1.0, pixmap.devicePixelRatio());
        const QImage screenImage = pixmap.toImage();
        if (screenImage.isNull())
            continue;

        const QRect sourceRect(qFloor((intersection.left() - screenGeometry.left()) * sourceDpr),
                               qFloor((intersection.top() - screenGeometry.top()) * sourceDpr),
                               qCeil(intersection.width() * sourceDpr),
                               qCeil(intersection.height() * sourceDpr));

        const QRect boundedSourceRect = sourceRect.intersected(screenImage.rect());
        if (boundedSourceRect.isEmpty())
            continue;

        const QRect targetRect(qRound((intersection.left() - logicalRect.left()) * targetDpr),
                               qRound((intersection.top() - logicalRect.top()) * targetDpr),
                               qRound(intersection.width() * targetDpr),
                               qRound(intersection.height() * targetDpr));

        painter.drawImage(targetRect, screenImage, boundedSourceRect);
    }

    return result;
}

} // namespace Capture







