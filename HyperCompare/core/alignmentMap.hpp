#pragma once
#include "compare.hpp"
#include <array>
#include <optional>

namespace hc::compare
{
    struct AlignmentRun
    {
        std::array<std::size_t, 2> begin_, end_;
        bool changed_ = false;
        std::size_t rowBegin_ = 0, rowEnd_ = 0;
    };
    // Document correspondence and shared, unwrapped display-row coordinates.
    // Zero-length spans represent insertions/deletions; no document bytes are added.
    class AlignmentMap
    {
    public:
        static AlignmentMap Build(std::size_t LeftLines, std::size_t RightLines,
            const std::vector<Edit>& Edits, const Budget& Limits = {});
        std::size_t MapLine(unsigned FromSide, std::size_t Line) const;
        std::size_t DisplayRow(unsigned Side, std::size_t Line) const;
        std::optional<std::size_t> LineAtRow(unsigned Side, std::size_t Row) const;
        std::size_t RowCount() const { return rows_; }
        const std::vector<AlignmentRun>& Runs() const { return runs_; }
        std::size_t LineCount(unsigned Side) const { return lines_.at(Side); }
    private:
        std::array<std::size_t, 2> lines_{};
        std::vector<AlignmentRun> runs_;
        std::size_t rows_ = 0;
    };
}
