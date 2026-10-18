#pragma once
#include "viewUiContribution.hpp"
#include <QJsonObject>
#include <QObject>
#include <QPointer>
class QMenu;
class QMenuBar;
class QToolBar;
namespace hc::document
{
    class WorkspaceChromeController final : public QObject
    {
    public:
        WorkspaceChromeController(QMenuBar* Menu, QToolBar* Toolbar, QObject* Parent);
        bool ApplySettings(const QJsonObject& Settings);
        QJsonObject Settings() const { return settings_; }
        void SetActive(ViewUiContribution Contribution);
        void SetGlobal(bool Menu, const QString& Mode);
        void SetViewOverride(const QString& ViewId, bool Menu, const QString& Mode);
        bool MenuVisible() const;
        bool ToolbarVisible() const;
    private:
        bool visible(bool Menu) const;
        void apply();
        QMenuBar* menu_;
        QToolBar* toolbar_;
        QMenu* contributionMenu_ = nullptr;
        ViewUiContribution active_;
        QList<QPointer<QAction>> activeActions_;
        QJsonObject settings_{{"schemaVersion", 1}, {"mainMenuMode", "recommended"},
            {"mainToolbarMode", "recommended"}, {"viewOverrides", QJsonObject{}}};
    };
}
