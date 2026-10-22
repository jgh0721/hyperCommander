#pragma once

namespace hc::compare
{
    // Private messages implemented by scintillaPadding.cmake in the standalone
    // HyperCompare engine only. Never send these to an unpatched shared editor.
    inline constexpr unsigned SetLeadingRows = 50010;
    inline constexpr unsigned GetLeadingRows = 50011;
    inline constexpr unsigned SetTrailingRows = 50012;
    inline constexpr unsigned ClearPaddingRows = 50013;
}
