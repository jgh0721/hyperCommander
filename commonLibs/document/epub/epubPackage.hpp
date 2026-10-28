#pragma once

/// Framework headers
#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <functional>
#include <memory>

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

    struct ResourceResult {
        QByteArray bytes_;
        QString error_, location_;
        explicit operator bool() const { return error_.isEmpty(); }
    };
    // One immutable publication. Resource reads are independent and thread-safe;
    // the cancellation callback must own its state and be safe on worker threads.
    class PublicationSession final {
    public:
        static std::shared_ptr<PublicationSession> Open(QByteArray Bytes, const Cancellation& Cancel = {});
        ~PublicationSession();
        PackageResult Index() const;
        ResourceResult ReadValidatedResource(const QString& Path) const;
        ResourceResult ValidateAll() const;
    private:
        struct State;
        explicit PublicationSession(std::unique_ptr<State> Value);
        std::unique_ptr<State> state_;
    };
}
