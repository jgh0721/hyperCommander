#pragma once
#include "core/compare.hpp"
#include "directory.hpp"
#include "core/mergeModel.hpp"
#include "core/alignmentMap.hpp"
#include "core/inlineDiff.hpp"
#include "commonLibs/storage/storage.hpp"
#include "commonLibs/storage/writableStorage.hpp"
#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QStringList>
#include <optional>

namespace hc::compare
{
    struct Request
    {
        QString mode_ = "auto", file1_, file2_, file3_, base_, output_, encoding_;
        QString format_ = "text", report_, takeAll_, theme_, planOut_, decisions_;
        QString semantic_ = "auto";
        QString textEquality_ = "content";
        bool headless_ = false, gui_ = false, wait_ = false, overwrite_ = false, dryRun_ = false;
        bool includeContent_ = false;
        bool reuse_ = false;
        int guiWaitMs_ = 0;
        QString requestFile_, requestId_;
        QString manifest_;
        int jobs_ = 1;
        bool failFast_ = false;
        QStringList protectedInputs_;
        std::array<QString, 3> inputEncodings_;
        QString applyPlan_, journal_, applyPlanDigest_;
        bool resume_ = false, allowDelete_ = false;
        bool mirror_ = false;
        bool help_ = false, version_ = false;
        DirectoryPolicy directory_;
        int timeoutMs_ = 60000, memoryMiB_ = 1024;
    };
    struct Parsed { Request request_; QString error_, help_; };
    inline QString InputEncoding(const Request& Input, int Index)
    { const auto value = Input.inputEncodings_[Index].isEmpty() ? Input.encoding_ : Input.inputEncodings_[Index]; return value == "auto" ? QString{} : value; }
    Parsed Parse(const QStringList& Arguments, bool Console);
    struct Snapshot
    {
        QString path_, encoding_; QByteArray raw_, utf8_; bool bom_ = false;
        storage::EntrySnapshot entry_;
    };
    struct PairChange
    {
        std::size_t begin_ = 0, end_ = 0, otherBegin_ = 0, otherEnd_ = 0;
    };
    // Immutable GUI metadata for one pair of already decoded input snapshots.
    // No replacement payloads or extra copies of source text are retained.
    struct PairComparison
    {
        std::array<int, 2> inputs_{0, 1};
        std::vector<PairChange> changes_;
        std::shared_ptr<const AlignmentMap> alignment_;
        InlineDiff inline_;
    };
    struct Result
    {
        int exitCode_ = 2;
        QString operation_, error_, outcome_ = "Unknown", completion_ = "Failed";
        QString requestId_;
        QString commit_ = "NotRequested";
        QVector<Snapshot> inputs_;
        std::optional<Snapshot> decisionsSnapshot_;
        QString planWritten_;
        std::vector<Edit> edits_;
        // Slots: FILE1/2, FILE1/3, FILE2/3. BASE/merge sessions use slot 0 only.
        std::array<std::shared_ptr<const PairComparison>, 3> pairViews_;
        MergeResult merged_;
        std::shared_ptr<const MergePlan> plan_;
        QByteArray output_;
        std::shared_ptr<const std::vector<DirectorySnapshot>> directories_;
        QJsonObject directoryPlan_;
        QJsonObject semantic_;
        QJsonObject engineProbe_;
        QJsonObject extension_;
        QJsonObject textComparison_;
        QJsonObject saveRecovery_;
        QString directoryLevel_;
        bool directoryFiltered_ = false;
        QJsonArray entries_, comparisons_, warnings_, recovery_;
        storage::DestinationExpectation expected_;
        bool hasOutputExpectation_ = false;
        bool inputsChanged_ = false;
        QJsonObject Json(bool IncludeContent = false) const;
    };
    Result Execute(const Request& Input, const std::shared_ptr<std::atomic_bool>& Canceled, bool Save);
    Result Recompare(const Request& Input, const Result& Previous, const std::vector<BlockState>& State,
        std::size_t ResultBytes, const std::shared_ptr<std::atomic_bool>& Canceled);
    void SaveResult(const Request& Input, Result& Value, const QByteArray& Utf8);
    struct ReportFile { QJsonArray warnings_, recovery_; };
    ReportFile SaveComparisonReport(const QString& Path, const Request& Input, const Result& Session,
        const QJsonObject& ViewState, bool ReplaceExisting, const Budget& Limits);
    int WriteReport(const Request& Input, const Result& Value);
    QJsonObject Capabilities();
    Snapshot ReadTextSnapshot(const QString& Path, const QString& Encoding, const Budget& Limits);
    Snapshot ReadFileSnapshot(const QString& Path, qsizetype MaxBytes, const Budget& Limits);
}
