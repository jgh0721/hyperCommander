#pragma once

#include "rstGlossaryIndex.hpp"
#include "text/RstStructure.h"
#include <QRegularExpression>
#include <QUrl>
#include <optional>

namespace hc::rst::reference {

enum class Kind { Term, Ref, Url };
struct Target
{
    Kind kind;
    QString value;
};

inline QString normalizedName( const QString& value )
{
    return value.simplified().toCaseFolded();
}

// Columns are zero-based UTF-16 offsets. The closing boundary
// also counts so F12 works immediately after typing a link.
inline std::optional<Target> targetAt( const QString& line, int column, bool isRst )
{
    if( column < 0 || column > line.size() ) return {};
    if( isRst )
    {
        const QByteArray utf8 = line.toUtf8();
        const int offset = static_cast<int>( line.left( column ).toUtf8().size() );
        std::vector<syntax::InlineToken> tokens;
        syntax::scanInline( std::string_view( utf8.constData(), utf8.size() ), tokens );
        for( const auto& token : tokens )
        {
            if( token.kind != syntax::InlineKind::Role || offset < token.start || offset > token.end ) continue;
            QString role = QString::fromUtf8( utf8.mid( token.nameStart, token.nameEnd - token.nameStart ) );
            if( role.startsWith( QLatin1String( "std:" ) ) ) role.remove( 0, 4 );
            if( role != QLatin1String( "term" ) && role != QLatin1String( "ref" ) ) continue;
            QString value = QString::fromUtf8( utf8.mid( token.nameEnd + 2, token.end - token.nameEnd - 3 ) ).trimmed();
            static const QRegularExpression title( QStringLiteral( R"(^.*<([^<>]+)>$)" ) );
            const auto explicitTitle = title.match( value );
            if( explicitTitle.hasMatch() ) value = explicitTitle.captured( 1 ).trimmed();
            if( value.startsWith( QLatin1Char( '~' ) ) ) value.remove( 0, 1 );
            if( !value.isEmpty() )
                return Target{ role == QLatin1String( "term" ) ? Kind::Term : Kind::Ref, value };
        }
    }
    static const QRegularExpression urlStart( QStringLiteral( R"(\bhttps?://)" ),
                                             QRegularExpression::CaseInsensitiveOption );
    auto matches = urlStart.globalMatch( line );
    while( matches.hasNext() )
    {
        const auto match = matches.next();
        const int start = static_cast<int>( match.capturedStart() );
        int end = static_cast<int>( match.capturedEnd() );
        int parentheses = 0;
        int brackets = 0;
        for( ; end < line.size(); ++end )
        {
            const QChar ch = line.at( end );
            if( ch.isSpace() || QStringLiteral( "<>\"'`" ).contains( ch ) ) break;
            if( ch == QLatin1Char( '(' ) ) ++parentheses;
            if( ch == QLatin1Char( ')' ) && --parentheses < 0 ) break;
            if( ch == QLatin1Char( '[' ) ) ++brackets;
            if( ch == QLatin1Char( ']' ) && --brackets < 0 ) break;
        }
        while( end > start && QStringLiteral( ".,;:!?" ).contains( line.at( end - 1 ) ) ) --end;
        if( column < start || column > end ) continue;
        const QString value = line.mid( start, end - start );
        const QUrl url( value );
        if( url.isValid() && !url.host().isEmpty() ) return Target{ Kind::Url, value };
    }
    return {};
}

// Returns a one-based source line, or zero if the document has no such target.
inline int definitionLine( const QString& text, const Target& target )
{
    const QString wanted = normalizedName( target.value );
    const QString source = text.startsWith( QChar( 0xfeff ) ) ? text.mid( 1 ) : text;
    if( target.kind == Kind::Term )
    {
        for( const auto& entry : parseGlossary( source ) )
            if( normalizedName( entry.term ) == wanted ) return entry.line;
    }
    else if( target.kind == Kind::Ref )
    {
        static const QRegularExpression label( QStringLiteral( R"(^\s*\.\.\s+_(?:`([^`]+)`|([^\r\n]+?)):\s*$)" ) );
        const auto lines = source.split( QLatin1Char( '\n' ) );
        for( int i = 0; i < lines.size(); ++i )
        {
            const auto match = label.match( lines.at( i ) );
            if( match.hasMatch() && normalizedName( match.captured( 1 ).isEmpty()
                ? match.captured( 2 ) : match.captured( 1 ) ) == wanted ) return i + 1;
        }
    }
    return 0;
}

} // namespace hc::rst::reference
