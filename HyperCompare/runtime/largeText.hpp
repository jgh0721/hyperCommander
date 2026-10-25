#pragma once
#include "session.hpp"
#include <QTemporaryFile>

namespace hc::compare
{
    // Immutable UTF-8 disk segments. A revision/Undo entry shares unchanged data.
    struct LargeTextBacking { QTemporaryFile file_; };
    struct LargeTextPiece
    {
        std::shared_ptr<LargeTextBacking> backing_;
        std::uint64_t begin_ = 0, bytes_ = 0;
    };
    struct LargeTextDocument
    {
        Snapshot original_; // Identity/encoding only; raw_/utf8_ remain empty.
        QByteArray rawHash_;
        std::vector<LargeTextPiece> pieces_;
        std::uint64_t bytes_ = 0;
    };
    struct LargeTextComparison
    {
        std::array<LargeTextDocument, 2> documents_;
        std::array<std::vector<std::uint64_t>, 2> lines_;
        std::vector<Edit> changes_;
        QJsonArray saveWarnings_, saveRecovery_;
    };
    constexpr std::uint64_t largeTextMaximum = 1024ull * 1024 * 1024;
    constexpr std::uint64_t largeTextPageMaximum = 1024 * 1024;
    LargeTextDocument ReadLargeText(const QString& Path, const QString& Encoding, const Budget& Limits, bool AllowMissing = false);
    LargeTextComparison CompareLargeText(std::array<LargeTextDocument, 2> Documents, const Budget& Limits);
    QByteArray ReadLargeTextRange(const LargeTextDocument& Document, std::uint64_t Begin, std::uint64_t End, const Budget& Limits);
    LargeTextDocument ReplaceLargeText(const LargeTextDocument& Target, std::uint64_t Begin, std::uint64_t End,
        const LargeTextDocument& Source, std::uint64_t SourceBegin, std::uint64_t SourceEnd, const Budget& Limits);
    LargeTextDocument EditLargeText(const LargeTextDocument& Target, std::uint64_t Begin, std::uint64_t End,
        const QByteArray& Utf8, const Budget& Limits);
    LargeTextDocument MergeLargeTextChange(const LargeTextComparison& Comparison, int Source, std::size_t Change,
        const std::optional<std::pair<std::uint64_t, std::uint64_t>>& Selection, const Budget& Limits);
    struct LargeTextSave { Snapshot snapshot_; QByteArray rawHash_; QJsonArray warnings_, recovery_; };
    LargeTextSave SaveLargeText(const Request& Input, const LargeTextDocument& Document, const Budget& Limits);
}
