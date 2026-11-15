#include "stdafx.h"
#include "registryResultModel.hpp"

/// 모듈
/// UIs
/// 기타 헤더파일
/// 고유 라이브러리
/// 공통 라이브러리
/// 외부 라이브러리
/// 선언 헤더파일들
/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )

DECLARE_CMNLIBSV2_NAMESPACE
Q_LOGGING_CATEGORY(lcRegistryResults, "hc.registry.results")
namespace hc::registry {
RegistryResultModel::RegistryResultModel(QStringList Headers, QObject* Parent)
    : QAbstractItemModel(Parent), root_(std::make_unique<Node>()), headers_(std::move(Headers)) {}
RegistryResultModel::~RegistryResultModel() = default;
RegistryResultModel::Node* RegistryResultModel::node(const QModelIndex& Index) const
{
    return Index.isValid() ? static_cast<Node*>(Index.internalPointer()) : root_.get();
}
void RegistryResultModel::SetRows(QList<RegistryResultRow> Rows)
{
    beginResetModel(); nodes_.clear(); root_ = std::make_unique<Node>(); QHash<QString, Node*> Groups; QSet<QString> Ids;
    for (auto& Row : Rows) {
        if (Row.id.isEmpty() || Ids.contains(Row.id)) continue;
        Ids.insert(Row.id); Node* Parent = root_.get(); QString Path;
        for (const auto& Name : Row.groups) {
            Path += '/' + QString::fromLatin1(Name.toUtf8().toHex());
            if (auto* Existing = Groups.value(Path, nullptr)) { Parent = Existing; continue; }
            auto Item = std::make_unique<Node>(); Item->parent = Parent; Item->row = int(Parent->children.size());
            Item->group = Name; Item->id = "group:" + Path;
            auto* Raw = Item.get(); nodes_.insert(Item->id, Raw); Parent->children.push_back(std::move(Item)); Groups.insert(Path, Raw); Parent = Raw;
        }
        auto Item = std::make_unique<Node>(); Item->parent = Parent; Item->row = int(Parent->children.size());
        Item->id = Row.id; Item->result = std::move(Row); nodes_.insert(Item->id, Item.get()); Parent->children.push_back(std::move(Item));
    }
    groups_ = std::move(Groups); ids_ = std::move(Ids); endResetModel();
}
void RegistryResultModel::AppendRows(QList<RegistryResultRow> Rows)
{
    const auto Index = [this](Node* Item) { return Item == root_.get() ? QModelIndex() : createIndex(Item->row, 0, Item); };
    for (auto& Row : Rows) {
        if (Row.id.isEmpty() || ids_.contains(Row.id)) continue;
        ids_.insert(Row.id); Node* Parent = root_.get(); QString Path;
        for (const auto& Name : Row.groups) {
            Path += '/' + QString::fromLatin1(Name.toUtf8().toHex());
            if (auto* Existing = groups_.value(Path, nullptr)) { Parent = Existing; continue; }
            const int At = int(Parent->children.size()); beginInsertRows(Index(Parent), At, At);
            auto Item = std::make_unique<Node>(); Item->parent = Parent; Item->row = At; Item->group = Name; Item->id = "group:" + Path;
            auto* Raw = Item.get(); nodes_.insert(Item->id, Raw); Parent->children.push_back(std::move(Item)); groups_.insert(Path, Raw); endInsertRows(); Parent = Raw;
        }
        const int At = int(Parent->children.size()); beginInsertRows(Index(Parent), At, At);
        auto Item = std::make_unique<Node>(); Item->parent = Parent; Item->row = At; Item->id = Row.id; Item->result = std::move(Row);
        nodes_.insert(Item->id, Item.get()); Parent->children.push_back(std::move(Item)); endInsertRows();
    }
}
QModelIndex RegistryResultModel::Find(const QString& Id) const
{
    const auto* Item = nodes_.value(Id, nullptr);
    return Item ? createIndex(Item->row, 0, const_cast<Node*>(Item)) : QModelIndex();
}
std::optional<RegistryResultRow> RegistryResultModel::Row(const QModelIndex& Index) const
{
    return Index.isValid() && Index.model() == this ? node(Index)->result : std::nullopt;
}
QModelIndex RegistryResultModel::index(int Row, int Column, const QModelIndex& Parent) const
{
    if (Row < 0 || Column < 0 || Column >= headers_.size() || (Parent.isValid() && (Parent.model() != this || Parent.column() != 0))) return {};
    const auto* ParentNode = node(Parent); if (Row >= int(ParentNode->children.size())) return {};
    return createIndex(Row, Column, ParentNode->children[Row].get());
}
QModelIndex RegistryResultModel::parent(const QModelIndex& Index) const
{
    if (!Index.isValid() || Index.model() != this) return {};
    const auto* Parent = node(Index)->parent; if (!Parent || Parent == root_.get()) return {};
    return createIndex(Parent->row, 0, const_cast<Node*>(Parent));
}
int RegistryResultModel::rowCount(const QModelIndex& Parent) const
{
    if (Parent.isValid() && (Parent.model() != this || Parent.column() != 0)) return 0;
    return int(node(Parent)->children.size());
}
int RegistryResultModel::columnCount(const QModelIndex&) const { return headers_.size(); }
QVariant RegistryResultModel::data(const QModelIndex& Index, int RoleValue) const
{
    if (!Index.isValid() || Index.model() != this) return {};
    const auto* Item = node(Index);
    if (RoleValue == GroupRole) return !Item->result.has_value();
    if (RoleValue == RegistryTreeModel::IdRole) return Item->id;
    if (RoleValue == RegistryTreeModel::KindRole)
        return int(!Item->result ? RegistryTreeModel::Kind::Status : Item->result->value ? RegistryTreeModel::Kind::Value : RegistryTreeModel::Kind::Key);
    if (!Item->result) {
        if (RoleValue == Qt::DisplayRole || RoleValue == RegistryTreeModel::GridSortRole) return Index.column() == 0 ? Item->group : QString();
        if (RoleValue == SearchRole) return Item->group;
        return {};
    }
    const auto& Row = *Item->result;
    if (RoleValue == Qt::DisplayRole || RoleValue == RegistryTreeModel::GridSortRole) return Row.cells.value(Index.column());
    if (RoleValue == Qt::ToolTipRole) return Row.cells.value(Index.column());
    if (RoleValue == PayloadRole) return Row.payload;
    if (RoleValue == SearchRole) return Row.searchText + '\n' + Row.cells.join('\n');
    if (RoleValue == RegistryTreeModel::LocationRole) return Row.location ? Row.location->Path() : QString();
    if (RoleValue == RegistryTreeModel::ValueNameRole) return Row.value ? QVariant(Row.value->name) : QVariant();
    if (RoleValue == RegistryTreeModel::ValueTypeRole) return Row.value ? QVariant(Row.value->type) : QVariant();
    if (RoleValue == RegistryTreeModel::RawValueRole) return Row.value ? QVariant(Row.value->bytes) : QVariant();
    return {};
}
QVariant RegistryResultModel::headerData(int Section, Qt::Orientation Orientation, int Role) const
{
    return Orientation == Qt::Horizontal && Role == Qt::DisplayRole ? headers_.value(Section) : QVariant();
}
Qt::ItemFlags RegistryResultModel::flags(const QModelIndex& Index) const
{
    return Index.isValid() ? Qt::ItemIsEnabled | Qt::ItemIsSelectable : Qt::NoItemFlags;
}
} // namespace hc::registry
