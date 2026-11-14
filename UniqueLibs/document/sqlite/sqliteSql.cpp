#include "stdafx.h"
#include "sqliteSql.hpp"

/// 모듈

/// UIs

/// 기타 헤더파일

/// 고유 라이브러리
#include "sqliteApi.hpp"
#include "sqliteRuntime.hpp"

/// 공통 라이브러리

/// 외부 라이브러리

/// 선언 헤더파일들

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QRegularExpression>

namespace hc::sqlite
{
    namespace
    {
        bool Space(char Ch) { return Ch == ' ' || Ch == '\t' || Ch == '\r' || Ch == '\n' || Ch == '\f' || Ch == '\v'; }

        // [From, To) 에서 공백 · 주석을 건너뛴 첫 위치.
        int SkipLeading(const QByteArray& Text, int From, int To)
        {
            int At = From;
            while (At < To) {
                if (Space(Text[At])) { ++At; continue; }
                if (Text[At] == '-' && At + 1 < To && Text[At + 1] == '-') { while (At < To && Text[At] != '\n') ++At; continue; }
                if (Text[At] == '/' && At + 1 < To && Text[At + 1] == '*') {
                    At += 2;
                    while (At + 1 < To && !(Text[At] == '*' && Text[At + 1] == '/')) ++At;
                    At = qMin(To, At + 2);
                    continue;
                }
                break;
            }
            return At;
        }

        int TrimTrailing(const QByteArray& Text, int From, int To)
        {
            int End = To;
            while (End > From && Space(Text[End - 1])) --End;
            return End;
        }
    }

    QVector<StatementRange> SplitStatements(const QByteArray& Utf8)
    {
        QVector<StatementRange> Result;
        const int Size = int(Utf8.size());
        const bool Complete = Runtime::Load();
        int Start = 0;
        const auto Push = [&](int End) {
            StatementRange Range;
            Range.start_ = Start;
            Range.end_ = End;
            Range.bodyStart_ = SkipLeading(Utf8, Start, End);
            Range.bodyEnd_ = TrimTrailing(Utf8, Range.bodyStart_, End);
            // ; 하나만 남은 빈 문은 뺀다.
            if (Range.bodyEnd_ - Range.bodyStart_ == 1 && Utf8[Range.bodyStart_] == ';') Range.bodyEnd_ = Range.bodyStart_;
            if (!Range.Empty()) Result.append(Range);
            Start = End;
        };
        for (int At = 0; At < Size; ++At) {
            const char Ch = Utf8[At];
            if (Ch == '\'' || Ch == '"' || Ch == '`') {
                for (++At; At < Size; ++At) {
                    if (Utf8[At] != Ch) continue;
                    if (At + 1 < Size && Utf8[At + 1] == Ch) { ++At; continue; }
                    break;
                }
            } else if (Ch == '[') {
                while (At < Size && Utf8[At] != ']') ++At;
            } else if (Ch == '-' && At + 1 < Size && Utf8[At + 1] == '-') {
                while (At < Size && Utf8[At] != '\n') ++At;
            } else if (Ch == '/' && At + 1 < Size && Utf8[At + 1] == '*') {
                At += 2;
                while (At + 1 < Size && !(Utf8[At] == '*' && Utf8[At + 1] == '/')) ++At;
                ++At;
            } else if (Ch == ';') {
                const auto Chunk = Utf8.mid(Start, At + 1 - Start);
                if (!Complete || sqlite3_complete(Chunk.constData())) Push(At + 1);
            }
        }
        if (Start < Size) Push(Size);
        return Result;
    }

    int StatementAt(const QVector<StatementRange>& Ranges, int Position)
    {
        int Before = -1;
        for (int Index = 0; Index < Ranges.size(); ++Index) {
            if (Position >= Ranges[Index].bodyStart_ && Position <= Ranges[Index].end_) return Index;
            if (Ranges[Index].end_ <= Position) Before = Index;
        }
        return Before >= 0 ? Before : (Ranges.isEmpty() ? -1 : 0);
    }

    bool IsUnboundedWrite(const QString& Statement)
    {
        static const QRegularExpression Comment(QStringLiteral("--[^\\n]*|/\\*.*?\\*/"), QRegularExpression::DotMatchesEverythingOption);
        static const QRegularExpression Quoted(QStringLiteral("'(?:[^']|'')*'|\"(?:[^\"]|\"\")*\"|\\[[^\\]]*\\]|`[^`]*`"));
        static const QRegularExpression Write(QStringLiteral("^\\s*(?:WITH\\b.*?\\)\\s*)?(UPDATE|DELETE)\\b"),
            QRegularExpression::CaseInsensitiveOption | QRegularExpression::DotMatchesEverythingOption);
        static const QRegularExpression Where(QStringLiteral("\\bWHERE\\b"), QRegularExpression::CaseInsensitiveOption);
        auto Text = Statement;
        Text.remove(Comment);
        Text.replace(Quoted, QStringLiteral("''"));
        return Write.match(Text).hasMatch() && !Where.match(Text).hasMatch();
    }

    QStringList Keywords()
    {
        QStringList Result;
        if (!Runtime::Load()) return Result;
        const int Count = sqlite3_keyword_count();
        for (int Index = 0; Index < Count; ++Index) {
            const char* Name = nullptr;
            int Length = 0;
            if (sqlite3_keyword_name(Index, &Name, &Length) == SQLITE_OK && Name) Result.append(QString::fromLatin1(Name, Length).toLower());
        }
        return Result;
    }

    QString Summary(const QString& Statement, int Length)
    {
        const auto Utf8 = Statement.toUtf8();
        const int Start = SkipLeading(Utf8, 0, int(Utf8.size()));
        auto Text = QString::fromUtf8(Utf8.mid(Start)).simplified();
        if (Text.size() > Length) Text = Text.left(Length - 1) + QChar(0x2026);
        return Text;
    }
}
