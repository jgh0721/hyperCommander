#pragma once
#include "core/compare.hpp"
#include <QByteArray>
#include <QJsonObject>
#include <QString>

namespace hc::compare
{
    // Input is the captured decoded snapshot, never reopened by the parser process.
    QJsonObject AnalyzeFunctions(const QString& Path, const QByteArray& Utf8, const Budget& Limits = {});
    // Private CLI entry point. Kept out of the GUI binary and public option parser.
    int SemanticWorkerMain();
}
