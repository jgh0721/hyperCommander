#pragma once

/// 고유 라이브러리
#include "registryHistory.hpp"
/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QAbstractItemModel>

namespace hc::registry {
class RegistryHistoryModel final : public QAbstractItemModel
{
    Q_OBJECT
public:
    enum Column { Time, Action, Path, Change, ColumnCount };
    enum Role { RecordIdRole = Qt::UserRole + 501, StateRole, BackupRole };
    explicit RegistryHistoryModel(QObject* Parent = nullptr);
    void Reload(const QString& Directory, const QString& CallerSid = {}, const QString& MachineId = {});
    void SetRecords(QList<RegistryHistoryRecord> Records);
    QModelIndex index(int Row, int Column, const QModelIndex& Parent = {}) const override;
    QModelIndex parent(const QModelIndex& Index) const override;
    std::optional<RegistryStep> SelectedStep(const QModelIndex& Index) const;
    int rowCount(const QModelIndex& Parent = {}) const override;
    int columnCount(const QModelIndex& Parent = {}) const override;
    QVariant data(const QModelIndex& Index, int Role = Qt::DisplayRole) const override;
    QVariant headerData(int Section, Qt::Orientation Orientation, int Role = Qt::DisplayRole) const override;
    std::optional<RegistryHistoryRecord> Record(const QModelIndex& Index) const;
    std::optional<RegistryHistoryRecord> UndoCandidate() const;
    std::optional<QPair<RegistryHistoryRecord, RegistryHistoryRecord>> RedoCandidate() const;
    bool Loading() const { return loading_; }
    quint32 Error() const { return error_; }
signals:
    void Reloaded();
private:
    QList<RegistryHistoryRecord> records_;
    quint64 generation_ = 0;
    quint32 error_ = 0;
    bool loading_ = false;
};
}