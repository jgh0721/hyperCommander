#pragma once
#include "compareSettings.hpp"
#include <QColor>
#include <QList>
#include <QString>
#include <utility>
#include <vector>
class ScintillaEditBase;
namespace hc::compare
{
    struct CompareColorRole { QString id_, label_, group_; bool alpha_ = false; };
    QList<CompareColorRole> CompareColorRoles();
    QColor CompareColor(const QString& Role, bool Dark, const CompareSettings& Settings = ActiveCompareSettings());
    int CompareAlpha(const QString& Role, bool Dark, const CompareSettings& Settings = ActiveCompareSettings());
    bool CompareIsDark();
    double CompareContrast(const QColor& Foreground, const QColor& Background);
    void ApplyCompareEditorColors(ScintillaEditBase* Editor, bool Dark);
    void ClearCompareMarks(ScintillaEditBase* Editor);
    // Markers 8..12: changed, added, removed, unresolved three-way conflict, resolved merge block.
    int CompareEditMarker(std::size_t LeftBegin, std::size_t LeftEnd, std::size_t RightBegin, std::size_t RightEnd);
    enum class SyntaxRole { Comment, Keyword, Type, String, Number, Preprocessor, Definition, Inactive };
    QString CompareSyntaxRoleId(SyntaxRole Role);
    // Lexilla lexer id (SCLEX_*) → its styles that carry a syntax role.
    const std::vector<std::pair<int, SyntaxRole>>& CompareSyntaxStyles(int LexerId);
    // Foregrounds only; call after STYLECLEARALL and before ApplyCompareEditorColors.
    void ApplyCompareSyntaxColors(ScintillaEditBase* Editor, int LexerId, bool Dark);
}
