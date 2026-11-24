#pragma once
#include "pluginProcess.hpp"
#include "UniqueLibs/document/documentHostBroker.hpp"
#include <QSet>
#include <map>
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
        storage::Result<QJsonObject> Poll(storage::OperationContext Context, storage::IMaterializer* Materializer);
        QString Bind(const std::shared_ptr<storage::IContentSource>& Source, std::shared_ptr<storage::IWritableStorage> Writer,
            std::shared_ptr<storage::IMaterializedFileLease> Lease, QByteArray Hash, bool EnableArchiveNavigation);
        bool HasArchiveFolder(const QString& Token) const { return archiveImages_.contains(Token); }
        void Close() { broker_.Close(); }
    private:
        struct ArchiveFolder {
            std::shared_ptr<storage::IStorageProvider> provider_;
            storage::Locator location_;
            std::optional<std::vector<storage::EntrySnapshot>> images_;
        };
        struct ArchiveImage {
            std::shared_ptr<ArchiveFolder> folder_;
            storage::ResourceId id_;
        };
        storage::Result<QJsonObject> prepareArchiveImage(const QString& Token, int Direction, storage::IMaterializer& Materializer,
            storage::OperationContext Context);

        PluginProcess& host_;
        QSet<QString> imageExtensions_;
        std::map<QString, ArchiveImage> archiveImages_;
        document::DocumentHostBroker broker_;
        QString workspace_;
        quint64 sequence_ = 0;
        QJsonObject state_;
    };
}
