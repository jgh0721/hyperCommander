#include "stdafx.h"
#include "comparePalette.hpp"
#include <ScintillaEditBase.h>
#include <fmstyle/ThemeManager.h>
#include <cmath>

namespace hc::compare
{
    QList<CompareColorRole> CompareColorRoles()
    {
        QList<CompareColorRole> result;
        const auto add = [&](const char* Id, const char* Label, const char* Group, bool Alpha = false) {
            result.append({QString::fromLatin1(Id), QObject::tr(Label), QObject::tr(Group), Alpha});
        };
        for (const auto& pair : {std::pair{"modified", "변경된 줄"}, {"added", "추가된 줄"}, {"removed", "삭제된 줄"}, {"gap", "정렬 빈 줄"}})
        {
            const auto id = QString::fromLatin1(pair.first); const auto label = QObject::tr(pair.second);
            result.append({id + "Bg", label + QObject::tr(" · 배경"), QObject::tr("본문·줄 강조")});
            if (id != "gap") result.append({id + "Fg", label + QObject::tr(" · 글자"), QObject::tr("본문·줄 강조")});
        }
        add("inline", "줄 안의 차이", "본문·줄 강조"); add("inlineAlpha", "줄 안의 차이 · 불투명도", "본문·줄 강조", true);
        add("barModified", "변경된 줄", "비교막대"); add("barAdded", "추가된 줄", "비교막대");
        add("barRemoved", "삭제된 줄", "비교막대"); add("barGap", "정렬 빈 줄", "비교막대");
        add("barTrack", "막대 배경", "비교막대"); add("barViewport", "현재 화면 테두리", "비교막대"); add("barFocus", "포커스 표시", "비교막대");
        add("conflictBg", "미해결 충돌 · 배경", "병합·연결 영역"); add("conflictFg", "미해결 충돌 · 글자", "병합·연결 영역");
        add("resolvedBg", "해결된 블록 · 배경", "병합·연결 영역"); add("resolvedFg", "해결된 블록 · 글자", "병합·연결 영역");
        add("connector", "연결 영역", "병합·연결 영역"); add("connectorOutline", "연결 테두리", "병합·연결 영역");
        add("connectorAlpha", "연결 영역 · 불투명도", "병합·연결 영역", true); add("resolvedAlpha", "해결된 연결 · 불투명도", "병합·연결 영역", true);
        add("background", "본문 배경", "고급"); add("foreground", "본문 기본 글자", "고급"); add("lineBg", "줄 번호 배경", "고급");
        add("lineFg", "줄 번호 글자", "고급"); add("caret", "캐럿", "고급"); add("selectionBg", "선택 배경", "고급");
        add("selectionFg", "선택 글자", "고급"); add("inactiveSelection", "비활성 선택 배경", "고급");
        add("search", "검색 일치 강조", "고급"); add("searchAlpha", "검색 일치 · 불투명도", "고급", true); add("imageBorder", "이미지 차이 테두리", "고급");
        return result;
    }
    bool CompareIsDark() { return fm::style::ThemeManager::instance().colors().isDark(); }
    QColor CompareColor(const QString& Role, bool Dark, const CompareSettings& Settings)
    {
        using namespace fm::style;
        const auto& theme = ThemeManager::instance();
        const auto variant = Dark ? (theme.darkTone() == ThemeManager::DarkTone::Navy ? Variant::Navy : Variant::Dark) : Variant::Light;
        const auto& tokens = theme.colors(variant); const auto& values = Settings.colors_[Dark ? 1 : 0];
        const QStringList bars{"barModified", "barAdded", "barRemoved", "barGap"};
        const auto bar = bars.indexOf(Role);
        if (bar >= 0 && Settings.linked_[Dark ? 1 : 0])
        {
            const QStringList roles{"modified", "added", "removed", "gap"}; const auto role = roles[bar];
            if (values.contains(role + "Fg")) return QColor(values[role + "Fg"].toString());
            if (values.contains(role + "Bg"))
            {
                auto color = QColor(values[role + "Bg"].toString()); const auto track = CompareColor("barTrack", Dark, Settings);
                const auto target = tokens[Token::Fg];
                // A linked line background may be too subtle for a one-pixel bar; blend toward readable foreground.
                for (int i = 0; i < 16 && CompareContrast(color, track) < 3; ++i)
                    color = QColor((color.red() * 3 + target.red()) / 4, (color.green() * 3 + target.green()) / 4, (color.blue() * 3 + target.blue()) / 4);
                return color;
            }
        }
        else if (values.contains(Role)) return QColor(values[Role].toString());
        if (bar >= 0) return tokens[std::array{Token::Warn, Token::Ok, Token::Danger, Token::Accent}[bar]];
        if (values.contains(Role)) return QColor(values[Role].toString());
        if (Role.endsWith("Fg") || Role == "foreground" || Role == "caret" || Role == "barViewport" || Role == "connectorOutline") return tokens[Token::Fg];
        if (Role == "modifiedBg" || Role == "addedBg" || Role == "removedBg" || Role == "conflictBg" || Role == "resolvedBg") return tokens[Token::WarnBg];
        if (Role == "lineBg") return tokens[Token::Alt];
        if (Role == "selectionBg") return tokens[Token::AccentSoft];
        if (Role == "inactiveSelection") return tokens[Token::SelIn];
        if (Role == "inline" || Role == "imageBorder") return tokens[Token::Danger];
        if (Role == "connector" || Role == "search" || Role == "barFocus") return tokens[Token::Accent];
        return tokens[Token::Surface];
    }
    int CompareAlpha(const QString& Role, bool Dark, const CompareSettings& Settings)
    {
        const auto value = Settings.colors_[Dark ? 1 : 0][Role]; if (value.isDouble()) return std::clamp(value.toInt(), 0, 255);
        if (Role == "inlineAlpha") return Dark ? 70 : 50;
        if (Role == "searchAlpha") return 55;
        if (Role == "resolvedAlpha") return 25;
        return 65;
    }
    double CompareContrast(const QColor& Foreground, const QColor& Background)
    {
        const auto luminance = [](const QColor& Color) {
            const auto channel = [](double Value) { return Value <= 0.04045 ? Value / 12.92 : std::pow((Value + 0.055) / 1.055, 2.4); };
            return channel(Color.redF()) * .2126 + channel(Color.greenF()) * .7152 + channel(Color.blueF()) * .0722;
        };
        const auto a = luminance(Foreground), b = luminance(Background); return (std::max(a, b) + .05) / (std::min(a, b) + .05);
    }
    void ClearCompareMarks(ScintillaEditBase* Editor)
    {
        for (int marker = 8; marker <= 12; ++marker) Editor->send(SCI_MARKERDELETEALL, marker);
        const auto old = Editor->send(SCI_GETINDICATORCURRENT);
        for (int indicator = 22; indicator <= 26; ++indicator) { Editor->send(SCI_SETINDICATORCURRENT, indicator); Editor->send(SCI_INDICATORCLEARRANGE, 0, Editor->send(SCI_GETLENGTH)); }
        Editor->send(SCI_SETINDICATORCURRENT, old);
    }
    int CompareEditMarker(std::size_t LeftBegin, std::size_t LeftEnd, std::size_t RightBegin, std::size_t RightEnd)
    { return LeftBegin == LeftEnd ? 9 : RightBegin == RightEnd ? 10 : 8; }
    void ApplyCompareEditorColors(ScintillaEditBase* Editor, bool Dark)
    {
        const auto rgb = [](const QColor& Color) -> sptr_t { return Color.red() | (Color.green() << 8) | (Color.blue() << 16); };
        const auto color = [&](const char* Role) { return rgb(CompareColor(Role, Dark)); };
        // Set all lexer backgrounds without clearing their syntax foreground or recomputing document styles.
        for (int style = 0; style <= STYLE_MAX; ++style) Editor->send(SCI_STYLESETBACK, style, color("background"));
        Editor->send(SCI_STYLESETFORE, STYLE_DEFAULT, color("foreground")); Editor->send(SCI_STYLESETFORE, 0, color("foreground"));
        Editor->send(SCI_STYLESETBACK, STYLE_LINENUMBER, color("lineBg")); Editor->send(SCI_STYLESETFORE, STYLE_LINENUMBER, color("lineFg"));
        Editor->send(SCI_SETCARETFORE, color("caret"));
        // Reserved padding style consumed only by our generated Scintilla padding patch.
        Editor->send(SCI_STYLESETBACK, 255, color("gapBg"));
        for (const auto element : {SC_ELEMENT_SELECTION_TEXT, SC_ELEMENT_SELECTION_ADDITIONAL_TEXT, SC_ELEMENT_SELECTION_INACTIVE_TEXT, SC_ELEMENT_SELECTION_INACTIVE_ADDITIONAL_TEXT})
            Editor->send(SCI_SETELEMENTCOLOUR, element, color("selectionFg") | 0xff000000u);
        for (const auto element : {SC_ELEMENT_SELECTION_BACK, SC_ELEMENT_SELECTION_ADDITIONAL_BACK}) Editor->send(SCI_SETELEMENTCOLOUR, element, color("selectionBg") | 0xff000000u);
        for (const auto element : {SC_ELEMENT_SELECTION_INACTIVE_BACK, SC_ELEMENT_SELECTION_INACTIVE_ADDITIONAL_BACK}) Editor->send(SCI_SETELEMENTCOLOUR, element, color("inactiveSelection") | 0xff000000u);
        const QStringList roles{"modified", "added", "removed", "conflict", "resolved"};
        const auto old = Editor->send(SCI_GETINDICATORCURRENT), oldValue = Editor->send(SCI_GETINDICATORVALUE);
        Editor->send(SCI_SETINDICATORVALUE, 1);
        for (int i = 0; i < roles.size(); ++i)
        {
            Editor->send(SCI_MARKERDEFINE, 8 + i, SC_MARK_BACKGROUND); Editor->send(SCI_MARKERSETBACK, 8 + i, rgb(CompareColor(roles[i] + "Bg", Dark)));
            Editor->send(SCI_SETINDICATORCURRENT, 22 + i); Editor->send(SCI_INDICATORCLEARRANGE, 0, Editor->send(SCI_GETLENGTH));
            if (!ActiveCompareSettings().colors_[Dark ? 1 : 0].contains(roles[i] + "Fg")) continue;
            Editor->send(SCI_INDICSETSTYLE, 22 + i, INDIC_TEXTFORE); Editor->send(SCI_INDICSETFORE, 22 + i, rgb(CompareColor(roles[i] + "Fg", Dark)));
            for (auto line = Editor->send(SCI_MARKERNEXT, 0, 1 << (8 + i)); line >= 0; line = Editor->send(SCI_MARKERNEXT, line + 1, 1 << (8 + i)))
            {
                const auto begin = Editor->send(SCI_POSITIONFROMLINE, line), end = Editor->send(SCI_GETLINEENDPOSITION, line);
                if (end > begin) Editor->send(SCI_INDICATORFILLRANGE, begin, end - begin);
            }
        }
        Editor->send(SCI_SETINDICATORCURRENT, old); Editor->send(SCI_SETINDICATORVALUE, oldValue);
    }
}
