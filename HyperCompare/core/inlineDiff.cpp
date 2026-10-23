#include "stdafx.h"
#include "inlineDiff.hpp"
#include <QString>
#include <QTextBoundaryFinder>

namespace hc::compare
{
    namespace
    {
        std::string_view content(std::string_view Line)
        {
            if (!Line.empty() && Line.back() == '\n') Line.remove_suffix(1);
            if (!Line.empty() && Line.back() == '\r') Line.remove_suffix(1);
            return Line;
        }
        struct Graphemes
        {
            std::string_view text_;
            std::vector<std::size_t> offsets_;
            explicit Graphemes(std::string_view Text) : text_(Text)
            {
                offsets_.reserve(Text.size() + 1);
                if (std::all_of(Text.begin(), Text.end(), [](unsigned char Byte) { return Byte < 0x80; }))
                {
                    for (std::size_t i = 0; i <= Text.size(); ++i) offsets_.push_back(i);
                    return;
                }
                const auto decoded = QString::fromUtf8(Text.data(), qsizetype(Text.size()));
                // Map UTF-16 boundary positions to the original UTF-8 snapshot.
                // Inputs are validated UTF-8. Surrogate interiors cannot be boundaries.
                std::vector<std::size_t> bytes(std::size_t(decoded.size()) + 1);
                std::size_t utf16 = 0;
                for (std::size_t i = 0; i < Text.size();)
                {
                    const auto lead = static_cast<unsigned char>(Text[i]);
                    bytes[utf16++] = i;
                    if (lead >= 0xf0) bytes[utf16++] = i;
                    i += lead < 0x80 ? 1 : lead < 0xe0 ? 2 : lead < 0xf0 ? 3 : 4;
                }
                bytes[utf16] = Text.size();
                QTextBoundaryFinder finder(QTextBoundaryFinder::Grapheme, decoded);
                int regionalRun = 0;
                for (qsizetype position = 0; position < decoded.size();)
                {
                    const auto first = decoded.at(position);
                    const auto scalar = first.isHighSurrogate() ? QChar::surrogateToUcs4(first, decoded.at(position + 1)) : char32_t(first.unicode());
                    const bool regional = scalar >= 0x1f1e6 && scalar <= 0x1f1ff;
                    finder.setPosition(position);
                    // GB12/GB13: only consecutive regional indicators count.
                    // Qt 6.12 can carry parity across an intervening emoji.
                    const bool boundary = regional && regionalRun ? regionalRun % 2 == 0 : finder.isAtBoundary();
                    if (boundary) offsets_.push_back(bytes[std::size_t(position)]);
                    regionalRun = regional ? regionalRun + 1 : 0;
                    position += first.isHighSurrogate() ? 2 : 1;
                }
                offsets_.push_back(Text.size());
            }
            std::size_t Size() const { return offsets_.size() - 1; }
            std::string_view At(std::size_t Index) const
            { return text_.substr(offsets_[Index], offsets_[Index + 1] - offsets_[Index]); }
        };
        using Ranges = std::array<std::vector<ByteRange>, 2>;
        bool compareLine(std::string_view Left, std::string_view Right, Ranges& Out,
            std::uint64_t& Cells, const Budget& Limits)
        {
            constexpr std::size_t maxLine = 16384;
            if (Left.size() > maxLine || Right.size() > maxLine) return false;
            const Graphemes a(Left), b(Right);
            std::size_t prefix = 0, suffix = 0;
            while (prefix < a.Size() && prefix < b.Size() && a.At(prefix) == b.At(prefix)) ++prefix;
            while (suffix < a.Size() - prefix && suffix < b.Size() - prefix &&
                a.At(a.Size() - suffix - 1) == b.At(b.Size() - suffix - 1)) ++suffix;
            const auto n = a.Size() - prefix - suffix, m = b.Size() - prefix - suffix;
            if (!n && !m) return true;
            if (!n || !m)
            {
                if (n) Out[0].push_back({a.offsets_[prefix], a.offsets_[prefix + n]});
                if (m) Out[1].push_back({b.offsets_[prefix], b.offsets_[prefix + m]});
                return true;
            }
            const auto cells = (n + 1) * (m + 1);
            const auto maximum = std::min<std::uint64_t>(4'000'000, Limits.steps_);
            if (cells > 262144 || cells * sizeof(std::uint16_t) > Limits.traceBytes_ ||
                cells > maximum - std::min(Cells, maximum)) return false;
            Cells += cells;
            // The full bounded table makes traceback deterministic without a
            // second expensive pass. A line has at most 16384 grapheme clusters. Tokenization is bounded by the
            // same 16 KiB line cap; it never normalizes the source bytes.
            std::vector<std::uint16_t> table(cells);
            const auto at = [&](std::size_t I, std::size_t J) -> std::uint16_t& { return table[I * (m + 1) + J]; };
            for (auto i = n; i-- > 0;)
            {
                Limits.Check();
                for (auto j = m; j-- > 0;)
                    at(i, j) = a.At(prefix + i) == b.At(prefix + j) ? std::uint16_t(at(i + 1, j + 1) + 1) :
                        std::max(at(i + 1, j), at(i, j + 1));
            }
            std::size_t i = 0, j = 0;
            while (i < n || j < m)
            {
                if (i < n && j < m && a.At(prefix + i) == b.At(prefix + j)) { ++i; ++j; continue; }
                const auto beginA = i, beginB = j;
                do
                {
                    if (i < n && (j == m || at(i + 1, j) >= at(i, j + 1))) ++i;
                    else ++j;
                } while ((i < n || j < m) && !(i < n && j < m && a.At(prefix + i) == b.At(prefix + j)));
                if (i != beginA) Out[0].push_back({a.offsets_[prefix + beginA], a.offsets_[prefix + i]});
                if (j != beginB) Out[1].push_back({b.offsets_[prefix + beginB], b.offsets_[prefix + j]});
            }
            return true;
        }
    }
    InlineDiff BuildInlineDiff(std::string_view Left, std::string_view Right, const std::vector<Edit>& Edits, const Budget& Limits)
    {
        InlineDiff result;
        if (Edits.empty())
        {
            try { Limits.Check(); }
            catch (const Failure& error) { if (error.code_ != "Timeout") throw; }
            return result;
        }
        Lines left, right;
        try { left = SplitLines(Left, Limits); right = SplitLines(Right, Limits); }
        catch (const Failure& error)
        {
            if (error.code_ != "Timeout") throw;
            for (const auto& edit : Edits)
            {
                if (edit.begin_ > edit.end_ || edit.otherBegin_ > edit.otherEnd_) throw Failure("InvalidInlineEdits");
                result.pairedLines_ += std::min(edit.end_ - edit.begin_, edit.otherEnd_ - edit.otherBegin_);
            }
            result.skippedLines_ = result.pairedLines_; return result;
        }
        std::size_t previousLeft = 0, previousRight = 0;
        for (const auto& edit : Edits)
        {
            if (edit.begin_ < previousLeft || edit.otherBegin_ < previousRight || edit.begin_ > edit.end_ ||
                edit.otherBegin_ > edit.otherEnd_ || edit.end_ > left.size() || edit.otherEnd_ > right.size())
                throw Failure("InvalidInlineEdits");
            previousLeft = edit.end_; previousRight = edit.otherEnd_;
            result.pairedLines_ += std::min(edit.end_ - edit.begin_, edit.otherEnd_ - edit.otherBegin_);
        }
        std::size_t attempted = 0, completed = 0, ranges = 0;
        try
        {
            for (const auto& edit : Edits)
            {
                Limits.Check();
                const auto count = std::min(edit.end_ - edit.begin_, edit.otherEnd_ - edit.otherBegin_);
                for (std::size_t line = 0; line < count; ++line)
                {
                    Limits.Check();
                    if (attempted++ >= 4096) break;
                    const auto a = content(left[edit.begin_ + line]), b = content(right[edit.otherBegin_ + line]);
                    Ranges pair;
                    if (!compareLine(a, b, pair, result.cells_, Limits)) continue;
                    const auto added = pair[0].size() + pair[1].size();
                    if (added > 32768 - ranges) continue;
                    const std::size_t offsets[] = {std::size_t(a.data() - Left.data()), std::size_t(b.data() - Right.data())};
                    for (std::size_t side = 0; side < 2; ++side)
                        for (const auto& range : pair[side])
                            result.ranges_[side].push_back({range.begin_ + offsets[side], range.end_ + offsets[side]});
                    ranges += added; ++completed;
                }
                if (attempted >= 4096) break;
            }
            Limits.Check();
        }
        catch (const Failure& error) { if (error.code_ != "Timeout") throw; }
        result.skippedLines_ = result.pairedLines_ - completed;
        return result;
    }
}
