#include "stdafx.h"
#include "scrollSynchronizer.hpp"
#include "scintillaPadding.hpp"
#include <ScintillaEditBase.h>
#include <algorithm>

namespace hc::compare
{
    ScrollSynchronizer::ScrollSynchronizer(ScintillaEditBase* Left, ScintillaEditBase* Right,
        std::shared_ptr<const AlignmentMap> Map, QObject* Parent)
        : QObject(Parent), editors_{Left, Right}, map_(std::move(Map))
    {
        for (unsigned side = 0; side < 2; ++side)
        {
            // Shorter documents can keep their corresponding EOF at the top.
            editors_[side]->send(SCI_SETENDATLASTLINE, 0);
            // Observe positions immediately so rapid input in the other pane wins
            // before a previously queued paint/update notification is delivered.
            connect(editors_[side], &ScintillaEditBase::verticalScrolled, this, [this, side](int) { changed(side); });
            connect(editors_[side], &ScintillaEditBase::horizontalScrolled, this, [this, side](int) { changed(side); });
            connect(editors_[side], &ScintillaEditBase::updateUi, this, [this, side](Scintilla::Update Updated) {
                if (int(Updated) & (SC_UPDATE_V_SCROLL | SC_UPDATE_H_SCROLL)) changed(side);
            });
        }
        remember();
    }
    ScrollSynchronizer::View ScrollSynchronizer::view(unsigned Side) const
    { return {editors_[Side]->send(SCI_GETFIRSTVISIBLELINE), editors_[Side]->send(SCI_GETXOFFSET)}; }
    void ScrollSynchronizer::remember() { for (unsigned side = 0; side < 2; ++side) known_[side] = view(side); }
    void ScrollSynchronizer::ClearPadding()
    {
        applying_ = true;
        for (auto* editor : editors_) editor->send(ClearPaddingRows);
        remember(); applying_ = false;
    }
    void ScrollSynchronizer::applyPadding()
    {
        if (!map_) return;
        for (unsigned side = 0; side < 2; ++side)
        {
            auto* editor = editors_[side]; std::size_t leading = 0;
            // Coalesce gaps at the same boundary without allocating per-row data.
            std::vector<std::pair<std::size_t, std::size_t>> gaps;
            for (const auto& run : map_->Runs())
            {
                const auto count = run.rowEnd_ - run.rowBegin_ - (run.end_[side] - run.begin_[side]);
                if (!count) continue;
                const auto end = run.end_[side];
                if (!end) leading += count;
                else if (!gaps.empty() && gaps.back().first == end - 1) gaps.back().second += count;
                else gaps.emplace_back(end - 1, count);
            }
            // Scintilla has a final empty document line only after EOL (and for
            // empty files). Give other files one virtual EOF row as well.
            const auto lines = map_->LineCount(side);
            if (lines && std::size_t(editor->send(SCI_GETLINECOUNT)) == lines)
            {
                if (!gaps.empty() && gaps.back().first == lines - 1) ++gaps.back().second;
                else gaps.emplace_back(lines - 1, 1);
            }
            for (const auto& [line, count] : gaps) editor->send(SetTrailingRows, line, sptr_t(count));
            editor->send(SetLeadingRows, leading);
        }
    }
    void ScrollSynchronizer::SetAligned(bool Aligned)
    {
        if (aligned_ == Aligned) return;
        std::array<std::size_t, 2> lines{};
        for (unsigned side = 0; side < 2; ++side)
            lines[side] = std::size_t(editors_[side]->send(SCI_DOCLINEFROMVISIBLE, uptr_t(view(side).top_)));
        ClearPadding(); applying_ = true; aligned_ = Aligned;
        if (aligned_) applyPadding();
        for (unsigned side = 0; side < 2; ++side) move(side, lines[side]);
        remember(); applying_ = false;
        if (enabled_) Synchronize(0);
    }
    void ScrollSynchronizer::SetMap(std::shared_ptr<const AlignmentMap> Map)
    {
        ClearPadding(); applying_ = true; map_ = std::move(Map);
        if (aligned_) applyPadding();
        for (auto* editor : editors_) editor->send(SCI_SETFIRSTVISIBLELINE, 0);
        remember(); applying_ = false;
    }
    void ScrollSynchronizer::SetEnabled(bool Enabled)
    { if (enabled_ != Enabled) { enabled_ = Enabled; remember(); } }
    void ScrollSynchronizer::move(unsigned Side, std::size_t Line)
    {
        const auto last = std::size_t(std::max<sptr_t>(0, editors_[Side]->send(SCI_GETLINECOUNT) - 1));
        const auto visible = editors_[Side]->send(SCI_VISIBLEFROMDOCLINE, std::min(Line, last));
        editors_[Side]->send(SCI_SETFIRSTVISIBLELINE, uptr_t(std::max<sptr_t>(0, visible)));
    }
    void ScrollSynchronizer::changed(unsigned Side)
    {
        if (applying_) return;
        const auto current = view(Side), previous = known_[Side]; known_[Side] = current;
        if (!enabled_ || !map_) return;
        const bool vertical = current.top_ != previous.top_, horizontal = current.x_ != previous.x_;
        if (!vertical && !horizontal) return;
        applying_ = true;
        if (vertical)
        {
            if (aligned_) editors_[1 - Side]->send(SCI_SETFIRSTVISIBLELINE, uptr_t(current.top_));
            else
            {
                const auto line = editors_[Side]->send(SCI_DOCLINEFROMVISIBLE, uptr_t(current.top_));
                move(1 - Side, map_->MapLine(Side, std::size_t(std::max<sptr_t>(0, line))));
            }
        }
        if (horizontal) editors_[1 - Side]->send(SCI_SETXOFFSET, uptr_t(current.x_));
        // SCN_UPDATEUI may arrive later during painting. Cache the actual target
        // positions so its delayed notification cannot bounce an approximate map.
        remember(); applying_ = false;
    }
    void ScrollSynchronizer::Synchronize(unsigned FromSide)
    {
        if (FromSide > 1 || !enabled_ || !map_) return;
        known_[FromSide] = {-1, -1}; changed(FromSide);
    }
    void ScrollSynchronizer::Reveal(std::size_t LeftLine, std::size_t RightLine)
    {
        applying_ = true;
        const std::array<std::size_t, 2> lines{LeftLine, RightLine};
        for (unsigned side = 0; side < 2; ++side)
        {
            editors_[side]->send(SCI_GOTOLINE, lines[side]); move(side, lines[side]);
        }
        if (aligned_ && map_)
        {
            // A zero-length insertion anchor maps to the following real line.
            // Show the earlier row so navigation includes its virtual gap.
            const auto row = std::min(map_->DisplayRow(0, std::min(LeftLine, map_->LineCount(0))),
                map_->DisplayRow(1, std::min(RightLine, map_->LineCount(1))));
            for (auto* editor : editors_) editor->send(SCI_SETFIRSTVISIBLELINE, row);
        }
        remember(); applying_ = false;
    }
}
