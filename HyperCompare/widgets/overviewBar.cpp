#include "stdafx.h"
#include "overviewBar.hpp"
#include "comparePalette.hpp"
#include <QPainter>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QFocusEvent>
#include <QHelpEvent>
#include <QToolTip>
#include <QAccessible>
#include <ScintillaEditBase.h>
#include <fmstyle/ThemeManager.h>

namespace hc::compare
{
    OverviewBar::OverviewBar(QWidget* Parent) : QWidget(Parent)
    {
        setObjectName("compareOverview"); setFocusPolicy(Qt::StrongFocus);
        setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
        SetSourceNumbers(1, 2);
        hide();
    }
    QSize OverviewBar::sizeHint() const { return {std::max(72, fontMetrics().horizontalAdvance("00") * 2 + 24), 160}; }
    int OverviewBar::headerHeight() const
    {
        if (editors_[0])
            return std::clamp(mapFromGlobal(editors_[0]->mapToGlobal(QPoint(0, 0))).y(), 0, std::max(0, height() - 1));
        return std::min(fontMetrics().height() + 8, std::max(0, height() - 1));
    }
    QRect OverviewBar::track(unsigned Side) const
    {
        const auto Lane = std::max(1, (width() - 20) / 2), Top = headerHeight();
        return {6 + int(Side) * (Lane + 8), Top, Lane, std::max(1, height() - Top - 4)};
    }
    void OverviewBar::SetSession(ScintillaEditBase* Left, ScintillaEditBase* Right, std::shared_ptr<const AlignmentMap> Map)
    {
        for (const auto& connection : connections_) disconnect(connection);
        for (const auto& Editor : editors_) if (Editor) Editor->removeEventFilter(this);
        connections_.clear(); editors_ = {Left, Right}; map_ = std::move(Map); active_ = Right && Right->hasFocus() ? 1 : 0;
        documents_ = {};
        for (unsigned side = 0; side < 2; ++side)
        {
            if (!editors_[side]) continue;
            editors_[side]->installEventFilter(this);
            connections_.push_back(connect(editors_[side], &ScintillaEditBase::verticalScrolled, this, [this](int) { updateDescription(); update(); }));
            connections_.push_back(connect(editors_[side], &ScintillaEditBase::updateUi, this, [this](Scintilla::Update) { update(); }));
            connections_.push_back(connect(editors_[side], &ScintillaEditBase::focusChanged, this, [this, side](bool Focused) {
                if (Focused) { active_ = side; updateDescription(); update(); }
            }));
            connections_.push_back(connect(editors_[side], &QObject::destroyed, this, [this] { hide(); }));
        }
        rebuild();
    }
    void OverviewBar::SetAligned(bool Aligned) { if (aligned_ != Aligned) { aligned_ = Aligned; rebuild(); } }
    void OverviewBar::SetDark(bool Dark) { dark_ = Dark; rebuild(); }
    void OverviewBar::SetDocuments(const std::array<OverviewDocument, 2>& Documents) { documents_ = Documents; rebuild(); }
    void OverviewBar::SetSourceNumbers(unsigned First, unsigned Second)
    {
        numbers_ = {First, Second};
        setAccessibleName(tr("원본 %1 / 원본 %2 차이 개요").arg(First).arg(Second));
        setToolTip(tr("왼쪽: 원본 %1 · 오른쪽: 원본 %2\n~ 변경 · + 추가 · − 삭제 · 사선: 대응 여백\n축소 선: 문서 윤곽 · 양옆 괄호선: 현재 화면 범위\n클릭·드래그로 이동, 좌우 키로 대상 전환, 위아래·PageUp/Down·Home/End로 스크롤").arg(First).arg(Second));
        updateDescription();
        update();
    }
    void OverviewBar::FocusSource() { if (editors_[active_]) editors_[active_]->setFocus(Qt::OtherFocusReason); }
    void OverviewBar::rebuild()
    {
        renderMap();
        updateDescription();
        update();
    }
    bool OverviewBar::eventFilter(QObject* Object, QEvent* Event)
    {
        if (Event->type() == QEvent::Resize || Event->type() == QEvent::Move || Event->type() == QEvent::Show) update();
        return QWidget::eventFilter(Object, Event);
    }
    bool OverviewBar::event(QEvent* Event)
    {
        if (Event->type() == QEvent::ToolTip && map_)
        {
            auto* Help = static_cast<QHelpEvent*>(Event);
            const unsigned Side = Help->pos().x() < width() / 2 ? 0 : 1;
            QToolTip::showText(Help->globalPos(), describe(Side, rowAt(Side, Help->pos().y())) + "\n" + toolTip(), this);
            return true;
        }
        return QWidget::event(Event);
    }
    std::intptr_t OverviewBar::rowAt(unsigned Side, int Y) const
    {
        const auto Area = track(Side);
        return std::intptr_t(std::size_t(std::clamp(Y - Area.top(), 0, Area.height() - 1)) * (raster_.rows_[Side] - 1) /
            std::size_t(std::max(1, Area.height() - 1)));
    }
    QString OverviewBar::describe(unsigned Side, std::intptr_t Row) const
    {
        if (!map_) return {};
        const auto Display = std::min<std::size_t>(std::size_t(std::max<std::intptr_t>(0, Row)), raster_.rows_[Side] - 1);
        const auto Line = aligned_ ? map_->LineAtRow(Side, Display) : std::optional<std::size_t>(Display);
        const auto& Bands = raster_.bands_[Side];
        const auto Mask = Bands.empty() ? 0 : Bands[std::min(Bands.size() - 1, Display * Bands.size() / raster_.rows_[Side])];
        QStringList Kinds;
        if (Mask & Modified) Kinds << tr("~ 변경");
        if (Mask & Added) Kinds << tr("+ 추가");
        if (Mask & Removed) Kinds << tr("− 삭제");
        if (Mask & Gap) Kinds << tr("대응 여백");
        if (Kinds.empty()) Kinds << tr("동일 구간");
        const bool AtEnd = Display == raster_.rows_[Side] - 1;
        const auto Position = AtEnd ? tr("파일 끝") : !Line ? tr("대응 여백") : tr("%1행").arg(*Line + 1);
        return tr("원본 %1 · %2 · %3").arg(numbers_[Side]).arg(Position, Kinds.join(" / "));
    }
    void OverviewBar::updateDescription()
    {
        const auto Position = editors_[active_] ? describe(active_, editors_[active_]->send(SCI_GETFIRSTVISIBLELINE)) : QString{};
        const auto Description = tr("왼쪽 열은 원본 %1, 오른쪽 열은 원본 %2입니다. 좌우 키로 대상을 선택하고 위아래·PageUp·PageDown·Home·End로 이동합니다. %3")
            .arg(numbers_[0]).arg(numbers_[1]).arg(Position);
        if (Description == accessibleDescription()) return;
        setAccessibleDescription(Description);
        QAccessibleEvent Changed(this, QAccessible::DescriptionChanged); QAccessible::updateAccessibility(&Changed);
    }
    void OverviewBar::resizeEvent(QResizeEvent* Event) { QWidget::resizeEvent(Event); rebuild(); }
    void OverviewBar::changeEvent(QEvent* Event)
    {
        QWidget::changeEvent(Event);
        if (Event->type() == QEvent::FontChange) { updateGeometry(); rebuild(); }
        else if (Event->type() == QEvent::PaletteChange || Event->type() == QEvent::EnabledChange || Event->type() == QEvent::DevicePixelRatioChange) rebuild();
    }
    void OverviewBar::renderMap()
    {
        const auto Dpr = devicePixelRatioF(); cachedTop_ = headerHeight();
        image_ = QPixmap((QSizeF(size()) * Dpr).toSize()); image_.setDevicePixelRatio(Dpr); image_.fill(palette().window().color());
        raster_ = map_ ? BuildOverviewRaster(*map_, std::size_t(std::ceil(track(0).height() * Dpr)), aligned_) : OverviewRaster{};
        QPainter painter(&image_);
        if (!map_ || !editors_[0] || !editors_[1]) return;
        // Changed · added · removed · alignment gap follow the status tokens, so they track the product theme and accent.
        const std::array<QColor, 4> colors{CompareColor("barModified", dark_), CompareColor("barAdded", dark_), CompareColor("barRemoved", dark_), CompareColor("barGap", dark_)};
        painter.setOpacity(isEnabled() ? 1.0 : 0.45);
        for (unsigned side = 0; side < 2; ++side)
        {
            const auto area = track(side); const auto& bands = raster_.bands_[side];
            painter.fillRect(area, CompareColor("barTrack", dark_));
            for (std::size_t begin = 0; begin < bands.size();)
            {
                auto end = begin + 1; while (end < bands.size() && bands[end] == bands[begin]) ++end;
                const auto mask = bands[begin]; int count = 0;
                for (unsigned kind = 0; kind < 4; ++kind) if (mask & (1 << kind)) ++count;
                int column = 0;
                for (unsigned kind = 0; kind < 4; ++kind)
                {
                    if (!(mask & (1 << kind))) continue;
                    const auto x = area.left() + column * area.width() / count;
                    const auto next = area.left() + (++column) * area.width() / count;
                    const auto y = area.top() + int(begin * std::size_t(area.height()) / bands.size());
                    const auto bottom = area.top() + int((end * std::size_t(area.height()) + bands.size() - 1) / bands.size());
                    const QRect Region(x, y, next - x, bottom - y);
                    if (kind == 3)
                    {
                        auto Hatch = colors[kind]; Hatch.setAlpha(150);
                        painter.fillRect(Region, QBrush(Hatch, Qt::BDiagPattern));
                    }
                    else
                    {
                        auto Background = colors[kind]; Background.setAlpha(55); painter.fillRect(Region, Background);
                        painter.fillRect(QRect(side ? Region.right() - 2 : Region.left(), y, std::min(3, Region.width()), bottom - y), colors[kind]);
                    }
                }
                begin = end;
            }
            // Geometry only: no source text or Scintilla line queries while painting.
            std::optional<std::size_t> Previous; double LastY = -3;
            for (std::size_t Pixel = 0; Pixel < bands.size(); ++Pixel)
            {
                const auto Row = Pixel * raster_.rows_[side] / bands.size();
                const auto Line = aligned_ ? map_->LineAtRow(side, Row) : std::optional<std::size_t>(Row);
                const auto Y = area.top() + double(Pixel) * area.height() / bands.size();
                if (!Line || Line == Previous || Y < LastY + 2) continue;
                Previous = Line;
                const auto Stroke = documents_[side].At(*Line);
                if (Stroke.end_ <= Stroke.indent_) continue;
                auto Color = palette().windowText().color(); Color.setAlpha(85);
                for (unsigned Kind = 0; Kind < 3; ++Kind) if (bands[Pixel] & (1 << Kind)) { Color = colors[Kind]; break; }
                const auto Space = std::max(1, area.width() - 7);
                const auto X = area.left() + 3 + double(Stroke.indent_) * Space / 128;
                const auto Length = std::max(1.0, double(Stroke.end_ - Stroke.indent_) * Space / 128);
                painter.fillRect(QRectF(X, Y, Length, 1.0 / Dpr), Color); LastY = Y;
            }
        }
    }
    void OverviewBar::paintEvent(QPaintEvent*)
    {
        if (image_.isNull() || image_.size() != (QSizeF(size()) * devicePixelRatioF()).toSize() ||
            image_.devicePixelRatio() != devicePixelRatioF() || cachedTop_ != headerHeight()) renderMap();
        QPainter painter(this); painter.drawPixmap(0, 0, image_);
        if (!map_ || !editors_[0] || !editors_[1]) return;
        painter.setOpacity(isEnabled() ? 1.0 : 0.45);
        for (unsigned side = 0; side < 2; ++side)
        {
            const auto area = track(side);
            painter.setPen(side == active_ ? CompareColor("barFocus", dark_) : palette().windowText().color());
            painter.drawText(QRect(area.left(), 0, area.width(), area.top()), Qt::AlignCenter, QString::number(numbers_[side]));
            const auto rows = double(raster_.rows_[side]);
            const auto top = std::clamp(double(editors_[side]->send(SCI_GETFIRSTVISIBLELINE)) / rows, 0.0, 1.0);
            const auto bottom = std::clamp(top + double(std::max<sptr_t>(1, editors_[side]->send(SCI_LINESONSCREEN))) / rows, top, 1.0);
            const auto y = std::min(area.bottom() - 1.0, area.top() + top * area.height());
            const auto end = std::min(double(area.bottom()), std::max(y + 2.0, area.top() + bottom * area.height()));
            painter.setPen(QPen(CompareColor("barViewport", dark_), side == active_ && hasFocus() ? 2 : 1));
            // Draw viewport brackets outside the change lane. An overlaid box
            // would cover one-pixel BOF/EOF changes with its horizontal edges.
            for (const auto x : {area.left() - 2.0, area.right() + 2.0})
            {
                painter.drawLine(QLineF(x, y, x, end));
                painter.drawLine(QLineF(x - 1, y, x + 1, y));
                painter.drawLine(QLineF(x - 1, end, x + 1, end));
            }
        }
        if (hasFocus())
        {
            painter.setPen(QPen(CompareColor("barFocus", dark_), 1, Qt::DotLine));
            painter.drawRect(rect().adjusted(1, 1, -2, -2));
        }
    }
    void OverviewBar::scrollTo(std::intptr_t Row)
    {
        if (!isEnabled() || !map_ || !editors_[active_]) return;
        const auto last = std::intptr_t(raster_.rows_[active_] - 1);
        editors_[active_]->send(SCI_SETFIRSTVISIBLELINE, uptr_t(std::clamp<std::intptr_t>(Row, 0, last))); updateDescription(); update();
    }
    void OverviewBar::navigate(int Y)
    {
        if (!map_ || !editors_[active_]) return;
        const auto row = rowAt(active_, Y);
        scrollTo(row - editors_[active_]->send(SCI_LINESONSCREEN) / 2);
    }
    void OverviewBar::mousePressEvent(QMouseEvent* Event)
    {
        if (Event->button() != Qt::LeftButton) { QWidget::mousePressEvent(Event); return; }
        active_ = Event->position().x() < width() / 2 ? 0 : 1; setFocus(Qt::MouseFocusReason);
        if (Event->position().y() >= track(active_).top()) navigate(int(Event->position().y()));
        updateDescription(); update(); Event->accept();
    }
    void OverviewBar::mouseMoveEvent(QMouseEvent* Event)
    {
        if (Event->buttons() & Qt::LeftButton) { navigate(int(Event->position().y())); Event->accept(); }
        else QWidget::mouseMoveEvent(Event);
    }
    void OverviewBar::keyPressEvent(QKeyEvent* Event)
    {
        if (!map_ || !editors_[active_]) { QWidget::keyPressEvent(Event); return; }
        auto row = editors_[active_]->send(SCI_GETFIRSTVISIBLELINE);
        const auto page = std::max<sptr_t>(1, editors_[active_]->send(SCI_LINESONSCREEN) - 1);
        switch (Event->key())
        {
        case Qt::Key_Left: active_ = 0; updateDescription(); update(); break;
        case Qt::Key_Right: active_ = 1; updateDescription(); update(); break;
        case Qt::Key_Escape: case Qt::Key_Return: FocusSource(); break;
        case Qt::Key_Up: scrollTo(row - 1); break;
        case Qt::Key_Down: scrollTo(row + 1); break;
        case Qt::Key_PageUp: scrollTo(row - page); break;
        case Qt::Key_PageDown: scrollTo(row + page); break;
        case Qt::Key_Home: scrollTo(0); break;
        case Qt::Key_End: scrollTo(std::intptr_t(raster_.rows_[active_] - 1)); break;
        default: QWidget::keyPressEvent(Event); return;
        }
        Event->accept();
    }
    void OverviewBar::focusInEvent(QFocusEvent* Event) { QWidget::focusInEvent(Event); update(); }
    void OverviewBar::focusOutEvent(QFocusEvent* Event) { QWidget::focusOutEvent(Event); update(); }
}
