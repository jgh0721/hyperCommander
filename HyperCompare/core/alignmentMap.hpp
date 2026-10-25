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
        // Optional, bounded presentation refinement; leaves edit/merge decisions intact.
        AlignmentMap WithSimilarLines(std::string_view Left, std::string_view Right, const Budget& Limits = {}) const;
        bool HasSimilarLines() const { return similar_; }
        std::uint64_t SimilarityWork() const { return similarityWork_; }
        std::size_t MapLine(unsigned FromSide, std::size_t Line) const;
        std::size_t DisplayRow(unsigned Side, std::size_t Line) const;
        std::optional<std::size_t> LineAtRow(unsigned Side, std::size_t Row) const;
        std::size_t RowCount() const { return rows_; }
        const std::vector<AlignmentRun>& Runs() const { return runs_; }
        std::size_t LineCount(unsigned Side) const { return lines_.at(Side); }
    private:
        friend class MultiAlignmentMap;
        std::array<std::size_t, 2> lines_{};
        std::vector<AlignmentRun> runs_;
        std::size_t rows_ = 0;
        bool similar_ = false;
        std::uint64_t similarityWork_ = 0;
    };
    struct MultiAlignmentRun
    {
        std::vector<std::size_t> begin_, end_;
        std::size_t rowBegin_ = 0, rowEnd_ = 0;
        unsigned changedPairs_ = 0;
    };
    class MultiAlignmentMap
    {
    public:
        // Texts[0] is the common anchor (BASE for a three-way merge).
        static MultiAlignmentMap Build(const std::vector<std::string_view>& Texts, const Budget& Limits = {});
        const std::vector<MultiAlignmentRun>& Runs() const { return runs_; }
        std::size_t LineCount(std::size_t Side) const { return lines_.at(Side); }
        std::size_t RowCount() const { return rows_; }
        AlignmentMap Pair(std::size_t Left, std::size_t Right) const;
        // Optional bounded refinement around the common anchor; never changes text.
        MultiAlignmentMap WithSimilarLines(const std::vector<std::string_view>& Texts, const Budget& Limits = {}) const;
    private:
        std::vector<std::size_t> lines_;
        std::vector<MultiAlignmentRun> runs_;
        std::size_t rows_ = 0;
    };
}
