#pragma once
#include "compare.hpp"
#include <array>

namespace hc::compare
{
    class AlignmentMap;
    struct ByteRange
    {
        std::size_t begin_ = 0, end_ = 0;
        bool operator==(const ByteRange&) const = default;
    };
    struct InlineDiff
    {
        std::array<std::vector<ByteRange>, 2> ranges_;
        std::size_t pairedLines_ = 0, skippedLines_ = 0;
        std::uint64_t cells_ = 0;
    };
    // Presentation only: pair changed lines using optional refined alignment (otherwise
    // ordinal), anchor unique exact words/
    // identifiers in increasing order, then compare grapheme clusters between anchors.
    // Words are runs of letters, numbers and underscores (at least two clusters), not
    // language-specific lexical analysis. Repeated words are not anchors. This prefers
    // whole-word correspondence over a globally maximal character subsequence.
    // No normalization; CR/LF omitted. Inputs must be valid decoded UTF-8 snapshots.
    // Limits: 16 KiB/line, 4096 pairs, 262144 cells/pair, 4M cells/session,
    // 32768 ranges/session. Oversized pairs retain their line-level decoration.
    // Timeout returns completed pairs; explicit cancellation still throws.
    InlineDiff BuildInlineDiff(std::string_view Left, std::string_view Right,
        const std::vector<Edit>& Edits, const Budget& Limits = {}, const AlignmentMap* Alignment = nullptr);
}
