#pragma once
#include "commonLibs/plugins/archiveBackend.hpp"
#include <QJsonObject>
namespace hc::plugins
{
struct ImageArchiveReply
{
    QJsonObject json_;
    QByteArray bytes_;
};
storage::Result<ImageArchiveReply> CallImageArchive(const QJsonObject &Endpoint, QJsonObject Request,
                                                    storage::OperationContext Context = {});
std::shared_ptr<IArchiveBackend> MakeImageArchiveClient(QJsonObject Endpoint);
} // namespace hc::plugins
