#include "stdafx.h"
#include "text/TextOverviewRuler.h"

/// 모듈

/// UIs

/// 기타 헤더파일
#include "text/TextMinimap.h"
#include "text/TextMinimapPeek.h"
#include "text/TextMinimapSource.h"

/// 고유 라이브러리

/// 공통 라이브러리

/// 외부 라이브러리

/// 선언 헤더파일들

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QStyle>
#include <QStyleOptionSlider>
#include <cmath>

namespace
{
    constexpr int MarkTolerance = 3;

    QStyleOptionSlider optionOf(const QScrollBar* Bar)
    {
        // QScrollBar::initStyleOption() is protected; this fills the same fields.
        QStyleOptionSlider Option;
        Option.initFrom(Bar);
        Option.subControls = QStyle::SC_All;
        Option.activeSubControls = QStyle::SC_None;
        Option.orientation = Bar->orientation();
        Option.minimum = Bar->minimum();
        Option.maximum = Bar->maximum();
        Option.sliderPosition = Bar->sliderPosition();
        Option.sliderValue = Bar->value();
        Option.singleStep = Bar->singleStep();
        Option.pageStep = Bar->pageStep();
        Option.upsideDown = Bar->invertedAppearance();
        if (Bar->orientation() == Qt::Horizontal)
            Option.state |= QStyle::State_Horizontal;
        return Option;
    }
}

TextOverviewRuler::TextOverviewRuler(TextMinimap* Owner, QScrollBar* Bar)
    : QWidget(Bar)
    , owner_(Owner)
    , bar_(Bar)
{
    setObjectName(QStringLiteral("textOverviewRuler"));
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setAttribute(Qt::WA_NoSystemBackground);
    setFocusPolicy(Qt::NoFocus);
    Bar->installEventFilter(this);
    Bar->setMouseTracking(true);
    peekTimer_.setSingleShot(true);
    connect(&peekTimer_, &QTimer::timeout, this, [this] {
        if (!bar_ || !isVisible()) // a Leave on the bar stops this timer
            return;
        const int Y = mapFromGlobal(peekPosition_).y();
        owner_->ShowPeek(RowAt(Y), QRect(bar_->mapToGlobal(QPoint(0, 0)), bar_->size()), peekPosition_.y());
    });
    reposition();
}

QRect TextOverviewRuler::Groove() const
{
    if (!bar_)
        return {};
    const auto Option = optionOf(bar_);
    QRect Groove = bar_->style()->subControlRect(QStyle::CC_ScrollBar, &Option, QStyle::SC_ScrollBarGroove, bar_);
    if (!Groove.isValid() || Groove.height() < 8)
        Groove = bar_->rect();
    return Groove;
}

QRect TextOverviewRuler::sliderRect() const
{
    if (!bar_)
        return {};
    const auto Option = optionOf(bar_);
    return bar_->style()->subControlRect(QStyle::CC_ScrollBar, &Option, QStyle::SC_ScrollBarSlider, bar_);
}

double TextOverviewRuler::YOfRow(double Row) const
{
    const int Rows = qMax(1, owner_->Source().DisplayRows());
    return 2 + Row * qMax(1, height() - 4) / Rows;
}

double TextOverviewRuler::RowAt(int Y) const
{
    const int Rows = qMax(1, owner_->Source().DisplayRows());
    return qMax(0.0, double(Y - 2) * Rows / qMax(1, height() - 4));
}

void TextOverviewRuler::reposition()
{
    if (!bar_)
        return;
    const QRect Target = Groove();
    if (geometry() != Target)
        setGeometry(Target);
    raise();
}

int TextOverviewRuler::MarkRowNear(int Y, int Tolerance) const
{
    const auto& Source = owner_->Source();
    const auto& Settings = owner_->Settings();
    int Best = -1;
    double BestDistance = Tolerance + 0.5;
    const auto Consider = [&](int Line) {
        const int Row = Source.RowOfLine(Line);
        const double Distance = std::abs(YOfRow(Row) - Y);
        if (Distance < BestDistance) { BestDistance = Distance; Best = Row; }
    };
    if (Settings.problems_)
        for (const auto& Problem : owner_->Problems()) Consider(Problem.line_);
    if (Settings.changes_)
        for (const auto& Mark : owner_->Changes()) Consider(Mark.line_);
    if (Settings.search_ && owner_->Search().Active())
        for (const auto& Hit : owner_->Search().Hits()) Consider(Hit.line_);
    return Best;
}

void TextOverviewRuler::paintEvent(QPaintEvent*)
{
    QPainter Painter(this);
    Painter.setPen(Qt::NoPen);
    const auto& Source = owner_->Source();
    const auto& Settings = owner_->Settings();
    const auto& Colors = owner_->Colors();
    const int Width = width();
    // Lanes: change history | search and selection | problems; two lanes on a narrow bar.
    const bool ThreeLanes = Width >= 12;
    const QRectF ChangeLane(1, 0, ThreeLanes ? 3 : Width / 2.0 - 1, 0);
    const QRectF SearchLane(ThreeLanes ? 5 : Width / 2.0, 0, ThreeLanes ? Width - 10 : Width / 2.0 - 1, 0);
    const QRectF ProblemLane(ThreeLanes ? Width - 4.5 : Width / 2.0, 0, ThreeLanes ? 3.5 : Width / 2.0 - 1, 0);
    const auto Mark = [&](const QRectF& Lane, int FirstRow, int EndRow, const QColor& Color) {
        const double Top = std::floor(YOfRow(FirstRow));
        const double Bottom = qMax(Top + 2, std::ceil(YOfRow(EndRow)));
        Painter.fillRect(QRectF(Lane.left(), Top, Lane.width(), Bottom - Top), Color);
    };
    if (Settings.changes_)
        for (const auto& Change : owner_->Changes()) {
            const int Row = Source.RowOfLine(Change.line_);
            Mark(ChangeLane, Row, Row + 1, Change.change_ == TextMinimapChange::Modified ? Colors.changeModified_ : Colors.changeSaved_);
        }
    qint64 SelectionStart = 0, SelectionEnd = 0;
    Source.Selection(SelectionStart, SelectionEnd);
    if (Settings.selection_ && SelectionStart != SelectionEnd) {
        QColor Selection = Colors.selection_;
        Selection.setAlpha(qMin(255, Selection.alpha() * 2));
        Mark(SearchLane, Source.RowOfLine(Source.LineFromPosition(SelectionStart)), Source.RowOfLine(Source.LineFromPosition(SelectionEnd) + 1), Selection);
    }
    const auto& Search = owner_->Search();
    if (Settings.search_ && Search.Active()) {
        double Last = -10;
        if (!Search.Truncated()) {
            for (const auto& Hit : Search.Hits()) {
                const double Y = std::floor(YOfRow(Source.RowOfLine(Hit.line_)));
                if (Y == Last) continue;
                Last = Y;
                Painter.fillRect(QRectF(SearchLane.left(), Y, SearchLane.width(), 2), Colors.search_);
            }
        } else {
            const auto& Density = Search.Density();
            for (int Bin = 0; Bin < Density.size(); ++Bin)
                if (Density[Bin] > 0)
                    Painter.fillRect(QRectF(SearchLane.left(), std::floor(YOfRow(Source.RowOfLine(int(qint64(Bin) * Search.DensityLines() / Density.size())))),
                                            SearchLane.width(), 2), Colors.search_);
        }
    }
    if (Settings.problems_)
        for (const auto& Problem : owner_->Problems()) {
            const int Row = Source.RowOfLine(Problem.line_);
            Mark(ProblemLane, Row, Row + 1, Problem.error_ ? Colors.error_ : Colors.warning_);
        }
    if (Settings.caret_ && SelectionStart == SelectionEnd) {
        QColor Caret = Colors.caret_;
        Caret.setAlpha(200);
        Painter.fillRect(QRectF(0, std::round(YOfRow(Source.RowOfLine(Source.LineFromPosition(Source.CaretPosition())))), Width, 2), Caret);
    }
}

bool TextOverviewRuler::eventFilter(QObject* Watched, QEvent* Event)
{
    if (Watched != bar_)
        return QWidget::eventFilter(Watched, Event);
    switch (Event->type()) {
    case QEvent::Resize:
    case QEvent::Show:
    case QEvent::StyleChange:
        reposition();
        break;
    case QEvent::MouseButtonPress: {
        auto* Mouse = static_cast<QMouseEvent*>(Event);
        if (!isVisible() || Mouse->button() != Qt::LeftButton)
            break;
        owner_->HidePeek();
        const QPoint At = Mouse->position().toPoint();
        if (sliderRect().contains(At))
            break; // thumb dragging stays with the scroll bar
        const int Row = MarkRowNear(mapFrom(bar_, At).y(), MarkTolerance);
        if (Row < 0)
            break; // empty track pages as before
        owner_->ScrollRowToCenter(Row);
        return true;
    }
    case QEvent::MouseMove: {
        auto* Mouse = static_cast<QMouseEvent*>(Event);
        if (!isVisible() || Mouse->buttons() != Qt::NoButton || !owner_->Settings().peek_)
            break;
        peekPosition_ = Mouse->globalPosition().toPoint();
        peekTimer_.start(owner_->Peek() && owner_->Peek()->isVisible() ? 0 : owner_->Settings().peekDelayMs_);
        break;
    }
    case QEvent::Leave:
    case QEvent::Wheel:
        peekTimer_.stop();
        owner_->HidePeek();
        break;
    default:
        break;
    }
    return QWidget::eventFilter(Watched, Event);
}
