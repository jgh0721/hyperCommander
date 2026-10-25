#pragma once
#include "session.hpp"
namespace hc::compare
{
    struct ArchiveSession
    {
        QVector<Snapshot> originals_;
        std::array<QByteArray, 2> working_;
        QJsonArray passwords_;
        std::array<QJsonArray, 2> members_;
        QJsonObject comparison_;
    };
    QJsonObject NativeArchiveWorkerRequest(const QJsonObject& Payload);
    QJsonObject RunArchiveWorker(const QVector<Snapshot>& Inputs, const QJsonArray& Passwords,
        const std::array<QJsonArray, 2>& Members, const Budget& Limits, const QJsonObject& Operation = {});
    bool TryArchiveComparison(const Request& Input, Result& Value, const Budget& Limits, bool Save, const QString& Format);
    Snapshot SaveArchiveSnapshot(const Request& Input, const Snapshot& Expected, const QByteArray& Bytes, const Budget& Limits);
    QJsonObject ArchiveOperation(const Request& Input, ArchiveSession& Session, const QJsonObject& Operation, const Budget& Limits);
}
