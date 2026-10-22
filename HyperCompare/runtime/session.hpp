#pragma once
#include "core/compare.hpp"
#include "core/mergeModel.hpp"
#include "commonLibs/storage/storage.hpp"
#include "commonLibs/storage/writableStorage.hpp"
#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QStringList>

namespace hc::compare
{
    struct Request
    {
        QString mode_ = "auto", file1_, file2_, file3_, base_, output_, encoding_;
        QString format_ = "text", report_, takeAll_, theme_;
        bool headless_ = false, gui_ = false, wait_ = false, overwrite_ = false, dryRun_ = false;
        bool help_ = false, version_ = false;
        int timeoutMs_ = 60000;
    };
    struct Parsed { Request request_; QString error_, help_; };
    Parsed Parse(const QStringList& Arguments, bool Console);
    struct Snapshot
    {
        QString path_, encoding_; QByteArray raw_, utf8_; bool bom_ = false;
        storage::EntrySnapshot entry_;
    };
    struct Result
    {
        int exitCode_ = 2;
        QString operation_, error_, outcome_ = "Unknown", completion_ = "Failed";
        QString commit_ = "NotRequested";
        QVector<Snapshot> inputs_;
        std::vector<Edit> edits_;
        MergeResult merged_;
        std::shared_ptr<const MergePlan> plan_;
        QByteArray output_;
        QJsonArray entries_, comparisons_, warnings_, recovery_;
        storage::DestinationExpectation expected_;
        bool hasOutputExpectation_ = false;
        bool inputsChanged_ = false;
        QJsonObject Json() const;
    };
    Result Execute(const Request& Input, const std::shared_ptr<std::atomic_bool>& Canceled, bool Save);
    Result Recompare(const Request& Input, const Result& Previous, const std::vector<BlockState>& State,
        std::size_t ResultBytes, const std::shared_ptr<std::atomic_bool>& Canceled);
    void SaveResult(const Request& Input, Result& Value, const QByteArray& Utf8);
    int WriteReport(const Request& Input, const Result& Value);
    QJsonObject Capabilities();
}
