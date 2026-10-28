#include "stdafx.h"
#include "compareSyntax.hpp"
#include "commonLibs/document/lexerKeywords.hpp"
#include <ILexer.h>
#include <Lexilla.h>

namespace hc::compare
{
    Scintilla::ILexer5* CreateCompareLexer(const CompareLanguage& Language)
    {
        auto* lexer = CreateLexer(Language.lexer_.constData());
        if (lexer) ConfigureCompareLexer(lexer, Language);
        return lexer;
    }
    void ConfigureCompareLexer(Scintilla::ILexer5* Lexer, const CompareLanguage& Language)
    {
        if (Language.id_ == "c") { Lexer->WordListSet(0, hc::lexer::cKeywords); Lexer->WordListSet(1, hc::lexer::cTypes); }
        else if (Language.lexer_ == "cpp") { Lexer->WordListSet(0, hc::lexer::cppKeywords); Lexer->WordListSet(1, hc::lexer::cppTypes); }
        else if (Language.lexer_ == "python")
        {
            static const QByteArray builtins = QByteArray(hc::lexer::pythonBuiltins) + ' ' + hc::lexer::pythonSoftKeywords;
            Lexer->WordListSet(0, hc::lexer::pythonKeywords); Lexer->WordListSet(1, builtins.constData());
        }
        // Escapes inside literals get their own style; the default (0) folds them into the string.
        if (Language.lexer_ == "cpp") Lexer->PropertySet("lexer.cpp.escape.sequence", "1");
    }
    int CompareLexerId(const CompareLanguage& Language)
    {
        auto* lexer = CreateLexer(Language.lexer_.constData());
        const int id = lexer ? lexer->GetIdentifier() : 0;
        if (lexer) lexer->Release();
        return id;
    }
}
