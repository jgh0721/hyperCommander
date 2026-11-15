#pragma once
/// 고유 라이브러리
#include "registryModels.hpp"
/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QVariantMap>
#include <QSet>

namespace hc::registry {
struct RegistryResultRow
{
    QString id;
    QStringList groups, cells;
    std::optional<RegistryLocation> location;
    std::optional<RegistryValue> value;
    QVariantMap payload;
    QString searchText;
};
// Shared grouped-result adapter for policies, autoruns and search results.
// The worker supplies immutable rows; model accessors perform no I/O.
class RegistryResultModel final : public QAbstractItemModel
{
    Q_OBJECT
public:
    enum Role { PayloadRole = Qt::UserRole + 701, SearchRole, GroupRole };
    explicit RegistryResultModel(QStringList Headers, QObject* Parent = nullptr);
    ~RegistryResultModel() override;
    void SetRows(QList<RegistryResultRow> Rows);
    void AppendRows(QList<RegistryResultRow> Rows);
    QModelIndex Find(const QString& Id) const;
    std::optional<RegistryResultRow> Row(const QModelIndex& Index) const;
    QModelIndex index(int Row, int Column, const QModelIndex& Parent = {}) const override;
    QModelIndex parent(const QModelIndex& Index) const override;
    int rowCount(const QModelIndex& Parent = {}) const override;
    int columnCount(const QModelIndex& Parent = {}) const override;
    QVariant data(const QModelIndex& Index, int Role = Qt::DisplayRole) const override;
    QVariant headerData(int Section, Qt::Orientation Orientation, int Role = Qt::DisplayRole) const override;
    Qt::ItemFlags flags(const QModelIndex& Index) const override;
private:
    struct Node { Node* parent = nullptr; int row = 0; QString group, id; std::optional<RegistryResultRow> result; std::vector<std::unique_ptr<Node>> children; };
    Node* node(const QModelIndex& Index) const;
    std::unique_ptr<Node> root_;
    QStringList headers_;
    QHash<QString, Node*> groups_;
    QSet<QString> ids_;
    QHash<QString, Node*> nodes_;
};
} // namespace hc::registry
