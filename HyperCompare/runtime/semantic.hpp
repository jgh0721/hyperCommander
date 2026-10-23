#pragma once
#include "core/compare.hpp"
#include <QByteArray>
#include <QJsonObject>
#include <QString>

namespace hc::compare
{
    // Input is the captured decoded snapshot, never reopened by the parser process.
    QJsonObject AnalyzeFunctions(const QString& Path, const QByteArray& Utf8, const Budget& Limits = {});
    QJsonObject MatchFunctions(const QJsonObject& Left, const QJsonObject& Right, const Budget& Limits = {});
    struct Request;
    struct Result;
    void AnalyzeSourceRelations(const Request& Input, Result& Value, const Budget& Limits);
    // Private CLI entry point. Kept out of the GUI binary and public option parser.
    int SemanticWorkerMain();
}
