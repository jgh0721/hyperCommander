#pragma once

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QObject>
#include <QPointer>
#include <QPersistentModelIndex>
namespace Qtitan { class TreeGrid; class GridTreeView; }
namespace fm::ui { class InlineNameEditor; }
// Product-side adapter: all editing chrome comes from fm; the grid supplies only
// cell geometry and a persistent index. No Qtitan implementation is copied.
class RegistryNameOverlay final : public QObject
{
    Q_OBJECT
public:
    explicit RegistryNameOverlay(Qtitan::TreeGrid* Grid, Qtitan::GridTreeView* View);
    bool Begin(const QModelIndex& Index, const QString& Name);
    void Finish(const QString& Error = {});
    void Cancel();
    bool Active() const;
    bool Pending() const { return pending_; }
signals:
    void CommitRequested(const QString& Name);
protected:
    bool eventFilter(QObject* Watched, QEvent* Event) override;
private:
    void position();
    QPointer<Qtitan::TreeGrid> grid_;
    Qtitan::GridTreeView* view_;
    QPointer<fm::ui::InlineNameEditor> editor_;
    QPersistentModelIndex index_;
    bool pending_ = false, positioning_ = false, initialFocus_ = false;
};
