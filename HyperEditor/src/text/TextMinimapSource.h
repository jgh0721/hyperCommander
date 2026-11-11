#pragma once

/// 모듈

/// UIs

/// 기타 헤더파일

/// 고유 라이브러리

/// 공통 라이브러리

/// 외부 라이브러리

/// 선언 헤더파일들
class ScintillaQtDirectBackend;

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QByteArray>
#include <QPointer>
#include <QtGlobal>

// Everything the minimap reads from the editor. The model, renderer and widgets never send Scintilla messages
// themselves; unit tests replace this class with an in-memory document.
//
// Every call is bounded: none reads or lays out a whole line or scans the document (no SCI_GETCOLUMN,
// SCI_WRAPCOUNT, SCI_GETLINEINDENTATION or SCI_MARKERNEXT). Callers ask for byte-limited prefixes instead.
class TextMinimapSource
{
public:
    virtual ~TextMinimapSource() = default;

    // Identifies the shown document (Scintilla document pointer); changes with AttachDocument().
    virtual qintptr DocumentId() const = 0;
    virtual int LineCount() const = 0;
    virtual qint64 Length() const = 0;
    // Display rows include wrapped sub-rows and skip folded lines. DisplayRows() is the end sentinel
    // (SCI_VISIBLEFROMDOCLINE of the line count), correct even when the last line is folded.
    virtual int DisplayRows() const = 0;
    virtual int RowOfLine(int Line) const = 0;
    virtual int LineOfRow(int Row) const = 0;
    virtual int FirstVisibleRow() const = 0;
    virtual int RowsOnScreen() const = 0;
    virtual void ScrollToRow(int Row) = 0;
    virtual qint64 LineStart(int Line) const = 0;
    virtual qint64 LineEnd(int Line) const = 0;
    virtual int LineFromPosition(qint64 Position) const = 0;
    // Wrapped sub-row of Position inside its line (0 for the first row).
    virtual int SubRowOf(qint64 Position) const = 0;
    virtual QByteArray Text(qint64 Start, qint64 End) const = 0;
    // Characters and their style bytes for [Start, End).
    virtual void StyledText(qint64 Start, qint64 End, QByteArray& Text, QByteArray& Styles) const = 0;
    virtual qint64 EndStyled() const = 0;
    virtual int TabWidth() const = 0;
    virtual int Marks(int Line) const = 0;
    virtual int FoldLevel(int Line) const = 0;
    // Next match of Utf8 in [Start, End) with Scintilla search flags; -1 when none. MatchEnd receives the end.
    virtual qint64 Find(const QByteArray& Utf8, int Flags, qint64 Start, qint64 End, qint64& MatchEnd) const = 0;
    virtual qint64 PositionAfter(qint64 Position) const = 0;
    virtual qint64 CaretPosition() const = 0;
    virtual void Selection(qint64& Start, qint64& End) const = 0;
    virtual bool HistoryQueryable() const = 0;
};

class ScintillaMinimapSource final : public TextMinimapSource
{
public:
    explicit ScintillaMinimapSource(ScintillaQtDirectBackend* Backend);

    qintptr DocumentId() const override;
    int LineCount() const override;
    qint64 Length() const override;
    int DisplayRows() const override;
    int RowOfLine(int Line) const override;
    int LineOfRow(int Row) const override;
    int FirstVisibleRow() const override;
    int RowsOnScreen() const override;
    void ScrollToRow(int Row) override;
    qint64 LineStart(int Line) const override;
    qint64 LineEnd(int Line) const override;
    int LineFromPosition(qint64 Position) const override;
    int SubRowOf(qint64 Position) const override;
    QByteArray Text(qint64 Start, qint64 End) const override;
    void StyledText(qint64 Start, qint64 End, QByteArray& Text, QByteArray& Styles) const override;
    qint64 EndStyled() const override;
    int TabWidth() const override;
    int Marks(int Line) const override;
    int FoldLevel(int Line) const override;
    qint64 Find(const QByteArray& Utf8, int Flags, qint64 Start, qint64 End, qint64& MatchEnd) const override;
    qint64 PositionAfter(qint64 Position) const override;
    qint64 CaretPosition() const override;
    void Selection(qint64& Start, qint64& End) const override;
    bool HistoryQueryable() const override;

private:
    qintptr send(unsigned int Message, quintptr W = 0, qintptr L = 0) const;
    QPointer<ScintillaQtDirectBackend> backend_;
};
