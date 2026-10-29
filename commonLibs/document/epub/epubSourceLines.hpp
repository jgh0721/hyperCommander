#pragma once
#include <QByteArray>
#include <QChar>
#include <QString>

namespace hc::document::epub {
namespace detail {
// Tag boundaries shared by SourceMap::Load and source line annotation, so both read markup the same way.
struct MarkupToken {
    enum Kind { Text, CData, Comment, Start, Empty, End, Other } kind_ = Text;
    qsizetype begin_ = 0, end_ = 0;   // CData spans its content; every other kind spans the whole construct.
};
inline char16_t markupUnit(QChar Value) { return Value.unicode(); }
inline char16_t markupUnit(char Value) { return static_cast<unsigned char>(Value); }
// Calls OnToken for each token in order; stops when it returns false. Unterminated constructs fail.
template<class Char, class Visit> bool ScanMarkup(const Char* Data, qsizetype Size, Visit&& OnToken) {
    const auto At = [&](qsizetype I) { return I < Size ? markupUnit(Data[I]) : char16_t(0); };
    const auto Starts = [&](qsizetype I, const char* Text) { for (qsizetype J = 0; Text[J]; ++J) if (At(I + J) != char16_t(Text[J])) return false; return true; };
    const auto Find = [&](qsizetype From, const char* Text) { for (qsizetype I = From; I < Size; ++I) if (Starts(I, Text)) return I; return qsizetype(-1); };
    for (qsizetype I = 0; I < Size;) {
        if (At(I) != u'<') { qsizetype End = I; while (End < Size && At(End) != u'<') ++End; if (!OnToken(MarkupToken{MarkupToken::Text, I, End})) return false; I = End; continue; }
        if (Starts(I, "<!--")) { const auto End = Find(I + 4, "-->"); if (End < 0 || !OnToken(MarkupToken{MarkupToken::Comment, I, End + 3})) return false; I = End + 3; continue; }
        if (Starts(I, "<![CDATA[")) { const auto End = Find(I + 9, "]]>"); if (End < 0 || !OnToken(MarkupToken{MarkupToken::CData, I + 9, End})) return false; I = End + 3; continue; }
        qsizetype End = I + 1; int Brackets = 0; char16_t Quote = 0;
        for (; End < Size; ++End) { const auto C = At(End); if (Quote) { if (C == Quote) Quote = 0; } else if (C == u'"' || C == u'\'') Quote = C; else if (C == u'[') ++Brackets; else if (C == u']') --Brackets; else if (C == u'>' && !Brackets) break; }
        if (End >= Size) return false;
        const auto Next = At(I + 1);
        const auto Kind = Next == u'/' ? MarkupToken::End : Next == u'!' || Next == u'?' ? MarkupToken::Other : At(End - 1) == u'/' ? MarkupToken::Empty : MarkupToken::Start;
        if (!OnToken(MarkupToken{Kind, I, End + 1})) return false;
        I = End + 1;
    }
    return true;
}
}
struct SourceLines {
    QByteArray bytes_;
    QString skipped_;   // Why the original bytes were kept; empty when annotated.
    int elements_ = 0;
};
// Adds data-hc-l="start line" to every element inside <body>, and data-hc-e="end line" when the element
// spans lines. Lines are 1-based; \r\n, \n and \r each end one line, as in the source editor.
SourceLines AnnotateSourceLines(const QByteArray& Xhtml);
}
