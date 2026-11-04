#pragma once

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QByteArrayView>
#include <QString>
#include <QStringView>

namespace hc::document
{
    // Line breaks by kind. A CR directly followed by LF is one CRLF; every other CR is a CR,
    // so "\r\r\n" is CR + CRLF and a trailing "\r" is CR.
    struct LineEndingStats
    {
        qsizetype   crlf_ = 0, lf_ = 0, cr_ = 0;

        qsizetype   Breaks() const { return crlf_ + lf_ + cr_; }
        // The editor's line count (SCI_GETLINECOUNT): the line after the last break counts even when empty.
        qsizetype   Lines() const { return Breaks() + 1; }
        // "CRLF", "LF" or "CR" when one kind occurs, "MIXED" when several do, "" without any break.
        QString     Dominant() const;
    };

    LineEndingStats CountLineEndings( QStringView Text );
    // Raw bytes of UTF-8 or of an ASCII-compatible code page (CP949, Shift_JIS, GB18030, ISO-8859-1, ANSI):
    // their multibyte trail bytes never take 0x0A or 0x0D. Not valid for UTF-16 bytes.
    LineEndingStats CountLineEndings( QByteArrayView Bytes );

    // useTabs_/width_ are the guess; width_ stays 4 unless spaces win and a width of 2, 4 or 8 is found.
    struct IndentGuess
    {
        bool        useTabs_ = false;
        int         width_ = 4;
        bool        confident_ = false;
        qsizetype   tabLines_ = 0, spaceLines_ = 0;
    };

    // Reads the first MaxLines lines (blank ones included; MaxLines <= 0 reads all).
    IndentGuess DetectIndent( QStringView Text, int MaxLines = 2000 );

    // 1-based. column_ counts UTF-16 code units like QTextView::currentColumn() and goToPosition().
    struct TextPosition { int line_ = 0, column_ = 0; };

    // Utf16Offset is clamped into the text. CR, LF and CRLF end a line; an offset between the CR
    // and LF of a CRLF stays on the line the CR ends.
    TextPosition PositionOfOffset( QStringView Text, qsizetype Utf16Offset );
}
