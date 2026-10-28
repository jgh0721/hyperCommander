#pragma once
#include "core/compare.hpp"
#include <QByteArray>
#include <QJsonObject>
#include <QString>

namespace hc::compare
{
    struct CompareLanguage;
    // Input is the captured decoded snapshot, never reopened by the parser process.
    QJsonObject AnalyzeFunctions(const QString& Path, const QByteArray& Utf8, const Budget& Limits = {});
    // One-shot worker with an explicit (resolved or user-selected) language.
    QJsonObject AnalyzeFunctionsAs(const CompareLanguage& Language, const QByteArray& Utf8, const Budget& Limits = {});
    // Checks a worker response against the analyzed text; false means WorkerProtocol.
    bool ValidateFunctionList(const QJsonObject& Result, const QString& Language, qsizetype Bytes);
    // Typical commit that parsing and digesting Bytes of source adds to a worker.
    // Admission and retention use it; dense code (many short tokens) needs up to twice as much.
    std::uint64_t SourceTreeMemory(const QString& Language, qsizetype Bytes);
    // One-shot worker job limit for Bytes of source, or 0 when even typical source exceeds the 512 MiB cap.
    std::uint64_t SourceWorkerMemory(const QString& Language, qsizetype Bytes);
    QJsonObject MatchFunctions(const QJsonObject& Left, const QJsonObject& Right, const Budget& Limits = {});
    struct Request;
    struct Result;
    void AnalyzeSourceRelations(const Request& Input, Result& Value, const Budget& Limits);
    // Private CLI entry point. Kept out of the GUI binary and public option parser.
    QJsonObject SemanticWorkerRequest(const QJsonObject& Payload);
}
