#include "stdafx.h"
#include "completionMatch.hpp"

/// 모듈

/// UIs

/// 기타 헤더파일

/// 고유 라이브러리

/// 공통 라이브러리

/// 외부 라이브러리

/// 선언 헤더파일들

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )

DECLARE_CMNLIBSV2_NAMESPACE
namespace hc::document::completion
{
    bool FuzzyMatch( const QString& Pattern, const QString& Candidate, int* Score, QVector< int >* MatchedPositions )
    {
        if( Score != nullptr )
            *Score = 0;
        if( MatchedPositions != nullptr )
            MatchedPositions->clear();

        if( Pattern.isEmpty() )
            return true;
        if( Pattern.length() > Candidate.length() )
            return false;

        const QString FoldedPattern = Pattern.toCaseFolded();
        const QString FoldedCandidate = Candidate.toCaseFolded();

        QVector< int > Positions;
        Positions.reserve( FoldedPattern.length() );
        int PatternIndex = 0;
        for( int Index = 0; Index < FoldedCandidate.length() && PatternIndex < FoldedPattern.length(); ++Index )
        {
            if( FoldedCandidate.at( Index ) == FoldedPattern.at( PatternIndex ) )
            {
                Positions.push_back( Index );
                ++PatternIndex;
            }
        }
        if( PatternIndex < FoldedPattern.length() )
            return false;

        static const QString BoundaryChars = QStringLiteral( "_-/\\ ." );
        int Total = 0;
        int Previous = -2;
        for( int Order = 0; Order < Positions.size(); ++Order )
        {
            const int Position = Positions.at( Order );
            if( Position == Previous + 1 )
                Total += 5;
            if( Position == 0 )
                Total += 10;
            else if( BoundaryChars.contains( Candidate.at( Position - 1 ) ) )
                Total += 8;
            if( Order == Position )
                Total += 3;
            Previous = Position;
        }
        // 흩어져 있을수록 감점. "cb" 가 "code-block" 보다 "c...b" 를 이기지 않게.
        Total -= ( Positions.last() - Positions.first() + 1 ) - Positions.size();

        if( Score != nullptr )
            *Score = Total;
        if( MatchedPositions != nullptr )
            *MatchedPositions = Positions;
        return true;
    }

    namespace
    {
        bool WordStart( const QString& Text, const int Index )
        {
            if( Index == 0 )
                return true;
            const QChar Current = Text.at( Index ), Before = Text.at( Index - 1 );
            if( Before == QLatin1Char( '_' ) || Before == QLatin1Char( '-' ) || Before == QLatin1Char( '.' ) || Before.isSpace() )
                return Current.isLetterOrNumber();
            if( Current.isUpper() && !Before.isUpper() )
                return true;
            return Current.isDigit() && !Before.isDigit();
        }
    }

    bool InitialsMatch( const QString& Pattern, const QString& Candidate, int* Score, QVector< int >* MatchedPositions )
    {
        if( Score != nullptr )
            *Score = 0;
        if( MatchedPositions != nullptr )
            MatchedPositions->clear();
        if( Pattern.isEmpty() )
            return true;
        if( Pattern.length() > Candidate.length() )
            return false;

        QVector< int > Positions;
        if( Candidate.startsWith( Pattern, Qt::CaseInsensitive ) )
        {
            for( int Index = 0; Index < Pattern.length(); ++Index )
                Positions.push_back( Index );
            if( Score != nullptr )
                *Score = 300 + ( Pattern.length() == Candidate.length() ? 50 : 0 );
            if( MatchedPositions != nullptr )
                *MatchedPositions = Positions;
            return true;
        }

        const QString FoldedPattern = Pattern.toCaseFolded(), FoldedCandidate = Candidate.toCaseFolded();
        // 첫 글자는 이름의 첫 글자여야 한다(접두어가 아니면 머리 글자 규칙).
        if( FoldedCandidate.at( 0 ) != FoldedPattern.at( 0 ) )
            return false;
        Positions.push_back( 0 );
        int Skipped = 0;
        for( int PatternIndex = 1; PatternIndex < FoldedPattern.length(); ++PatternIndex )
        {
            const QChar Wanted = FoldedPattern.at( PatternIndex );
            const int Last = Positions.last();
            int Found = -1;
            for( int Index = Last + 1; Index < FoldedCandidate.length(); ++Index )
            {
                if( !WordStart( Candidate, Index ) )
                    continue;
                if( FoldedCandidate.at( Index ) == Wanted ) { Found = Index; break; }
                ++Skipped;
            }
            if( Found < 0 && Last + 1 < FoldedCandidate.length() && FoldedCandidate.at( Last + 1 ) == Wanted )
                Found = Last + 1;
            if( Found < 0 )
                return false;
            Positions.push_back( Found );
        }
        if( Score != nullptr )
            *Score = qMax( 1, 200 - Skipped * 10 );
        if( MatchedPositions != nullptr )
            *MatchedPositions = Positions;
        return true;
    }
}
