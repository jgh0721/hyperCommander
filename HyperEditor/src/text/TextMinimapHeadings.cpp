#include "stdafx.h"
#include "text/TextMinimapHeadings.h"

/// 모듈

/// UIs

/// 기타 헤더파일
#include "text/TextMinimapModel.h"
#include "text/TextMinimapSource.h"

/// 고유 라이브러리

/// 공통 라이브러리

/// 외부 라이브러리
#include <Scintilla.h>

/// 선언 헤더파일들

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QRegularExpression>

namespace
{
    constexpr int TitleLength = 80;

    QString title(const QString& Text)
    {
        auto Value = Text.simplified();
        if (Value.size() > TitleLength)
            Value = Value.left(TitleLength - 1) + QChar(0x2026);
        return Value;
    }

    QByteArrayView trimEnd(QByteArrayView Line)
    {
        while (!Line.isEmpty() && (Line.back() == '\r' || Line.back() == '\n'))
            Line.chop(1);
        return Line;
    }
}

TextMinimapHeadings::Kind TextMinimapHeadings::KindForLanguage(const QString& Language)
{
    const auto Name = Language.trimmed().toLower();
    if (Name == QLatin1String("markdown"))
        return Kind::Markdown;
    if (Name == QLatin1String("html") || Name == QLatin1String("xml") || Name == QLatin1String("xhtml"))
        return Kind::Markup;
    if (Name.isEmpty() || Name == QLatin1String("none") || Name == QLatin1String("text"))
        return Kind::None;
    return Kind::Fold;
}

int TextMinimapHeadings::MarkdownAtxLevel(QByteArrayView Line, QString* Title)
{
    Line = trimEnd(Line);
    qsizetype At = 0;
    while (At < Line.size() && At < 3 && Line[At] == ' ')
        ++At;
    int Level = 0;
    while (At + Level < Line.size() && Line[At + Level] == '#')
        ++Level;
    if (Level < 1 || Level > 6)
        return 0;
    const qsizetype Rest = At + Level;
    if (Rest < Line.size() && Line[Rest] != ' ' && Line[Rest] != '\t')
        return 0;
    if (Title) {
        auto Text = QString::fromUtf8(Line.sliced(Rest)).trimmed();
        // Optional closing sequence: "## Title ##".
        static const QRegularExpression Closing(QStringLiteral("\\s+#+\\s*$"));
        Text.remove(Closing);
        *Title = title(Text == QLatin1String("#") ? QString() : Text);
    }
    return Level;
}

int TextMinimapHeadings::MarkdownSetextLevel(QByteArrayView Line)
{
    Line = trimEnd(Line);
    qsizetype At = 0;
    while (At < Line.size() && At < 3 && Line[At] == ' ')
        ++At;
    if (At >= Line.size() || (Line[At] != '=' && Line[At] != '-'))
        return 0;
    const char Marker = Line[At];
    qsizetype End = Line.size();
    while (End > At && (Line[End - 1] == ' ' || Line[End - 1] == '\t'))
        --End;
    for (qsizetype Index = At; Index < End; ++Index)
        if (Line[Index] != Marker)
            return 0;
    return Marker == '=' ? 1 : 2;
}

bool TextMinimapHeadings::MarkdownFence(QByteArrayView Line, char* Marker)
{
    qsizetype At = 0;
    while (At < Line.size() && At < 3 && Line[At] == ' ')
        ++At;
    if (At + 3 > Line.size())
        return false;
    const char Ch = Line[At];
    if ((Ch != '`' && Ch != '~') || Line[At + 1] != Ch || Line[At + 2] != Ch)
        return false;
    if (Marker)
        *Marker = Ch;
    return true;
}

int TextMinimapHeadings::MarkupLevel(QByteArrayView Line, QString* Title)
{
    static const QRegularExpression Heading(QStringLiteral("<h([1-6])(?:\\s[^>]*)?>(.*?)</h\\1\\s*>"), QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression Tags(QStringLiteral("<[^>]*>"));
    const auto Match = Heading.match(QString::fromUtf8(Line));
    if (!Match.hasMatch())
        return 0;
    if (Title) {
        auto Text = Match.captured(2);
        Text.remove(Tags);
        Text.replace(QLatin1String("&amp;"), QLatin1String("&")).replace(QLatin1String("&lt;"), QLatin1String("<"))
            .replace(QLatin1String("&gt;"), QLatin1String(">")).replace(QLatin1String("&nbsp;"), QLatin1String(" "));
        *Title = title(Text);
    }
    return Match.captured(1).toInt();
}

void TextMinimapHeadings::Start(Kind Scan, int LineCount)
{
    kind_ = Scan;
    lines_ = Scan == Kind::None ? 0 : qMax(0, LineCount);
    next_ = 0;
    fence_ = 0;
    previous_.clear();
    previousLine_ = -1;
    pending_.clear();
    if (Scan == Kind::None)
        headings_.clear();
}

bool TextMinimapHeadings::Step(const TextMinimapSource& Source, const QElapsedTimer& Clock, qint64 BudgetMs)
{
    while (next_ < lines_) {
        if ((next_ & 63) == 0 && Clock.elapsed() >= BudgetMs)
            return false;
        const int Line = next_++;
        if (kind_ == Kind::Fold) {
            const int Fold = Source.FoldLevel(Line);
            const int Level = (Fold & SC_FOLDLEVELNUMBERMASK) - SC_FOLDLEVELBASE;
            if (!(Fold & SC_FOLDLEVELHEADERFLAG) || Level > 1)
                continue;
            const qint64 Start = Source.LineStart(Line);
            const auto Text = title(QString::fromUtf8(Source.Text(Start, qMin(Source.LineEnd(Line), Start + TextMinimapPrefixBytes))));
            if (!Text.isEmpty())
                pending_.append({Line, Level + 1, Text});
            continue;
        }
        const qint64 Start = Source.LineStart(Line);
        const QByteArray Bytes = Source.Text(Start, qMin(Source.LineEnd(Line), Start + TextMinimapPrefixBytes));
        if (kind_ == Kind::Markup) {
            QString Title;
            if (const int Level = MarkupLevel(Bytes, &Title); Level > 0 && !Title.isEmpty())
                pending_.append({Line, Level, Title});
            continue;
        }
        // Markdown
        char Marker = 0;
        if (MarkdownFence(Bytes, &Marker)) {
            fence_ = fence_ == 0 ? Marker : (fence_ == Marker ? 0 : fence_);
            previous_.clear();
            continue;
        }
        if (fence_ != 0)
            continue;
        QString Title;
        if (const int Level = MarkdownAtxLevel(Bytes, &Title); Level > 0) {
            if (!Title.isEmpty())
                pending_.append({Line, Level, Title});
            previous_.clear();
            continue;
        }
        if (const int Level = MarkdownSetextLevel(Bytes); Level > 0 && !previous_.isEmpty() && previousLine_ == Line - 1) {
            pending_.append({previousLine_, Level, title(previous_)});
            previous_.clear();
            continue;
        }
        const auto Text = QString::fromUtf8(Bytes).trimmed();
        previous_ = Text;
        previousLine_ = Line;
    }
    headings_ = pending_;
    return true;
}
