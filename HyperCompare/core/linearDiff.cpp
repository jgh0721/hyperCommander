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
            if (!Limits.rangesOnly_) for (auto i = r.b_; i < r.endB_; ++i) { step(); edit.replacement_.append(Right[i]); }
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
    std::optional<std::vector<Edit>> SparseDiff(const Lines& Left, const Lines& Right, std::size_t Prefix, std::size_t Suffix,
        const Budget& Limits, std::uint64_t& Steps)
    {
        Limits.Check();
        if (Prefix > Left.size() || Prefix > Right.size() || Suffix > Left.size() - Prefix || Suffix > Right.size() - Prefix)
            throw Failure("InvalidAlignmentInput");
        const auto n = Left.size() - Prefix - Suffix, m = Right.size() - Prefix - Suffix, common = std::min(n, m);
        struct Node { std::size_t a_, b_, previous_; };
        const auto fixed = std::uint64_t(m) * sizeof(std::size_t) + std::uint64_t(common) * (sizeof(std::size_t) + sizeof(std::pair<std::size_t, std::size_t>));
        if (fixed >= Limits.traceBytes_) return std::nullopt;
        const auto maximum = std::min<std::uint64_t>(1'000'000, (Limits.traceBytes_ - fixed) / sizeof(Node));
        const auto step = [&] { if (Steps >= Limits.steps_) throw Failure("DiffWorkLimit"); ++Steps; if ((Steps & 1023) == 0) Limits.Check(); };
        std::vector<std::size_t> order; order.reserve(m);
        for (std::size_t i = 0; i < m; ++i) { step(); order.push_back(i); }
        std::sort(order.begin(), order.end(), [&](std::size_t A, std::size_t B) {
            step(); return Right[Prefix + A] != Right[Prefix + B] ? Right[Prefix + A] < Right[Prefix + B] : A < B;
        });
        const auto matches = [&](std::size_t A) {
            const auto text = Left[Prefix + A];
            const auto first = std::lower_bound(order.begin(), order.end(), text, [&](std::size_t B, std::string_view Text) { step(); return Right[Prefix + B] < Text; });
            const auto last = std::upper_bound(first, order.end(), text, [&](std::string_view Text, std::size_t B) { step(); return Text < Right[Prefix + B]; });
            return std::pair{first, last};
        };
        std::uint64_t count = 0;
        for (std::size_t i = 0; i < n; ++i)
        {
            step(); const auto [first, last] = matches(i); const auto added = std::uint64_t(last - first);
            if (added > maximum - count) return std::nullopt;
            count += added;
        }
        Limits.Check(); std::vector<Node> nodes; nodes.reserve(std::size_t(count));
        std::vector<std::size_t> tails; tails.reserve(common); const auto none = std::numeric_limits<std::size_t>::max();
        for (std::size_t i = 0; i < n; ++i)
        {
            step(); const auto [first, last] = matches(i);
            // Descending positions prevent two matches from consuming one left line.
            for (auto at = last; at != first;)
            {
                step(); const auto b = *--at;
                const auto slot = std::lower_bound(tails.begin(), tails.end(), b, [&](std::size_t Index, std::size_t B) { step(); return nodes[Index].b_ < B; });
                const auto index = std::size_t(slot - tails.begin());
                nodes.push_back({i, b, index ? tails[index - 1] : none});
                if (slot == tails.end()) tails.push_back(nodes.size() - 1); else *slot = nodes.size() - 1;
            }
        }
        std::vector<std::pair<std::size_t, std::size_t>> anchors; anchors.reserve(common);
        for (auto at = tails.empty() ? none : tails.back(); at != none; at = nodes[at].previous_)
        { step(); anchors.emplace_back(nodes[at].a_, nodes[at].b_); }
        std::reverse(anchors.begin(), anchors.end()); std::vector<Edit> edits; std::size_t a = 0, b = 0;
        const auto emit = [&](std::size_t EndA, std::size_t EndB) {
            if (a == EndA && b == EndB) return;
            Edit edit{Prefix + a, Prefix + EndA, Prefix + b, Prefix + EndB, {}};
            if (!Limits.rangesOnly_) for (auto i = b; i < EndB; ++i) { step(); edit.replacement_.append(Right[Prefix + i]); }
            edits.push_back(std::move(edit));
        };
        for (const auto& anchor : anchors) { step(); emit(anchor.first, anchor.second); a = anchor.first + 1; b = anchor.second + 1; }
        emit(n, m); Limits.Check(); return edits;
    }

}
