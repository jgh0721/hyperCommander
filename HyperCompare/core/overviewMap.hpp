#pragma once
#include "alignmentMap.hpp"

namespace hc::compare
{
    enum OverviewMark : std::uint8_t { Modified = 1, Added = 2, Removed = 4, Gap = 8 };
    struct OverviewStroke
    {
        std::uint16_t indent_ = 0, end_ = 0;
        bool operator==(const OverviewStroke&) const = default;
    };
    struct OverviewDocument
    {
        std::size_t lines_ = 0;
        // At most 4096 representative lines, in 128 display columns (Scintilla's default tab width).
        std::vector<OverviewStroke> strokes_;
        OverviewStroke At(std::size_t Line) const;
    };
    // Worker-only: reuse the bounded line scanner; never retain source text.
    OverviewDocument BuildOverviewDocument(std::string_view Text, const Budget& Limits = {});
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
