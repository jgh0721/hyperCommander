#include "stdafx.h"
#include "alignmentMap.hpp"
#include <algorithm>

namespace hc::compare
{
    MultiAlignmentMap MultiAlignmentMap::Build(const std::vector<std::string_view>& Texts, const Budget& Limits)
    {
        Limits.Check(); if (Texts.size() < 2 || Texts.size() > 4) throw Failure("InvalidAlignmentSides");
        MultiAlignmentMap map; std::vector<std::vector<Edit>> edits(Texts.size());
        std::vector<Lines> lines; lines.reserve(Texts.size());
        struct Change { std::size_t side_; const Edit* edit_; }; std::vector<Change> changes;
        for (std::size_t side = 0; side < Texts.size(); ++side)
        {
            lines.push_back(SplitLines(Texts[side], Limits)); map.lines_.push_back(lines.back().size());
            if (!side) continue;
            edits[side] = Diff(Texts[0], Texts[side], Limits);
            for (const auto& edit : edits[side]) changes.push_back({side, &edit});
        }
        std::size_t comparisons = 0;
        std::stable_sort(changes.begin(), changes.end(), [&](const auto& A, const auto& B) {
            if ((++comparisons & 1023) == 0) Limits.Check();
            return A.edit_->begin_ != B.edit_->begin_ ? A.edit_->begin_ < B.edit_->begin_ : A.edit_->end_ < B.edit_->end_;
        });
        std::vector<std::size_t> cursor(Texts.size(), 0);
        const auto append = [&](const std::vector<std::size_t>& Counts, bool Changed) {
            Limits.Check(); MultiAlignmentRun run; run.begin_ = cursor; run.end_ = cursor;
            std::size_t height = 0;
            for (std::size_t side = 0; side < Counts.size(); ++side)
            {
                if (Counts[side] > map.lines_[side] - cursor[side]) throw Failure("InvalidAlignmentEdits");
                run.end_[side] += Counts[side]; height = std::max(height, Counts[side]);
            }
            if (!height) return;
            if (Changed)
            {
                std::array<std::string_view, 4> spans;
                for (std::size_t side = 0; side < Counts.size(); ++side)
                {
                    if (!Counts[side]) continue;
                    const auto first = lines[side][run.begin_[side]], last = lines[side][run.end_[side] - 1];
                    spans[side] = {first.data(), std::size_t(last.data() + last.size() - first.data())};
                }
                for (std::size_t a = 0; a < Counts.size(); ++a) for (std::size_t b = a + 1; b < Counts.size(); ++b)
                    if (spans[a] != spans[b]) run.changedPairs_ |= (1u << (a * 4 + b)) | (1u << (b * 4 + a));
            }
            run.rowBegin_ = map.rows_; map.rows_ += height; run.rowEnd_ = map.rows_; cursor = run.end_; map.runs_.push_back(std::move(run));
        };
        for (std::size_t i = 0; i < changes.size();)
        {
            Limits.Check(); const auto begin = changes[i].edit_->begin_; auto end = changes[i].edit_->end_; auto next = i + 1;
            while (next < changes.size())
            {
                const auto& edit = *changes[next].edit_;
                const bool overlap = begin == end && edit.begin_ == edit.end_ ? begin == edit.begin_ :
                    std::max(begin, edit.begin_) < std::min(end, edit.end_) || (edit.begin_ == edit.end_ && begin < edit.begin_ && edit.begin_ < end);
                if (!overlap) break;
                end = std::max(end, edit.end_); ++next;
            }
            if (begin < cursor[0]) throw Failure("InvalidAlignmentEdits");
            append(std::vector<std::size_t>(Texts.size(), begin - cursor[0]), false);
            std::vector<std::size_t> counts(Texts.size(), end - begin);
            for (auto j = i; j < next; ++j)
            {
                const auto& edit = *changes[j].edit_; auto& count = counts[changes[j].side_];
                const auto removed = edit.end_ - edit.begin_, added = edit.otherEnd_ - edit.otherBegin_;
                if (removed > count) throw Failure("InvalidAlignmentEdits"); count = count - removed + added;
            }
            append(counts, true); i = next;
        }
        append(std::vector<std::size_t>(Texts.size(), map.lines_[0] - cursor[0]), false);
        if (cursor != map.lines_) throw Failure("InvalidAlignmentEdits");
        Limits.Check(); return map;
    }
    AlignmentMap MultiAlignmentMap::Pair(std::size_t Left, std::size_t Right) const
    {
        if (Left == Right || Left >= lines_.size() || Right >= lines_.size()) throw Failure("InvalidAlignmentSide");
        AlignmentMap map; map.lines_ = {lines_[Left], lines_[Right]}; map.rows_ = rows_; map.runs_.reserve(runs_.size());
        for (const auto& run : runs_) map.runs_.push_back({{run.begin_[Left], run.begin_[Right]}, {run.end_[Left], run.end_[Right]}, (run.changedPairs_ & (1u << (Left * 4 + Right))) != 0, run.rowBegin_, run.rowEnd_});
        return map;
    }
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
