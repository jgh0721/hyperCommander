#pragma once
#include <QList>
#include <QString>
class QAction;
namespace hc::document
{
    struct ViewUiContribution
    {
        QString viewId_ = "text", menuTitle_;
        QList<QAction*> menuActions_;
        bool mainMenuRecommended_ = true, mainToolbarRecommended_ = true;
    };
}
