#pragma once
#include "commonLibs/document/documentRecovery.hpp"
#include <QDateTime>
#include <QString>
#include <QJsonObject>
#include <QStringList>

namespace hc::document
{
    /// One document's preserved generations as files, without reading their content.
    struct StoredRecoveryDocument
    {
        DocumentId id_;
        QStringList files_;          // snapshot files and the P0 alias, names relative to the root
        qint64 bytes_ = 0;
        QDateTime newest_;           // the newest file's modification time
        std::uint64_t generation_ = 0, revision_ = 0;
    };

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
        /// Indexed documents and P0 aliases by file statistics only (retention and size accounting).
        storage::Result<std::vector<StoredRecoveryDocument>> Documents() const;
        /// Removes every generation, the alias and the deletion fence of documents nobody writes any more.
        storage::Result<storage::Unit> Drop(const QStringList& Ids);
        /// Copies another store's copies into this one: sessions of ProfileId (or without a profile) with their
        /// snapshots, and snapshots no other profile's session owns. Live sessions stay. The other store keeps
        /// its files and records the import in migrated.json (once per destination). Returns the document count.
        storage::Result<int> ImportFrom(const QString& Other, const QString& ProfileId);
        /// On an older store: removes the files every recorded import copied, once all imports are before Before.
        storage::Result<int> RetireImported(const QDateTime& Before);
        QString Root() const { return root_; }
    private:
        QString root_;
    };
}
