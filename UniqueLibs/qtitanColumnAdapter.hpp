#pragma once
#include <QObject>
#include <QPointer>
#include <QTimer>
#include "columnWidthSettings.hpp"
#include "commonLibs/columns/columnProfileStorage.hpp"

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
    hc::columns::ColumnProfile Profile() const;
    void UseProfileWidths();
    QString PaneKey() const;
    bool Writable() const { return unsupportedLayout_.isEmpty() && settings_.writable_; }
    QString LayoutMode() const { return layoutMode_; }
    bool SetLayoutMode(const QString& Mode);
    QJsonObject WorkspaceLayout() const;
    void RestoreWorkspaceLayout(const QJsonObject& View);
    void SetWorkspaceOverrides(const QJsonObject& Columns);
    QJsonObject WorkspaceOverrides() const;
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
    void activateLayout(const QString& Mode);
    QString profileForLayout(const QString& Mode) const;
    QMap<QString, hc::view::ColumnWidthPolicy> defaultsForLayout(const QString& Mode) const;
    std::optional<hc::view::ColumnWidthResult> resolveWidth(const QString& Id, int Available) const;
    QPointer<QWidget> owner_;
    Qtitan::Grid* grid_;
    Qtitan::GridBandedTableView* view_;
    QTimer timer_;
    QTimer dragTimer_;
    QMap<int, QString> ids_;
    QMap<QString, int> readableFloors_;
    QMap<QString, hc::view::ColumnWidthPolicy> defaults_;
    QMap<QString, hc::view::ColumnWidthPolicy> singleLineDefaults_;
    QMap<QString, int> applied_;
    hc::view::WidthSettings settings_;
    QString profile_;
    QString baseProfile_;
    QString layoutMode_ = "details";
    QJsonObject layoutWidths_;
    QJsonObject unsupportedLayout_;
    int nameColumn_ = -1;
    QString viewId_;
    hc::columns::ColumnProfile definition_;
    bool defaultPresentation_ = false;
    bool applying_ = false;
    bool dragging_ = false;
    bool workspaceOwned_ = false;
    QMap<QString, hc::view::ColumnWidthPolicy> inherited_;
    int lastWidth_ = 0;
    quint64 applyCount_ = 0;
    qint64 lastApplyNs_ = 0;
};
