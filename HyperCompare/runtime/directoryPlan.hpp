#pragma once
#include "directory.hpp"
#include <QJsonObject>

namespace hc::compare
{
    struct Request;
    struct Result;
    constexpr qsizetype directoryPlanLimit = 64 * 1024 * 1024;
    QJsonObject MakeDirectoryPlan(const Request& Input, const Result& Comparison, const Budget& Limits);
    void SaveDirectoryPlan(const QJsonObject& Plan, const QString& Path, Result& Value, const Budget& Limits);
    void ExportDirectoryPlan(const Request& Input, Result& Value, const Budget& Limits);
    // Shared by export/apply. Rejects reparse ancestors and ambiguous native path syntax.
    QString CheckedDirectoryPath(const QString& Path);
    bool DirectoryPathsOverlap(const QString& A, const QString& B);
    QJsonObject DirectoryEntryEvidence(const storage::EntrySnapshot& Entry, const Budget& Limits);
    QByteArray DirectoryFileDigest(const storage::EntrySnapshot& Entry, const Budget& Limits);
}
