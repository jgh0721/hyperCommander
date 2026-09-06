#pragma once
#include <QObject>
#include <QPointer>
#include <QTimer>
#include "columnWidthSettings.hpp"

class QWidget;
struct ColumnView;
namespace Qtitan { class Grid; class GridBandedTableView; }

// Keep every vendor-specific width dependency in the implementation file.
class QtitanColumnAdapter final : public QObject
{
public:
    QtitanColumnAdapter(QWidget* Owner, Qtitan::Grid* Grid, Qtitan::GridBandedTableView* View);
    void Bind(const ColumnView& Columns);
    void Request();
    void Apply();
    void Reload();
    void ShowSettings();
    bool SetPolicy(const QString& Id, const hc::view::ColumnWidthPolicy& Policy, bool Persist = true);
    const QMap<QString, hc::view::ColumnWidthPolicy>& Policies() const { return settings_.columns_; }
    QString ColumnId(int ModelColumn) const { return ids_.value(ModelColumn); }
    int AvailableWidth() const;
    quint64 ApplyCount() const { return applyCount_; }
    qint64 LastApplyNanoseconds() const { return lastApplyNs_; }
protected:
    bool eventFilter(QObject* Watched, QEvent* Event) override;
private:
    void captureDrag();
    bool save();
    QPointer<QWidget> owner_;
    Qtitan::Grid* grid_;
    Qtitan::GridBandedTableView* view_;
    QTimer timer_;
    QTimer dragTimer_;
    QMap<int, QString> ids_;
    QMap<QString, int> legacy_;
    QMap<QString, int> applied_;
    hc::view::WidthSettings settings_;
    QString profile_;
    QString viewId_;
    bool applying_ = false;
    bool dragging_ = false;
    int lastWidth_ = 0;
    quint64 applyCount_ = 0;
    qint64 lastApplyNs_ = 0;
};
