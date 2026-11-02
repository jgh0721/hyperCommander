#pragma once
#include <QtCore>
#if defined(QT_GUI_LIB)
#include <QFont>
#include <QColor>
#include <QGuiApplication>
#include <QKeySequence>
#endif
#include <array>

namespace hc::defaults::compare
{
    inline constexpr auto Theme = "product";
#if defined(QT_GUI_LIB)
    struct Command { QString id_, label_, category_, scope_; QList<QKeySequence> defaults_; };

    inline QList<Command> Commands()
    {
        const auto Tr = [](const char* Text) { return QObject::tr(Text); };
        QList<Command> Result;
        const auto Add = [&](const char* Id, const char* Label, const char* Category, const char* Key = "", const char* Scope = "비교창") {
            Result.append({QString::fromLatin1(Id), Tr(Label), Tr(Category), Tr(Scope), *Key ? QList<QKeySequence>{QKeySequence(QString::fromLatin1(Key))} : QList<QKeySequence>{}});
        };
        Add("navigate.next", "다음 차이", "비교·이동", "F7"); Add("navigate.previous", "이전 차이", "비교·이동", "Shift+F7");
        Add("find.open", "찾기", "찾기", "Ctrl+F"); Add("find.next", "다음 찾기", "찾기", "F3");
        Add("find.previous", "이전 찾기", "찾기", "Shift+F3"); Add("find.all", "모두 찾기", "찾기", "Ctrl+Shift+F");
        Add("file.save", "결과 저장", "파일·작업", "Ctrl+S"); Add("work.save", "병합 작업 저장", "파일·작업", "Ctrl+Shift+S");
        Add("work.open", "병합 작업 열기", "파일·작업", "Ctrl+Shift+O");
        Add("edit.undo", "실행 취소", "편집"); Result.last().defaults_ = QKeySequence::keyBindings(QKeySequence::Undo);
        Add("edit.redo", "다시 실행", "편집"); Result.last().defaults_ = QKeySequence::keyBindings(QKeySequence::Redo);
        Add("directory.rename", "디렉터리 이름 변경", "파일·작업", "F2", "디렉터리 트리");
        Add("file.open", "파일 열기", "파일·작업"); Add("directory.open", "디렉터리 열기", "파일·작업");
        Add("compare.again", "다시 비교", "비교·이동"); Add("compare.cancel", "비교 취소", "비교·이동");
        Add("merge.left", "블록 LEFT", "병합"); Add("merge.right", "블록 RIGHT", "병합"); Add("merge.base", "블록 BASE", "병합");
        Add("merge.leftRight", "LEFT → RIGHT", "병합"); Add("merge.rightLeft", "RIGHT → LEFT", "병합");
        Add("merge.omit", "블록 제거", "병합"); Add("merge.manual", "블록 수동 완료", "병합");
        Add("view.alignAll", "전체 패널 표시 줄 정렬", "보기·설정"); Add("view.alignRows", "왼쪽·오른쪽 표시 줄 정렬", "보기·설정");
        Add("view.sync", "왼쪽·오른쪽 스크롤 동기화", "보기·설정"); Add("view.inline", "줄 내부 차이 강조", "보기·설정");
        Add("view.overview", "차이 개요 막대", "보기·설정"); Add("app.settings", "설정", "보기·설정", "Ctrl+,");
        Add("view.syntax.auto", "구문 강조 · 자동", "보기·설정"); Add("view.syntax.c", "구문 강조 · C", "보기·설정");
        Add("view.syntax.cpp", "구문 강조 · C++", "보기·설정"); Add("view.syntax.python", "구문 강조 · Python", "보기·설정");
        Add("view.syntax.json", "구문 강조 · JSON", "보기·설정"); Add("view.syntax.xml", "구문 강조 · XML", "보기·설정");
        Add("view.syntax.none", "구문 강조 · 일반 텍스트", "보기·설정");
        return Result;
    }

    inline constexpr std::array<const char*, 5> ViewKeys{"view/alignAllPanes", "view/alignSourceRows",
        "view/synchronizeSources", "view/inlineHighlights", "view/overview"};
    inline constexpr std::array<bool, 5> ViewOptions{true, true, true, true, true};
    inline constexpr bool LinkColors = true;

    inline std::array<QFont, 3> Fonts(const QFont& Ui)
    {
        return {Ui, QFont("D2Coding", 12), QFont("D2Coding ligature", 14)};
    }

    inline QVariant Value(const QString& Key, const QVariant& Fallback = {})
    {
        if (Key == "fonts/ui") return QGuiApplication::font().toString();
        if (Key == "fonts/list") return Fonts(QFont{})[1].toString();
        if (Key == "fonts/text") return Fonts(QFont{})[2].toString();
        for (std::size_t Index = 0; Index < ViewKeys.size(); ++Index)
            if (Key == QLatin1String(ViewKeys[Index])) return ViewOptions[Index];
        static const QHash<QString, QVariant> Values{{"theme/mode", Theme}, {"theme/dark", false},
            {"find/caseSensitive", false}, {"find/wholeWord", false}, {"settings/lastTab", 0}, {"directory/level", "text"}};
        return Values.value(Key, Fallback);
    }
#endif

    inline constexpr auto ConfigFileName = "HyperCompare.json";
    inline constexpr auto LegacyOrganization = "HyperTools";
    inline constexpr auto LegacyApplication = "HyperCompare";
    inline QString ConfigDirectory() { return QCoreApplication::applicationDirPath(); }
    inline QString SettingsFile(const QString& Directory) { return QDir(Directory).filePath(ConfigFileName); }
    inline QString RecoveryDirectory()
    {
        const auto Base = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
        return Base.isEmpty() ? QString{} : Base + "/recovery";
    }
    inline QString ThemeMode(const QJsonValue& Mode, const QJsonValue& LegacyDark)
    {
        if (Mode.isUndefined()) return LegacyDark.toBool() ? QString("dark") : QString(Theme);
        const auto Text = Mode.toString();
        return Text == "light" || Text == "dark" || Text == "product" ? Text : QString(Theme);
    }

    inline constexpr int ConfigSchemaVersion = 1;
    inline int Alpha(const QString& Role, bool Dark)
    {
        if (Role == "inlineAlpha") return Dark ? 70 : 50;
        if (Role == "searchAlpha") return 55;
        if (Role == "resolvedAlpha") return 25;
        return 65;
    }
#if defined(QT_GUI_LIB)
    inline QColor SyntaxColor(const QString& Role, bool Dark)
    {
        static const QHash<QString, std::pair<const char*, const char*>> Colors{
            {"syntaxComment", {"#51774c", "#90b58b"}}, {"syntaxKeyword", {"#803da1", "#cb9fda"}},
            {"syntaxType", {"#1f6f6a", "#7cc9c0"}}, {"syntaxString", {"#9b5f28", "#d7b080"}},
            {"syntaxNumber", {"#3769a0", "#92bfdd"}}, {"syntaxPreprocessor", {"#a03562", "#e597ba"}},
            {"syntaxDefinition", {"#7a5a00", "#e0c46c"}}, {"syntaxInactive", {"#73787f", "#8b919c"}}};
        const auto At = Colors.constFind(Role);
        return At == Colors.cend() ? QColor{} : QColor(Dark ? At->second : At->first);
    }
    template<typename Token, typename Palette>
    inline QColor Color(const QString& Role, bool Dark, const Palette& Tokens)
    {
        const auto Bar = QStringList{"barModified", "barAdded", "barRemoved", "barGap"}.indexOf(Role);
        if (Bar >= 0) return Tokens[std::array{Token::Warn, Token::Ok, Token::Danger, Token::Accent}[Bar]];
        if (const auto Syntax = SyntaxColor(Role, Dark); Syntax.isValid()) return Syntax;
        if (Role.endsWith("Fg") || Role == "foreground" || Role == "caret" || Role == "barViewport" || Role == "connectorOutline") return Tokens[Token::Fg];
        if (Role == "modifiedBg" || Role == "addedBg" || Role == "removedBg" || Role == "conflictBg" || Role == "resolvedBg") return Tokens[Token::WarnBg];
        if (Role == "lineBg") return Tokens[Token::Alt];
        if (Role == "selectionBg") return Tokens[Token::AccentSoft];
        if (Role == "inactiveSelection") return Tokens[Token::SelIn];
        if (Role == "inline" || Role == "imageBorder") return Tokens[Token::Danger];
        if (Role == "connector" || Role == "search" || Role == "barFocus") return Tokens[Token::Accent];
        return Tokens[Token::Surface];
    }
#endif

    inline QKeyCombination AuxiliaryShortcut(const QString& Id)
    { return Id == "find.close" ? QKeyCombination(Qt::Key_Escape) : QKeyCombination::fromCombined(0); }

#if defined(QT_GUI_LIB)
    inline QList<QKeySequence> ReservedEditingShortcuts()
    {
        QList<QKeySequence> Result;
        for (const auto Standard : {QKeySequence::Copy, QKeySequence::Cut, QKeySequence::Paste, QKeySequence::SelectAll})
            Result.append(QKeySequence::keyBindings(Standard));
        return Result;
    }
#endif

}
