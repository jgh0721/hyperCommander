#include "stdafx.h"
#include "thumbnailModel.hpp"
#include "cmpView.hpp"
#include "builtInFsModel.hpp"
#include "UniqueLibs/quickFindHighlight.hpp"
#include "qtWorkspaceState.hpp"
#include <QtitanGrid.h>
#include <QTextLayout>
#include <fmfilelist/FileRoles.h>
#include <fmfilelist/FileGroups.h>

using namespace fm::filelist;
ThumbnailModel::ThumbnailModel(CGridView* Owner) : QAbstractTableModel(Owner), owner_(Owner), provider_(new ThumbnailProvider(this))
{
    rebuildTimer_.setSingleShot(true); rebuildTimer_.setInterval(0);
    connect(&rebuildTimer_, &QTimer::timeout, this, &ThumbnailModel::Rebuild);
    connect(provider_, &ThumbnailProvider::ready, this, [this](const QString& Path) {
        // A row may have moved while work completed. Match the current snapshot's
        // path, then the provider checks its mtime/size key on the next paint.
        for (int Row = 0; Row < tiles_.size(); ++Row)
            if (tiles_[Row].path_ == Path) emit dataChanged(index(Row, 0), index(Row, ColumnCount - 1), {ArtRole, ThumbnailRole});
        Repaint(); // Admit newly visible tiles after the bounded queue drains.
    });
    connect(provider_, &ThumbnailProvider::settingsChanged, this, &ThumbnailModel::Repaint);
}
int ThumbnailModel::rowCount(const QModelIndex& Parent) const { return Parent.isValid() ? 0 : tiles_.size(); }
int ThumbnailModel::columnCount(const QModelIndex& Parent) const { return Parent.isValid() ? 0 : ColumnCount; }
QModelIndex ThumbnailModel::SourceIndex(int Row) const { return Row >= 0 && Row < tiles_.size() ? QModelIndex(tiles_[Row].index_) : QModelIndex{}; }
void ThumbnailModel::Bind(QAbstractItemModel* Source)
{
    for (const auto& Connection : connections_) disconnect(Connection);
    connections_.clear();
    if (Source) {
        connections_.append(connect(Source, &QAbstractItemModel::modelAboutToBeReset, this, [this] {
            provider_->CancelRequests(); beginResetModel(); tiles_.clear(); endResetModel();
        }));
        connections_.append(connect(Source, &QAbstractItemModel::modelReset, this, &ThumbnailModel::Schedule));
        connections_.append(connect(Source, &QAbstractItemModel::layoutChanged, this, &ThumbnailModel::Schedule));
        connections_.append(connect(Source, &QAbstractItemModel::rowsInserted, this, &ThumbnailModel::Schedule));
        connections_.append(connect(Source, &QAbstractItemModel::rowsRemoved, this, &ThumbnailModel::Schedule));
        connections_.append(connect(Source, &QAbstractItemModel::dataChanged, this, &ThumbnailModel::Schedule));
    }
    Schedule();
}
void ThumbnailModel::Schedule() { if (enabled_) rebuildTimer_.start(); }
void ThumbnailModel::Rebuild()
{
    auto* Native = owner_->BaseView(); auto* Model = owner_->BaseModel();
    if (!Native || !Model) return;
    QVector<Tile> Tiles;
    for (int Row = 0; Row < Native->getRowCount(); ++Row) {
        const auto GridRow = Native->getRow(Row);
        if (GridRow.type() != Qtitan::GridRow::DataRow) continue;
        const auto Visible = GridRow.modelIndex(0);
        const auto* Proxy = qobject_cast<const QAbstractProxyModel*>(Visible.model());
        const auto Source = Proxy ? Proxy->mapToSource(Visible) : Visible;
        const auto Item = Model->GetFileInfo(Source);
        if (!Item) continue;
        Tile Value; Value.index_ = Visible; Value.name_ = Model->GetName(Source); Value.up_ = Value.name_ == "..";
        if (Item->storageEntry_) {
            Value.entry_ = *Item->storageEntry_; Value.key_ = hc::workspace::SelectionKey(Value.entry_);
            // Remote and removable local paths use the provider's opt-in policy.
            // Archive/plugin capabilities lack a safe local thumbnail contract.
            if (Value.entry_.locator_.kind_ == hc::storage::LocatorKind::Local || Value.entry_.locator_.kind_ == hc::storage::LocatorKind::Smb)
                Value.path_ = QString::fromStdString(Value.entry_.locator_.root_);
        }
        Tiles.append(std::move(Value));
    }
    bool Changed = Tiles.size() != tiles_.size();
    for (int Row = 0; !Changed && Row < Tiles.size(); ++Row) {
        const auto& A = Tiles[Row]; const auto& B = tiles_[Row];
        Changed = A.index_ != B.index_ || A.key_ != B.key_ || A.name_ != B.name_ || A.path_ != B.path_ ||
            A.entry_.modifiedUtcMs_ != B.entry_.modifiedUtcMs_ || A.entry_.size_ != B.entry_.size_;
    }
    if (Changed) {
        provider_->CancelRequests(); beginResetModel(); tiles_ = std::move(Tiles); endResetModel();
    }
    owner_->SyncThumbnailCursor(); Repaint();
}
QVariant ThumbnailModel::data(const QModelIndex& Index, int Role) const
{
    if (!Index.isValid() || Index.row() >= tiles_.size()) return {};
    const auto& Tile = tiles_[Index.row()];
    if (!Tile.index_.isValid()) return {};
    const auto& Entry = Tile.entry_;
    const bool Directory = Tile.up_ || Entry.directory_;
    const auto Extension = Directory ? QString{} : QFileInfo(Tile.name_).suffix();
    switch (Role) {
    case SearchHighlightRole: {
        QVariantList Ranges; const auto Query = owner_->property("hc.quickFind.text").toString();
        if (owner_->property("hc.quickFind.highlight").toBool() && !Tile.up_)
            for (const auto& Range : hc::quickfind::HighlightRanges(Tile.name_, Query, owner_->property("hc.quickFind.match").toString()))
                Ranges.append(QVariant(QVariantList{Range.first, Range.second}));
        return Ranges;
    }
    case Qt::DisplayRole: case FullNameRole: return Tile.name_;
    case Qt::ToolTipRole: return Tile.name_ + (Tile.path_.isEmpty() ? QString{} : "\n" + Tile.path_);
    case IsUpRole: return Tile.up_;
    case IsDirRole: return Directory;
    case KindRole: return int(Tile.up_ ? Kind::Up : Directory ? Kind::Folder : kindForExtension(Extension));
    case ExtRole: case BadgeRole: return Extension.toUpper();
    case HiddenRole: return Tile.index_.data(CFsModelT::USR_ROLE_HIDDENOPACITY).isValid() && Tile.index_.data(CFsModelT::USR_ROLE_HIDDENOPACITY).toDouble() < 1.0;
    case MarkedRole: {
        const auto Row = owner_->BaseView()->getRow(Tile.index_);
        return Row.isValid() && owner_->BaseView()->modelController()->isRowSelected(Row);
    }
    case Qt::DecorationRole: {
        const auto Icon = Tile.index_.data(Qt::DecorationRole);
        return Icon.metaType() == QMetaType::fromType<QPixmap>() ? QVariant::fromValue(QIcon(Icon.value<QPixmap>())) : Icon;
    }
    case Qt::ForegroundRole: case Qt::BackgroundRole: case Qt::FontRole: return Tile.index_.data(Role);
    case GroupStyleRole: {
        ResolvedGroupStyle Style;
        if (Tile.index_.data(CFSModel::FileColorOverrideRole).toBool()) {
            Style.groups.append("product");
            Style.textLight = Style.textDark = Tile.index_.data(Qt::ForegroundRole).value<QColor>();
            Style.backLight = Style.backDark = Tile.index_.data(Qt::BackgroundRole).value<QColor>();
            const auto Font = Tile.index_.data(Qt::FontRole).value<QFont>();
            Style.bold = Font.bold(); Style.italic = Font.italic(); Style.underline = Font.underline(); Style.strike = Font.strikeOut();
        }
        return QVariant::fromValue(Style);
    }
    case FilePathRole: return Tile.path_;
    case SizeBytesRole: return Directory ? qint64(-1) : qint64(Entry.size_);
    case SizeTextRole: return Directory ? QString{} : formatSize(Entry.size_);
    case ModifiedRole: return QDateTime::fromMSecsSinceEpoch(Entry.modifiedUtcMs_);
    case DateTextRole: return Entry.modifiedUtcMs_ ? QDateTime::fromMSecsSinceEpoch(Entry.modifiedUtcMs_).toString("yyyy-MM-dd") : QString{};
    case ArtRole: case ThumbnailRole: {
        if (Directory || !enabled_ || Tile.path_.isEmpty() || !provider_->isTarget(Extension))
            return Role == ArtRole ? QVariant(int(Art::None)) : QVariant{};
        const auto Time = QDateTime::fromMSecsSinceEpoch(Entry.modifiedUtcMs_);
        const auto Image = provider_->thumbnail(Tile.path_, Time, Entry.size_);
        if (Role == ThumbnailRole) return Image;
        return int(!Image.isNull() ? Art::Image : provider_->isPending(Tile.path_, Time, Entry.size_) ? Art::Loading : Art::None);
    }
    default: return {};
    }
}
bool ThumbnailModel::setData(const QModelIndex& Index, const QVariant& Value, int Role)
{
    if (Role != MarkedRole || !Index.isValid() || Index.row() >= tiles_.size() || tiles_[Index.row()].up_) return false;
    const auto Row = owner_->BaseView()->getRow(SourceIndex(Index.row()));
    if (!Row.isValid()) return false;
    owner_->BaseView()->selectRow(Row.rowIndex(), Value.toBool() ? Qtitan::Select : Qtitan::Unselect);
    emit dataChanged(index(Index.row(), 0), index(Index.row(), ColumnCount - 1), {MarkedRole});
    emit owner_->sigThumbnailSelectionChanged(); return true;
}
void ThumbnailModel::Repaint()
{
    if (!tiles_.isEmpty()) emit dataChanged(index(0, 0), index(tiles_.size() - 1, ColumnCount - 1));
}
void ThumbnailModel::SetEnabled(bool Enabled)
{
    if (enabled_ == Enabled && (Enabled || tiles_.isEmpty())) return;
    enabled_ = Enabled;
    if (!Enabled) {
        rebuildTimer_.stop(); provider_->clearCache();
        beginResetModel(); tiles_.clear(); endResetModel();
    } // The native grid owns identity/selection while inactive tiles release memory.
    else Schedule();
}
void ThumbnailModel::Apply(const hc::ui::ThumbnailSettings& Settings)
{
    provider_->setMethod(Settings.method_); provider_->setTargets(Settings.targets_);
    provider_->setSkipSlowVolumes(Settings.skipSlow_); provider_->setMaxThreads(Settings.threads_);
    provider_->SetCacheLimitMb(Settings.cacheMb_);
}
void ThumbnailModel::ClearCache() { provider_->clearCache(); }
int ThumbnailModel::FullNameLines(int Width, const QFont& Font) const
{
    int Maximum = 1;
    for (const auto& Tile : tiles_) {
        Maximum = std::max(Maximum, hc::ui::ThumbnailNameLines(Tile.name_, Width, Font));
    }
    return Maximum;
}
double ThumbnailModel::TargetPercent() const
{
    int Count = 0, Targets = 0;
    QVector<Tile> Candidates;
    if (owner_->GroupingActive()) {
        for (const auto& Source : owner_->SelectionCandidates()) {
            const auto Item = owner_->BaseModel()->GetFileInfo(Source);
            if (!Item || !Item->storageEntry_) continue;
            Tile Value; Value.entry_ = *Item->storageEntry_; Value.name_ = Item->Name_; Value.up_ = Value.name_ == "..";
            if (Value.entry_.locator_.kind_ == hc::storage::LocatorKind::Local || Value.entry_.locator_.kind_ == hc::storage::LocatorKind::Smb)
                Value.path_ = QString::fromStdString(Value.entry_.locator_.root_);
            Candidates.append(std::move(Value));
        }
    }
    const auto& Eligible = owner_->GroupingActive() ? Candidates : tiles_;
    for (const auto& Tile : Eligible) {
        if (Tile.up_ || Tile.entry_.directory_) continue;
        ++Count;
        const auto Extension = QFileInfo(Tile.name_).suffix();
        const auto Media = ThumbnailProvider::targetOf(Extension) & (ThumbnailProvider::Images | ThumbnailProvider::Videos);
        if (Media && !Tile.path_.isEmpty() && provider_->isTarget(Extension) &&
            (provider_->method() != ThumbnailProvider::Method::Builtin || isImageExtension(Extension)) &&
            (!provider_->skipsSlowVolumes() || !ThumbnailProvider::isSlowVolume(Tile.path_))) ++Targets;
    }
    return Count ? 100.0 * Targets / Count : 0;
}
