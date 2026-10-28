#pragma once
#include "core/compare.hpp"
#include "compareLanguage.hpp"
#include <QByteArray>
#include <QJsonObject>
#include <array>
#include <cstdint>
#include <mutex>

namespace hc::compare
{
    // Documents larger than this use the one-shot worker; their trees are not kept.
    inline constexpr qsizetype residentSourceLimit = 8 * 1024 * 1024;
    struct SourceWorkerStats
    {
        std::uint64_t starts_ = 0, opens_ = 0, edits_ = 0, analyses_ = 0, cached_ = 0, reused_ = 0, resyncs_ = 0, failures_ = 0;
    };
    // Source documents of one comparison window, kept in the GUI process's resident
    // tree-sitter worker. An unchanged input is answered from the last result; an
    // edited input sends only its changed range and is reparsed incrementally.
    class SourceDocuments final
    {
    public:
        SourceDocuments() = default;
        ~SourceDocuments();
        SourceDocuments(const SourceDocuments&) = delete;
        SourceDocuments& operator=(const SourceDocuments&) = delete;
        // Same result contract as AnalyzeFunctionsAs.
        QJsonObject Analyze(int Input, const CompareLanguage& Language, const QByteArray& Utf8, const Budget& Limits);
    private:
        struct Entry { std::uint64_t id_ = 0; QByteArray text_; QString language_; QJsonObject result_; };
        std::mutex mutex_;
        std::array<Entry, 3> entries_;
    };
    SourceWorkerStats ResidentSourceWorkerStats();
    // Test hooks. Kill ends the worker as a crash between requests would; the fault
    // kills it while each of the next Requests analyses is running.
    void KillResidentSourceWorkerForTesting();
    void FailResidentSourceRequestsForTesting(int Requests);
    bool ResidentSourceWorkerRunningForTesting();
}
