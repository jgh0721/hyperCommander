#pragma once
#include "runtime/compareLanguage.hpp"

namespace Scintilla { class ILexer5; }
namespace hc::compare
{
    // Keywords and properties are set on the ILexer5 itself, so an editor and the
    // large-text IDocument produce identical style bytes for the same language.
    Scintilla::ILexer5* CreateCompareLexer(const CompareLanguage& Language);
    void ConfigureCompareLexer(Scintilla::ILexer5* Lexer, const CompareLanguage& Language);
    int CompareLexerId(const CompareLanguage& Language);
}
