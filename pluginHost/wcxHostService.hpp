#pragma once
#include "archiveCodec.hpp"
#include "commonLibs/plugins/pluginWire.hpp"

namespace hc::plugins
{
    class WcxHostService final
    {
    public:
        WcxHostService(std::string Binary, bool Write, std::string WriteFailure = "host-archive-write-not-approved");
        storage::Result<QJsonObject> Handle(WireKind Kind, const QJsonObject& Request, storage::PasswordPrompt Prompt = {}, std::optional<storage::OperationContext> SessionContext = {});
        storage::Result<QJsonObject> AppendBinary(const QJsonObject& Request, const QByteArray& Bytes, storage::OperationContext Context);
    private:
        std::shared_ptr<IArchiveBackend> backend_;
        bool write_;
        std::string writeFailure_;
        std::map<std::uint64_t, std::unique_ptr<IArchiveListing>> listings_;
        std::uint64_t serial_ = 0, upload_ = 0, uploadSize_ = 0;
        QByteArray data_, hash_;
    };
}
