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
        QByteArray rawHash_, contentHash_;
        std::vector<LargeTextPiece> pieces_;
        std::uint64_t bytes_ = 0;
    };
    struct LargeTextConflict
    {
        LargeTextDocument base_, left_, right_;
        std::uint64_t begin_ = 0, end_ = 0;
        Resolution choice_ = Resolution::Unresolved;
    };
    struct LargeTextSession
    {
        std::vector<LargeTextDocument> documents_;
        QStringList titles_;
        int result_ = -1;
        bool reviewRequired_ = false;
        std::vector<LargeTextConflict> conflicts_;
        std::array<int, 2> pair_{0, 1};
        QJsonArray comparisons_;
    };
    struct LargeTextComparison
    {
        std::shared_ptr<const LargeTextSession> session_;
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
    void StreamLargeText(const LargeTextDocument& Document, const Budget& Limits, const std::function<void(QByteArrayView)>& Output);
    LargeTextDocument ReplaceLargeText(const LargeTextDocument& Target, std::uint64_t Begin, std::uint64_t End,
        const LargeTextDocument& Source, std::uint64_t SourceBegin, std::uint64_t SourceEnd, const Budget& Limits);
    LargeTextDocument EditLargeText(const LargeTextDocument& Target, std::uint64_t Begin, std::uint64_t End,
        const QByteArray& Utf8, const Budget& Limits);
    LargeTextDocument MergeLargeTextChange(const LargeTextComparison& Comparison, int Source, std::size_t Change,
        const std::optional<std::pair<std::uint64_t, std::uint64_t>>& Selection, const Budget& Limits);
    std::shared_ptr<LargeTextComparison> OpenLargeTextSession(const Request& Input, const Budget& Limits);
    std::shared_ptr<LargeTextComparison> PairLargeTextSession(LargeTextSession Session, std::array<int, 2> Pair, const Budget& Limits);
    std::shared_ptr<LargeTextComparison> UpdateLargeTextSession(const LargeTextComparison& Previous,
        std::array<LargeTextDocument, 2> Documents, const Budget& Limits, bool Manual = true);
    std::shared_ptr<LargeTextComparison> ResolveLargeTextConflict(const LargeTextComparison& Previous,
        std::size_t Index, Resolution Choice, const Budget& Limits);
    std::shared_ptr<LargeTextComparison> BindLargeTextOutput(const LargeTextComparison& Previous,
        const QString& Path, bool ReplaceExisting, const Request& Input, const Budget& Limits);
    std::size_t LargeTextUnresolved(const LargeTextComparison& Value);
    void ValidateLargeTextInputs(const LargeTextSession& Session, const Budget& Limits);
    QJsonObject LargeTextSessionJson(const LargeTextComparison& Value);
    struct LargeTextSave { Snapshot snapshot_; QByteArray rawHash_; QJsonArray warnings_, recovery_; };
    LargeTextSave SaveLargeText(const Request& Input, const LargeTextDocument& Document, const Budget& Limits);
}
