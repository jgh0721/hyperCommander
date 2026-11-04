#pragma once
#include "UniqueLibs/ProductDefaults_Edit.hpp"
#include "viewUiContribution.hpp"
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <functional>
class QMenu;
class QMenuBar;
class QToolBar;
namespace hc::document
{
    class WorkspaceChromeController final : public QObject
    {
    public:
        enum class Part { Menu, Toolbar, Status };
        WorkspaceChromeController(QMenuBar* Menu, QToolBar* Toolbar, QObject* Parent);
        bool ApplySettings(const QJsonObject& Settings);
        QJsonObject Settings() const { return settings_; }
        void SetActive(ViewUiContribution Contribution);
        void SetSuppressed(bool Suppressed);
        void SetGlobal(bool Menu, const QString& Mode);
        void SetViewOverride(const QString& ViewId, bool Menu, const QString& Mode);
        // Visible/hidden for one part, globally or for the active view only ("이 보기에만 적용").
        void SetPart(Part Which, bool Visible, bool ActiveViewOnly);
        // "모든 보기 권장값으로": recommended globally and no per-view overrides.
        void ResetParts();
        bool MenuVisible() const;
        bool ToolbarVisible() const;
        bool StatusVisible() const;
        bool ActiveViewOnly() const { return settings_["applyToView"].toBool(); }
        void SetActiveViewOnly(bool Enabled);
        QString ActiveViewId() const { return active_.viewId_; }
        // Placement of the view-contributed parts inside the fixed workspace menus.
        void SetStatusWidget(QWidget* Status) { status_ = Status; apply(); }
        void SetModeMenuBefore(QAction* Anchor) { modeAnchor_ = Anchor; }
        void SetEditMenu(QMenu* Menu, QAction* Before) { editMenu_ = Menu; editAnchor_ = Before; }
        void SetViewMenu(QMenu* Menu, QAction* Before) { viewMenu_ = Menu; viewAnchor_ = Before; }
        void SetToolbarRowProvider(std::function<void(bool)> Provider) { toolbarRow_ = std::move(Provider); apply(); }
    private:
        bool visible(Part Which) const;
        void apply();
        void clearContributed();
        QMenuBar* menu_;
        QToolBar* toolbar_;
        QPointer<QWidget> status_;
        QPointer<QAction> modeAnchor_, editAnchor_, viewAnchor_;
        QPointer<QMenu> editMenu_, viewMenu_;
        QMenu* contributionMenu_ = nullptr;
        QPointer<QMenu> attachedMenu_;
        QList<QPointer<QAction>> contributedEdit_, contributedView_;
        ViewUiContribution active_;
        bool suppressed_ = false;
        QList<QPointer<QAction>> activeActions_;
        std::function<void(bool)> toolbarRow_;
        QJsonObject settings_ = hc::defaults::edit::Workspace();
    };
}
