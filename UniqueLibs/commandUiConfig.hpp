#pragma once
#include "UniqueLibs/productDefaults.hpp"
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

inline bool IsCommandLineShortcut(const QString& Id)
{
    return Id.compare("hc.commandLine.execute", Qt::CaseInsensitive) == 0
        || Id.compare("hc.commandLine.keepOpen", Qt::CaseInsensitive) == 0;
}

inline TyMapShortcutToCMDStr DefaultCommandLineShortcuts()
{
    TyMapShortcutToCMDStr Values;
    for (const auto& Command : {BuiltInCMD_CommandLine_Execute, BuiltInCMD_CommandLine_KeepOpen})
        Values[Command.Default] = Command.Name;
    return Values;
}

// 입력 범위. 명령행 입력처럼 포커스가 있는 보기의 키 맵을 전역 맵보다 먼저 찾는다.
// 범위 키 맵은 settings의 shortcuts 객체 안에 범위 이름으로 둔다. 이름은 키로 해석되지 않아야 한다.
enum class InputScope { Global, CommandLine, ProcessView, ServiceView };

inline QString ScopeKey(InputScope Scope)
{
    switch (Scope) {
    case InputScope::CommandLine: return QStringLiteral("commandLine");
    case InputScope::ProcessView: return QStringLiteral("processView");
    case InputScope::ServiceView: return QStringLiteral("serviceView");
    default: return {};
    }
}

inline InputScope ScopeFromKey(const QString& Key)
{
    if (Key == "commandLine") return InputScope::CommandLine;
    if (Key == "processView") return InputScope::ProcessView;
    if (Key == "serviceView") return InputScope::ServiceView;
    return InputScope::Global;
}

// 두 보기 범위가 함께 쓰는 명령. 키는 범위마다 따로 저장한다.
inline bool IsSharedViewCommand(const QString& Id)
{
    const auto Command = CommandId(Id);
    return Command == "hc.system.refreshPause" || Command == "hc.system.followLocation";
}

// 키를 보기 범위에만 두는 명령. hc.process.run은 전역에서도 실행할 수 있어 제외한다.
inline bool IsViewScopedCommand(const QString& Id)
{
    const auto Command = CommandId(Id);
    return (Command.startsWith("hc.process.") && Command != "hc.process.run") || Command.startsWith("hc.service.") || IsSharedViewCommand(Command);
}

// 프로세스·서비스 보기에서도 그대로 실행하는 전역 명령. 나머지(파일 작업)는 덮인 파일 목록에 닿지 않도록
// 보기가 키를 삼키고, 메뉴·도구 모음에서는 비활성으로 보인다.
inline bool IsSystemViewGlobalCommand(const QString& Id)
{
    const auto Command = CommandId(Id);
    static const QStringList Prefixes{"hc.tab.", "hc.system.", "hc.service.", "hc.process.", "hc.audio.", "hc.panel.",
                                      "hc.navigate.gotoDrive", "hc.commandLine.", "hc.app.", "hc.menu."};
    for (const auto& Prefix : Prefixes)
        if (Command.startsWith(Prefix)) return true;
    static const QStringList Exact{"hc.navigate.systemProcesses", "hc.navigate.systemServices",
                                   "hc.navigate.refresh", "hc.navigate.forceRefresh", "hc.navigate.back", "hc.navigate.forward",
                                   "hc.navigate.path", "hc.navigate.favorites", "hc.search.focus", "hc.search.everything",
                                   "hc.viewer.quick", "hc.viewer.close", "hc.settings.show", "hc.jobs.show", "hc.automation.show"};
    return Exact.contains(Command);
}

inline bool ScopeAccepts(InputScope Scope, const QString& Id)
{
    const auto Command = CommandId(Id);
    switch (Scope) {
    case InputScope::CommandLine: return IsCommandLineShortcut(Command);
    case InputScope::ProcessView: return Command.startsWith("hc.process.") || IsSharedViewCommand(Command);
    case InputScope::ServiceView: return Command.startsWith("hc.service.") || IsSharedViewCommand(Command);
    default: return !IsCommandLineShortcut(Command) && !IsViewScopedCommand(Command);
    }
}

inline InputScope HomeScope(const QString& Id)
{
    const auto Command = CommandId(Id);
    if (IsCommandLineShortcut(Command)) return InputScope::CommandLine;
    if (Command.startsWith("hc.service.")) return InputScope::ServiceView;
    if (IsViewScopedCommand(Command)) return InputScope::ProcessView;
    return InputScope::Global;
}

// 패널 전환·탭·검색·탐색 키는 보기 범위에서 다시 정의할 수 없다.
inline bool IsProtectedScopeKey(QKeyCombination Key)
{
    return hc::defaults::ProtectedScopeKeys().contains(QKeyCombination(Key.keyboardModifiers() & ~Qt::KeypadModifier, Key.key()));
}

inline TyMapShortcutToCMDStr DefaultScopeShortcuts(InputScope Scope)
{
    if (Scope == InputScope::CommandLine) return DefaultCommandLineShortcuts();
    TyMapShortcutToCMDStr Values;
    for (const auto& [Key, Id] : hc::defaults::ScopeShortcutDefaults(ScopeKey(Scope))) {
        const auto Command = FindCommand(Id);
        if (!Command.Name.isEmpty()) Values[Key] = Command.Name;
    }
    return Values;
}

inline QJsonObject CommandLineShortcutOverrides(const QJsonObject& Overrides)
{
    auto Scoped = Overrides["commandLine"].toObject();
    // Retain legacy execution-key customizations, but panel bindings (including
    // their explicit removal) never disable the command input's Enter keys.
    for (auto It = Overrides.begin(); It != Overrides.end(); ++It)
        if (It.value().isString() && IsCommandLineShortcut(It.value().toString()) && !Scoped.contains(It.key()))
            Scoped[It.key()] = It.value();
    return Scoped;
}

inline TyMapShortcutToCMDStr EffectiveCommandLineShortcuts(const QJsonObject& Overrides)
{
    // These are input-widget defaults, independent of the panel preset toggle.
    auto Values = DefaultCommandLineShortcuts();
    const auto Scoped = CommandLineShortcutOverrides(Overrides);
    for (auto It = Scoped.begin(); It != Scoped.end(); ++It) {
        const auto Key = QKeySequence::fromString(It.key(), QKeySequence::PortableText);
        if (Key.count() != 1 || Key[0].key() == Qt::Key_unknown) continue;
        if (It.value().isNull()) Values.remove(Key[0]);
        else if (IsCommandLineShortcut(It.value().toString())) Values[Key[0]] = FindCommand(It.value().toString()).Name;
    }
    return Values;
}

// 보기 범위의 실제 키 맵: 기본값 위에 사용자 지정(null은 해제)을 적용한다.
inline TyMapShortcutToCMDStr EffectiveScopeShortcuts(const QJsonObject& Overrides, InputScope Scope)
{
    if (Scope == InputScope::CommandLine) return EffectiveCommandLineShortcuts(Overrides);
    auto Values = DefaultScopeShortcuts(Scope);
    const auto Scoped = Overrides[ScopeKey(Scope)].toObject();
    for (auto It = Scoped.begin(); It != Scoped.end(); ++It) {
        const auto Key = QKeySequence::fromString(It.key(), QKeySequence::PortableText);
        if (Key.count() != 1 || Key[0].key() == Qt::Key_unknown) continue;
        if (It.value().isNull()) { Values.remove(Key[0]); continue; }
        const auto Command = FindCommand(It.value().toString());
        if (!Command.Name.isEmpty() && ScopeAccepts(Scope, Command.Name) && !IsProtectedScopeKey(Key[0])) Values[Key[0]] = Command.Name;
    }
    return Values;
}

inline TyMapShortcutToCMDStr DefaultShortcuts(const QJsonObject& General)
{
    TyMapShortcutToCMDStr Values;
    if (!General["useBuiltInShortcuts"].toBool(hc::defaults::DefaultValue("general/useBuiltInShortcuts").toBool())) return Values;
    for (const auto& Command : hc::defaults::Commands())
        if (Command.Default.key() != Qt::Key_unknown && !IsCommandLineShortcut(Command.Name) && !IsViewScopedCommand(Command.Name)) Values[Command.Default] = Command.Name;
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
        if (!Command.Name.isEmpty() && !IsCommandLineShortcut(Command.Name) && !IsViewScopedCommand(Command.Name)) Values[Key[0]] = Command.Name;
    }
    return Values;
}

// 기능 키 막대의 보기 종류. 파일 패널은 기존 functionCommands·functionCaptions를 쓴다.
enum class FunctionBarKind { File, ProcessView, ServiceView };

inline QString FunctionBarKey(FunctionBarKind Kind)
{
    return Kind == FunctionBarKind::ProcessView ? QStringLiteral("processView") : Kind == FunctionBarKind::ServiceView ? QStringLiteral("serviceView") : QString{};
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

inline QStringList FunctionCommandsFor(const QJsonObject& General, FunctionBarKind Kind)
{
    if (Kind == FunctionBarKind::File) return FunctionCommands(General);
    const auto Defaults = Kind == FunctionBarKind::ProcessView ? hc::defaults::ProcessFunctionCommands() : hc::defaults::ServiceFunctionCommands();
    const auto Items = General["functionBars"].toObject()[FunctionBarKey(Kind)].toObject()["commands"].toArray();
    if (Items.size() != 9) return Defaults;
    QStringList Result;
    for (const auto Item : Items) {
        if (!Item.isString() || FindCommand(Item.toString()).Name.isEmpty()) return Defaults;
        Result.append(CommandId(Item.toString()));
    }
    return Result;
}

inline QString FunctionCaptionFor(const QJsonObject& General, FunctionBarKind Kind, int Slot, const QString& Id, const QString& Fallback)
{
    if (Kind == FunctionBarKind::File) return FunctionCaption(General, Slot, Id, Fallback);
    const auto Captions = General["functionBars"].toObject()[FunctionBarKey(Kind)].toObject()["captions"].toArray();
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

// 보기 입력 범위의 키만 해석한다. 찾지 못하면 빈 문자열(호출자가 전역 맵으로 넘어간다).
inline QString ScopeShortcut(const QKeyEvent& Event, const QJsonObject& Overrides, InputScope Scope)
{
    return ShortcutCommand(Event, EffectiveScopeShortcuts(Overrides, Scope));
}

// Resolve only the command input's scope; panel shortcuts may use the same key.
inline QString CommandLineShortcut(const QKeyEvent& Event, const QJsonObject& Overrides)
{
    if (Event.matches(QKeySequence::Copy) || Event.matches(QKeySequence::Cut)
        || Event.matches(QKeySequence::Paste) || Event.matches(QKeySequence::SelectAll)
        || Event.matches(QKeySequence::Undo) || Event.matches(QKeySequence::Redo)) return {};
    auto Key = Event.keyCombination();
    const auto Scoped = CommandLineShortcutOverrides(Overrides);
    const auto Values = EffectiveCommandLineShortcuts(Overrides);
    const auto OriginalName = QKeySequence(Key).toString(QKeySequence::PortableText);
    if (Event.key() == Qt::Key_Enter && !Scoped.contains(OriginalName) && !Values.contains(Key))
        Key = QKeyCombination(Event.modifiers() & ~Qt::KeypadModifier, Qt::Key_Return);
    return CommandId(Values.value(Key));
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
