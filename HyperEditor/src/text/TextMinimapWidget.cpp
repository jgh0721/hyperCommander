#include "stdafx.h"
#include "text/TextMinimapWidget.h"

/// 모듈

/// UIs

/// 기타 헤더파일
#include "text/ScintillaQtDirectBackend.h"
#include "text/TextMinimap.h"
#include "text/TextMinimapPeek.h"
#include "text/TextMinimapSource.h"

/// 고유 라이브러리

/// 공통 라이브러리

/// 외부 라이브러리

/// 선언 헤더파일들

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QAbstractScrollArea>
#include <QApplication>
#include <QContextMenuEvent>
#include <QLocale>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>

namespace
{
    constexpr int LabelHeight = 13;
    constexpr int MaxExactSearchMarks = 200;
}

TextMinimapWidget::TextMinimapWidget(TextMinimap* Owner, QWidget* Parent)
    : QWidget(Parent)
    , owner_(Owner)
{
    setObjectName(QStringLiteral("textMinimap"));
    setAccessibleName(tr("미니맵"));
    setAccessibleDescription(tr("클릭하거나 끌어서 문서 위치로 이동"));
    setFocusPolicy(Qt::NoFocus);
    setMouseTracking(true);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setCursor(Qt::ArrowCursor);
    peekTimer_.setSingleShot(true);
    connect(&peekTimer_, &QTimer::timeout, this, [this] {
        if (dragging_) // leaveEvent() stops this timer
            return;
        const auto Map = Mapping();
        const double Row = Map.RowAt(mapFromGlobal(peekPosition_).y());
        owner_->ShowPeek(Row, QRect(mapToGlobal(QPoint(0, 0)), size()), peekPosition_.y());
    });
}

TextMinimapMapping TextMinimapWidget::Mapping() const
{
    TextMinimapMapping Map;
    const auto& Source = owner_->Source();
    Map.document_ = owner_->Mode().column_ == TextMinimapMode::Column::Document;
    const int Rows = Source.DisplayRows(), First = Source.FirstVisibleRow(), OnScreen = Source.RowsOnScreen();
    if (Map.document_)
        Map.documentGeometry_ = TextMinimapDocumentGeometryFor(Rows, First, OnScreen, height(), owner_->Settings().scale_ == 2 ? 4 : 2);
    else
        Map.fitGeometry_ = TextMinimapFitGeometryFor(Rows, First, OnScreen, height(), 3, 3.0, 6);
    return Map;
}

void TextMinimapWidget::paintEvent(QPaintEvent*)
{
    QPainter Painter(this);
    const auto& Colors = owner_->Colors();
    const auto& Settings = owner_->Settings();
    Painter.fillRect(rect(), Colors.background_);
    const auto Map = Mapping();
    if (Map.document_) {
        TextMinimapRenderer::Options Options;
        Options.rowHeight_ = Settings.scale_ == 2 ? 4 : 2;
        Options.charWidth_ = Settings.scale_ == 2 ? 2 : 1;
        Options.width_ = width();
        Options.blocks_ = Settings.blocks_;
        Options.dpr_ = devicePixelRatioF();
        owner_->Renderer().PaintDocument(Painter, owner_->Source(), Map.documentGeometry_, height(), Options,
                                         [this](int Style) { return owner_->StyleColor(Style); }, Colors.pending_);
    } else {
        QVector<int> HeadingRows;
        for (const auto& Heading : owner_->Headings())
            if (Heading.level_ <= 2)
                HeadingRows.append(owner_->Source().RowOfLine(Heading.line_));
        std::sort(HeadingRows.begin(), HeadingRows.end());
        TextMinimapRenderer::PaintOutline(Painter, owner_->Outline(), Map.fitGeometry_, width(), Colors.outline_, Colors.label_, HeadingRows);
    }
    paintMarks(Painter, Map);
    if (Settings.labels_)
        paintLabels(Painter, Map);
    paintViewport(Painter, Map);
    // The text-side edge.
    const bool Right = Settings.side_ == TextMinimapSettings::Side::Right;
    Painter.fillRect(QRect(Right ? 0 : width() - 1, 0, 1, height()), Colors.separator_);
    if (!Map.document_ && Settings.kind_ == TextMinimapSettings::Kind::Auto && owner_->Mode().large_) {
        QFont Small = font();
        Small.setPointSizeF(qMax(6.0, Small.pointSizeF() - 2));
        Painter.setFont(Small);
        const QString Notice = tr("윤곽 · %1줄").arg(QLocale().toString(owner_->Source().LineCount()));
        const QRect Box = Painter.fontMetrics().boundingRect(Notice).adjusted(-4, -1, 4, 1).translated(6, 6 + Painter.fontMetrics().ascent());
        Painter.fillRect(Box, Colors.background_);
        Painter.setPen(Colors.labelMinor_);
        Painter.drawText(Box, Qt::AlignCenter, Notice);
    }
}

void TextMinimapWidget::paintMarks(QPainter& Painter, const TextMinimapMapping& Map) const
{
    const auto& Source = owner_->Source();
    const auto& Settings = owner_->Settings();
    const auto& Colors = owner_->Colors();
    const double RowHeight = Map.RowHeight();
    const int Width = width();
    const auto RowsOf = [&Source](int Line) { return qMax(1, Source.RowOfLine(Line + 1) - Source.RowOfLine(Line)); };
    const auto Visible = [this](double Y) { return Y > -8 && Y < height() + 8; };
    Painter.setPen(Qt::NoPen);

    qint64 SelectionStart = 0, SelectionEnd = 0;
    Source.Selection(SelectionStart, SelectionEnd);
    if (Settings.selection_ && SelectionStart != SelectionEnd) {
        const int First = Source.LineFromPosition(SelectionStart), Last = Source.LineFromPosition(SelectionEnd);
        const double Top = Map.YOfRow(Source.RowOfLine(First)), Bottom = Map.YOfRow(Source.RowOfLine(Last + 1));
        Painter.fillRect(QRectF(0, Top, Width, qMax(2.0, Bottom - Top)), Colors.selection_);
    }

    const auto& Search = owner_->Search();
    if (Settings.search_ && Search.Active()) {
        Painter.setBrush(Colors.search_);
        const auto& Hits = Search.Hits();
        if (Map.document_) {
            const int TopLine = Source.LineOfRow(qMax(0, int(Map.RowAt(0))));
            const int BottomLine = Source.LineOfRow(qMax(0, int(Map.RowAt(height())) + 1));
            const auto First = std::lower_bound(Hits.cbegin(), Hits.cend(), TopLine, [](const auto& Hit, int Line) { return Hit.line_ < Line; });
            const int CharWidth = Settings.scale_ == 2 ? 2 : 1;
            const int Tab = Source.TabWidth();
            int Exact = 0;
            for (auto It = First; It != Hits.cend() && It->line_ <= BottomLine; ++It) {
                const bool Precise = Exact++ < MaxExactSearchMarks;
                const double Y = Map.YOfRow(Source.RowOfLine(It->line_) + (Precise ? Source.SubRowOf(It->start_) : 0));
                const int Column = Precise && RowsOf(It->line_) == 1 ? TextMinimapRenderer::ColumnOf(Source, It->line_, It->start_, Tab) : -1;
                if (Column >= 0) {
                    const int EndColumn = qMax(Column + 1, TextMinimapRenderer::ColumnOf(Source, It->line_, It->end_, Tab));
                    Painter.drawRect(QRectF(4 + Column * CharWidth - 1, Y, qMax(3, (EndColumn - Column) * CharWidth + 2), RowHeight));
                } else {
                    Painter.drawRect(QRectF(Width - 12, Y, 8, qMax(2.0, RowHeight)));
                }
            }
        } else if (!Search.Truncated()) {
            double Last = -10;
            for (const auto& Hit : Hits) {
                const double Y = std::round(Map.YOfRow(Source.RowOfLine(Hit.line_)));
                if (Y == Last) continue;
                Last = Y;
                Painter.drawRect(QRectF(Width - 12, Y, 8, 2));
            }
        } else {
            const auto& Density = Search.Density();
            for (int Bin = 0; Bin < Density.size(); ++Bin)
                if (Density[Bin] > 0)
                    Painter.drawRect(QRectF(Width - 12, std::round(Map.YOfRow(Source.RowOfLine(int(qint64(Bin) * Search.DensityLines() / Density.size())))), 8, 2));
        }
    }

    if (Settings.caret_ && SelectionStart == SelectionEnd) {
        const qint64 Caret = Source.CaretPosition();
        const int Line = Source.LineFromPosition(Caret);
        const double Y = Map.YOfRow(Source.RowOfLine(Line) + (Map.document_ ? Source.SubRowOf(Caret) : 0));
        QColor Caretline = Colors.caret_;
        Caretline.setAlpha(Map.document_ ? 110 : 220);
        Painter.fillRect(QRectF(0, std::round(Y), Width, Map.document_ ? RowHeight : 2.0), Caretline);
    }

    if (Settings.changes_) {
        for (const auto& Mark : owner_->Changes()) {
            const double Y = Map.YOfRow(Source.RowOfLine(Mark.line_));
            if (!Visible(Y)) continue;
            Painter.setBrush(Mark.change_ == TextMinimapChange::Modified ? Colors.changeModified_ : Colors.changeSaved_);
            Painter.drawRect(QRectF(Settings.side_ == TextMinimapSettings::Side::Right ? 1 : Width - 4, Y, 3, qMax(2.0, RowsOf(Mark.line_) * RowHeight)));
        }
    }

    if (Settings.problems_) {
        for (const auto& Problem : owner_->Problems()) {
            const double Y = Map.YOfRow(Source.RowOfLine(Problem.line_));
            if (!Visible(Y)) continue;
            Painter.setBrush(Problem.error_ ? Colors.error_ : Colors.warning_);
            Painter.drawRect(QRectF(Settings.side_ == TextMinimapSettings::Side::Right ? Width - 4 : 1, Y - 1, 4, qMax(3.0, RowsOf(Problem.line_) * RowHeight + 1)));
        }
    }
}

void TextMinimapWidget::paintLabels(QPainter& Painter, const TextMinimapMapping& Map) const
{
    const auto& Headings = owner_->Headings();
    if (Headings.isEmpty())
        return;
    int Top = 6;
    for (const auto& Heading : Headings)
        Top = qMin(Top, Heading.level_);
    const int Deepest = Top + 1;
    const auto& Colors = owner_->Colors();
    QFont Label = font();
    Label.setPointSizeF(qMax(6.0, Label.pointSizeF() - 2.5));
    Label.setBold(true);
    Painter.setFont(Label);
    const QFontMetrics Metrics(Label);
    QVector<double> Placed;
    QColor Band = Colors.background_;
    Band.setAlpha(230);
    for (const auto& Heading : Headings) {
        if (Heading.level_ > Deepest)
            continue;
        const double Y = std::round(Map.YOfRow(owner_->Source().RowOfLine(Heading.line_))) - 1;
        if (Y < -LabelHeight || Y > height())
            continue;
        if (std::any_of(Placed.cbegin(), Placed.cend(), [Y](double Other) { return std::abs(Other - Y) < LabelHeight; }))
            continue;
        Placed.append(Y);
        const bool Major = Heading.level_ == Top;
        Painter.fillRect(QRectF(0, Y, width(), LabelHeight), Band);
        QColor Rule = Colors.label_;
        Rule.setAlpha(Major ? 100 : 56);
        Painter.fillRect(QRectF(0, Y, width(), 1), Rule);
        Painter.setPen(Major ? Colors.label_ : Colors.labelMinor_);
        Painter.drawText(QRectF(5, Y + 1, width() - 10, LabelHeight - 1), Qt::AlignLeft | Qt::AlignVCenter,
                         Metrics.elidedText(Heading.text_, Qt::ElideRight, width() - 10));
    }
}

void TextMinimapWidget::paintViewport(QPainter& Painter, const TextMinimapMapping& Map) const
{
    const auto& Colors = owner_->Colors();
    const double Top = Map.SliderTop(), Height = Map.SliderHeight();
    switch (owner_->Settings().slider_) {
    case TextMinimapSettings::Slider::Bracket: {
        QPen Pen(Colors.bracket_, 1.5);
        Painter.setPen(Pen);
        Painter.setBrush(Qt::NoBrush);
        const double Right = width() - 1.5;
        Painter.drawPolyline(QPolygonF({QPointF(4, Top), QPointF(1.5, Top), QPointF(1.5, Top + Height), QPointF(4, Top + Height)}));
        Painter.drawPolyline(QPolygonF({QPointF(Right - 2.5, Top), QPointF(Right, Top), QPointF(Right, Top + Height), QPointF(Right - 2.5, Top + Height)}));
        return;
    }
    case TextMinimapSettings::Slider::Hover:
        if (!hover_ && !dragging_)
            return;
        [[fallthrough]];
    case TextMinimapSettings::Slider::Always:
    default:
        Painter.fillRect(QRectF(0, Top, width(), Height), dragging_ ? Colors.sliderActive_ : hover_ ? Colors.sliderHover_ : Colors.slider_);
        Painter.fillRect(QRectF(0, std::round(Top), width(), 1), Colors.sliderActive_);
        Painter.fillRect(QRectF(0, std::round(Top + Height) - 1, width(), 1), Colors.sliderActive_);
        return;
    }
}

// ── 입력 ───────────────────────────────────────────────────────────────

void TextMinimapWidget::mousePressEvent(QMouseEvent* Event)
{
    if (Event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(Event);
        return;
    }
    peekTimer_.stop();
    owner_->HidePeek();
    const double Y = Event->position().y();
    const auto Map = Mapping();
    if (!firstPressClock_.isValid() || firstPressClock_.elapsed() > QApplication::doubleClickInterval() || std::abs(Y - firstPressY_) > 4) {
        firstPressRow_ = Map.RowAt(Y);
        firstPressY_ = Y;
        firstPressClock_.start();
    }
    if (Y >= Map.SliderTop() && Y <= Map.SliderTop() + Map.SliderHeight()) {
        dragOffset_ = Y - Map.SliderTop();
    } else {
        owner_->ScrollRowToCenter(Map.RowAt(Y));
        dragOffset_ = Map.SliderHeight() / 2;
    }
    dragging_ = true;
    update();
}

void TextMinimapWidget::dragTo(double Y)
{
    const auto Map = Mapping();
    const auto& Source = owner_->Source();
    if (Map.document_ && Map.documentGeometry_.scrolls_) {
        const int MaxFirst = qMax(0, Source.DisplayRows() - Source.RowsOnScreen());
        const double Range = qMax(1.0, height() - Map.SliderHeight());
        owner_->ScrollToRow(int(std::lround(qBound(0.0, (Y - dragOffset_) / Range, 1.0) * MaxFirst)));
    } else {
        owner_->ScrollToRow(int(std::lround(Map.RowAt(Y - dragOffset_))));
    }
}

void TextMinimapWidget::mouseMoveEvent(QMouseEvent* Event)
{
    if (dragging_) {
        dragTo(Event->position().y());
        return;
    }
    if (!hover_) {
        hover_ = true;
        update();
    }
    peekPosition_ = Event->globalPosition().toPoint();
    if (owner_->Settings().peek_) {
        if (owner_->Peek() && owner_->Peek()->isVisible())
            peekTimer_.start(0);
        else
            peekTimer_.start(owner_->Settings().peekDelayMs_);
    }
}

void TextMinimapWidget::mouseReleaseEvent(QMouseEvent* Event)
{
    if (Event->button() == Qt::LeftButton && dragging_) {
        dragging_ = false;
        update();
    }
}

void TextMinimapWidget::mouseDoubleClickEvent(QMouseEvent* Event)
{
    if (Event->button() != Qt::LeftButton)
        return;
    dragging_ = false;
    // Normally the first press of the pair recorded the row. A double click without that press (synthetic input)
    // uses the row under the pointer.
    const bool Paired = firstPressRow_ >= 0 && firstPressClock_.isValid() && firstPressClock_.elapsed() <= 2 * QApplication::doubleClickInterval();
    const double Row = Paired ? firstPressRow_ : Mapping().RowAt(Event->position().y());
    owner_->GoToLine(owner_->Source().LineOfRow(qMax(0, int(Row))));
    firstPressClock_.invalidate();
    update();
}

void TextMinimapWidget::wheelEvent(QWheelEvent* Event)
{
    owner_->HidePeek();
    Event->accept();
    if (Event->modifiers() & Qt::ControlModifier)
        return; // no zoom from the minimap
    auto* Area = qobject_cast<QAbstractScrollArea*>(parentWidget());
    if (!Area)
        return;
    QWidget* Viewport = Area->viewport();
    QWheelEvent Forward(Viewport->mapFromGlobal(Event->globalPosition()), Event->globalPosition(), Event->pixelDelta(), Event->angleDelta(),
                        Event->buttons(), Event->modifiers(), Event->phase(), Event->inverted(), Event->source());
    QCoreApplication::sendEvent(Viewport, &Forward);
}

void TextMinimapWidget::leaveEvent(QEvent* Event)
{
    hover_ = false;
    peekTimer_.stop();
    owner_->HidePeek();
    update();
    QWidget::leaveEvent(Event);
}

void TextMinimapWidget::contextMenuEvent(QContextMenuEvent* Event)
{
    owner_->HidePeek();
    QMenu Menu(this);
    TextMinimap::PopulateMenu(&Menu, owner_->View());
    const auto& Mode = owner_->Mode();
    const QString State = Mode.column_ == TextMinimapMode::Column::Outline && Mode.large_ ? tr("윤곽 표시 중 (대용량 문서)")
        : Mode.column_ == TextMinimapMode::Column::Outline ? tr("윤곽 표시 중") : tr("문서 축소판 표시 중");
    auto* Header = new QAction(State, &Menu);
    Header->setEnabled(false);
    const auto First = Menu.actions().value(0);
    Menu.insertAction(First, Header);
    Menu.insertSeparator(First);
    Menu.exec(Event->globalPos());
}
