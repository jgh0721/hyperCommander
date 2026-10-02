#pragma once
#include "pipeChannel.hpp"
#include "commonLibs/storage/resourceQuota.hpp"
#include <QJsonObject>
#include <condition_variable>
#include <thread>

namespace hc::plugins
{
    QByteArray EncodeSessionPayload(const QJsonObject& Envelope, const QByteArray& Binary = {});
    storage::Result<std::pair<QJsonObject, QByteArray>> DecodeSessionPayload(const QByteArray& Payload);
    // One persistent SDK worker per approved process. Only Run owns pipe I/O;
    // the worker exchanges prompts/results through bounded in-memory slots.
    class HostSession final
    {
    public:
        using Handler = std::function<storage::Result<QByteArray>(WireKind, const QJsonObject&, const QByteArray&, storage::OperationContext)>;
        explicit HostSession(Handler Dispatch);
        ~HostSession();
        QString Epoch() const { return epoch_; }
        storage::Result<QByteArray> Invoke(std::function<storage::Result<QByteArray>()> Call);
        storage::Result<storage::Unit> Run(const WireFrame& Frame, HANDLE Pipe, HANDLE Parent);
    private:
        struct Work;
        void work(std::stop_token Stop);
        QString epoch_;
        Handler dispatch_;
        std::mutex mutex_;
        std::condition_variable_any changed_;
        std::shared_ptr<Work> pending_;
        storage::ResourceQuota quota_{{8 * WirePayloadLimit, 2 * WirePayloadLimit, 1, 64, UINT64_MAX}};
        std::jthread worker_;
    };
}
