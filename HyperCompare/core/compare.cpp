#include "stdafx.h"
#include "compare.hpp"
#include "linearDiff.hpp"
#include <functional>

namespace hc::compare
{
    void Budget::Check() const
    {
        if (canceled_ && canceled_->load(std::memory_order_relaxed)) throw Failure("Canceled");
        if (std::chrono::steady_clock::now() >= deadline_) throw Failure("Timeout");
        if (memoryCheck_) memoryCheck_();
    }
    namespace
    {
        template<class Emit> std::size_t scanLines(std::string_view Text, const Budget& Limits,
            std::size_t MaxLines, std::size_t MaxLineBytes, Emit Output)
        {
            Limits.Check(); std::size_t start = 0, count = 0, nextCheck = 0;
            const auto emit = [&](std::size_t End) {
                if (count == MaxLines) throw Failure("LineLimit");
                Output(Text.substr(start, End - start)); ++count; start = End;
            };
            for (std::size_t i = 0; i < Text.size(); ++i)
            {
                if (i >= nextCheck) { Limits.Check(); nextCheck = i + 4096; }
                const bool end = Text[i] == '\n' || Text[i] == '\r';
                if (Text[i] == '\r' && i + 1 < Text.size() && Text[i + 1] == '\n') ++i;
                if (i + 1 - start > MaxLineBytes) throw Failure("LongLineLimit");
                if (end) emit(i + 1);
            }
            if (start < Text.size()) emit(Text.size());
            Limits.Check(); return count;
        }
    }
    std::size_t CountLines(std::string_view Text, const Budget& Limits, std::size_t MaxLines, std::size_t MaxLineBytes)
    { return scanLines(Text, Limits, MaxLines, MaxLineBytes, [](std::string_view) {}); }
    Lines SplitLines(std::string_view Text, const Budget& Limits, std::size_t MaxLines, std::size_t MaxLineBytes)
    {
        // Reject pathological input before allocating, then allocate the exact index.
        const auto count = CountLines(Text, Limits, MaxLines, MaxLineBytes);
        Lines result; result.reserve(count);
        scanLines(Text, Limits, MaxLines, MaxLineBytes, [&](std::string_view Line) { result.push_back(Line); });
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
        std::size_t sideLine(std::size_t Line, bool Gap, const std::vector<Edit>& Edits)
        {
            const auto at = std::lower_bound(Edits.begin(), Edits.end(), Line,
                [](const Edit& Edit, std::size_t Value) { return Edit.begin_ < Value; });
            if (at != Edits.end() && at->begin_ == Line)
            {
                // A separate insertion at a nonempty range's beginning precedes it.
                return Gap || at->end_ > Line ? at->otherBegin_ : at->otherEnd_;
            }
            if (at == Edits.begin()) return Line;
            const auto& previous = *std::prev(at);
            return previous.end_ > Line ? previous.otherBegin_ : previous.otherEnd_ + Line - previous.end_;
        }
    }
    std::vector<Edit> Diff(std::string_view Left, std::string_view Right, const Budget& Limits)
    {
        Limits.Check(); if (Left == Right) return {};
        const auto left = SplitLines(Left, Limits);
        const auto right = SplitLines(Right, Limits, 2'000'000 - left.size());
        std::uint64_t steps = 0;
        const auto step = [&] { if (++steps > Limits.steps_) throw Failure("DiffWorkLimit"); if ((steps & 1023) == 0) Limits.Check(); };
        std::size_t prefix = 0, suffix = 0;
        while (prefix < left.size() && prefix < right.size() && left[prefix] == right[prefix]) { step(); ++prefix; }
        while (suffix < left.size() - prefix && suffix < right.size() - prefix && left[left.size() - suffix - 1] == right[right.size() - suffix - 1]) { step(); ++suffix; }
        using Index = std::int64_t;
        const auto n = static_cast<Index>(left.size() - prefix - suffix), m = static_cast<Index>(right.size() - prefix - suffix);
        const auto replace = [&] {
            Edit edit{prefix, left.size() - suffix, prefix, right.size() - suffix, {}};
            for (auto i = prefix; i < right.size() - suffix; ++i) { step(); edit.replacement_.append(right[i]); }
            Limits.Check(); return std::vector<Edit>{std::move(edit)};
        };
        if (!n || !m) return replace();
        if (n + m > 2048 && std::uint64_t(std::min(n, m)) <= Limits.traceBytes_ / sizeof(std::size_t))
        {
            // A disjoint hash set proves there is no equal line, so a single
            // replacement is an exact shortest edit script. Hash collisions
            // merely disable this shortcut; they never make two lines equal.
            const auto& small = n <= m ? left : right; const auto& large = n <= m ? right : left;
            std::vector<std::size_t> hashes; hashes.reserve(std::size_t(std::min(n, m)));
            for (auto i = prefix; i < small.size() - suffix; ++i) { step(); hashes.push_back(std::hash<std::string_view>{}(small[i])); }
            const auto less = [&](std::size_t A, std::size_t B) { step(); return A < B; }; std::sort(hashes.begin(), hashes.end(), less);
            bool common = false;
            for (auto i = prefix; i < large.size() - suffix; ++i)
            {
                step(); if (std::binary_search(hashes.begin(), hashes.end(), std::hash<std::string_view>{}(large[i]), less)) { common = true; break; }
            }
            if (!common) return replace();
        }
        std::vector<std::vector<Index>> trace;
        std::uint64_t bytes = 0;
        Index distance = 0; bool done = false;
        // Prefer bounded history for small changes. If the linear workspace cannot
        // fit a custom budget, keep the original history allowance available.
        const auto traceLimit = LinearDiffWorkspaceBytes(std::size_t(n + m)) <= Limits.traceBytes_
            ? std::min<std::uint64_t>(Limits.traceBytes_, 8 * 1024 * 1024) : Limits.traceBytes_;
        // Keep only reachable diagonals and reject trace growth before allocating.
        for (Index d = 0; d <= n + m && !done; ++d)
        {
            Limits.Check(); const auto cost = std::uint64_t(d + 1) * sizeof(Index);
            if (cost > traceLimit || bytes > traceLimit - cost)
            {
                std::vector<std::vector<Index>>{}.swap(trace);
                return LinearDiff(left, right, prefix, suffix, Limits, steps);
            }
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
                while (x < n && y < m && x >= 0 && y >= 0 && left[prefix + std::size_t(x)] == right[prefix + std::size_t(y)])
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
        std::vector<Edit> edits; std::size_t a = prefix, b = prefix; bool open = false;
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
        const auto base = SplitLines(Base, Limits); MergeResult result; std::size_t cursor = 0;
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
            MergeRegion region{beginLine, endLine, sideLine(beginLine, beginLine == endLine, l),
                sideLine(beginLine, beginLine == endLine, r), left.size(), right.size(), result.text_.size()};
            if (ls.empty()) { result.text_ += right; region.origin_ = MergeOrigin::Right; }
            else if (rs.empty() || left == right) { result.text_ += left; region.origin_ = MergeOrigin::Left; }
            else
            {
                Conflict conflict; conflict.baseBegin_ = beginLine; conflict.baseEnd_ = endLine;
                conflict.leftBegin_ = sideLine(beginLine, beginLine == endLine, l);
                conflict.rightBegin_ = sideLine(beginLine, beginLine == endLine, r);
                conflict.resultBegin_ = result.text_.size(); append(conflict.base_, base, beginLine, endLine);
                conflict.left_ = left; conflict.right_ = right; result.text_ += conflict.base_;
                conflict.resultEnd_ = result.text_.size(); result.conflicts_.push_back(std::move(conflict));
            }
            region.resultEnd_ = result.text_.size(); result.regions_.push_back(region);
            cursor = endLine; i = end;
        }
        append(result.text_, base, cursor, base.size()); return result;
    }
}
