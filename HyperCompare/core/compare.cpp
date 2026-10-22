#include "stdafx.h"
#include "compare.hpp"

namespace hc::compare
{
    void Budget::Check() const
    {
        if (canceled_ && canceled_->load(std::memory_order_relaxed)) throw Failure("Canceled");
        if (std::chrono::steady_clock::now() >= deadline_) throw Failure("Timeout");
    }
    Lines SplitLines(std::string_view Text)
    {
        Lines result;
        std::size_t start = 0;
        for (std::size_t i = 0; i < Text.size(); ++i)
        {
            if (Text[i] != '\n' && Text[i] != '\r') continue;
            if (Text[i] == '\r' && i + 1 < Text.size() && Text[i + 1] == '\n') ++i;
            result.push_back(Text.substr(start, i + 1 - start)); start = i + 1;
        }
        if (start < Text.size()) result.push_back(Text.substr(start));
        return result;
    }
    namespace
    {
        void append(std::string& Out, const Lines& Text, std::size_t Begin, std::size_t End)
        { for (auto i = Begin; i < End; ++i) Out.append(Text.at(i)); }
        bool overlaps(const Edit& A, const Edit& B)
        {
            if (A.begin_ == A.end_ && B.begin_ == B.end_) return A.begin_ == B.begin_;
            if (A.begin_ == A.end_) return B.begin_ < A.begin_ && A.begin_ < B.end_;
            if (B.begin_ == B.end_) return A.begin_ < B.begin_ && B.begin_ < A.end_;
            return std::max(A.begin_, B.begin_) < std::min(A.end_, B.end_);
        }
        std::string variant(const Lines& Base, std::size_t Begin, std::size_t End, const std::vector<const Edit*>& Edits)
        {
            std::string result; auto cursor = Begin;
            for (const auto* edit : Edits)
            { append(result, Base, cursor, edit->begin_); result += edit->replacement_; cursor = edit->end_; }
            append(result, Base, cursor, End); return result;
        }
    }
    std::vector<Edit> Diff(std::string_view Left, std::string_view Right, const Budget& Limits)
    {
        Limits.Check(); if (Left == Right) return {};
        const auto left = SplitLines(Left), right = SplitLines(Right);
        if (left.size() + right.size() > 2'000'000) throw Failure("LineLimit");
        using Index = std::int64_t;
        const auto n = static_cast<Index>(left.size()), m = static_cast<Index>(right.size());
        std::vector<std::vector<Index>> trace;
        std::uint64_t bytes = 0, steps = 0;
        Index distance = 0; bool done = false;
        // Keep only reachable diagonals and reject trace growth before allocating.
        for (Index d = 0; d <= n + m && !done; ++d)
        {
            Limits.Check(); const auto cost = std::uint64_t(d + 1) * sizeof(Index);
            if (cost > Limits.traceBytes_ || bytes > Limits.traceBytes_ - cost) throw Failure("DiffMemoryLimit");
            bytes += cost;
            std::vector<Index> row(static_cast<std::size_t>(d + 1));
            for (Index i = 0; i <= d; ++i)
            {
                if (++steps > Limits.steps_) throw Failure("DiffWorkLimit");
                if ((steps & 1023) == 0) Limits.Check();
                const auto k = -d + 2 * i; Index x = 0;
                if (d)
                {
                    const auto& previous = trace.back();
                    if (i == 0 || (i != d && previous[std::size_t(i - 1)] < previous[std::size_t(i)])) x = previous[std::size_t(i)];
                    else x = previous[std::size_t(i - 1)] + 1;
                }
                auto y = x - k;
                while (x < n && y < m && x >= 0 && y >= 0 && left[std::size_t(x)] == right[std::size_t(y)])
                {
                    ++x; ++y;
                    if (++steps > Limits.steps_) throw Failure("DiffWorkLimit");
                    if ((steps & 1023) == 0) Limits.Check();
                }
                row[std::size_t(i)] = x;
                if (x >= n && y >= m) { distance = d; done = true; break; }
            }
            trace.push_back(std::move(row));
        }
        std::vector<char> operations; operations.reserve(static_cast<std::size_t>(n + m));
        auto x = n, y = m;
        for (auto d = distance; d > 0; --d)
        {
            Limits.Check(); const auto k = x - y, i = (k + d) / 2;
            const auto& previous = trace[std::size_t(d - 1)];
            const bool insert = i == 0 || (i != d && previous[std::size_t(i - 1)] < previous[std::size_t(i)]);
            const auto px = previous[std::size_t(insert ? i : i - 1)];
            const auto py = px - (k + (insert ? 1 : -1));
            while (x > px && y > py) { operations.push_back('='); --x; --y; }
            operations.push_back(insert ? '+' : '-'); x = px; y = py;
        }
        while (x > 0 && y > 0) { operations.push_back('='); --x; --y; }
        std::reverse(operations.begin(), operations.end());
        std::vector<Edit> edits; std::size_t a = 0, b = 0; bool open = false;
        for (const auto op : operations)
        {
            if (op == '=') { ++a; ++b; open = false; continue; }
            if (!open) { edits.push_back({a, a, b, b, {}}); open = true; }
            auto& edit = edits.back();
            if (op == '-') edit.end_ = ++a;
            else { edit.replacement_.append(right[b++]); edit.otherEnd_ = b; }
        }
        Limits.Check(); return edits;
    }
    std::string Apply(std::string_view Base, const std::vector<Edit>& Edits)
    {
        const auto lines = SplitLines(Base); std::string result; std::size_t cursor = 0;
        for (const auto& edit : Edits)
        {
            if (edit.begin_ < cursor || edit.end_ < edit.begin_ || edit.end_ > lines.size()) throw Failure("InvalidEdit");
            append(result, lines, cursor, edit.begin_); result += edit.replacement_; cursor = edit.end_;
        }
        append(result, lines, cursor, lines.size()); return result;
    }
    MergeResult Merge(std::string_view Base, std::string_view Left, std::string_view Right, const Budget& Limits)
    {
        const auto l = Diff(Base, Left, Limits), r = Diff(Base, Right, Limits);
        struct Tagged { const Edit* edit_; bool left_; }; std::vector<Tagged> all;
        for (const auto& e : l) all.push_back({&e, true});
        for (const auto& e : r) all.push_back({&e, false});
        std::stable_sort(all.begin(), all.end(), [](auto a, auto b) {
            if (a.edit_->begin_ != b.edit_->begin_) return a.edit_->begin_ < b.edit_->begin_;
            return a.edit_->end_ < b.edit_->end_;
        });
        const auto base = SplitLines(Base); MergeResult result; std::size_t cursor = 0;
        for (std::size_t i = 0; i < all.size();)
        {
            Limits.Check(); auto end = i + 1;
            const auto beginLine = all[i].edit_->begin_; auto endLine = all[i].edit_->end_;
            while (end < all.size())
            {
                const Edit range{beginLine, endLine};
                if (!overlaps(range, *all[end].edit_)) break;
                endLine = std::max(endLine, all[end].edit_->end_); ++end;
            }
            std::vector<const Edit*> ls, rs;
            for (auto j = i; j < end; ++j) (all[j].left_ ? ls : rs).push_back(all[j].edit_);
            const auto left = variant(base, beginLine, endLine, ls), right = variant(base, beginLine, endLine, rs);
            append(result.text_, base, cursor, beginLine);
            if (ls.empty()) result.text_ += right;
            else if (rs.empty() || left == right) result.text_ += left;
            else
            {
                Conflict conflict; conflict.baseBegin_ = beginLine; conflict.baseEnd_ = endLine;
                conflict.resultBegin_ = result.text_.size(); append(conflict.base_, base, beginLine, endLine);
                conflict.left_ = left; conflict.right_ = right; result.text_ += conflict.base_;
                conflict.resultEnd_ = result.text_.size(); result.conflicts_.push_back(std::move(conflict));
            }
            cursor = endLine; i = end;
        }
        append(result.text_, base, cursor, base.size()); return result;
    }
}
