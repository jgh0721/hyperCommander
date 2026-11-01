#include "stdafx.h"
#include "overviewMap.hpp"
#include <QString>
#include <QList>

namespace hc::compare
{
    OverviewStroke OverviewDocument::At(std::size_t Line) const
    {
        if (Line >= lines_ || strokes_.empty()) return {};
        return strokes_[std::min(strokes_.size() - 1, Line * strokes_.size() / lines_)];
    }
    OverviewDocument BuildOverviewDocument(std::string_view Text, const Budget& Limits)
    {
        const auto Lines = SplitLines(Text, Limits);
        OverviewDocument Result; Result.lines_ = Lines.size();
        const auto Count = std::min<std::size_t>(4096, Lines.size()); Result.strokes_.resize(Count);
        for (std::size_t Bin = 0; Bin < Count; ++Bin)
        {
            if (!(Bin % 64)) Limits.Check();
            const auto Line = Lines[Bin * Lines.size() / Count];
            // Bound decoding/column work even for a very long single line.
            const auto Prefix = QString::fromUtf8(Line.data(), qsizetype(std::min<std::size_t>(512, Line.size()))).toUcs4();
            unsigned Column = 0, Indent = 0, End = 0; bool Content = false;
            for (const auto Code : Prefix)
            {
                if (Code == '\r' || Code == '\n' || Column >= 128) break;
                if (Code == '\t') Column = std::min(128u, (Column / 8 + 1) * 8);
                else
                {
                    const auto Category = QChar::category(Code);
                    if (Category == QChar::Mark_NonSpacing || Category == QChar::Mark_Enclosing || Code == 0x200d) continue;
                    const bool Wide = (Code >= 0x1100 && Code <= 0x115f) || (Code >= 0x2e80 && Code <= 0xa4cf) ||
                        (Code >= 0xac00 && Code <= 0xd7a3) || (Code >= 0xf900 && Code <= 0xfaff) ||
                        (Code >= 0xfe10 && Code <= 0xfe6f) || (Code >= 0xff01 && Code <= 0xff60) ||
                        (Code >= 0xffe0 && Code <= 0xffe6) || Code >= 0x1f300;
                    Column = std::min(128u, Column + (Wide ? 2 : 1));
                }
                if (!Content && QChar::isSpace(Code)) Indent = Column;
                else if (!QChar::isSpace(Code)) { Content = true; End = Column; }
            }
            if (Content) Result.strokes_[Bin] = {std::uint16_t(Indent), std::uint16_t(End)};
        }
        Limits.Check(); return Result;
    }
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
