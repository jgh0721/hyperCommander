#include "stdafx.h"
#include "text/TextMinimapRenderer.h"

/// 모듈

/// UIs

/// 기타 헤더파일
#include "text/TextMinimapSource.h"

/// 고유 라이브러리

/// 공통 라이브러리

/// 외부 라이브러리

/// 선언 헤더파일들

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QPainter>
#include <QtMath>
#include <algorithm>

namespace
{
    constexpr int LeftPadding = 4;
    constexpr int MaxLineBytes = 64 * 1024;

    struct Glyph
    {
        char32_t code_ = 0;
        quint8 style_ = 0;
        int column_ = 0;
        int width_ = 1;
    };

    // Decodes UTF-8 and keeps each character's style (the style of its lead byte). Tabs expand to the next stop.
    QVector<Glyph> decode(const QByteArray& Text, const QByteArray& Styles, int TabWidth, int* Columns)
    {
        QVector<Glyph> Result;
        Result.reserve(Text.size());
        int Column = 0;
        const auto* Bytes = reinterpret_cast<const unsigned char*>(Text.constData());
        const qsizetype Size = Text.size();
        for (qsizetype Index = 0; Index < Size;) {
            const unsigned char Lead = Bytes[Index];
            int Length = 1;
            char32_t Code = Lead;
            if (Lead >= 0xF0) { Length = 4; Code = Lead & 0x07; }
            else if (Lead >= 0xE0) { Length = 3; Code = Lead & 0x0F; }
            else if (Lead >= 0xC0) { Length = 2; Code = Lead & 0x1F; }
            if (Index + Length > Size)
                break;
            for (int Next = 1; Next < Length; ++Next)
                Code = (Code << 6) | (Bytes[Index + Next] & 0x3F);
            const quint8 Style = Index < Styles.size() ? quint8(Styles[Index]) : 0;
            Index += Length;
            if (Code == '\r' || Code == '\n')
                break;
            if (Code == '\t') {
                Column = (Column / qMax(1, TabWidth) + 1) * qMax(1, TabWidth);
                continue;
            }
            const int Width = TextMinimapCharColumns(Code);
            if (Width == 0)
                continue;
            Result.append({Code, Style, Column, Width});
            Column += Width;
        }
        if (Columns)
            *Columns = Column;
        return Result;
    }

    bool punctuation(char32_t Code)
    {
        return Code < 0x80 && !((Code >= '0' && Code <= '9') || (Code >= 'A' && Code <= 'Z') || (Code >= 'a' && Code <= 'z'));
    }

    // Glyph texture of the "characters" rendering: ascenders, descenders and punctuation differ in shape so that
    // code keeps its look at 1-2 px per character.
    void paintGlyph(QPainter& Painter, qreal X, qreal Y, const Glyph& Item, int RowHeight)
    {
        const char32_t Code = Item.code_;
        if (Code == ' ' || Code == 0x3000)
            return;
        if (RowHeight <= 2) {
            if (Item.width_ == 2) Painter.drawRect(QRectF(X, Y, 1.7, 2));
            else if (punctuation(Code)) Painter.drawRect(QRectF(X, Y + 1, 1, 1));
            else Painter.drawRect(QRectF(X, Y, 1, 2));
            return;
        }
        if (Item.width_ == 2) { Painter.drawRect(QRectF(X + 0.3, Y, 3.4, 3)); return; }
        if (punctuation(Code)) { Painter.drawRect(QRectF(X + 0.4, Y + 2, 1.2, 1)); return; }
        const bool Ascender = (Code >= 'A' && Code <= 'Z') || (Code >= '0' && Code <= '9') || Code == 'b' || Code == 'd' ||
            Code == 'f' || Code == 'h' || Code == 'k' || Code == 'l' || Code == 't';
        const bool Descender = Code == 'g' || Code == 'j' || Code == 'p' || Code == 'q' || Code == 'y';
        if (Ascender) Painter.drawRect(QRectF(X, Y, 1.6, 3));
        else if (Descender) Painter.drawRect(QRectF(X, Y + 1, 1.6, 3));
        else Painter.drawRect(QRectF(X, Y + 1, 1.6, 2));
    }
}

void TextMinimapRenderer::Clear()
{
    tiles_.clear();
    recent_.clear();
    bytes_ = 0;
}

void TextMinimapRenderer::InvalidateLines(int First, int Last)
{
    for (int Index = qMax(0, First) / TileLines; Index <= qMax(First, Last) / TileLines; ++Index) {
        const auto It = tiles_.constFind(Index);
        if (It == tiles_.cend())
            continue;
        bytes_ -= It->image_.sizeInBytes();
        tiles_.erase(It);
        recent_.removeAll(Index);
    }
}

void TextMinimapRenderer::InvalidateFrom(int Line)
{
    const int First = qMax(0, Line) / TileLines;
    for (auto It = tiles_.begin(); It != tiles_.end();) {
        if (It.key() < First) { ++It; continue; }
        bytes_ -= It->image_.sizeInBytes();
        recent_.removeAll(It.key());
        It = tiles_.erase(It);
    }
}

void TextMinimapRenderer::touch(int Index)
{
    recent_.removeAll(Index);
    recent_.append(Index);
}

void TextMinimapRenderer::trim()
{
    while (bytes_ > CacheBytes && recent_.size() > 1) {
        const int Oldest = recent_.takeFirst();
        const auto It = tiles_.constFind(Oldest);
        if (It == tiles_.cend())
            continue;
        bytes_ -= It->image_.sizeInBytes();
        tiles_.erase(It);
    }
}

TextMinimapRenderer::Tile TextMinimapRenderer::render(int Index, const TextMinimapSource& Source, const StyleColor& Colors, const QColor& Pending) const
{
    Tile Result;
    const int Lines = Source.LineCount();
    const int FirstLine = Index * TileLines;
    const int EndLine = qMin(Lines, FirstLine + TileLines);
    const int FirstRow = Source.RowOfLine(FirstLine);
    Result.rows_ = Source.RowOfLine(EndLine) - FirstRow;
    if (Result.rows_ <= 0)
        return Result;
    const auto& O = options_;
    const QSize Pixels(qCeil(O.width_ * O.dpr_), qCeil(Result.rows_ * O.rowHeight_ * O.dpr_));
    Result.image_ = QImage(Pixels, QImage::Format_ARGB32_Premultiplied);
    Result.image_.setDevicePixelRatio(O.dpr_);
    Result.image_.fill(Qt::transparent);
    QPainter Painter(&Result.image_);
    Painter.setRenderHint(QPainter::Antialiasing, false);
    Painter.setPen(Qt::NoPen);
    const qint64 Styled = Source.EndStyled();
    const int Tab = Source.TabWidth();
    const int MaxColumns = qMax(1, (O.width_ - 2 * LeftPadding) / qMax(1, O.charWidth_));
    QByteArray Text, Styles;
    for (int Line = FirstLine; Line < EndLine; ++Line) {
        const int Row = Source.RowOfLine(Line) - FirstRow;
        const int LineRows = Source.RowOfLine(Line + 1) - Source.RowOfLine(Line);
        if (LineRows <= 0)
            continue; // folded away
        const qint64 Start = Source.LineStart(Line);
        const qint64 Bytes = qMin(Source.LineEnd(Line) - Start, qMin<qint64>(MaxLineBytes, qint64(MaxColumns) * LineRows * 4));
        const bool Unstyled = Start + Bytes > Styled;
        if (Unstyled) {
            Text = Source.Text(Start, Start + Bytes);
            Styles = QByteArray(Text.size(), '\0');
            Result.pending_ = true;
        } else {
            Source.StyledText(Start, Start + Bytes, Text, Styles);
        }
        int Columns = 0;
        const auto Glyphs = decode(Text, Styles, Tab, &Columns);
        // Rows of a wrapped line share its characters evenly; the real break positions would need a layout per line.
        const int PerRow = LineRows > 1 ? qMax(1, (Columns + LineRows - 1) / LineRows) : MaxColumns;
        int RunStyle = -1, RunRow = -1, RunStart = 0, RunEnd = 0;
        const auto Flush = [&] {
            if (RunStyle < 0 || RunEnd <= RunStart)
                return;
            Painter.setBrush(Unstyled ? Pending : Colors(RunStyle));
            const qreal Height = O.rowHeight_ <= 2 ? 2 : O.rowHeight_ - 1;
            Painter.drawRect(QRectF(LeftPadding + RunStart * O.charWidth_, (Row + RunRow) * O.rowHeight_, (RunEnd - RunStart) * O.charWidth_, Height));
            RunStyle = -1;
        };
        int LastStyle = -1;
        for (const auto& Item : Glyphs) {
            const int SubRow = qMin(LineRows - 1, Item.column_ / PerRow);
            const int Column = Item.column_ - SubRow * PerRow;
            if (Column >= MaxColumns) {
                if (LineRows == 1)
                    break;
                continue;
            }
            if (O.blocks_) {
                const bool Space = Item.code_ == ' ' || Item.code_ == 0x3000;
                if (Space || Item.style_ != RunStyle || SubRow != RunRow || Column != RunEnd) {
                    Flush();
                    if (Space)
                        continue;
                    RunStyle = Item.style_; RunRow = SubRow; RunStart = Column;
                }
                RunEnd = qMin(MaxColumns, Column + Item.width_);
                continue;
            }
            if (Item.style_ != LastStyle) {
                Painter.setBrush(Unstyled ? Pending : Colors(Item.style_));
                LastStyle = Item.style_;
            }
            paintGlyph(Painter, LeftPadding + Column * O.charWidth_, (Row + SubRow) * O.rowHeight_, Item, O.rowHeight_);
        }
        Flush();
    }
    return Result;
}

void TextMinimapRenderer::PaintDocument(QPainter& Painter, const TextMinimapSource& Source, const TextMinimapDocumentGeometry& Geometry,
                                        int Height, const Options& Settings, const StyleColor& Colors, const QColor& Pending)
{
    if (!(Settings == options_)) {
        Clear();
        options_ = Settings;
    }
    const int Lines = Source.LineCount();
    const int TopRow = qMax(0, int(std::floor(Geometry.RowAt(0))));
    const int BottomRow = qMax(TopRow, int(std::ceil(Geometry.RowAt(Height))));
    const int FirstLine = qBound(0, Source.LineOfRow(TopRow), Lines - 1);
    const int LastLine = qBound(FirstLine, Source.LineOfRow(BottomRow), Lines - 1);
    const qint64 Styled = Source.EndStyled();
    for (int Index = FirstLine / TileLines; Index <= LastLine / TileLines; ++Index) {
        const int TileFirst = Index * TileLines;
        const int Row = Source.RowOfLine(TileFirst);
        const int Rows = Source.RowOfLine(qMin(Lines, TileFirst + TileLines)) - Row;
        if (Rows <= 0)
            continue;
        auto It = tiles_.find(Index);
        const qint64 TileEnd = Source.LineEnd(qMin(Lines, TileFirst + TileLines) - 1);
        // A different row count means wrapping, folding or zoom changed; a pending tile waits for styling.
        if (It == tiles_.end() || It->rows_ != Rows || (It->pending_ && Styled >= TileEnd)) {
            if (It != tiles_.end()) {
                bytes_ -= It->image_.sizeInBytes();
                tiles_.erase(It);
            }
            It = tiles_.insert(Index, render(Index, Source, Colors, Pending));
            bytes_ += It->image_.sizeInBytes();
        }
        touch(Index);
        if (!It->image_.isNull())
            Painter.drawImage(QPointF(0, Geometry.YOfRow(Row)), It->image_);
    }
    trim();
}

void TextMinimapRenderer::PaintOutline(QPainter& Painter, const TextMinimapOutline& Outline, const TextMinimapFitGeometry& Geometry,
                                       int Width, const QColor& Stroke, const QColor& Heading, const QVector<int>& HeadingRows)
{
    const int Count = Outline.BinCount();
    if (Count <= 0)
        return;
    constexpr int VisibleColumns = 100;
    const qreal Scale = qMax(0.1, (Width - 2.0 * LeftPadding - 2) / VisibleColumns);
    const int Rows = Outline.DisplayRows();
    Painter.setPen(Qt::NoPen);
    const auto& Bins = Outline.Bins();
    for (int Index = 0; Index < Count; ++Index) {
        const auto& Bin = Bins[Index];
        if (!Bin.done_ || Bin.stroke_.Empty())
            continue;
        const int FirstRow = int(qint64(Index) * Rows / Count);
        const int EndRow = qMax(FirstRow + 1, int(qint64(Index + 1) * Rows / Count));
        const qreal Y = Geometry.YOfRow(FirstRow);
        const qreal Span = (EndRow - FirstRow) * Geometry.perRow_;
        const qreal Height = Span >= 2 ? qMax<qreal>(1, Span - 1) : 1;
        const int Indent = qMin(int(Bin.stroke_.indent_), 60);
        const int End = qMin(int(Bin.stroke_.end_), VisibleColumns);
        const auto Hit = std::lower_bound(HeadingRows.cbegin(), HeadingRows.cend(), FirstRow);
        const bool IsHeading = Hit != HeadingRows.cend() && *Hit < EndRow;
        Painter.setBrush(IsHeading ? Heading : Stroke);
        Painter.drawRect(QRectF(LeftPadding + Indent * Scale, Y, qMax<qreal>(1, (End - Indent) * Scale), Height));
    }
}

int TextMinimapRenderer::ColumnOf(const TextMinimapSource& Source, int Line, qint64 Position, int TabWidth)
{
    const qint64 Start = Source.LineStart(Line);
    if (Position < Start || Position - Start > TextMinimapPrefixBytes)
        return -1;
    int Columns = 0;
    decode(Source.Text(Start, Position), {}, TabWidth, &Columns);
    return Columns;
}
