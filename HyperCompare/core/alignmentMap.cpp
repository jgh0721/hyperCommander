#include "stdafx.h"
#include "alignmentMap.hpp"
#include <algorithm>

namespace hc::compare
{
    AlignmentMap AlignmentMap::Build(std::size_t LeftLines, std::size_t RightLines,
        const std::vector<Edit>& Edits, const Budget& Limits)
    {
        Limits.Check();
        if (LeftLines > 2'000'000 || RightLines > 2'000'000 || Edits.size() > LeftLines + RightLines) throw Failure("AlignmentSizeLimit");
        AlignmentMap map; map.lines_ = {LeftLines, RightLines};
        std::size_t left = 0, right = 0;
        for (const auto& edit : Edits)
        {
            Limits.Check();
            if (edit.begin_ < left || edit.otherBegin_ < right || edit.end_ < edit.begin_ || edit.otherEnd_ < edit.otherBegin_ ||
                edit.end_ > LeftLines || edit.otherEnd_ > RightLines || edit.begin_ - left != edit.otherBegin_ - right ||
                (edit.begin_ == edit.end_ && edit.otherBegin_ == edit.otherEnd_)) throw Failure("InvalidAlignmentEdits");
            if (edit.begin_ > left) map.runs_.push_back({{left, right}, {edit.begin_, edit.otherBegin_}, false});
            map.runs_.push_back({{edit.begin_, edit.otherBegin_}, {edit.end_, edit.otherEnd_}, true});
            left = edit.end_; right = edit.otherEnd_;
        }
        if (LeftLines - left != RightLines - right) throw Failure("InvalidAlignmentEdits");
        if (LeftLines > left) map.runs_.push_back({{left, right}, {LeftLines, RightLines}, false});
        for (auto& run : map.runs_)
        {
            Limits.Check(); run.rowBegin_ = map.rows_;
            map.rows_ += std::max(run.end_[0] - run.begin_[0], run.end_[1] - run.begin_[1]);
            run.rowEnd_ = map.rows_;
        }
        Limits.Check(); return map;
    }
    std::size_t AlignmentMap::DisplayRow(unsigned Side, std::size_t Line) const
    {
        if (Side > 1) throw Failure("InvalidAlignmentSide");
        if (Line > lines_[Side]) throw Failure("InvalidAlignmentLine");
        if (Line == lines_[Side]) return rows_;
        const auto run = std::upper_bound(runs_.begin(), runs_.end(), Line,
            [Side](std::size_t At, const AlignmentRun& Run) { return At < Run.end_[Side]; });
        return run->rowBegin_ + Line - run->begin_[Side];
    }
    std::optional<std::size_t> AlignmentMap::LineAtRow(unsigned Side, std::size_t Row) const
    {
        if (Side > 1) throw Failure("InvalidAlignmentSide");
        if (Row >= rows_) return std::nullopt;
        const auto run = std::upper_bound(runs_.begin(), runs_.end(), Row,
            [](std::size_t At, const AlignmentRun& Run) { return At < Run.rowEnd_; });
        const auto offset = Row - run->rowBegin_;
        if (offset >= run->end_[Side] - run->begin_[Side]) return std::nullopt;
        return run->begin_[Side] + offset;
    }
    std::size_t AlignmentMap::MapLine(unsigned FromSide, std::size_t Line) const
    {
        if (FromSide > 1) throw Failure("InvalidAlignmentSide");
        const auto to = 1 - FromSide;
        // EOF is a boundary, including the editor's trailing empty line after EOL.
        if (Line >= lines_[FromSide]) return lines_[to];
        const auto run = std::upper_bound(runs_.begin(), runs_.end(), Line,
            [FromSide](std::size_t At, const AlignmentRun& Run) { return At < Run.end_[FromSide]; });
        if (run == runs_.end()) throw Failure("InvalidAlignmentEdits");
        const auto count = run->end_[to] - run->begin_[to], offset = Line - run->begin_[FromSide];
        if (!count) return run->begin_[to];
        // Inside changed spans this is a monotone navigation estimate, not a
        // claim of semantic equality. Unchanged spans map exactly line-for-line.
        return run->begin_[to] + std::size_t(std::uint64_t(offset) * count / (run->end_[FromSide] - run->begin_[FromSide]));
    }
}
