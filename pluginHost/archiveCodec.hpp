#pragma once
#include "commonLibs/plugins/archiveBackend.hpp"
#include <QJsonObject>

namespace hc::plugins
{
    QJsonObject EncodeArchiveHeader(const ArchiveHeader& Header);
    storage::Result<ArchiveHeader> DecodeArchiveHeader(const QJsonObject& Object);
    QJsonObject EncodeArchiveUpdate(const ArchiveUpdate& Update);
    storage::Result<ArchiveUpdate> DecodeArchiveUpdate(const QJsonObject& Object);
    QJsonObject EncodeContext(const storage::OperationContext& Context, unsigned MaximumMilliseconds = 60000);
    storage::Result<storage::OperationContext> DecodeContext(const QJsonObject& Object);
    storage::Result<std::uint64_t> WireUnsigned(const QJsonValue& Value);
}
