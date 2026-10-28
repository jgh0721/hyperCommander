#pragma once
#include "core/compare.hpp"
#include <QJsonObject>
#include <QString>
namespace hc::compare
{
    struct WorkerPolicy
    {
        int timeoutMs_ = 2500;
        qsizetype requestBytes_ = 24 * 1024 * 1024, responseBytes_ = 8 * 1024 * 1024;
        std::size_t memoryBytes_ = 256 * 1024 * 1024;
    };
    QJsonObject RunWorker(const QString& Engine, const QJsonObject& Payload, const Budget& Limits, const WorkerPolicy& Policy = {});
    int WorkerMain();
    // Long-lived source.tree-sitter worker of one GUI process (sourceWorkerHost.cpp).
    int ResidentWorkerMain();
}
