#include "stdafx.h"
#include "overviewMap.hpp"

namespace hc::compare
{
    OverviewRaster BuildOverviewRaster(const AlignmentMap& Map, std::size_t Pixels, bool Aligned)
    {
        OverviewRaster result; Pixels = std::min<std::size_t>(Pixels, 4096);
        for (unsigned side = 0; side < 2; ++side)
        {
            const auto rows = (Aligned ? Map.RowCount() : Map.LineCount(side)) + 1;
            result.rows_[side] = rows; result.bands_[side].resize(Pixels);
            if (!Pixels) continue;
            std::vector<std::array<int, 4>> events(Pixels + 1);
            const auto mark = [&](std::size_t Begin, std::size_t End, unsigned Kind) {
                const auto first = std::min(Pixels - 1, Begin * Pixels / rows);
                const auto last = std::min(Pixels, std::max(first + 1, (End * Pixels + rows - 1) / rows));
                ++events[first][Kind]; --events[last][Kind];
            };
            for (const auto& run : Map.Runs())
            {
                if (!run.changed_) continue;
                const auto count = run.end_[side] - run.begin_[side];
                const auto other = run.end_[1 - side] - run.begin_[1 - side];
                const auto begin = Aligned ? run.rowBegin_ : run.begin_[side];
                if (count) mark(begin, begin + count, other ? 0 : side == 0 ? 2 : 1);
                if (count < other) mark(begin + count, Aligned ? run.rowEnd_ : begin + count, 3);
            }
            std::array<int, 4> counts{};
            for (std::size_t pixel = 0; pixel < Pixels; ++pixel)
                for (unsigned kind = 0; kind < 4; ++kind)
                {
                    counts[kind] += events[pixel][kind];
                    if (counts[kind]) result.bands_[side][pixel] |= std::uint8_t(1 << kind);
                }
        }
        return result;
    }
}
