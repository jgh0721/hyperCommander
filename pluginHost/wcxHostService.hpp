#pragma once
#include "archiveCodec.hpp"
#include "commonLibs/plugins/pluginWire.hpp"

namespace hc::plugins
{
    class WcxHostService final
    {
    public:
        WcxHostService(std::string Binary, bool Write);
        storage::Result<QJsonObject> Handle(WireKind Kind, const QJsonObject& Request);
    private:
        std::shared_ptr<IArchiveBackend> backend_;
        bool write_;
        std::map<std::uint64_t, std::unique_ptr<IArchiveListing>> listings_;
        std::uint64_t serial_ = 0, upload_ = 0, uploadSize_ = 0;
        QByteArray data_, hash_;
    };
}
