#pragma once
#include "fileBatchOperations.hpp"

namespace hc::filetools {
enum class RenameState { Queued, Running, Completed, Restored, RecoveryRequired };
struct RenameRecord {
    QString id_, jobId_, folder_, parentId_, kind_ = "rename", error_;
    QByteArray folderIdentity_;
    qint64 createdUtcMs_ = 0;
    int count_ = 0;
    RenameState state_ = RenameState::Queued;
    QList<RenameRow> rows_;
    QStringList temporary_;
};
// Plans and write-ahead receipts belong to this configuration, not the install
// directory. A scheduler job owns the record lock for its entire lifetime.
QString RenameJournalDirectory();
storage::Result<RenameRecord> ReadRenameRecord(const QString& Directory, const QString& Id);
storage::Result<QList<RenameRecord>> ReadRenameHistory(const QString& Directory);
storage::Result<operations::PreparedJob> PrepareBatchRename(const QString& Directory,
    QList<RenameRow> Rows, const QString& ParentId = {}, const QString& Kind = "rename");
storage::Result<operations::PreparedJob> PrepareRenameUndo(const QString& Directory, const QString& Id);
storage::Result<operations::PreparedJob> PrepareRenameRecovery(const QString& Directory,
    const QString& Id, bool RestoreOriginal);
}
