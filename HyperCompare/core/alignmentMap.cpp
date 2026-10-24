#include "stdafx.h"
#include "alignmentMap.hpp"
#include <algorithm>

namespace hc::compare
{
    MultiAlignmentMap MultiAlignmentMap::Build(const std::vector<std::string_view>& Texts, const Budget& Limits)
    {
        Limits.Check(); if (Texts.size() < 2 || Texts.size() > 4) throw Failure("InvalidAlignmentSides");
        std::uint64_t lineCount = 0, textBytes = 0;
        for (const auto text : Texts) { lineCount += CountLines(text, Limits); textBytes += text.size(); }
        // Retained pair edits, line indexes, run-vector growth and per-side run locations.
        const auto estimate = lineCount * (sizeof(std::string_view) + 2 * sizeof(Edit)
            + 2 * (sizeof(MultiAlignmentRun) + Texts.size() * 3 * sizeof(std::size_t))) + textBytes * 2;
        const auto reservation = Limits.memoryReserve_ ? Limits.memoryReserve_(estimate) : std::shared_ptr<void>{};
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
    MultiAlignmentMap MultiAlignmentMap::WithSimilarLines(const std::vector<std::string_view>& Texts, const Budget& Limits) const
    {
        if (Texts.size() != lines_.size()) throw Failure("InvalidAlignmentSides");
        try
        {
            auto budget = Limits; budget.steps_ = std::min<std::uint64_t>(budget.steps_, 4'000'000);
            std::vector<Lines> lines;
            for (std::size_t side = 0; side < Texts.size(); ++side)
            {
                lines.push_back(SplitLines(Texts[side], budget));
                if (lines.back().size() != lines_[side]) throw Failure("InvalidAlignmentInput");
            }
            MultiAlignmentMap result; result.lines_ = lines_;
            const auto span = [&](std::size_t Side, std::size_t Begin, std::size_t End) -> std::string_view {
                if (Begin == End) return {};
                const auto first = lines[Side][Begin], last = lines[Side][End - 1];
                return {first.data(), std::size_t(last.data() + last.size() - first.data())};
            };
            const auto append = [&](MultiAlignmentRun Run) {
                std::size_t height = 0; Run.changedPairs_ = 0;
                for (std::size_t a = 0; a < Texts.size(); ++a)
                {
                    height = std::max(height, Run.end_[a] - Run.begin_[a]);
                    for (std::size_t b = a + 1; b < Texts.size(); ++b)
                        if (span(a, Run.begin_[a], Run.end_[a]) != span(b, Run.begin_[b], Run.end_[b]))
                            Run.changedPairs_ |= (1u << (a * 4 + b)) | (1u << (b * 4 + a));
                }
                if (!height) return;
                Run.rowBegin_ = result.rows_; result.rows_ += height; Run.rowEnd_ = result.rows_;
                result.runs_.push_back(std::move(Run));
            };
            for (const auto& run : runs_)
            {
                budget.Check(); const auto n = run.end_[0] - run.begin_[0];
                bool bounded = run.changedPairs_ && n && budget.steps_;
                for (std::size_t side = 0; side < Texts.size(); ++side) bounded &= run.end_[side] - run.begin_[side] <= 128;
                if (bounded) for (std::size_t side = 0; side < Texts.size(); ++side)
                    for (auto i = run.begin_[side]; i < run.end_[side]; ++i) bounded &= lines[side][i].size() <= 2048;
                if (!bounded) { auto copy = run; copy.rowBegin_ = result.rows_; result.rows_ += run.rowEnd_ - run.rowBegin_; copy.rowEnd_ = result.rows_; result.runs_.push_back(std::move(copy)); continue; }
                // Each side contributes insertions before an anchor line and at most
                // one corresponding line. Union these slots across all panes.
                std::vector<std::vector<std::size_t>> gaps(Texts.size(), std::vector<std::size_t>(n + 1));
                std::vector<std::vector<std::size_t>> matched(Texts.size(), std::vector<std::size_t>(n));
                std::fill(matched[0].begin(), matched[0].end(), 1); bool refined = false;
                for (std::size_t side = 1; side < Texts.size(); ++side)
                {
                    const auto m = run.end_[side] - run.begin_[side];
                    const auto pair = AlignmentMap::Build(n, m, {{0, n, 0, m, {}}}, budget).WithSimilarLines(
                        span(0, run.begin_[0], run.end_[0]), span(side, run.begin_[side], run.end_[side]), budget);
                    budget.steps_ -= pair.SimilarityWork(); refined |= pair.HasSimilarLines();
                    for (const auto& part : pair.Runs())
                    {
                        const auto a = part.end_[0] - part.begin_[0], b = part.end_[1] - part.begin_[1];
                        const auto common = std::min(a, b);
                        for (std::size_t i = 0; i < common; ++i) matched[side][part.begin_[0] + i] = 1;
                        gaps[side][part.end_[0]] += b - common;
                    }
                }
                if (!refined) { append(run); continue; }
                auto cursor = run.begin_;
                for (std::size_t i = 0; i <= n; ++i)
                {
                    budget.Check(); MultiAlignmentRun gap; gap.begin_ = cursor; gap.end_ = cursor;
                    for (std::size_t side = 0; side < Texts.size(); ++side) gap.end_[side] += gaps[side][i];
                    cursor = gap.end_; append(std::move(gap));
                    if (i == n) break;
                    MultiAlignmentRun row; row.begin_ = cursor; row.end_ = cursor;
                    for (std::size_t side = 0; side < Texts.size(); ++side) row.end_[side] += matched[side][i];
                    cursor = row.end_; append(std::move(row));
                }
                if (cursor != run.end_) throw Failure("InvalidAlignmentEdits");
            }
            budget.Check(); return result;
        }
        catch (const Failure& error) { if (error.code_ != "Timeout") throw; return *this; }
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
    AlignmentMap AlignmentMap::WithSimilarLines(std::string_view Left, std::string_view Right, const Budget& Limits) const
    {
        AlignmentMap result = *this; result.runs_.clear(); result.rows_ = 0; result.similarityWork_ = 0;
        const auto append = [&](std::size_t A, std::size_t EndA, std::size_t B, std::size_t EndB, bool Changed) {
            const auto height = std::max(EndA - A, EndB - B); if (!height) return;
            result.runs_.push_back({{A, B}, {EndA, EndB}, Changed, result.rows_, result.rows_ + height}); result.rows_ += height;
        };
        try
        {
            const auto left = SplitLines(Left, Limits), right = SplitLines(Right, Limits);
            if (left.size() != lines_[0] || right.size() != lines_[1]) throw Failure("InvalidAlignmentInput");
            const auto trim = [](std::string_view Text) {
                const auto space = [](char C) { return C == ' ' || C == '\t' || C == '\r' || C == '\n'; };
                while (!Text.empty() && space(Text.front())) Text.remove_prefix(1);
                while (!Text.empty() && space(Text.back())) Text.remove_suffix(1);
                return Text;
            };
            const auto maximum = std::min<std::uint64_t>(4'000'000, Limits.steps_);
            for (const auto& run : runs_)
            {
                Limits.Check(); const auto n = run.end_[0] - run.begin_[0], m = run.end_[1] - run.begin_[1];
                std::vector<std::pair<std::size_t, std::size_t>> candidates;
                if (run.changed_ && n && m && n <= 128 && m <= 128 && result.similarityWork_ < maximum)
                {
                    std::vector<std::string_view> a, b; std::uint64_t bytesA = 0, bytesB = 0; bool bounded = true;
                    for (std::size_t i = 0; i < n; ++i) { a.push_back(trim(left[run.begin_[0] + i])); bytesA += a.back().size(); bounded &= a.back().size() <= 2048; }
                    for (std::size_t j = 0; j < m; ++j) { b.push_back(trim(right[run.begin_[1] + j])); bytesB += b.back().size(); bounded &= b.back().size() <= 2048; }
                    const auto work = bytesA * m + bytesB * n + n * m;
                    if (bounded && work <= maximum - result.similarityWork_)
                    {
                        result.similarityWork_ += work;
                        std::vector<unsigned> rowBest(n), rowSecond(n), colBest(m), colSecond(m);
                        std::vector<std::size_t> rowChoice(n), colChoice(m);
                        const auto best = [](unsigned Score, std::size_t At, unsigned& First, unsigned& Second, std::size_t& Choice) {
                            if (Score > First) { Second = First; First = Score; Choice = At; } else Second = std::max(Second, Score);
                        };
                        for (std::size_t i = 0; i < n; ++i)
                        {
                            Limits.Check();
                            for (std::size_t j = 0; j < m; ++j)
                            {
                                std::size_t prefix = 0, suffix = 0;
                                while (prefix < a[i].size() && prefix < b[j].size() && a[i][prefix] == b[j][prefix]) ++prefix;
                                while (suffix < a[i].size() - prefix && suffix < b[j].size() - prefix &&
                                    a[i][a[i].size() - suffix - 1] == b[j][b[j].size() - suffix - 1]) ++suffix;
                                const auto common = prefix + suffix;
                                const unsigned score = common >= 4 ? unsigned(2000 * common / (a[i].size() + b[j].size())) : 0;
                                best(score, j, rowBest[i], rowSecond[i], rowChoice[i]); best(score, i, colBest[j], colSecond[j], colChoice[j]);
                            }
                        }
                        for (std::size_t i = 0; i < n; ++i)
                        {
                            const auto j = rowChoice[i];
                            if (rowBest[i] >= 700 && rowBest[i] >= rowSecond[i] + 80 && colChoice[j] == i && colBest[j] >= colSecond[j] + 80)
                                candidates.emplace_back(i, j);
                        }
                    }
                }
                // Unique mutual best matches, in monotone order. Repeated or weak
                // candidates retain the established ordinal fallback.
                std::vector<std::size_t> tails; std::vector<int> previous(candidates.size(), -1);
                for (std::size_t i = 0; i < candidates.size(); ++i)
                {
                    const auto at = std::lower_bound(tails.begin(), tails.end(), candidates[i].second,
                        [&](std::size_t Index, std::size_t Value) { return candidates[Index].second < Value; });
                    const auto position = std::size_t(at - tails.begin()); if (position) previous[i] = int(tails[position - 1]);
                    if (at == tails.end()) tails.push_back(i); else *at = i;
                }
                std::vector<std::pair<std::size_t, std::size_t>> anchors;
                for (int i = tails.empty() ? -1 : int(tails.back()); i >= 0; i = previous[std::size_t(i)]) anchors.push_back(candidates[std::size_t(i)]);
                std::reverse(anchors.begin(), anchors.end());
                if (std::none_of(anchors.begin(), anchors.end(), [](const auto& Pair) { return Pair.first != Pair.second; }))
                { append(run.begin_[0], run.end_[0], run.begin_[1], run.end_[1], run.changed_); continue; }
                result.similar_ = true; auto a = run.begin_[0], b = run.begin_[1];
                for (const auto& [i, j] : anchors)
                {
                    const auto endA = run.begin_[0] + i, endB = run.begin_[1] + j;
                    append(a, endA, b, endB, true); append(endA, endA + 1, endB, endB + 1, true); a = endA + 1; b = endB + 1;
                }
                append(a, run.end_[0], b, run.end_[1], true);
            }
            Limits.Check(); return result;
        }
        catch (const Failure& error)
        {
            if (error.code_ != "Timeout") throw;
            auto fallback = *this; fallback.similarityWork_ = result.similarityWork_; return fallback;
        }
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
