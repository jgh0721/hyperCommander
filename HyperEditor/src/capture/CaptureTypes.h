#pragma once

#include <QImage>
#include <QRect>

namespace Capture {

enum class TargetMode {
    FullScreen,
    Window,
    Rectangle,
    Ellipse,
    Freeform
};

enum class MarkupTool {
    Pen,
    Highlighter,
    Eraser,
    Shape
};

enum class MarkupShape {
    Rectangle,
    Ellipse,
    Line,
    Arrow,
    Emoji
};

struct CaptureOptions {
    TargetMode targetMode = TargetMode::Rectangle;
    bool markupEnabled = false;
};

struct CaptureResult {
    bool accepted = false;
    bool canceled = true;
    CaptureOptions options;
    QRect logicalRect;
    QImage image;
};

} // namespace Capture

