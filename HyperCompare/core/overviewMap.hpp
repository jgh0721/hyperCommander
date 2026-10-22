#pragma once
#include "alignmentMap.hpp"

namespace hc::compare
{
    enum OverviewMark : std::uint8_t { Modified = 1, Added = 2, Removed = 4, Gap = 8 };
    struct OverviewRaster
    {
        std::array<std::vector<std::uint8_t>, 2> bands_;
        // Include an EOF boundary row, even for an empty/no-final-EOL document.
        std::array<std::size_t, 2> rows_{1, 1};
    };
    // O(runs + pixels) time, O(pixels) storage, capped at 4096 bins per side.
    // Overlapping changes retain every kind's bit, including zero-length anchors.
    OverviewRaster BuildOverviewRaster(const AlignmentMap& Map, std::size_t Pixels, bool Aligned);
}
