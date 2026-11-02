#include "stdafx.h"
#include "compareCommands.hpp"
#include "compareSettings.hpp"
#include <QAction>
#include <QShortcut>
#include <QVariant>
#include <QWidget>
#include <QKeyEvent>

namespace hc::compare
{
    QList<CompareCommand> CompareCommands()
    {
        return hc::defaults::compare::Commands();
    }

    QList<QKeySequence> CompareKeys(const QString& Id, const QJsonObject& Overrides)
    {
        if (Overrides.contains(Id)) { const auto value = Overrides[Id].toString(); return value.isEmpty() ? QList<QKeySequence>{} : QList<QKeySequence>{QKeySequence::fromString(value, QKeySequence::PortableText)}; }
        for (const auto& command : CompareCommands()) if (command.id_ == Id) return command.defaults_;
        return {};
    }
    QString CompareKeyError(const QKeySequence& Key)
    {
        if (Key.isEmpty()) return {};
        if (Key.count() != 1) return QObject::tr("한 번의 키 조합만 지정할 수 있습니다.");
        const auto key = Key[0].key(); const auto modifiers = Key[0].keyboardModifiers();
        if (key == Qt::Key_unknown || key == Qt::Key_Control || key == Qt::Key_Shift || key == Qt::Key_Alt || key == Qt::Key_Meta ||
            modifiers.testFlag(Qt::MetaModifier) || (modifiers.testFlag(Qt::ControlModifier) && modifiers.testFlag(Qt::AltModifier)) ||
            key == Qt::Key_Escape || key == Qt::Key_Tab || key == Qt::Key_Backtab || key == Qt::Key_Return || key == Qt::Key_Enter ||
            key == Qt::Key_Delete || key == Qt::Key_Backspace || (key >= Qt::Key_Home && key <= Qt::Key_PageDown) ||
            (key == Qt::Key_F4 && modifiers.testFlag(Qt::AltModifier)) || (key == Qt::Key_Space && modifiers.testFlag(Qt::AltModifier)) ||
            (!(modifiers & (Qt::ControlModifier | Qt::AltModifier)) && !(key >= Qt::Key_F1 && key <= Qt::Key_F35)))
            return QObject::tr("입력·탐색 또는 운영체제 예약 키입니다. 다른 조합을 사용하십시오.");
        if (hc::defaults::compare::ReservedEditingShortcuts().contains(Key)) return QObject::tr("기본 편집에 사용하는 키입니다.");
        return {};
    }
    QString ValidateCompareShortcuts(const QJsonObject& Overrides)
    {
        QMap<QKeySequence, QString> owners;
        // Directory child shortcuts overlap window shortcuts; validate conservatively across every command.
        for (const auto& command : CompareCommands()) for (const auto& key : CompareKeys(command.id_, Overrides))
        {
            if (Overrides.contains(command.id_)) if (const auto error = CompareKeyError(key); !error.isEmpty()) return command.label_ + ": " + error;
            if (key.isEmpty()) continue;
            if (owners.contains(key) && owners[key] != command.label_)
                return QObject::tr("%1: ‘%2’와 ‘%3’에서 사용 중입니다.").arg(key.toString(QKeySequence::NativeText), owners[key], command.label_);
            owners[key] = command.label_;
        }
        return {};
    }
    namespace
    {
        void apply(QObject* Object)
        {
            const auto id = Object->property("compareCommand").toString(); if (id.isEmpty()) return;
            const auto keys = CompareKeys(id, ActiveCompareSettings().shortcuts_);
            if (auto* action = qobject_cast<QAction*>(Object))
            {
                action->setShortcuts(keys);
                const auto base = action->property("compareBaseTip").toString(); QStringList labels;
                for (const auto& key : keys) labels.append(key.toString(QKeySequence::NativeText));
                action->setToolTip(base + (labels.isEmpty() ? QString{} : " (" + labels.join(", ") + ")"));
            }
            else if (auto* shortcut = qobject_cast<QShortcut*>(Object)) shortcut->setKeys(keys);
        }
    }
    void BindCompareCommand(QObject* Object, const QString& Id)
    {
        Object->setProperty("compareCommand", Id);
        if (auto* action = qobject_cast<QAction*>(Object)) action->setProperty("compareBaseTip", action->toolTip());
        apply(Object);
    }
    void ApplyCompareShortcuts(QObject* Root) { apply(Root); for (auto* child : Root->findChildren<QObject*>()) apply(child); }
    bool HandleCompareEditorKey(QWidget* Editor, QKeyEvent* Event, bool Execute)
    {
        const QKeySequence key(QKeyCombination(Event->modifiers() & ~Qt::KeypadModifier, Qt::Key(Event->key())));
        for (auto* action : Editor->window()->findChildren<QAction*>())
        {
            if (action->property("compareCommand").toString().isEmpty() || !action->isEnabled() || !action->shortcuts().contains(key)) continue;
            if (Execute && (!Event->isAutoRepeat() || action->autoRepeat())) action->trigger();
            Event->accept(); return true;
        }
        // A remapped or cleared Undo/Redo must also stop Scintilla's old default binding.
        for (const auto& entry : {std::pair{"edit.undo", QKeySequence::Undo}, {"edit.redo", QKeySequence::Redo}})
            if (ActiveCompareSettings().shortcuts_.contains(entry.first) && Event->matches(entry.second) &&
                !CompareKeys(entry.first, ActiveCompareSettings().shortcuts_).contains(key)) { Event->accept(); return true; }
        return false;
    }
}
