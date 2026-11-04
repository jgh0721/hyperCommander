#pragma once
#include <QList>
#include <QMenu>
#include <QPointer>
#include <QString>
#include <QWidget>
class QAction;
namespace hc::document
{
    struct ViewUiContribution
    {
        QString viewId_ = "text", menuTitle_;
        // Legacy: a mode menu generated from these toolbar actions (views without menu_).
        QList<QAction*> menuActions_;
        // The view-owned mode menu (title from menuTitle_, which carries the mnemonic).
        QPointer<QMenu> menu_;
        // View items placed inside the workspace Edit menu (before "파일에서 찾기") and at the top of the View menu.
        QList<QAction*> editActions_, viewActions_;
        bool mainMenuRecommended_ = true, mainToolbarRecommended_ = true;
    };
}
