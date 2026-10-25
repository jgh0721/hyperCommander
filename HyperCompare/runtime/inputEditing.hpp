#pragma once
#include "session.hpp"
namespace hc::compare
{
    Result CompareOpenBuffers(const Request& Input, Result Previous, const QVector<QByteArray>& Texts, const Budget& Limits);
    struct InputSaveResult
    {
        Snapshot snapshot_;
        QString error_;
        QJsonArray warnings_, recovery_;
        bool committed_ = false;
    };
    InputSaveResult SaveInputBuffer(const Snapshot& Original, const QByteArray& Utf8, const Budget& Limits);
}
