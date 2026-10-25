#pragma once
#include "core/compare.hpp"
#include <QJsonObject>
#include <QString>
namespace hc::compare
{
    struct Request;
    struct Result;
    struct Snapshot;
    bool TryExtensionComparison(const Request& Input, Result& Value, const Budget& Limits, bool Save = false);
    QJsonObject ProbeExtension(const QString& Format, const Snapshot& Input, const Budget& Limits);
    QJsonObject PngWorkerRequest(const QJsonObject& Payload);
}
