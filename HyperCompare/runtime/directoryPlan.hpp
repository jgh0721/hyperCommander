#pragma once
#include "directory.hpp"
#include "commonLibs/storage/writableStorage.hpp"
#include <QJsonObject>

namespace hc::compare
{
    struct Request;
    struct Result;
    constexpr qsizetype directoryPlanLimit = 64 * 1024 * 1024;
    QJsonObject MakeDirectoryPlan(const Request& Input, const Result& Comparison, const Budget& Limits);
    QJsonObject MakeDirectoryTransferPlan(const Request& Input, const Result& Comparison, int Source, const QString& Path, const Budget& Limits);
    void SaveDirectoryPlan(const QJsonObject& Plan, const QString& Path, Result& Value, const Budget& Limits);
    void ExportDirectoryPlan(const Request& Input, Result& Value, const Budget& Limits);
    // Shared by export/apply. Rejects reparse ancestors and ambiguous native path syntax.
    QString CheckedDirectoryPath(const QString& Path);
    bool DirectoryPathsOverlap(const QString& A, const QString& B);
    inline bool DirectoryCopyExtras(storage::CopyOptions Copy) { return Copy.metadata_ || Copy.acl_ || Copy.streams_; }
    QJsonObject DirectoryCopyEvidence(const storage::EntrySnapshot& Entry, const Budget& Limits, storage::CopyOptions Copy);
    QJsonObject DirectoryEntryEvidence(const storage::EntrySnapshot& Entry, const Budget& Limits, storage::CopyOptions Copy = {false, true, false, false});
    QByteArray DirectoryFileDigest(const storage::EntrySnapshot& Entry, const Budget& Limits);
}
