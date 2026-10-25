#pragma once
#include "runtime/largeText.hpp"

namespace hc::compare
{
    class LargeTextSyntax;
    // Cache is confined to one view's sequential worker jobs. Styles and source
    // bytes stay on disk; a page receives only its own style bytes.
    QByteArray StyleLargeText(const LargeTextDocument& Document, const QByteArray& Lexer,
        std::uint64_t Begin, std::uint64_t End, std::shared_ptr<LargeTextSyntax>& Cache, const Budget& Limits);
}
