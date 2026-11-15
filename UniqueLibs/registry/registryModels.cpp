#include "stdafx.h"
#include "registryModels.hpp"

/// 모듈
/// UIs
/// 기타 헤더파일
/// 고유 라이브러리
/// 공통 라이브러리
/// 외부 라이브러리
/// 선언 헤더파일들
/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QSet>
#include <QCollator>
#include <QtEndian>

DECLARE_CMNLIBSV2_NAMESPACE
Q_LOGGING_CATEGORY(lcRegistryModels, "hc.registry.models")

namespace hc::registry {
namespace {
QString TypeName(quint32 Type)
{
    switch (Type) {
    case REG_NONE: return "REG_NONE"; case REG_SZ: return "REG_SZ"; case REG_EXPAND_SZ: return "REG_EXPAND_SZ";
    case REG_BINARY: return "REG_BINARY"; case REG_DWORD: return "REG_DWORD"; case REG_DWORD_BIG_ENDIAN: return "REG_DWORD_BIG_ENDIAN";
    case REG_LINK: return "REG_LINK"; case REG_MULTI_SZ: return "REG_MULTI_SZ"; case REG_QWORD: return "REG_QWORD";
    default: return QString("REG_%1").arg(Type);
    }
}
QString Preview(const RegistryValue& Value)
{
    if (Value.type == REG_DWORD && Value.bytes.size() == 4) {
        const auto Number = qFromLittleEndian<quint32>(Value.bytes.constData());
        return QString("0x%1 (%2)").arg(Number, 8, 16, QChar('0')).arg(Number);
    }
    if (Value.type == REG_QWORD && Value.bytes.size() == 8) {
        const auto Number = qFromLittleEndian<quint64>(Value.bytes.constData());
        return QString("0x%1 (%2)").arg(Number, 16, 16, QChar('0')).arg(Number);
    }
    if ((Value.type == REG_SZ || Value.type == REG_EXPAND_SZ || Value.type == REG_MULTI_SZ) && Value.bytes.size() % 2 == 0) {
        QString Text; const auto Count = std::min<qsizetype>(Value.bytes.size() / 2, 512);
        for (qsizetype I = 0; I < Count; ++I) Text += QChar(qFromLittleEndian<quint16>(Value.bytes.constData() + I * 2));
        while (Text.endsWith(QChar(0))) Text.chop(1);
        Text.replace(QChar(0), " · ");
        return Text + (Value.bytes.size() > 1024 ? QStringLiteral("…") : QString());
    }
    return QString::fromLatin1(Value.bytes.left(64).toHex(' ')) + (Value.bytes.size() > 64 ? QStringLiteral(" …") : QString());
}
}
RegistryTreeModel::RegistryTreeModel(RegistryService* Service, bool IncludeValues, QObject* Parent)
    : QAbstractItemModel(Parent), service_(Service), includeValues_(IncludeValues)
{
    resetRoots();
    connect(Service, &RegistryService::Changed, this, &RegistryTreeModel::apply);
}
RegistryTreeModel::~RegistryTreeModel() = default;
QString RegistryTreeModel::ValueTypeLabel(quint32 Type) { return TypeName(Type); }
QString RegistryTreeModel::ValuePreview(const RegistryValue& Value) { return Preview(Value); }
QString RegistryTreeModel::ValueId(const RegistryLocation& Location, const QString& Name)
{
    return Location.Id() + "/v/" + QString::fromLatin1(Name.toCaseFolded().toUtf8().toHex());
}
void RegistryTreeModel::resetRoots()
{
    nodes_.clear(); root_ = std::make_unique<Node>(); root_->loaded = true;
    for (const auto HiveValue : {Hive::ClassesRoot, Hive::CurrentUser, Hive::LocalMachine, Hive::Users, Hive::CurrentConfig}) {
        auto Item = std::make_unique<Node>(); Item->parent = root_.get(); Item->location.hive = HiveValue; Item->location.view = view_;
        Item->info.name = Item->location.Path(); Item->id = Item->location.Id();
        nodes_.insert(Item->id, Item.get()); root_->children.push_back(std::move(Item));
    }
}
RegistryTreeModel::Node* RegistryTreeModel::node(const QModelIndex& Index) const
{
    return Index.isValid() && Index.model() == this ? static_cast<Node*>(Index.internalPointer()) : root_.get();
}
QModelIndex RegistryTreeModel::indexOf(const Node* Item, int Column) const
{
    if (!Item || !Item->parent) return {};
    const auto& Siblings = Item->parent->children;
    for (int I = 0; I < int(Siblings.size()); ++I) if (Siblings[I].get() == Item) return createIndex(I, Column, const_cast<Node*>(Item));
    return {};
}
QModelIndex RegistryTreeModel::index(int Row, int Column, const QModelIndex& Parent) const
{
    if (Row < 0 || Column < 0 || Column >= columnCount() || (Parent.isValid() && Parent.column() != 0)) return {};
    const auto* Item = node(Parent); if (Row >= int(Item->children.size())) return {};
    return createIndex(Row, Column, Item->children[Row].get());
}
QModelIndex RegistryTreeModel::parent(const QModelIndex& Index) const { return Index.isValid() ? indexOf(node(Index)->parent) : QModelIndex(); }
int RegistryTreeModel::rowCount(const QModelIndex& Parent) const { return Parent.isValid() && Parent.column() != 0 ? 0 : int(node(Parent)->children.size()); }
int RegistryTreeModel::columnCount(const QModelIndex&) const { return includeValues_ ? ColumnCount : 1; }
QVariant RegistryTreeModel::data(const QModelIndex& Index, int Role) const
{
    if (!Index.isValid() || Index.model() != this) return {};
    const auto* Item = node(Index);
    switch (Role) {
    case GridSortRole: return QVariant::fromValue(Item->sortRank);
    case IdRole: return Item->id;
    case KindRole: return int(Item->kind);
    case LocationRole: return Item->location.Path();
    case ValueNameRole: return Item->kind == Kind::Value ? QVariant(Item->value.name) : QVariant();
    case RawValueRole: return Item->kind == Kind::Value ? QVariant(Item->value.bytes) : QVariant();
    case ValueTypeRole: return Item->kind == Kind::Value ? QVariant(Item->value.type) : QVariant();
    case ErrorRole: return Item->error;
    case Qt::ToolTipRole: return Item->kind == Kind::Value ? Item->value.Warning() : Item->status;
    case Qt::DisplayRole:
        if (Item->kind == Kind::Status) return Index.column() == Name ? Item->status : QVariant();
        switch (Index.column()) {
        case Name: return Item->kind == Kind::Key ? Item->info.name : (Item->value.name.isEmpty() ? tr("(기본값)") : Item->value.name);
        case Type: return Item->kind == Kind::Key ? (Item->info.link ? tr("링크 키") : tr("키")) : TypeName(Item->value.type);
        case Data: return Item->kind == Kind::Value ? Preview(Item->value) : QVariant();
        case Description: {
            const auto Warning = Item->kind == Kind::Value ? Item->value.Warning() : QString();
            const auto DescriptionText = description_ ? description_(Item->location,
                Item->kind == Kind::Value ? std::optional<QString>(Item->value.name) : std::nullopt,
                Item->kind == Kind::Value ? std::optional<quint32>(Item->value.type) : std::nullopt) : QString();
            return Warning.isEmpty() ? DescriptionText : DescriptionText.isEmpty() ? Warning : DescriptionText + " · " + Warning;
        }
        }
    }
    return {};
}
QVariant RegistryTreeModel::headerData(int Section, Qt::Orientation Orientation, int Role) const
{
    if (Orientation != Qt::Horizontal || Role != Qt::DisplayRole) return {};
    switch (Section) { case Name: return tr("이름"); case Type: return tr("형식"); case Data: return tr("데이터"); case Description: return tr("설명"); }
    return {};
}
Qt::ItemFlags RegistryTreeModel::flags(const QModelIndex& Index) const
{
    if (!Index.isValid() || node(Index)->kind == Kind::Status) return Qt::NoItemFlags;
    return Qt::ItemIsEnabled | Qt::ItemIsSelectable;
}
bool RegistryTreeModel::hasChildren(const QModelIndex& Parent) const
{
    if (Parent.isValid() && Parent.column() != 0) return false;
    const auto* Item = node(Parent);
    return !Item->children.empty() || (Item->kind == Kind::Key && (!Item->info.link || includeValues_) && !Item->loaded);
}
bool RegistryTreeModel::canFetchMore(const QModelIndex& Parent) const
{
    if (!service_ || !Parent.isValid() || Parent.column() != 0) return false;
    const auto* Item = node(Parent); if (Item->kind != Kind::Key || (Item->info.link && !includeValues_) || Item->loaded) return false;
    const auto* Cache = service_->Cached(Item->location); return !Cache || !Cache->pending;
}
void RegistryTreeModel::fetchMore(const QModelIndex& Parent)
{
    if (!canFetchMore(Parent)) return;
    const auto Key = node(Parent)->location;
    const auto* Cache = service_->Cached(Key);
    if (Cache && Cache->loaded) apply(Key.Id()); else service_->Request(Key);
}
void RegistryTreeModel::SetView(View SelectedView)
{
    if (view_ == SelectedView) return;
    beginResetModel(); view_ = SelectedView; resetRoots(); endResetModel();
}
QModelIndex RegistryTreeModel::Find(const QString& Id) const { return indexOf(nodes_.value(Id)); }
std::optional<RegistryLocation> RegistryTreeModel::Location(const QModelIndex& Index) const
{
    if (!Index.isValid() || node(Index)->kind == Kind::Status) return {};
    return node(Index)->location;
}
void RegistryTreeModel::Refresh(const QModelIndex& Index)
{
    const auto Key = Location(Index); if (Key && service_) service_->Request(*Key, true);
}
void RegistryTreeModel::removeLookup(Node* Item)
{
    for (auto& Child : Item->children) removeLookup(Child.get()); nodes_.remove(Item->id);
}
void RegistryTreeModel::SetDescriptions(DescriptionProvider Provider)
{
    description_ = std::move(Provider);
    emit BatchAboutToChange();
    for (auto* Item : nodes_) if (Item->kind == Kind::Key && !Item->children.empty()) {
        const auto Parent = indexOf(Item);
        emit dataChanged(index(0, Description, Parent), index(int(Item->children.size()) - 1, Description, Parent), {Qt::DisplayRole});
    }
    emit BatchChanged();
    if (sortColumn_ == Description) SetSort(sortColumn_, sortOrder_);
}
void RegistryTreeModel::SetSort(int Column, Qt::SortOrder Order)
{
    if (Column < 0 || Column >= columnCount()) return;
    sortColumn_ = Column; sortOrder_ = Order;
    emit BatchAboutToChange();
    updateRanks(root_.get());
    for (auto* Item : nodes_) if (Item->kind == Kind::Key) updateRanks(Item);
    emit BatchChanged();
}
void RegistryTreeModel::updateRanks(Node* Parent)
{
    QList<Node*> Sorted; for (auto& Item : Parent->children) Sorted.append(Item.get());
    QCollator Collator; Collator.setCaseSensitivity(Qt::CaseInsensitive); Collator.setNumericMode(true);
    std::sort(Sorted.begin(), Sorted.end(), [&](const Node* A, const Node* B) {
        // Kind priority is independent of direction: keys always precede values,
        // and loading/error rows are never mixed into the editable rows.
        if (A->kind != B->kind) return int(A->kind) < int(B->kind);
        // The computer root is a navigation list, with the conventional hive
        // order in both sort directions; sibling keys retain natural sorting.
        if (Parent == root_.get()) return int(A->location.hive) < int(B->location.hive);
        const auto Left = data(indexOf(A, sortColumn_)).toString(); const auto Right = data(indexOf(B, sortColumn_)).toString();
        const int Compared = Collator.compare(Left, Right);
        if (Compared != 0) return sortOrder_ == Qt::AscendingOrder ? Compared < 0 : Compared > 0;
        return A->id < B->id;
    });
    for (int I = 0; I < Sorted.size(); ++I) Sorted[I]->sortRank = sortOrder_ == Qt::AscendingOrder ? I : -I;
    if (!Sorted.isEmpty()) emit dataChanged(index(0, 0, indexOf(Parent)), index(int(Sorted.size()) - 1, columnCount() - 1, indexOf(Parent)), {GridSortRole});
}
void RegistryTreeModel::apply(const QString& Id)
{
    auto* Parent = nodes_.value(Id); if (!Parent || !service_) return;
    const auto* Cache = service_->Cached(Parent->location); if (!Cache) return;
    // Copy before emitting model signals: a connected view may request another
    // key and rehash the service cache during an insertion notification.
    const auto Snapshot = *Cache;
    std::vector<std::unique_ptr<Node>> Desired;
    if (Snapshot.loaded) {
        for (const auto& Key : Snapshot.snapshot.value.keys) {
            auto Item = std::make_unique<Node>(); Item->location = Parent->location.Child(Key.name); Item->id = Item->location.Id(); Item->info = Key;
            Desired.push_back(std::move(Item));
        }
        if (includeValues_) for (const auto& Value : Snapshot.snapshot.value.values) {
            auto Item = std::make_unique<Node>(); Item->kind = Kind::Value; Item->location = Parent->location; Item->value = Value;
            Item->id = ValueId(Parent->location, Value.name);
            Desired.push_back(std::move(Item));
        }
    }
    if (Snapshot.pending || (Snapshot.loaded && !Snapshot.snapshot)) {
        auto Item = std::make_unique<Node>(); Item->kind = Kind::Status; Item->location = Parent->location; Item->id = Id + "/status";
        Item->error = Snapshot.snapshot.error;
        Item->status = Snapshot.pending ? tr("불러오는 중…") : tr("읽기 실패 (%1) · 새로 고침으로 다시 시도").arg(Item->error);
        Desired.push_back(std::move(Item));
    }
    emit BatchAboutToChange();
    const auto ParentIndex = indexOf(Parent);
    QSet<QString> Keep; for (const auto& Item : Desired) Keep.insert(Item->id);
    for (int I = int(Parent->children.size()) - 1; I >= 0; --I) if (!Keep.contains(Parent->children[I]->id)) {
        beginRemoveRows(ParentIndex, I, I); removeLookup(Parent->children[I].get()); Parent->children.erase(Parent->children.begin() + I); endRemoveRows();
    }
    for (int I = 0; I < int(Desired.size()); ++I) {
        auto& Item = Desired[I]; int Existing = I;
        while (Existing < int(Parent->children.size()) && Parent->children[Existing]->id != Item->id) ++Existing;
        if (Existing == int(Parent->children.size())) {
            beginInsertRows(ParentIndex, I, I); Item->parent = Parent; nodes_.insert(Item->id, Item.get());
            Parent->children.insert(Parent->children.begin() + I, std::move(Item)); endInsertRows();
        } else {
            if (Existing != I) {
                beginMoveRows(ParentIndex, Existing, Existing, ParentIndex, I);
                auto Moving = std::move(Parent->children[Existing]); Parent->children.erase(Parent->children.begin() + Existing);
                Parent->children.insert(Parent->children.begin() + I, std::move(Moving)); endMoveRows();
            }
            auto* Current = Parent->children[I].get(); Current->info = Item->info; Current->value = Item->value;
            Current->location = Item->location; Current->status = Item->status; Current->error = Item->error;
            emit dataChanged(index(I, 0, ParentIndex), index(I, columnCount() - 1, ParentIndex));
        }
    }
    Parent->loaded = Snapshot.loaded; Parent->info.link = Snapshot.snapshot.value.info.link;
    updateRanks(Parent);
    emit BatchChanged();
}
} // namespace hc::registry
