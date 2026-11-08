#pragma once
#include "commonLibs/document/documentRecovery.hpp"
#include <QString>
#include <QJsonObject>
#include <QStringList>

namespace hc::document
{
    class DocumentRecoveryStore final : public IRecoveryStore
    {
    public:
        explicit DocumentRecoveryStore(QString Root) : root_(std::move(Root)) {}
        storage::Result<RecoverySnapshot> Store(RecoverySnapshot Snapshot) override;
        storage::Result<std::vector<RecoverySnapshot>> List() override;
        storage::Result<RecoverySnapshot> Read(const std::string& Path) override;
        storage::Result<storage::Unit> Remove(const DocumentId& Id, std::uint64_t ThroughRevision) override;
        storage::Result<storage::Unit> RemoveThrough(const DocumentId& Id, std::uint64_t Generation, std::uint64_t Revision);
        storage::Result<storage::Unit> StoreSession(const QString& Id, const QJsonObject& Session);
        storage::Result<QJsonObject> ReadSession(const QString& Id) const;
        QStringList Sessions() const;
        QString SessionLockPath(const QString& Id) const;
        bool RemoveSession(const QString& Id);
        bool IsRetired(const DocumentSnapshot& Document) const;
    private:
        QString root_;
    };
}
