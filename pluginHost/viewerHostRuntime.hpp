#pragma once
#include "pipeChannel.hpp"
#include <QJsonObject>

namespace hc::plugins
{
    inline constexpr unsigned ViewerControlLimit = 16 * 1024;
    bool RuntimeIdentity(const QString& Value);
    bool IsRuntimeCommand(WireKind Kind);
    QByteArray RuntimeJson(const QJsonObject& Value);
    storage::Result<QJsonObject> RuntimeObject(const QByteArray& Value);
    struct ViewerRuntimeCallbacks
    {
        std::function<storage::Result<QJsonObject>(WireKind, const QJsonObject&)> execute_;
        // Cleanup may call the old viewer's SDK. Arm only resets a backend-owned
        // event and is called after cleanup and the cancellation latch check.
        std::function<void(WireKind)> prepare_;
        std::function<storage::Result<HANDLE>(WireKind)> arm_;
        std::function<void(WireKind, bool)> finish_;
        std::function<void()> cleanup_;
        std::function<void()> unload_;
    };
    int RunViewerHostRuntime(HANDLE Pipe, HANDLE Parent, DWORD ParentId, const WireFrame& Hello,
        ViewerRuntimeCallbacks Callbacks);
}
