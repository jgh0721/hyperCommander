#pragma once
#include "pluginProcess.hpp"
#include "UniqueLibs/document/documentHostBroker.hpp"
namespace hc::plugins
{
    // One worker owns the negotiated workspace and all of its source leases.
    class DocumentViewerClient final
    {
    public:
        explicit DocumentViewerClient(PluginProcess& Host) : host_(Host) {}
        storage::Result<storage::Unit> Hello(storage::OperationContext Context);
        storage::Result<QJsonObject> Call(QString Type, QJsonObject Payload = {}, QString Document = {}, QString Generation = {}, storage::OperationContext Context = {});
        storage::Result<QJsonObject> Execute(QJsonObject Request, storage::OperationContext Context = {});
        storage::Result<QJsonObject> Poll(storage::OperationContext Context = {});
        QString Bind(storage::EntrySnapshot Source, std::shared_ptr<storage::IWritableStorage> Writer,
            std::shared_ptr<storage::IMaterializedFileLease> Lease, QByteArray Hash = {});
        void Close() { broker_.Close(); }
    private:
        PluginProcess& host_;
        document::DocumentHostBroker broker_;
        QString workspace_;
        quint64 sequence_ = 0;
        QJsonObject state_;
    };
}
