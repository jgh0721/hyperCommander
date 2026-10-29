#pragma once
#include <QList>
#include <QPointer>
#include <QString>
#include <QWidget>
class QAction;
namespace hc::document
{
    struct ViewUiContribution
    {
        QString viewId_ = "text", menuTitle_;
        QList<QAction*> menuActions_;
        bool mainMenuRecommended_ = true, mainToolbarRecommended_ = true;
        // Shown in the workspace status row while the view is active.
        QPointer<QWidget> statusWidget_;
    };
}
