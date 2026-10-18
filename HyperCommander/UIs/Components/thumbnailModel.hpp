#pragma once
#include <QAbstractTableModel>
#include <QPersistentModelIndex>
#include <QTimer>
#include <fmfilelist/ThumbnailProvider.h>
#include "thumbnailSettings.hpp"
#include "commonLibs/storage/storage.hpp"

class CGridView;
// The presentation model follows the native grid's sorted rows. Commands and marks
// still operate on that grid, so a tile never acquires a separate file selection.
class ThumbnailModel final : public QAbstractTableModel {
public:
    explicit ThumbnailModel(CGridView* Owner);
    int rowCount(const QModelIndex& Parent = {}) const override;
    int columnCount(const QModelIndex& Parent = {}) const override;
    QVariant data(const QModelIndex& Index, int Role = Qt::DisplayRole) const override;
    bool setData(const QModelIndex& Index, const QVariant& Value, int Role) override;
    void Bind(QAbstractItemModel* Source);
    void Rebuild();
    void Schedule();
    void SetEnabled(bool Enabled);
    void Apply(const hc::ui::ThumbnailSettings& Settings);
    void ClearCache();
    void Repaint();
    QModelIndex SourceIndex(int Row) const;
    int FullNameLines(int Width, const QFont& Font) const;
    double TargetPercent() const;
    fm::filelist::ThumbnailProvider* Provider() const { return provider_; }
private:
    struct Tile {
        QPersistentModelIndex index_;
        hc::storage::EntrySnapshot entry_;
        QString key_, name_, path_;
        bool up_ = false;
    };
    CGridView* owner_;
    fm::filelist::ThumbnailProvider* provider_;
    QVector<Tile> tiles_;
    QTimer rebuildTimer_;
    QVector<QMetaObject::Connection> connections_;
    bool enabled_ = false;
};
