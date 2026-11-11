#pragma once
#include "pluginProcess.hpp"
#include "pipeChannel.hpp"
#include <thread>
#include <mutex>

namespace hc::plugins
{
    // Each pipe has exactly one I/O owner for its entire negotiated lifetime.
    // A caller owns a completion subscription, never a ReadFile operation.
    class ViewerRuntimeClient final
    {
    public:
        ViewerRuntimeClient(HANDLE Pipe, HANDLE Control, HANDLE Peer, HANDLE Job, QString Epoch, HostLimits Limits);
        ~ViewerRuntimeClient();
        storage::Result<QJsonObject> Call(WireKind Kind, std::uint64_t Sequence, const QByteArray& Payload, storage::OperationContext Context);
        storage::Result<QJsonObject> AwaitPending(storage::OperationContext Context, WireKind* Kind);
        HostSnapshot Snapshot() const;
        std::function<HostSnapshot()> Observe() const;
        std::function<void()> ForceStopSource() const;
        void Close();
    private:
        struct State;
        std::shared_ptr<State> state_;
        std::mutex closeMutex_;
        std::jthread command_, control_;
    };
}
