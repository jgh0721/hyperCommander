#include "stdafx.h"
#include "chunkGutter.hpp"
#include <ScintillaEditBase.h>
#include <QPainter>
#include <QPainterPath>
#include <QToolButton>
#include <QMenu>
#include <QTimer>
#include <QEvent>
#include <set>

namespace hc::compare
{
    ChunkGutter::ChunkGutter(ScintillaEditBase* Left, ScintillaEditBase* Right, int SourceSide, QWidget* Parent)
        : QWidget(Parent), editors_{Left, Right}, sourceSide_(SourceSide)
    {
        setObjectName("chunkGutter"); setFixedWidth(84); setFocusPolicy(Qt::StrongFocus);
        setAccessibleName(tr("청크 병합 조작"));
        for (auto* editor : editors_)
        {
            editor->installEventFilter(this);
            connect(editor, &ScintillaEditBase::updateUi, this, [this] { Refresh(); });
            connect(editor, &ScintillaEditBase::verticalScrolled, this, [this] { Refresh(); });
        }
    }
    void ChunkGutter::SetChunks(std::vector<GutterChunk> Chunks, std::uint64_t Revision)
    {
        if (Revision == revision_ && Chunks == chunks_) { Refresh(); return; }
        // Hide old controls immediately so queued painting cannot apply stale IDs.
        for (auto& row : buttons_) for (auto* button : row) button->hide();
        chunks_ = std::move(Chunks); revision_ = Revision; Refresh();
    }
    void ChunkGutter::Refresh()
    {
        if (queued_) return; queued_ = true;
        QTimer::singleShot(0, this, [this] { queued_ = false; arrange(); update(); });
    }
    bool ChunkGutter::eventFilter(QObject*, QEvent* Event)
    {
        if (Event->type() == QEvent::Resize || Event->type() == QEvent::Move || Event->type() == QEvent::Show || Event->type() == QEvent::FontChange) Refresh();
        return false;
    }
    void ChunkGutter::resizeEvent(QResizeEvent* Event) { QWidget::resizeEvent(Event); Refresh(); }
    void ChunkGutter::changeEvent(QEvent* Event) { QWidget::changeEvent(Event); Refresh(); }
    int ChunkGutter::y(unsigned Side, std::size_t Position) const
    {
        auto* editor = editors_[Side];
        const auto length = std::size_t(editor->send(SCI_GETLENGTH));
        return mapFromGlobal(editor->mapToGlobal(QPoint(0, 0))).y() + int(editor->send(SCI_POINTYFROMPOSITION, 0, sptr_t(std::min(Position, length))));
    }
    void ChunkGutter::arrange()
    {
        const int cell = std::max(26, fontMetrics().height() + 8);
        // Keep a visible connector lane beside the controls, even for one-line chunks.
        setFixedWidth(3 * (cell + 1) + 27);
        rows_.clear(); std::set<std::size_t> candidates;
        // Ranges are monotonic on both sides. Binary search avoids scanning every
        // chunk on each scroll, including files with tens of thousands of changes.
        for (unsigned side = 0; side < 2; ++side)
        {
            auto* editor = editors_[side];
            const auto first = editor->send(SCI_DOCLINEFROMVISIBLE, editor->send(SCI_GETFIRSTVISIBLELINE));
            const auto last = editor->send(SCI_DOCLINEFROMVISIBLE, editor->send(SCI_GETFIRSTVISIBLELINE) + editor->send(SCI_LINESONSCREEN) + 1);
            const auto begin = std::size_t(std::max<sptr_t>(0, editor->send(SCI_POSITIONFROMLINE, first)));
            const auto rawEnd = editor->send(SCI_GETLINEENDPOSITION, last);
            const auto end = std::size_t(rawEnd < 0 ? editor->send(SCI_GETLENGTH) : rawEnd);
            auto it = std::lower_bound(chunks_.begin(), chunks_.end(), begin, [side](const auto& Chunk, std::size_t At) { return Chunk.end_[side] < At; });
            for (; it != chunks_.end() && it->begin_[side] <= end && candidates.size() < 256; ++it)
                candidates.insert(std::size_t(it - chunks_.begin()));
        }
        for (const auto index : candidates)
        {
            const auto& chunk = chunks_[index];
            const auto bottom = [&](unsigned side) {
                return chunk.end_[side] == chunk.begin_[side] ? y(side, chunk.begin_[side]) + 2 :
                    y(side, chunk.end_[side] - 1) + int(editors_[side]->send(SCI_TEXTHEIGHT, 0));
            };
            rows_.push_back({index, y(0, chunk.begin_[0]), bottom(0), y(1, chunk.begin_[1]), bottom(1)});
        }
        std::size_t used = 0; int previous = -100;
        for (const auto& row : rows_)
        {
            const auto& chunk = chunks_[row.index_];
            const unsigned anchor = sourceSide_ == 1 ? 1 : 0;
            const int viewTop = mapFromGlobal(editors_[anchor]->mapToGlobal(QPoint{})).y();
            const int viewBottom = std::min(height(), viewTop + editors_[anchor]->height());
            const int top = anchor == 0 ? row.leftTop_ : row.rightTop_;
            const int bottom = anchor == 0 ? row.leftBottom_ : row.rightBottom_;
            if (bottom <= viewTop || top >= viewBottom || viewBottom - viewTop < cell) continue;
            const int at = std::clamp(top, viewTop, viewBottom - cell);
            if (at < previous + cell + 1) continue; previous = at;
            if (used == buttons_.size())
            {
                std::array<QToolButton*, 3> group{};
                for (int j = 0; j < 3; ++j)
                {
                    auto* button = new QToolButton(this); group[j] = button;
                    button->setFocusPolicy(Qt::StrongFocus); button->setAutoRaise(false);
                    connect(button, &QToolButton::clicked, this, [this, button, j] {
                        const auto id = button->property("chunkId").toULongLong();
                        const auto revision = button->property("revision").toULongLong();
                        if (j == 2 && sourceSide_ >= 0)
                        {
                            QMenu menu(this);
                            for (const auto& entry : {std::pair{tr("LEFT 다음 RIGHT"), Resolution::LeftThenRight}, std::pair{tr("RIGHT 다음 LEFT"), Resolution::RightThenLeft}})
                                menu.addAction(entry.first, this, [this, id, revision, choice = entry.second] { if (choose_) choose_(id, revision, choice); });
                            menu.exec(button->mapToGlobal(QPoint(0, button->height()))); return;
                        }
                        const auto choice = sourceSide_ < 0 ? (j == 0 ? Resolution::Left : Resolution::Right) :
                            j == 1 ? Resolution::Omit : sourceSide_ == 0 ? Resolution::Left : Resolution::Right;
                        if (choose_) choose_(id, revision, choice);
                    });
                }
                buttons_.push_back(group);
            }
            auto& group = buttons_[used++];
            for (int j = 0; j < 3; ++j)
            {
                auto* button = group[j];
                if (button->property("chunkId").toULongLong() != chunk.id_)
                {
                    button->setDown(false);
                    if (button->hasFocus()) setFocus(Qt::OtherFocusReason);
                }
                button->setProperty("chunkId", qulonglong(chunk.id_)); button->setProperty("revision", qulonglong(revision_));
                button->setObjectName(QString("chunk%1_action%2").arg(chunk.id_).arg(j));
                const auto symbol = sourceSide_ < 0 ? (j == 0 ? ">" : "<") : j == 0 ? (sourceSide_ == 0 ? ">" : "<") : j == 1 ? "×" : "⋮";
                const auto purpose = sourceSide_ < 0 ? (j == 0 ? tr("왼쪽 청크를 오른쪽 문서에 반영 (저장은 별도)") : tr("오른쪽 청크를 왼쪽 문서에 반영 (저장은 별도)")) :
                    j == 0 ? tr("%1 청크를 RESULT에 적용").arg(sourceSide_ == 0 ? "LEFT" : "RIGHT") : j == 1 ? tr("RESULT 청크 제거 (실행 취소 가능)") : tr("양쪽 청크 결합");
                button->setText(QString::fromUtf8(symbol)); button->setToolTip(tr("청크 %1: %2").arg(chunk.id_).arg(purpose)); button->setAccessibleName(button->toolTip());
                button->setGeometry((sourceSide_ == 1 ? 26 : 2) + j * (cell + 1), at, cell, cell); button->setEnabled(chunk.enabled_);
                button->setVisible(sourceSide_ >= 0 || j < 2);
            }
        }
        for (; used < buttons_.size(); ++used) for (auto* button : buttons_[used]) button->hide();
    }
    void ChunkGutter::paintEvent(QPaintEvent*)
    {
        QPainter painter(this); painter.fillRect(rect(), palette().window()); painter.setRenderHint(QPainter::Antialiasing);
        const int top = std::max(mapFromGlobal(editors_[0]->mapToGlobal(QPoint{})).y(), mapFromGlobal(editors_[1]->mapToGlobal(QPoint{})).y());
        painter.setClipRect(QRect(0, top, width(), std::max(0, height() - top)));
        for (const auto& row : rows_)
        {
            const auto& chunk = chunks_[row.index_];
            const auto clip = [this](int Value) { return std::clamp(Value, -100, height() + 100); };
            QPainterPath path; path.moveTo(0, clip(row.leftTop_));
            path.cubicTo(width() / 2, clip(row.leftTop_), width() / 2, clip(row.rightTop_), width(), clip(row.rightTop_));
            path.lineTo(width(), clip(row.rightBottom_));
            path.cubicTo(width() / 2, clip(row.rightBottom_), width() / 2, clip(row.leftBottom_), 0, clip(row.leftBottom_)); path.closeSubpath();
            auto color = palette().highlight().color(); color.setAlpha(chunk.resolved_ ? 25 : 65);
            auto outline = palette().windowText().color(); outline.setAlpha(130);
            painter.fillPath(path, color); painter.setPen(QPen(outline, 1)); painter.drawPath(path);
        }
    }
}
