#pragma once
#include <QString>
#include <QList>
#include <QVector>
#include <QVarLengthArray>
#include <algorithm>

namespace hc::text {
// File-name glob, independent of the regular-expression engine. Two DP rows
// bound pathological star patterns to O(name length * pattern length).
class FileNameGlob
{
public:
    QString Compile(const QString& Pattern) {
        tokens_.clear(); literal_.clear(); runs_.clear(); valid_ = false;
        const auto Points = Pattern.toUcs4();
        for (qsizetype I = 0; I < Points.size(); ++I) {
            const auto C = Points[I]; Token T;
            if (C == '*') {
                if (!tokens_.isEmpty() && tokens_.last().kind_ == Star) continue;
                T.kind_ = Star;
            } else if (C == '?') T.kind_ = Any;
            else if (C == '[') {
                T.kind_ = Class; ++I;
                if (I < Points.size() && (Points[I] == '!' || Points[I] == '^')) { T.negate_ = true; ++I; }
                bool Closed = false;
                for (; I < Points.size(); ++I) {
                    if (Points[I] == ']' && (!T.ranges_.isEmpty() || !T.classes_.isEmpty())) { Closed = true; break; }
                    if (Points[I] == '[' && I + 1 < Points.size() && Points[I + 1] == ':') {
                        qsizetype End = I + 2;
                        while (End + 1 < Points.size() && !(Points[End] == ':' && Points[End + 1] == ']')) ++End;
                        if (End + 1 >= Points.size()) return QStringLiteral("POSIX 문자 집합이 올바르지 않습니다.");
                        auto Name = QString::fromUcs4(Points.constData() + I + 2, End - I - 2);
                        const bool Negate = Name.startsWith('^'); if (Negate) Name.remove(0, 1);
                        const auto Index = QStringList{"alnum", "alpha", "ascii", "blank", "cntrl", "digit", "graph", "lower", "print", "punct", "space", "upper", "word", "xdigit"}.indexOf(Name);
                        if (Index < 0) return QStringLiteral("알 수 없는 POSIX 문자 집합입니다.");
                        T.classes_.append({int(Index), Negate}); I = End + 1; continue;
                    }
                    const auto First = Points[I]; auto Last = First;
                    if (I + 2 < Points.size() && Points[I + 1] == '-' && Points[I + 2] != ']') {
                        Last = Points[I + 2]; I += 2;
                        if (Last < First) return QStringLiteral("문자 범위의 시작이 끝보다 큽니다.");
                    }
                    T.ranges_.append({First, Last});
                }
                if (!Closed) return QStringLiteral("문자 집합의 닫는 ]가 필요합니다.");
                // Unicode has asymmetric case sets (Kelvin sign/K/k, final
                // sigma, titlecase letters). Include reverse equivalents once
                // while compiling; evaluation remains independent of regex.
                const auto Ranges = T.ranges_;
                if (std::any_of(Ranges.cbegin(), Ranges.cend(), [](const auto& R) { return R.second >= 128; }))
                    for (const auto& Pair : reverseCases()) for (const auto& Range : Ranges)
                        if (Pair.first >= Range.first && Pair.first <= Range.second) { T.ranges_.append({Pair.second, Pair.second}); break; }
            } else { T.kind_ = Literal; T.character_ = QChar::toCaseFolded(C); }
            tokens_.append(std::move(T));
        }
        valid_ = true;
        simple_ = std::all_of(tokens_.cbegin(), tokens_.cend(), [](const auto& T) { return T.kind_ == Literal; });
        if (simple_) literal_ = Pattern;
        // Star-only patterns ('*.tmp', '~$*', '*draft*') need no DP. A literal
        // separator keeps the DP: only it knows a star must not cross one.
        else if (std::all_of(tokens_.cbegin(), tokens_.cend(), [](const auto& T) {
                return T.kind_ == Star || (T.kind_ == Literal && T.character_ != '/' && T.character_ != '\\'); })) {
            runs_.emplaceBack();
            for (const auto& T : tokens_) if (T.kind_ == Star) runs_.emplaceBack(); else runs_.last().append(T.character_);
        }
        return {};
    }
    bool IsValid() const { return valid_; }
    bool Matches(QStringView Name) const {
        if (!valid_) return false;
        if (simple_) return Name.compare(literal_, Qt::CaseInsensitive) == 0;
        if (!runs_.isEmpty()) return matchesRuns(Name);
        // Small arrays keep the common case on the stack, and no state is shared
        // between readers of a compiled rule set.
        QVarLengthArray<quint8, 128> Previous(tokens_.size() + 1), Next(tokens_.size() + 1);
        std::fill(Previous.begin(), Previous.end(), quint8(0));
        Previous[0] = 1;
        for (qsizetype J = 0; J < tokens_.size() && tokens_[J].kind_ == Star; ++J) Previous[J + 1] = 1;
        for (qsizetype At = 0; At < Name.size();) {
            const char32_t Point = next(Name, At);
            Next[0] = 0;
            for (qsizetype J = 0; J < tokens_.size(); ++J) {
                const auto& T = tokens_[J];
                Next[J + 1] = T.kind_ == Star ? (Next[J] || (Previous[J + 1] && Point != '/' && Point != '\\'))
                    : Previous[J] && accepts(T, Point);
            }
            std::swap(Previous, Next);
        }
        return Previous[tokens_.size()];
    }
private:
    enum Kind { Literal, Any, Star, Class };
    struct Token {
        Kind kind_ = Literal; char32_t character_ = 0; bool negate_ = false;
        QList<QPair<char32_t, char32_t>> ranges_;
        QList<QPair<int, bool>> classes_;
    };
    static const QList<QPair<char32_t, char32_t>>& reverseCases() {
        static const auto Cases = [] {
            QList<QPair<char32_t, char32_t>> Result;
            for (char32_t C = 128; C <= 0x10ffff; ++C) {
                const auto Fold = QChar::toCaseFolded(C);
                if (C != Fold && C != QChar::toLower(Fold) && C != QChar::toUpper(Fold)) Result.append({C, Fold});
            }
            return Result;
        }();
        return Cases;
    }
    static char32_t fold(char32_t Point) {
        // ASCII folds by range; the rest goes through the Unicode table.
        return Point < 128 ? (Point - U'A' < 26u ? Point + 32 : Point) : QChar::toCaseFolded(Point);
    }
    // Pairs decode identically in both directions, so boundaries found from
    // either end of a name agree.
    static char32_t next(QStringView Name, qsizetype& At) {
        char32_t Point = Name[At].unicode();
        if (Name[At].isHighSurrogate() && At + 1 < Name.size() && Name[At + 1].isLowSurrogate()) {
            Point = QChar::surrogateToUcs4(Name[At], Name[At + 1]); ++At;
        }
        ++At; return Point;
    }
    static char32_t previous(QStringView Name, qsizetype& At) {
        char32_t Point = Name[--At].unicode();
        if (Name[At].isLowSurrogate() && At > 0 && Name[At - 1].isHighSurrogate()) {
            --At; Point = QChar::surrogateToUcs4(Name[At], Name[At + 1]);
        }
        return Point;
    }
    bool matchesRuns(QStringView Name) const {
        // Anchor the outer runs, then take each inner run leftmost: with stars
        // alone, an earlier placement never prevents a later one.
        qsizetype Begin = 0, End = Name.size();
        for (const auto C : runs_.first()) if (Begin >= End || fold(next(Name, Begin)) != C) return false;
        for (auto It = runs_.last().crbegin(); It != runs_.last().crend(); ++It) if (End <= Begin || fold(previous(Name, End)) != *It) return false;
        for (qsizetype R = 1; R + 1 < runs_.size(); ++R) {
            for (;;) {
                if (Begin >= End) return false;
                qsizetype At = Begin; bool Hit = true;
                for (const auto C : runs_[R]) if (At >= End || fold(next(Name, At)) != C) { Hit = false; break; }
                if (Hit) { Begin = At; break; }
                next(Name, Begin);
            }
        }
        // No literal can take a separator, so in any placement one would fall
        // to a star.
        return !Name.contains(u'/') && !Name.contains(u'\\');
    }
    static bool accepts(const Token& T, char32_t Point) {
        if (T.kind_ == Literal) return fold(Point) == T.character_;
        if (T.kind_ == Any) return Point != '/' && Point != '\\';
        const char32_t Variants[]{Point, QChar::toLower(Point), QChar::toUpper(Point),
            QChar::toCaseFolded(Point), QChar::toUpper(QChar::toCaseFolded(Point))};
        bool Hit = false;
        for (const auto& Range : T.ranges_) for (const auto Value : Variants)
            Hit |= Value >= Range.first && Value <= Range.second;
        for (const auto& Class : T.classes_) {
            bool ClassHit = false;
            for (const auto C : {Point}) {
                const bool Alpha = (C >= 'a' && C <= 'z') || (C >= 'A' && C <= 'Z'), Digit = C >= '0' && C <= '9';
                switch (Class.first) {
                case 0: ClassHit |= Alpha || Digit; break; case 1: ClassHit |= Alpha; break;
                case 2: ClassHit |= C <= 127; break; case 3: ClassHit |= C == ' ' || C == '\t'; break;
                case 4: ClassHit |= C < 32 || C == 127; break; case 5: ClassHit |= Digit; break;
                case 6: ClassHit |= C >= 33 && C <= 126; break; case 7: case 11: ClassHit |= Alpha; break;
                case 8: ClassHit |= C >= 32 && C <= 126; break;
                case 9: ClassHit |= C >= 33 && C <= 126 && !Alpha && !Digit; break;
                case 10: ClassHit |= C == ' ' || (C >= '\t' && C <= '\r'); break;
                case 12: ClassHit |= Alpha || Digit || C == '_'; break;
                case 13: ClassHit |= Digit || (C >= 'a' && C <= 'f') || (C >= 'A' && C <= 'F'); break;
                }
            }
            Hit |= ClassHit != Class.second;
        }
        return Hit != T.negate_;
    }
    QList<Token> tokens_;
    QString literal_;
    // Folded literal runs between the stars of a star-only pattern; empty
    // whenever the DP decides.
    QList<QList<char32_t>> runs_;
    bool valid_ = false, simple_ = false;
};
} // namespace hc::text
