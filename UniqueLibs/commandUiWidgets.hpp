#pragma once
#include <QAction>
#include <QPointer>
#include <fmwidgets/FunctionKeyBar.h>

namespace hc::ui {
inline void SetFunctionCaption(fm::ui::FunctionKeyButton* Button, QAction* Action, const QString& Caption)
{
    Button->setText(Caption);
    const auto Full = Action ? Action->property("hc.baseLabel").toString() : QString{};
    Button->setAccessibleName(Caption + (Full.isEmpty() || Full == Caption ? QString{} : " · " + Full) +
        (Button->keys().isEmpty() ? QString{} : " " + Button->keys()));
}
inline void BindFunctionButton(fm::ui::FunctionKeyButton* Button, QAction* Action)
{
    const QPointer<QAction> Command = Action;
    Button->setFocusPolicy(Qt::NoFocus);
    const auto Sync = [Button, Command] {
        if (!Command) { Button->setEnabled(false); return; }
        Button->setKeys(Command->property("hc.keyLabel").toString());
        SetFunctionCaption(Button, Command, Button->text());
        Button->setToolTip(Command->toolTip()); Button->setEnabled(Command->isEnabled());
    };
    Sync();
    QObject::connect(Action, &QAction::changed, Button, Sync);
    QObject::connect(Action, &QObject::destroyed, Button, [Button] { Button->setEnabled(false); });
    QObject::connect(Button, &QPushButton::clicked, Action, [Command] { if (Command) Command->trigger(); });
}
} // namespace hc::ui
