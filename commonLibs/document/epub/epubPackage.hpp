#pragma once

/// Framework headers
#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <functional>

namespace hc::document::epub
{
    inline constexpr qint64 InputLimit = 64 * 1024 * 1024;
    inline constexpr qint64 SnapshotLimit = 32 * 1024 * 1024;
    using Cancellation = std::function<bool()>;

    struct PackageResult
    {
        QJsonObject publication_;
        QString error_;
        QString location_;
        explicit operator bool() const { return error_.isEmpty() && !publication_.isEmpty(); }
    };

    // Worker-thread operations. No extraction, file writes, scripts or network access.
    // The publication identity hashes exactly the immutable bytes inspected here.
    PackageResult Prepare(const QByteArray& Bytes, const Cancellation& Cancel = {});
    PackageResult PrepareFile(const QString& Path, const Cancellation& Cancel = {});
}
