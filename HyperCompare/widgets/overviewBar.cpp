#include "stdafx.h"
#include "overviewBar.hpp"
#include <QPainter>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QFocusEvent>
#include <ScintillaEditBase.h>

namespace hc::compare
{
    OverviewBar::OverviewBar(QWidget* Parent) : QWidget(Parent)
    {
        setObjectName("compareOverview"); setFocusPolicy(Qt::StrongFocus);
        setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
        SetSourceNumbers(1, 2);
        hide();
    }
    QSize OverviewBar::sizeHint() const { return {std::max(48, fontMetrics().horizontalAdvance("00") * 2 + 16), 160}; }
    QRect OverviewBar::track(unsigned Side) const
    {
        const auto lane = std::max(1, (width() - 12) / 2), top = fontMetrics().height() + 8;
        return {4 + int(Side) * (lane + 4), top, lane, std::max(1, height() - top - 4)};
    }
    void OverviewBar::SetSession(ScintillaEditBase* Left, ScintillaEditBase* Right, std::shared_ptr<const AlignmentMap> Map)
    {
        for (const auto& connection : connections_) disconnect(connection);
        connections_.clear(); editors_ = {Left, Right}; map_ = std::move(Map); active_ = 0;
        for (unsigned side = 0; side < 2; ++side)
        {
            if (!editors_[side]) continue;
            connections_.push_back(connect(editors_[side], &ScintillaEditBase::verticalScrolled, this, [this](int) { update(); }));
            connections_.push_back(connect(editors_[side], &ScintillaEditBase::updateUi, this, [this](Scintilla::Update) { update(); }));
            connections_.push_back(connect(editors_[side], &ScintillaEditBase::focusChanged, this, [this, side](bool Focused) {
                if (Focused) { active_ = side; update(); }
            }));
            connections_.push_back(connect(editors_[side], &QObject::destroyed, this, [this] { hide(); }));
        }
        rebuild();
    }
    void OverviewBar::SetAligned(bool Aligned) { if (aligned_ != Aligned) { aligned_ = Aligned; rebuild(); } }
    void OverviewBar::SetDark(bool Dark) { dark_ = Dark; update(); }
    void OverviewBar::SetSourceNumbers(unsigned First, unsigned Second)
    {
        numbers_ = {First, Second};
        setAccessibleName(tr("원본 %1 / 원본 %2 차이 개요").arg(First).arg(Second));
        setAccessibleDescription(tr("왼쪽 열은 원본 %1, 오른쪽 열은 원본 %2입니다. 좌우 키로 대상을 선택하고 위아래·PageUp·PageDown·Home·End로 이동합니다.").arg(First).arg(Second));
        setToolTip(tr("왼쪽: 원본 %1 · 오른쪽: 원본 %2\n노랑: 변경 · 초록: 오른쪽에 추가 · 빨강: 왼쪽에서 삭제 · 파랑: 대응 여백\n양옆 괄호선: 현재 화면 범위\n클릭·드래그로 이동, 좌우 키로 대상 전환, 위아래·PageUp/Down·Home/End로 스크롤").arg(First).arg(Second));
        update();
    }
    void OverviewBar::FocusSource() { if (editors_[active_]) editors_[active_]->setFocus(Qt::OtherFocusReason); }
    void OverviewBar::rebuild()
    {
        raster_ = map_ ? BuildOverviewRaster(*map_, std::size_t(track(0).height()), aligned_) : OverviewRaster{};
        update();
    }
    void OverviewBar::resizeEvent(QResizeEvent* Event) { QWidget::resizeEvent(Event); rebuild(); }
    void OverviewBar::changeEvent(QEvent* Event)
    {
        QWidget::changeEvent(Event);
        if (Event->type() == QEvent::FontChange) { updateGeometry(); rebuild(); }
        else if (Event->type() == QEvent::PaletteChange || Event->type() == QEvent::EnabledChange) update();
    }
    void OverviewBar::paintEvent(QPaintEvent*)
    {
        QPainter painter(this); painter.fillRect(rect(), palette().window());
        if (!map_ || !editors_[0] || !editors_[1]) return;
        const std::array<QColor, 4> colors = dark_ ?
            std::array<QColor, 4>{QColor("#d5b561"), QColor("#87c29b"), QColor("#e18c8c"), QColor("#8fb2de")} :
            std::array<QColor, 4>{QColor("#a87916"), QColor("#31804b"), QColor("#b44848"), QColor("#376fac")};
        painter.setOpacity(isEnabled() ? 1.0 : 0.45);
        for (unsigned side = 0; side < 2; ++side)
        {
            const auto area = track(side); const auto& bands = raster_.bands_[side];
            painter.setPen(side == active_ && hasFocus() ? palette().highlight().color() : palette().windowText().color());
            painter.drawText(QRect(area.left(), 0, area.width(), area.top()), Qt::AlignCenter, QString::number(numbers_[side]));
            painter.fillRect(area, palette().base());
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
                    painter.fillRect(QRect(x, y, next - x, bottom - y), colors[kind]);
                }
                begin = end;
            }
            const auto rows = double(raster_.rows_[side]);
            const auto top = std::clamp(double(editors_[side]->send(SCI_GETFIRSTVISIBLELINE)) / rows, 0.0, 1.0);
            const auto bottom = std::clamp(top + double(std::max<sptr_t>(1, editors_[side]->send(SCI_LINESONSCREEN))) / rows, top, 1.0);
            const auto y = std::min(area.bottom() - 1.0, area.top() + top * area.height());
            const auto end = std::min(double(area.bottom()), std::max(y + 2.0, area.top() + bottom * area.height()));
            painter.setPen(QPen(palette().windowText().color(), side == active_ && hasFocus() ? 2 : 1));
            // Draw viewport brackets outside the change lane. An overlaid box
            // would cover one-pixel BOF/EOF changes with its horizontal edges.
            for (const auto x : {area.left() - 2.0, area.right() + 2.0})
            {
                painter.drawLine(QLineF(x, y, x, end));
                painter.drawLine(QLineF(x - 1, y, x + 1, y));
                painter.drawLine(QLineF(x - 1, end, x + 1, end));
            }
        }
    }
    void OverviewBar::scrollTo(std::intptr_t Row)
    {
        if (!isEnabled() || !map_ || !editors_[active_]) return;
        const auto last = std::intptr_t(raster_.rows_[active_] - 1);
        editors_[active_]->send(SCI_SETFIRSTVISIBLELINE, uptr_t(std::clamp<std::intptr_t>(Row, 0, last))); update();
    }
    void OverviewBar::navigate(int Y)
    {
        if (!map_ || !editors_[active_]) return;
        const auto area = track(active_);
        const auto position = std::clamp(Y - area.top(), 0, area.height() - 1);
        const auto row = std::intptr_t(std::size_t(position) * (raster_.rows_[active_] - 1) / std::size_t(std::max(1, area.height() - 1)));
        scrollTo(row - editors_[active_]->send(SCI_LINESONSCREEN) / 2);
    }
    void OverviewBar::mousePressEvent(QMouseEvent* Event)
    {
        if (Event->button() != Qt::LeftButton) { QWidget::mousePressEvent(Event); return; }
        active_ = Event->position().x() < width() / 2 ? 0 : 1; setFocus(Qt::MouseFocusReason);
        if (Event->position().y() >= track(active_).top()) navigate(int(Event->position().y()));
        update(); Event->accept();
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
        case Qt::Key_Left: active_ = 0; update(); break;
        case Qt::Key_Right: active_ = 1; update(); break;
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
