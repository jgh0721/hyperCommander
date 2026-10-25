#pragma once
#include "core/compare.hpp"
#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <vector>

namespace hc::compare
{
    struct Request;
    // Static descriptors use serialized schema IDs, not a public C++ plugin ABI.
    // Decoder, comparison, writer and view capabilities remain independent.
    struct EngineDescriptor
    {
        QString id_, version_, kind_;
        QStringList formats_;
        QString decoder_, writer_, view_, locationSchema_;
        bool compareTwo_ = false, compareThree_ = false, mergeTwo_ = false, mergeBase_ = false;
        bool independentThree_ = false, isolated_ = false;
        qint64 maxInputBytes_ = 0;
        QJsonObject limits_;
        QJsonObject Json() const;
    };
    const std::vector<EngineDescriptor>& RegisteredEngines();
    const EngineDescriptor* FindComparisonEngine(const QString& Format);
    QString DetectResourceFormat(const QString& Path, const QByteArray& Prefix);
    QString InspectResourceFormat(const QString& Path, const Budget& Limits);
    QJsonObject EngineCatalog();
    QJsonObject ProbeComparison(const Request& Input, const Budget& Limits);
}
