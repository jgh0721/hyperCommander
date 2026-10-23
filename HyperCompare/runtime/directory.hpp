#pragma once
#include "core/compare.hpp"
#include "commonLibs/storage/storage.hpp"
#include <QJsonArray>
#include <QMap>
#include <QStringList>

namespace hc::compare
{
    struct Request;
    struct Result;
    struct DirectoryPolicy
    {
        QStringList include_, exclude_;
        QString level_ = "bytes";
    };
    using DirectoryTree = QMap<QString, storage::EntrySnapshot>;
    struct DirectorySnapshot
    {
        QString root_;
        storage::EntrySnapshot entry_;
        DirectoryTree entries_;
    };
    DirectorySnapshot CaptureDirectory(const QString& Root, const DirectoryPolicy& Policy, const Budget& Limits);
    void ValidateDirectory(const DirectorySnapshot& Snapshot, const DirectoryPolicy& Policy, const Budget& Limits);
    void CompareDirectory(const Request& Input, Result& Value, const Budget& Limits);
}
