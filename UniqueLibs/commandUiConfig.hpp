#pragma once
#include "commandMgr_Defs.hpp"
#include <QJsonObject>
#include <QJsonArray>
#include <QKeySequence>
#include <QStringList>
#include <QKeyEvent>
#include <functional>
#include <algorithm>

namespace hc::ui {
// IDs are stable across menus, buttons and persisted shortcut aliases. A single
// command can have several default keys, but only one UI action.
inline QString CommandId( const QString& Name )
{
    return hc::defaults::CommandId( Name );
}

inline TyHC_COMMAND FindCommand(const QString& Name)
{
    const auto Id = CommandId(Name);
    for (const auto& Command : hc::defaults::Commands())
        if (CommandId(Command.Name) == Id) return Command;
    return {};
}

inline TyMapShortcutToCMDStr DefaultShortcuts(const QJsonObject& General)
{
    TyMapShortcutToCMDStr Values;
    if (!General["useBuiltInShortcuts"].toBool(true)) return Values;
    for (const auto& Command : hc::defaults::Commands())
        if (Command.Default.key() != Qt::Key_unknown) Values[Command.Default] = Command.Name;
    const auto Changes = hc::defaults::ShortcutPresetChanges(General["shortcutPreset"].toString(hc::defaults::ExistingShortcutPreset));
    for (auto It = Changes.cbegin(); It != Changes.cend(); ++It) {
        if (It.value().isEmpty()) Values.remove(It.key());
        else {
            const auto Command = FindCommand(It.value());
            if (!Command.Name.isEmpty()) Values[It.key()] = Command.Name;
        }
    }
    return Values;
}

inline TyMapShortcutToCMDStr EffectiveShortcuts(const QJsonObject& General, const QJsonObject& Overrides)
{
    auto Values = DefaultShortcuts(General);
    for (auto It = Overrides.begin(); It != Overrides.end(); ++It) {
        const auto Key = QKeySequence::fromString(It.key(), QKeySequence::PortableText);
        if (Key.count() != 1 || Key[0].key() == Qt::Key_unknown) continue;
        if (It.value().isNull()) { Values.remove(Key[0]); continue; }
        const auto Command = FindCommand(It.value().toString());
        if (!Command.Name.isEmpty()) Values[Key[0]] = Command.Name;
    }
    return Values;
}

inline QStringList FunctionCommands(const QJsonObject& General) {
    const auto Defaults = hc::defaults::FunctionCommands();
    const auto Items = General["functionCommands"].toArray(); if (Items.size() != 9) return Defaults;
    QStringList Result; for (const auto I : Items) { if (!I.isString() || FindCommand(I.toString()).Name.isEmpty()) return Defaults; Result.append(CommandId(I.toString())); } return Result;
}

inline QString FunctionCaption(const QJsonObject& General, int Slot, const QString& Id, const QString& Fallback)
{
    const auto Captions = General["functionCaptions"].toArray();
    if (Slot >= 0 && Slot < Captions.size() && Captions[Slot].isString() && !Captions[Slot].toString().trimmed().isEmpty())
        return Captions[Slot].toString();
    static const auto Labels = hc::defaults::FunctionCaptions();
    return Labels.value(CommandId(Id), Fallback);
}

inline QList<QKeySequence> CommandKeys(const TyMapShortcutToCMDStr& Values, const QString& Id)
{
    QList<QKeySequence> Keys;
    for (auto It = Values.cbegin(); It != Values.cend(); ++It)
        if (CommandId(It.value()) == CommandId(Id)) Keys.append(QKeySequence(It.key()));
    const auto Primary = FindCommand(Id).Default;
    std::sort(Keys.begin(), Keys.end(), [Primary](const auto& Left, const auto& Right) {
        // A plain function key is the representative when aliases coexist.
        const bool LeftFunction = Left[0].keyboardModifiers() == Qt::NoModifier && Left[0].key() >= Qt::Key_F1 && Left[0].key() <= Qt::Key_F35;
        const bool RightFunction = Right[0].keyboardModifiers() == Qt::NoModifier && Right[0].key() >= Qt::Key_F1 && Right[0].key() <= Qt::Key_F35;
        if (LeftFunction != RightFunction) return LeftFunction;
        if ((Left[0] == Primary) != (Right[0] == Primary)) return Left[0] == Primary;
        return Left.toString(QKeySequence::PortableText) < Right.toString(QKeySequence::PortableText);
    });
    return Keys;
}

inline QString ShortcutCommand(const QKeyEvent& Event, const TyMapShortcutToCMDStr& Values)
{
    auto Key = Event.keyCombination();
    if (Values.contains(Key)) return CommandId(Values.value(Key));
    // Windows marks navigation keys from a numeric keypad as KeypadModifier.
    // Use the configured navigation shortcut unless the keypad has its own one.
    if (Key.keyboardModifiers().testFlag(Qt::KeypadModifier)) {
        switch (Key.key()) {
        case Qt::Key_Home: case Qt::Key_End: case Qt::Key_PageUp: case Qt::Key_PageDown:
        case Qt::Key_Left: case Qt::Key_Right: case Qt::Key_Up: case Qt::Key_Down:
            Key = QKeyCombination(Key.keyboardModifiers() & ~Qt::KeypadModifier, Key.key());
            break;
        default: break;
        }
    }
    return CommandId(Values.value(Key));
}

inline bool InvokeShortcut(const QKeyEvent& Event, const TyMapShortcutToCMDStr& Values,
    const std::function<bool(const QString&)>& Invoke)
{
    const auto Command = ShortcutCommand(Event, Values);
    return !Command.isEmpty() && Invoke(Command);
}

// Command-edit bindings are contextual: panel Enter/Shift+Enter keep their
// existing actions. Explicit user overrides (including null) take precedence.
inline QString CommandLineShortcut(const QKeyEvent& Event, const TyMapShortcutToCMDStr& Values,
    const QJsonObject& Overrides)
{
    if (Event.matches(QKeySequence::Copy) || Event.matches(QKeySequence::Cut)
        || Event.matches(QKeySequence::Paste) || Event.matches(QKeySequence::SelectAll)
        || Event.matches(QKeySequence::Undo) || Event.matches(QKeySequence::Redo)) return {};
    auto Key = Event.keyCombination();
    const auto OriginalName = QKeySequence(Key).toString(QKeySequence::PortableText);
    if (Event.key() == Qt::Key_Enter && !Overrides.contains(OriginalName))
        Key = QKeyCombination(Event.modifiers() & ~Qt::KeypadModifier, Qt::Key_Return);
    auto Id = CommandId(Values.value(Key));
    if (!Overrides.contains(QKeySequence(Key).toString(QKeySequence::PortableText))) {
        if (Key == QKeyCombination(Qt::Key_Return)) Id = "hc.commandLine.execute";
        if (Key == QKeyCombination(Qt::ShiftModifier, Qt::Key_Return)) Id = "hc.commandLine.keepOpen";
    }
    return Id == "hc.commandLine.execute" || Id == "hc.commandLine.keepOpen" ? Id : QString{};
}

inline QString CommandLinePrompt(const QString& Path, int MaxCharacters = 36)
{
    const auto Full = Path.endsWith(u'>') ? Path : Path + u'>';
    const auto Characters = Full.toUcs4();
    MaxCharacters = qMax(8, MaxCharacters);
    if (Characters.size() <= MaxCharacters) return Full;
    const auto Prefix = qMin(12, (MaxCharacters - 3) / 2);
    const auto Suffix = MaxCharacters - 3 - Prefix;
    // Count Unicode code points so an emoji/supplementary character is intact.
    return QString::fromUcs4(Characters.constData(), Prefix) + "..."
        + QString::fromUcs4(Characters.constData() + Characters.size() - Suffix, Suffix);
}
} // namespace hc::ui
