#pragma once
#include "hostSession.hpp"

namespace hc::plugins
{
    storage::Result<WireFrame> CallSession(HANDLE Pipe, HANDLE Process, const QString& Epoch, std::uint64_t Sequence,
        WireKind Kind, QJsonObject Request, QByteArray Binary, storage::OperationContext Context,
        std::uint32_t ResponseMs, std::uint32_t NoProgressMs, std::uint32_t CancelMs, std::function<void(bool)> Waiting);
}
