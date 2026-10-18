#pragma once

#include <QAction>
#include <QStringList>

class QToolBar;

namespace ToolbarIcons {

struct ActionMeta
{
    QString key;
    QString iconName;
    QString toolTip;
    bool    checkable = false;
};

enum class TextBehavior {
    ClearText,
    PreserveText
};

QAction* createAction(QToolBar* toolBar, const QString& actionKey, const char* file, int line);
void     apply(QAction* action, const QString& actionKey, const char* file, int line,
               TextBehavior textBehavior = TextBehavior::ClearText);
void     refreshAction(QAction* action);
ActionMeta metaForActionKey(const QString& actionKey, const char* file, int line);
QIcon    iconForActionKey(const QString& actionKey, const char* file, int line);
QIcon    iconByName(const QString& iconName);
void     refreshToolBar(QToolBar* toolBar);
QStringList registeredActionKeys();

} // namespace ToolbarIcons

#define MV_CREATE_TOOLBAR_ACTION(toolbar, key) \
    ::ToolbarIcons::createAction((toolbar), QStringLiteral(key), __FILE__, __LINE__)

#define MV_APPLY_TOOLBAR_ACTION(action, key) \
    ::ToolbarIcons::apply((action), QStringLiteral(key), __FILE__, __LINE__)


