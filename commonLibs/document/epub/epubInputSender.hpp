#pragma once

#include <QByteArray>
#include <QString>
#include <functional>

namespace hc::document::epub
{
    struct InputSendResult
    {
        bool accepted_ = false;
        // Once transport was attempted, failure requires owner retirement even if cancellation was acknowledged.
        bool retireRequired_ = false;
        QString error_;
    };
    using InputExchange = std::function<bool(const QByteArray&, QByteArray&, int)>;
    // Blocking worker-only API for an already authenticated owner. Accepting bytes is not renderer Ready.
    // Caller retains source/session/native binding through proven process retirement on any failed transfer.
    InputSendResult SendInput(const QByteArray& Publication, const QString& DocumentId, qint64 Generation,
        const QString& Engine, const InputExchange& Exchange, const std::function<bool()>& Cancelled = {},
        int TimeoutMs = 30000);
}
