#pragma once
#include <QRegularExpression>
#include <QString>

namespace hc::quickfind {
inline int ChoseongIndex(QChar Character)
{
    static constexpr signed char Indices[] = {
        0,1,-1,2,-1,-1,3,4,5,-1,-1,-1,-1,-1,-1,-1,6,7,8,-1,9,10,11,12,13,14,15,16,17,18
    };
    const auto Code = Character.unicode();
    return Code >= 0x3131 && Code <= 0x314e ? Indices[Code - 0x3131] : -1;
}
inline QString ChoseongAtom(int Index)
{
    static const QString Jamo = QStringLiteral("ㄱㄲㄴㄷㄸㄹㅁㅂㅃㅅㅆㅇㅈㅉㅊㅋㅌㅍㅎ");
    if (Index < 0 || Index >= Jamo.size()) return {};
    const auto First = ushort(0xac00 + Index * 588);
    return QStringLiteral("(?:[%1%2-%3]|%4[ᅡ-ᅵ][ᆨ-ᇂ]?(?![ᆨ-ᇂ]))")
        .arg(Jamo[Index]).arg(QChar(First)).arg(QChar(ushort(First + 587))).arg(QChar(ushort(0x1100 + Index)));
}
inline QString ExactHangulAtom(QChar Character)
{
    const QString Text(Character);
    return QStringLiteral("(?:%1|%2(?![ᆨ-ᇂ]))")
        .arg(Text, Text.normalized(QString::NormalizationForm_D));
}
// Composition is explicit: committed final syllables must never be broadened.
inline QString KoreanSearchPattern(const QString& Input, const QString& Match, int ComposingIndex = -1)
{
    QString Pattern;
    for (qsizetype At = 0; At < Input.size(); ++At) {
        const auto Character = Input[At];
        const int Cho = ChoseongIndex(Character), Code = Character.unicode();
        if (Cho >= 0) { Pattern += ChoseongAtom(Cho); continue; }
        if (Code < 0xac00 || Code > 0xd7a3) { Pattern += QRegularExpression::escape(QString(Character)); continue; }
        if (At != ComposingIndex) { Pattern += ExactHangulAtom(Character); continue; }
        const int Tail = (Code - 0xac00) % 28;
        if (!Tail) {
            const auto Nfd = QString(Character).normalized(QString::NormalizationForm_D);
            Pattern += QStringLiteral("(?:[%1-%2]|%3[ᆨ-ᇂ]?(?![ᆨ-ᇂ]))")
                .arg(Character).arg(QChar(ushort(Code + 27))).arg(Nfd);
        } else {
            static constexpr int Remain[] = {0,0,0,1,0,4,4,0,0,8,8,8,8,8,8,8,0,0,17,0,0,0,0,0,0,0,0,0};
            static constexpr int Next[] = {-1,0,1,9,2,12,18,3,5,0,6,7,9,16,17,18,6,7,9,9,10,11,12,14,15,16,17,18};
            Pattern += QStringLiteral("(?:%1|%2%3)").arg(ExactHangulAtom(Character),
                ExactHangulAtom(QChar(ushort(Code - Tail + Remain[Tail]))), ChoseongAtom(Next[Tail]));
        }
    }
    return (Match == "starts" ? QStringLiteral("^") : QString{}) + Pattern
        + (Match == "ends" ? QStringLiteral("$") : QString{});
}
}
