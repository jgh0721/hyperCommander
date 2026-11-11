#pragma once

/// 모듈

/// UIs

/// 기타 헤더파일
#include "text/TextMinimapModel.h"

/// 고유 라이브러리

/// 공통 라이브러리

/// 외부 라이브러리

/// 선언 헤더파일들
class TextMinimapSource;

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QColor>
#include <QHash>
#include <QImage>
#include <QList>
#include <functional>

class QPainter;

// Draws the document thumbnail from tiles of TileLines document lines. A tile is keyed by its first document line,
// so a wrap change in one line only redraws that tile; the tiles below just move. Lines past SCI_GETENDSTYLED are
// drawn in the outline colour and the tile is redrawn once styling reaches it.
class TextMinimapRenderer
{
public:
    static constexpr int TileLines = 64;
    static constexpr qint64 CacheBytes = 16LL * 1024 * 1024;

    struct Options
    {
        int rowHeight_ = 2;
        int charWidth_ = 1;
        int width_ = 100;
        bool blocks_ = false;
        qreal dpr_ = 1.0;
        bool operator==(const Options&) const = default;
    };
    using StyleColor = std::function<QColor(int Style)>;

    void Clear();
    void InvalidateLines(int First, int Last);
    void InvalidateFrom(int Line);
    // Draws the rows visible under Geometry into a widget of Height logical pixels.
    void PaintDocument(QPainter& Painter, const TextMinimapSource& Source, const TextMinimapDocumentGeometry& Geometry,
                       int Height, const Options& Settings, const StyleColor& Colors, const QColor& Pending);
    static void PaintOutline(QPainter& Painter, const TextMinimapOutline& Outline, const TextMinimapFitGeometry& Geometry,
                             int Width, const QColor& Stroke, const QColor& Heading, const QVector<int>& HeadingRows);
    // Logical x of a byte offset inside a line's first 512 bytes (columns × char width); -1 beyond that prefix.
    static int ColumnOf(const TextMinimapSource& Source, int Line, qint64 Position, int TabWidth);
    int CachedTiles() const { return int(tiles_.size()); }

private:
    struct Tile
    {
        QImage image_;
        int rows_ = 0;
        bool pending_ = false;
    };
    Tile render(int Index, const TextMinimapSource& Source, const StyleColor& Colors, const QColor& Pending) const;
    void touch(int Index);
    void trim();

    Options options_;
    QHash<int, Tile> tiles_;
    QList<int> recent_;
    qint64 bytes_ = 0;
};
