#include "stdafx.h"
#include "scrollSynchronizer.hpp"
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
    void ScrollSynchronizer::SetMap(std::shared_ptr<const AlignmentMap> Map) { map_ = std::move(Map); remember(); }
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
            const auto line = editors_[Side]->send(SCI_DOCLINEFROMVISIBLE, uptr_t(current.top_));
            move(1 - Side, map_->MapLine(Side, std::size_t(std::max<sptr_t>(0, line))));
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
        remember(); applying_ = false;
    }
}
