#include "stdafx.h"
#include "text/TextMinimapPeek.h"

/// 모듈

/// UIs

/// 기타 헤더파일
#include "text/TextMinimap.h"
#include "text/TextMinimapSource.h"

/// 고유 라이브러리

/// 공통 라이브러리

/// 외부 라이브러리

/// 선언 헤더파일들

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QGuiApplication>
#include <QLocale>
#include <QPainter>
#include <QScreen>

namespace
{
    constexpr int Lines = 9;
    constexpr int Padding = 8;
    constexpr int PreviewColumns = 72;
}

TextMinimapPeek::TextMinimapPeek(TextMinimap* Owner, QWidget* Parent)
    : QWidget(Parent, Qt::ToolTip | Qt::FramelessWindowHint | Qt::WindowDoesNotAcceptFocus)
    , owner_(Owner)
{
    setObjectName(QStringLiteral("textMinimapPeek"));
    setAttribute(Qt::WA_ShowWithoutActivating);
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setFocusPolicy(Qt::NoFocus);
    hide();
}

void TextMinimapPeek::ShowAt(int CenterLine, const QRect& Anchor, int GlobalY, bool LeftOfAnchor)
{
    const auto& Source = owner_->Source();
    const int Count = Source.LineCount();
    center_ = qBound(0, CenterLine, Count - 1);
    const int First = qBound(0, center_ - Lines / 2, qMax(0, Count - Lines));
    const int Last = qMin(Count - 1, First + Lines - 1);
    const int Tab = Source.TabWidth();
    rows_.clear();
    QByteArray Text, Styles;
    for (int Line = First; Line <= Last; ++Line) {
        const qint64 Start = Source.LineStart(Line);
        const qint64 End = qMin(Source.LineEnd(Line), Start + TextMinimapPrefixBytes);
        if (Start + (End - Start) <= Source.EndStyled())
            Source.StyledText(Start, End, Text, Styles);
        else {
            Text = Source.Text(Start, End);
            Styles = QByteArray(Text.size(), '\0');
        }
        // Group bytes into runs of one style, then decode each run; tabs become spaces up to the next stop.
        Row Item;
        Item.line_ = Line;
        int Column = 0;
        qsizetype RunStart = 0;
        const auto Flush = [&](qsizetype End) {
            if (End <= RunStart)
                return;
            QString Part;
            for (const auto Code : QString::fromUtf8(Text.mid(RunStart, End - RunStart)).toUcs4()) {
                if (Column >= PreviewColumns)
                    break;
                if (Code == '\t') {
                    const int Next = (Column / Tab + 1) * Tab;
                    Part += QString(Next - Column, QLatin1Char(' '));
                    Column = Next;
                    continue;
                }
                Part += QString::fromUcs4(&Code, 1);
                Column += TextMinimapCharColumns(Code);
            }
            QColor Color = owner_->StyleColor(quint8(Styles[RunStart]));
            Color.setAlpha(255);
            Item.runs_.append({Part, Color});
            RunStart = End;
        };
        for (qsizetype Index = 1; Index <= Text.size(); ++Index) {
            // Style changes only on character lead bytes, so runs never split a UTF-8 sequence.
            const bool Continuation = Index < Text.size() && (quint8(Text[Index]) & 0xC0) == 0x80;
            if (Index == Text.size() || (!Continuation && Styles[Index] != Styles[RunStart]))
                Flush(Index);
        }
        rows_.append(Item);
    }
    header_ = tr("줄 %1–%2").arg(QLocale().toString(First + 1), QLocale().toString(Last + 1));
    section_.clear();
    for (const auto& Heading : owner_->Headings()) {
        if (Heading.line_ > center_) break;
        section_ = Heading.text_;
    }
    chips_.clear();
    int Hits = 0, Problems = 0, Changes = 0;
    for (const auto& Hit : owner_->Search().Hits())
        if (Hit.line_ >= First && Hit.line_ <= Last) ++Hits;
    for (const auto& Problem : owner_->Problems())
        if (Problem.line_ >= First && Problem.line_ <= Last) ++Problems;
    for (const auto& Mark : owner_->Changes())
        if (Mark.line_ >= First && Mark.line_ <= Last) ++Changes;
    if (Hits) chips_ << tr("검색 %1").arg(Hits);
    if (Problems) chips_ << tr("문제 %1").arg(Problems);
    if (Changes) chips_ << tr("변경 %1줄").arg(Changes);

    font_ = owner_->EditorFont();
    font_.setPointSizeF(qMax(7.0, font_.pointSizeF() * 0.9));
    const QFontMetrics Metrics(font_);
    const QFontMetrics UiMetrics(font());
    const int Number = Metrics.horizontalAdvance(QStringLiteral("0")) * (QString::number(Count).size() + 1);
    const int Width = qMin(640, Number + Padding * 3 + Metrics.horizontalAdvance(QLatin1Char('M')) * 56);
    const int Height = UiMetrics.height() + Padding + Metrics.lineSpacing() * int(rows_.size()) + Padding;
    resize(Width, Height);
    QRect Available = QGuiApplication::screenAt(Anchor.center()) ? QGuiApplication::screenAt(Anchor.center())->availableGeometry() : QRect(0, 0, 4096, 4096);
    int X = LeftOfAnchor ? Anchor.left() - Width - 8 : Anchor.right() + 8;
    X = qBound(Available.left() + 4, X, Available.right() - Width - 4);
    const int Y = qBound(Available.top() + 4, GlobalY - Height / 2, Available.bottom() - Height - 4);
    move(X, Y);
    show();
    raise();
    update();
}

void TextMinimapPeek::paintEvent(QPaintEvent*)
{
    QPainter Painter(this);
    const auto& Colors = owner_->Colors();
    Painter.fillRect(rect(), Colors.background_);
    Painter.setPen(Colors.separator_);
    Painter.drawRect(rect().adjusted(0, 0, -1, -1));
    const QFontMetrics UiMetrics(font());
    const int HeaderHeight = UiMetrics.height() + Padding / 2;
    QColor HeaderBand = Colors.slider_;
    Painter.fillRect(QRect(1, 1, width() - 2, HeaderHeight), HeaderBand);
    Painter.setFont(font());
    Painter.setPen(Colors.label_);
    int X = Padding;
    Painter.drawText(QRect(X, 1, width(), HeaderHeight), Qt::AlignVCenter | Qt::AlignLeft, header_);
    X += UiMetrics.horizontalAdvance(header_) + Padding;
    const QString Chips = chips_.join(QStringLiteral(" · "));
    const int ChipsWidth = Chips.isEmpty() ? 0 : UiMetrics.horizontalAdvance(Chips) + Padding;
    if (!section_.isEmpty()) {
        Painter.setPen(Colors.labelMinor_);
        Painter.drawText(QRect(X, 1, width() - X - ChipsWidth - Padding, HeaderHeight), Qt::AlignVCenter | Qt::AlignLeft,
                         UiMetrics.elidedText(section_, Qt::ElideRight, width() - X - ChipsWidth - Padding));
    }
    if (!Chips.isEmpty()) {
        Painter.setPen(Colors.search_);
        Painter.drawText(QRect(0, 1, width() - Padding, HeaderHeight), Qt::AlignVCenter | Qt::AlignRight, Chips);
    }
    Painter.setFont(font_);
    const QFontMetrics Metrics(font_);
    const int NumberWidth = Metrics.horizontalAdvance(QStringLiteral("0")) * (QString::number(owner_->Source().LineCount()).size() + 1);
    int Y = HeaderHeight + Padding / 2;
    for (const auto& Item : rows_) {
        if (Item.line_ == center_)
            Painter.fillRect(QRect(1, Y, width() - 2, Metrics.lineSpacing()), Colors.selection_);
        Painter.setPen(Colors.labelMinor_);
        Painter.drawText(QRect(Padding, Y, NumberWidth, Metrics.lineSpacing()), Qt::AlignRight | Qt::AlignVCenter, QLocale().toString(Item.line_ + 1));
        int TextX = Padding * 2 + NumberWidth;
        for (const auto& Run : Item.runs_) {
            Painter.setPen(Run.color_);
            Painter.drawText(QPoint(TextX, Y + Metrics.ascent()), Run.text_);
            TextX += Metrics.horizontalAdvance(Run.text_);
            if (TextX > width() - Padding)
                break;
        }
        Y += Metrics.lineSpacing();
    }
}
