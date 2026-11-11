#include "stdafx.h"
#include "text/TextMinimapSource.h"

/// 모듈

/// UIs

/// 기타 헤더파일
#include "text/ScintillaQtDirectBackend.h"

/// 고유 라이브러리

/// 공통 라이브러리

/// 외부 라이브러리
#include <Scintilla.h>

/// 선언 헤더파일들

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QtGlobal>

ScintillaMinimapSource::ScintillaMinimapSource(ScintillaQtDirectBackend* Backend)
    : backend_(Backend)
{
}

qintptr ScintillaMinimapSource::send(unsigned int Message, quintptr W, qintptr L) const
{
    return backend_ ? backend_->Send(Message, W, L) : 0;
}

qintptr ScintillaMinimapSource::DocumentId() const { return send(SCI_GETDOCPOINTER); }
int ScintillaMinimapSource::LineCount() const { return qMax(1, int(send(SCI_GETLINECOUNT))); }
qint64 ScintillaMinimapSource::Length() const { return send(SCI_GETLENGTH); }
int ScintillaMinimapSource::DisplayRows() const { return qMax(1, int(send(SCI_VISIBLEFROMDOCLINE, quintptr(LineCount())))); }
int ScintillaMinimapSource::RowOfLine(int Line) const { return int(send(SCI_VISIBLEFROMDOCLINE, quintptr(qMax(0, Line)))); }
int ScintillaMinimapSource::LineOfRow(int Row) const { return int(send(SCI_DOCLINEFROMVISIBLE, quintptr(qMax(0, Row)))); }
int ScintillaMinimapSource::FirstVisibleRow() const { return int(send(SCI_GETFIRSTVISIBLELINE)); }
int ScintillaMinimapSource::RowsOnScreen() const { return qMax(1, int(send(SCI_LINESONSCREEN))); }
void ScintillaMinimapSource::ScrollToRow(int Row) { send(SCI_SETFIRSTVISIBLELINE, quintptr(qMax(0, Row))); }
qint64 ScintillaMinimapSource::LineStart(int Line) const { return send(SCI_POSITIONFROMLINE, quintptr(qMax(0, Line))); }
qint64 ScintillaMinimapSource::LineEnd(int Line) const { return send(SCI_GETLINEENDPOSITION, quintptr(qMax(0, Line))); }
int ScintillaMinimapSource::LineFromPosition(qint64 Position) const { return int(send(SCI_LINEFROMPOSITION, quintptr(qMax<qint64>(0, Position)))); }

int ScintillaMinimapSource::SubRowOf(qint64 Position) const
{
    // One layout of the containing line; callers use it for the caret and for the few matches in view only.
    const int Line = LineFromPosition(Position);
    const qintptr Height = qMax<qintptr>(1, send(SCI_TEXTHEIGHT, quintptr(Line)));
    const qintptr Top = send(SCI_POINTYFROMPOSITION, 0, qintptr(LineStart(Line)));
    const qintptr Y = send(SCI_POINTYFROMPOSITION, 0, qintptr(Position));
    return qMax(0, int((Y - Top) / Height));
}

QByteArray ScintillaMinimapSource::Text(qint64 Start, qint64 End) const
{
    if (!backend_ || End <= Start)
        return {};
    QByteArray Buffer(qsizetype(End - Start) + 1, '\0');
    Sci_TextRangeFull Range{{Sci_Position(Start), Sci_Position(End)}, Buffer.data()};
    const auto Read = send(SCI_GETTEXTRANGEFULL, 0, reinterpret_cast<qintptr>(&Range));
    Buffer.truncate(qMax<qintptr>(0, Read));
    return Buffer;
}

void ScintillaMinimapSource::StyledText(qint64 Start, qint64 End, QByteArray& Text, QByteArray& Styles) const
{
    Text.clear(); Styles.clear();
    if (!backend_ || End <= Start)
        return;
    // SCI_GETSTYLEDTEXTFULL writes character and style bytes alternately, plus two terminating zero bytes.
    QByteArray Buffer(qsizetype(End - Start) * 2 + 2, '\0');
    Sci_TextRangeFull Range{{Sci_Position(Start), Sci_Position(End)}, Buffer.data()};
    const auto Bytes = send(SCI_GETSTYLEDTEXTFULL, 0, reinterpret_cast<qintptr>(&Range));
    const qsizetype Count = qMin<qsizetype>(qsizetype(End - Start), qsizetype(Bytes / 2));
    Text.resize(Count); Styles.resize(Count);
    for (qsizetype Index = 0; Index < Count; ++Index) {
        Text[Index] = Buffer[Index * 2];
        Styles[Index] = Buffer[Index * 2 + 1];
    }
}

qint64 ScintillaMinimapSource::EndStyled() const { return send(SCI_GETENDSTYLED); }
int ScintillaMinimapSource::TabWidth() const { return qMax(1, int(send(SCI_GETTABWIDTH))); }
int ScintillaMinimapSource::Marks(int Line) const { return int(send(SCI_MARKERGET, quintptr(qMax(0, Line)))); }
int ScintillaMinimapSource::FoldLevel(int Line) const { return int(send(SCI_GETFOLDLEVEL, quintptr(qMax(0, Line)))); }

qint64 ScintillaMinimapSource::Find(const QByteArray& Utf8, int Flags, qint64 Start, qint64 End, qint64& MatchEnd) const
{
    MatchEnd = -1;
    if (!backend_ || Utf8.isEmpty() || End <= Start)
        return -1;
    // SCI_FINDTEXTFULL leaves the target range alone, so a running search never disturbs the find bar.
    QByteArray Text = Utf8;
    Sci_TextToFindFull Find{{Sci_Position(Start), Sci_Position(End)}, Text.data(), {0, 0}};
    const auto Found = send(SCI_FINDTEXTFULL, quintptr(Flags), reinterpret_cast<qintptr>(&Find));
    if (Found < 0)
        return -1;
    MatchEnd = Find.chrgText.cpMax;
    return Found;
}

qint64 ScintillaMinimapSource::PositionAfter(qint64 Position) const { return send(SCI_POSITIONAFTER, quintptr(qMax<qint64>(0, Position))); }
qint64 ScintillaMinimapSource::CaretPosition() const { return send(SCI_GETCURRENTPOS); }

void ScintillaMinimapSource::Selection(qint64& Start, qint64& End) const
{
    Start = send(SCI_GETSELECTIONSTART);
    End = send(SCI_GETSELECTIONEND);
}

bool ScintillaMinimapSource::HistoryQueryable() const { return backend_ && backend_->ChangeHistoryQueryable(); }
