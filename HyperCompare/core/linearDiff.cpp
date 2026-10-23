#include "stdafx.h"
#include "linearDiff.hpp"
#include <array>
#include <algorithm>

namespace hc::compare
{
    namespace { struct Range { std::size_t a_, endA_, b_, endB_; }; }
    std::uint64_t LinearDiffWorkspaceBytes(std::size_t Lines)
    { return (2 * ((std::uint64_t(Lines) + 1) / 2) + 3) * 2 * sizeof(std::int64_t) + sizeof(std::array<Range, 128>); }
    std::vector<Edit> LinearDiff(const Lines& Left, const Lines& Right, std::size_t Prefix, std::size_t Suffix,
        const Budget& Limits, std::uint64_t& Steps)
    {
        Limits.Check();
        using Index = std::int64_t;
        const auto step = [&] { if (++Steps > Limits.steps_) throw Failure("DiffWorkLimit"); if ((Steps & 1023) == 0) Limits.Check(); };
        if (Prefix > Left.size() || Prefix > Right.size() || Suffix > Left.size() - Prefix || Suffix > Right.size() - Prefix) throw Failure("InvalidDiffRange");
        const auto size = Left.size() + Right.size() - 2 * (Prefix + Suffix);
        const auto width = 2 * ((size + 1) / 2) + 3;
        std::array<Range, 128> pending;
        if (LinearDiffWorkspaceBytes(size) > Limits.traceBytes_) throw Failure("DiffMemoryLimit");
        std::vector<Index> forward(width), reverse(width); std::size_t count = 1;
        pending[0] = {Prefix, Left.size() - Suffix, Prefix, Right.size() - Suffix};
        std::vector<Edit> edits;
        const auto emit = [&](const Range& r) {
            if (r.a_ == r.endA_ && r.b_ == r.endB_) return;
            if (edits.empty() || edits.back().end_ != r.a_ || edits.back().otherEnd_ != r.b_)
                edits.push_back({r.a_, r.a_, r.b_, r.b_, {}});
            auto& edit = edits.back(); edit.end_ = r.endA_; edit.otherEnd_ = r.endB_;
            for (auto i = r.b_; i < r.endB_; ++i) { step(); edit.replacement_.append(Right[i]); }
        };
        // One pair of vectors is reused after each partition; no recursion retains it.
        // Myers 1986, section 4b: forward/reverse furthest paths overlap on delta-k.
        const auto partition = [&](const Range& r) -> std::pair<std::size_t, std::size_t> {
            const auto n = Index(r.endA_ - r.a_), m = Index(r.endB_ - r.b_), delta = n - m;
            const auto maximum = (n + m + 1) / 2, offset = maximum + 1;
            const auto used = std::size_t(2 * maximum + 3);
            std::fill_n(forward.begin(), used, Index(-1)); std::fill_n(reverse.begin(), used, Index(-1));
            forward[std::size_t(offset + 1)] = reverse[std::size_t(offset + 1)] = 0;
            for (Index d = 0; d <= maximum; ++d)
            {
                Limits.Check();
                for (Index k = -d; k <= d; k += 2)
                {
                    step(); const auto at = std::size_t(offset + k);
                    auto x = k == -d || (k != d && forward[at - 1] < forward[at + 1]) ? forward[at + 1] : forward[at - 1] + 1;
                    auto y = x - k;
                    if (x < 0 || y < 0 || x > n || y > m) { forward[at] = -1; continue; }
                    while (x < n && y < m && Left[r.a_ + std::size_t(x)] == Right[r.b_ + std::size_t(y)]) { step(); ++x; ++y; }
                    forward[at] = x;
                    const auto opposing = delta - k;
                    if (delta % 2 && opposing >= -(d - 1) && opposing <= d - 1 && reverse[std::size_t(offset + opposing)] >= 0 && x >= n - reverse[std::size_t(offset + opposing)])
                        return {r.a_ + std::size_t(x), r.b_ + std::size_t(y)};
                }
                for (Index k = -d; k <= d; k += 2)
                {
                    step(); const auto at = std::size_t(offset + k);
                    auto x = k == -d || (k != d && reverse[at - 1] < reverse[at + 1]) ? reverse[at + 1] : reverse[at - 1] + 1;
                    auto y = x - k;
                    if (x < 0 || y < 0 || x > n || y > m) { reverse[at] = -1; continue; }
                    while (x < n && y < m && Left[r.endA_ - 1 - std::size_t(x)] == Right[r.endB_ - 1 - std::size_t(y)]) { step(); ++x; ++y; }
                    reverse[at] = x;
                    const auto opposing = delta - k;
                    if (!(delta % 2) && opposing >= -d && opposing <= d && forward[std::size_t(offset + opposing)] >= 0 && forward[std::size_t(offset + opposing)] >= n - x)
                    {
                        const auto fx = forward[std::size_t(offset + opposing)];
                        return {r.a_ + std::size_t(fx), r.b_ + std::size_t(fx - opposing)};
                    }
                }
            }
            throw Failure("DiffPartition");
        };
        while (count)
        {
            Limits.Check(); auto r = pending[--count];
            while (r.a_ < r.endA_ && r.b_ < r.endB_ && Left[r.a_] == Right[r.b_]) { step(); ++r.a_; ++r.b_; }
            while (r.a_ < r.endA_ && r.b_ < r.endB_ && Left[r.endA_ - 1] == Right[r.endB_ - 1]) { step(); --r.endA_; --r.endB_; }
            if (r.a_ == r.endA_ || r.b_ == r.endB_) { emit(r); continue; }
            if (r.endA_ - r.a_ == 1)
            {
                auto at = r.b_; while (at < r.endB_) { step(); if (Left[r.a_] == Right[at]) break; ++at; }
                if (at == r.endB_) emit(r);
                else { emit({r.a_, r.a_, r.b_, at}); emit({r.endA_, r.endA_, at + 1, r.endB_}); }
                continue;
            }
            if (r.endB_ - r.b_ == 1)
            {
                auto at = r.a_; while (at < r.endA_) { step(); if (Left[at] == Right[r.b_]) break; ++at; }
                if (at == r.endA_) emit(r);
                else { emit({r.a_, at, r.b_, r.b_}); emit({at + 1, r.endA_, r.endB_, r.endB_}); }
                continue;
            }
            const auto [a, b] = partition(r);
            if ((a == r.a_ && b == r.b_) || (a == r.endA_ && b == r.endB_) || a < r.a_ || a > r.endA_ || b < r.b_ || b > r.endB_) throw Failure("DiffPartition");
            if (count + 2 > pending.size()) throw Failure("DiffMemoryLimit");
            pending[count++] = {a, r.endA_, b, r.endB_}; pending[count++] = {r.a_, a, r.b_, b};
        }
        Limits.Check(); return edits;
    }
}
