#pragma once

/// 고유 라이브러리
#include "registryService.hpp"
/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QAbstractItemModel>
#include <QPointer>
#include <vector>
#include <functional>

namespace hc::registry {
class RegistryTreeModel : public QAbstractItemModel
{
    Q_OBJECT
public:
    enum Column { Name, Type, Data, Description, ColumnCount };
    static constexpr int GridSortRole = Qt::UserRole + 65;
    enum Role { IdRole = Qt::UserRole + 301, KindRole, LocationRole, ValueNameRole, RawValueRole, ValueTypeRole, ErrorRole };
    enum class Kind { Key, Value, Status };
    explicit RegistryTreeModel(RegistryService* Service, bool IncludeValues, QObject* Parent = nullptr);
    ~RegistryTreeModel() override;
    QModelIndex index(int Row, int Column, const QModelIndex& Parent = {}) const override;
    QModelIndex parent(const QModelIndex& Index) const override;
    int rowCount(const QModelIndex& Parent = {}) const override;
    int columnCount(const QModelIndex& Parent = {}) const override;
    QVariant data(const QModelIndex& Index, int Role = Qt::DisplayRole) const override;
    QVariant headerData(int Section, Qt::Orientation Orientation, int Role = Qt::DisplayRole) const override;
    Qt::ItemFlags flags(const QModelIndex& Index) const override;
    bool hasChildren(const QModelIndex& Parent = {}) const override;
    bool canFetchMore(const QModelIndex& Parent) const override;
    void fetchMore(const QModelIndex& Parent) override;
    void SetView(View SelectedView);
    QModelIndex Find(const QString& Id) const;
    static QString ValueTypeLabel(quint32 Type);
    static QString ValuePreview(const RegistryValue& Value);
    static QString ValueId(const RegistryLocation& Location, const QString& Name);
    std::optional<RegistryLocation> Location(const QModelIndex& Index) const;
    void Refresh(const QModelIndex& Index);
    void SetSort(int Column, Qt::SortOrder Order);
    using DescriptionProvider = std::function<QString(const RegistryLocation&, const std::optional<QString>&, std::optional<quint32>)>;
    void SetDescriptions(DescriptionProvider Provider);
signals:
    void BatchAboutToChange();
    void BatchChanged();
private:
    struct Node {
        Node* parent = nullptr;
        Kind kind = Kind::Key;
        RegistryLocation location;
        KeyInfo info;
        RegistryValue value;
        QString id, status;
        quint32 error = 0;
        bool loaded = false;
        qlonglong sortRank = 0;
        std::vector<std::unique_ptr<Node>> children;
    };
    Node* node(const QModelIndex& Index) const;
    QModelIndex indexOf(const Node* Node, int Column = 0) const;
    void resetRoots();
    void apply(const QString& Id);
    void removeLookup(Node* Item);
    void updateRanks(Node* Parent);
    DescriptionProvider description_;
    QPointer<RegistryService> service_;
    bool includeValues_;
    View view_ = View::Native64;
    int sortColumn_ = Name;
    Qt::SortOrder sortOrder_ = Qt::AscendingOrder;
    std::unique_ptr<Node> root_;
    QHash<QString, Node*> nodes_;
};
class RegistryKeyTreeModel final : public RegistryTreeModel
{
public:
    explicit RegistryKeyTreeModel(RegistryService* Service, QObject* Parent = nullptr) : RegistryTreeModel(Service, false, Parent) {}
};
class RegistryItemsModel final : public RegistryTreeModel
{
public:
    explicit RegistryItemsModel(RegistryService* Service, QObject* Parent = nullptr) : RegistryTreeModel(Service, true, Parent) {}
};
} // namespace hc::registry
