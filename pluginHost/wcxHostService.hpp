#pragma once
#include "archiveCodec.hpp"
#include "commonLibs/plugins/pluginWire.hpp"

namespace hc::plugins
{
    class WcxHostService final
    {
    public:
        WcxHostService(std::string Binary, bool Write, std::string WriteFailure = "host-archive-write-not-approved");
        explicit WcxHostService(std::shared_ptr<IArchiveBackend> Backend, bool Write = false) : backend_(std::move(Backend)), write_(Write), writeFailure_("archive-read-only") {}
        storage::Result<QJsonObject> Handle(WireKind Kind, const QJsonObject& Request, storage::PasswordPrompt Prompt = {}, std::optional<storage::OperationContext> SessionContext = {});
        storage::Result<storage::ReadChunk> ReadStream(const QJsonObject& Request, storage::OperationContext Context);
        storage::Result<QJsonObject> AppendBinary(const QJsonObject& Request, const QByteArray& Bytes, storage::OperationContext Context);
    private:
        std::shared_ptr<IArchiveBackend> backend_;
        bool write_;
        std::string writeFailure_;
        std::map<std::uint64_t, std::unique_ptr<IArchiveListing>> listings_;
        std::map<std::uint64_t,std::unique_ptr<IArchiveReadSession>> readStreams_;
        std::uint64_t serial_ = 0, upload_ = 0, uploadSize_ = 0;
        QByteArray data_, hash_;
        std::optional<ArchiveUpdate> streamed_;
        std::uint64_t added_ = 0, removed_ = 0, origins_ = 0, reused_ = 0, received_ = 0;
    };
}
