#include "stdafx.h"
#include "textDocumentMetrics.hpp"

/// 모듈

/// UIs

/// 기타 헤더파일

/// 고유 라이브러리

/// 공통 라이브러리

/// 외부 라이브러리

/// 선언 헤더파일들

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <array>
#include <numeric>

DECLARE_CMNLIBSV2_NAMESPACE

namespace hc::document
{
    namespace
    {
        // Same classification as the open-time scan in documentStorageAdapter.cpp and the serialization
        // scan in DocumentViewPane::serializeContent. Those stop at the second kind; this counts every break.
        template< class Unit >
        LineEndingStats CountBreaks( const Unit* Data, qsizetype Size )
        {
            LineEndingStats Stats;
            for( qsizetype At = 0; At < Size; ++At )
            {
                if( Data[ At ] == Unit( '\r' ) )
                {
                    if( At + 1 < Size && Data[ At + 1 ] == Unit( '\n' ) ) { ++Stats.crlf_; ++At; }
                    else ++Stats.cr_;
                }
                else if( Data[ At ] == Unit( '\n' ) )
                    ++Stats.lf_;
            }
            return Stats;
        }

        bool IsBreak( char16_t Unit ) { return Unit == u'\r' || Unit == u'\n'; }
    }

    QString LineEndingStats::Dominant() const
    {
        if( int( crlf_ > 0 ) + int( lf_ > 0 ) + int( cr_ > 0 ) > 1 )
            return QStringLiteral( "MIXED" );
        return crlf_ ? QStringLiteral( "CRLF" ) : lf_ ? QStringLiteral( "LF" ) : cr_ ? QStringLiteral( "CR" ) : QString();
    }

    LineEndingStats CountLineEndings( QStringView Text )
    {
        return CountBreaks( Text.utf16(), Text.size() );
    }

    LineEndingStats CountLineEndings( QByteArrayView Bytes )
    {
        return CountBreaks( Bytes.constData(), Bytes.size() );
    }

    IndentGuess DetectIndent( QStringView Text, int MaxLines )
    {
        IndentGuess Guess;
        // Steps[ n ] counts indentation increases of n spaces between consecutive non-blank lines. The
        // nesting step is the width; absolute depths cannot tell 4 from 8 when most blocks sit deep.
        std::array< qsizetype, 9 > Steps{};
        // Space-indented lines whose depth is a multiple of 2, 4 and 8.
        std::array< qsizetype, 9 > Divisible{};
        // Previous is the depth of the last non-blank line; a tab line breaks the chain with -1.
        qsizetype   Gcd = 0, Previous = -1, Start = 0;
        const auto* Data = Text.utf16();
        const auto  Size = Text.size();
        for( int Line = 0; Start < Size && ( MaxLines <= 0 || Line < MaxLines ); ++Line )
        {
            qsizetype End = Start;
            while( End < Size && !IsBreak( Data[ End ] ) ) ++End;
            qsizetype Spaces = 0;
            while( Start + Spaces < End && Data[ Start + Spaces ] == u' ' ) ++Spaces;
            qsizetype Content = Start + Spaces;
            while( Content < End && ( Data[ Content ] == u' ' || Data[ Content ] == u'\t' ) ) ++Content;
            if( Content < End )
            {
                if( Data[ Start ] == u'\t' )
                {
                    ++Guess.tabLines_;
                    Previous = -1;
                }
                // One space is a block-comment continuation (" * ") or alignment, never a level.
                else if( Spaces != 1 )
                {
                    if( Spaces >= 2 )
                    {
                        ++Guess.spaceLines_;
                        Gcd = std::gcd( Gcd, Spaces );
                        for( const int Multiple : { 2, 4, 8 } )
                            if( Spaces % Multiple == 0 ) ++Divisible[ Multiple ];
                    }
                    if( Previous >= 0 && Spaces > Previous && Spaces - Previous <= 8 ) ++Steps[ Spaces - Previous ];
                    Previous = Spaces;
                }
            }
            Start = End;
            if( Start < Size ) Start += Data[ Start ] == u'\r' && Start + 1 < Size && Data[ Start + 1 ] == u'\n' ? 2 : 1;
        }

        const qsizetype StepTotal = Steps[ 2 ] + Steps[ 4 ] + Steps[ 8 ];
        int             Width = 0;
        qsizetype       Best = 0;
        // 4 goes first so that it keeps a tie.
        for( const int Candidate : { 4, 2, 8 } )
            if( Steps[ Candidate ] > Best ) { Best = Steps[ Candidate ]; Width = Candidate; }
        bool WidthSure = Width && Best * 2 > StepTotal && Divisible[ Width ] * 5 >= Guess.spaceLines_ * 4;
        if( !Width && Gcd >= 2 )
        {
            // No usable step (every increase was odd or deeper than 8): use the common divisor of the depths.
            Width = Gcd % 8 == 0 ? 8 : Gcd % 4 == 0 ? 4 : Gcd % 2 == 0 ? 2 : 0;
            WidthSure = Width != 0;
        }

        const qsizetype Indented = Guess.tabLines_ + Guess.spaceLines_;
        Guess.useTabs_ = Guess.tabLines_ > Guess.spaceLines_;
        if( !Guess.useTabs_ && Width ) Guess.width_ = Width;
        // Confident: at least 3 indented lines, the winning style on 80 % of them, and for spaces a width
        // that explains more than half of the nesting steps and divides 80 % of the space depths.
        Guess.confident_ = Indented >= 3 && qMax( Guess.tabLines_, Guess.spaceLines_ ) * 5 >= Indented * 4
                        && ( Guess.useTabs_ || WidthSure );
        return Guess;
    }

    TextPosition PositionOfOffset( QStringView Text, qsizetype Utf16Offset )
    {
        const auto  Offset = qBound( qsizetype( 0 ), Utf16Offset, Text.size() );
        const auto* Data = Text.utf16();
        qsizetype   Line = 1, LineStart = 0;
        for( qsizetype At = 0; At < Offset; ++At )
        {
            // The LF of a CRLF ends the line; its CR alone does not.
            if( Data[ At ] == u'\n' || ( Data[ At ] == u'\r' && ( At + 1 >= Text.size() || Data[ At + 1 ] != u'\n' ) ) )
            {
                ++Line;
                LineStart = At + 1;
            }
        }
        return { int( Line ), int( Offset - LineStart + 1 ) };
    }
}
