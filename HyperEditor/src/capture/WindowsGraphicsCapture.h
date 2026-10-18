#pragma once

#include <QImage>
#include <QRect>

namespace Capture {

/// Windows.Graphics.Capture integration façade.
/// The still-image path currently captures a coherent desktop snapshot for the custom Qt overlay;
/// WGC support detection is exposed so the backend can be upgraded to frame-pool capture without
/// changing MainWindow/CaptureOverlay call sites.
class WindowsGraphicsCapture
{
public:
    static bool isSupported();
    static QImage captureVirtualDesktop();
    static QImage captureLogicalRect(const QRect& logicalRect);
};

} // namespace Capture

