#pragma once
#include "compare.hpp"
#include <optional>
namespace hc::compare
{
    // Exact sparse-match LCS; nullopt leaves dense inputs to linear Myers.
    std::optional<std::vector<Edit>> SparseDiff(const Lines& Left, const Lines& Right, std::size_t Prefix, std::size_t Suffix,
        const Budget& Limits, std::uint64_t& Steps);
    std::uint64_t LinearDiffWorkspaceBytes(std::size_t Lines);
    // Exact Myers bidirectional partition; Steps includes earlier trace work.
    std::vector<Edit> LinearDiff(const Lines& Left, const Lines& Right, std::size_t Prefix, std::size_t Suffix,
        const Budget& Limits, std::uint64_t& Steps);
}
