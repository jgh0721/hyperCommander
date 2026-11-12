// Qt 비의존 순수 모듈이다. stdafx.h 를 포함하지 않으므로 테스트 타깃에서
// Qt Widgets/WebEngine 없이 그대로 컴파일할 수 있다.
//
// 판정 규칙은 이 파일에 두지 않는다. 장식 문자·directive·필드·인라인 마크업의
// 정의는 전부 RstStructure 에 있고, 개요·접기·자동완성·인덱스가 같은 것을 쓴다.
// 예전에는 같은 규칙이 다섯 곳에 서로 다르게 구현되어 실제 문서에서 갈라졌다.
//
// std::regex 는 쓰지 않는다. 열한 개를 줄마다 돌리던 예전 구현은 677KB 한국어
// 문서 전체 강조에 364ms 를 썼고 그중 93% 가 정규식 두 개(강조·하이퍼링크)에서
// 나왔다. 같은 판정을 문자 스캐너로 하면 0.9ms 다(실측).
#include "text/RstContainerLexer.h"

#include "text/RstStructure.h"

#include <algorithm>

namespace hc::rst::syntax {
namespace {

[[nodiscard]] std::string trim( const std::string& value )
{
    const std::string_view view = trimView( value );
    return std::string( view );
}

[[nodiscard]] std::string firstLine( std::string_view value )
{
    const std::size_t pos = value.find( '\n' );
    return std::string( pos == std::string_view::npos ? value : value.substr( 0, pos ) );
}

[[nodiscard]] bool startsWith( std::string_view value, std::string_view prefix )
{
    return value.size() >= prefix.size() && value.substr( 0, prefix.size() ) == prefix;
}

[[nodiscard]] bool endsWith( std::string_view value, std::string_view suffix )
{
    return value.size() >= suffix.size() && value.substr( value.size() - suffix.size() ) == suffix;
}

[[nodiscard]] std::string toLower( std::string value )
{
    std::transform( value.begin(), value.end(), value.begin(), []( unsigned char ch ) {
        return static_cast< char >( ( ch >= 'A' && ch <= 'Z' ) ? ch + ( 'a' - 'A' ) : ch );
    } );
    return value;
}

[[nodiscard]] std::string stripColons( std::string value )
{
    while( !value.empty() && value.front() == ':' )
        value.erase( value.begin() );
    while( !value.empty() && value.back() == ':' )
        value.pop_back();
    return value;
}

/// `${...}` 스니펫 자리표시자를 지운다. 예전에는 정규식이었다.
[[nodiscard]] std::string stripSnippetPlaceholders( std::string_view value )
{
    std::string out;
    out.reserve( value.size() );
    for( std::size_t i = 0; i < value.size(); )
    {
        if( value[ i ] == '$' && i + 1 < value.size() && value[ i + 1 ] == '{' )
        {
            const std::size_t close = value.find( '}', i + 2 );
            if( close != std::string_view::npos )
            {
                i = close + 1;
                continue;
            }
        }
        out.push_back( value[ i ] );
        ++i;
    }
    return out;
}

[[nodiscard]] int styleForInline( const InlineKind kind, const RstMetadataCache& cache,
                                  std::string_view roleName )
{
    switch( kind )
    {
        case InlineKind::Literal:      return STYLE_INLINE_LITERAL;
        case InlineKind::Strong:       return STYLE_STRONG;
        case InlineKind::Emphasis:     return STYLE_EMPHASIS;
        case InlineKind::Substitution: return STYLE_SUBSTITUTION;
        case InlineKind::Hyperlink:    return STYLE_HYPERLINK;
        case InlineKind::Interpreted:  return STYLE_INTERPRETED;
        case InlineKind::Role:         return cache.roleStyle( std::string( roleName ) );
    }
    return STYLE_DEFAULT;
}

/// out 의 [start, end) 를 style 로 채운다. 범위를 벗어나면 잘라 낸다.
void paint( std::span< unsigned char > out, const std::size_t start, const std::size_t end,
            const int style )
{
    if( start >= out.size() )
        return;
    const std::size_t last = std::min( end, out.size() );
    if( last <= start )
        return;
    std::fill( out.begin() + static_cast< std::ptrdiff_t >( start ),
               out.begin() + static_cast< std::ptrdiff_t >( last ),
               static_cast< unsigned char >( style ) );
}

/// 제목 묶음이 아닌 줄 하나를 칠한다. base 는 줄의 첫 바이트 오프셋이다.
void paintOrdinaryLine( std::string_view line, const std::size_t base,
                        const RstMetadataCache& cache, std::span< unsigned char > out,
                        std::vector< InlineToken >& scratch )
{
    if( line.empty() )
        return;

    if( isTransitionLine( line ) )
    {
        paint( out, base, base + line.size(), STYLE_TRANSITION );
        return;
    }

    if( const std::optional< DirectiveParts > parts = parseDirective( line ) )
    {
        paint( out, base, base + parts->prefixEnd, STYLE_EXPLICIT_MARKUP );
        if( parts->hasSubstitution() )
            paint( out, base + parts->subStart, base + parts->subEnd, STYLE_SUBSTITUTION );
        const std::string name( line.substr( parts->nameStart, parts->nameEnd - parts->nameStart ) );
        paint( out, base + parts->nameStart, base + parts->colonsEnd, cache.directiveStyle( name ) );
        return;
    }

    if( hasExplicitMarkupPrefix( line ) )
    {
        paint( out, base, base + line.size(), STYLE_COMMENT );
        return;
    }

    if( const std::optional< FieldParts > parts = parseField( line ) )
    {
        paint( out, base + parts->indentEnd, base + parts->markerEnd, STYLE_FIELD_NAME );
        return;
    }

    scratch.clear();
    scanInline( line, scratch );
    for( const InlineToken& token : scratch )
    {
        const std::string_view name =
            line.substr( token.nameStart, token.nameEnd > token.nameStart
                                              ? token.nameEnd - token.nameStart
                                              : 0 );
        paint( out, base + token.start, base + token.end, styleForInline( token.kind, cache, name ) );
    }
}

/// 제목 장식의 종류. 같은 문자라도 윗줄이 있는 제목과 없는 제목은
/// docutils 가 서로 다른 단계로 센다.
struct AdornmentStyle
{
    char character = '\0';
    bool overlined = false;

    [[nodiscard]] bool operator==( const AdornmentStyle& other ) const
    {
        return character == other.character && overlined == other.overlined;
    }
};

}   // namespace

// ── 접기 깊이 ────────────────────────────────────────────

std::vector< FoldLine > computeFoldLevels( const std::string& utf8Text )
{
    const LineIndex   lines( utf8Text );
    const std::size_t count = lines.size();

    std::vector< FoldLine > folds( count );
    if( count == 0 )
        return folds;

    // 장식 양식이 처음 나온 순서가 곧 섹션 깊이다.
    std::vector< AdornmentStyle > sectionStack;
    // 본문 들여쓰기 깊이. 섹션이 바뀌면 처음부터 다시 센다.
    std::vector< int > indentStack{ 0 };

    const auto depthForAdornment = [ &sectionStack ]( const AdornmentStyle& style ) -> int {
        for( std::size_t i = 0; i < sectionStack.size(); ++i )
        {
            if( sectionStack[ i ] == style )
            {
                // 이미 본 단계다. 그 아래 단계들은 여기서 끝난다.
                sectionStack.resize( i + 1 );
                return static_cast< int >( i );
            }
        }
        sectionStack.push_back( style );
        return static_cast< int >( sectionStack.size() - 1 );
    };

    int  sectionDepth = 0;
    bool sawSection = false;

    for( std::size_t i = 0; i < count; )
    {
        const std::string_view line = lines.line( i );

        if( isBlank( line ) )
        {
            folds[ i ].blank = true;
            folds[ i ].level = -1;   // 레벨은 뒤에서 이웃 것으로 채운다
            ++i;
            continue;
        }

        const TitleScan scan = titleScanAt( lines, i );
        if( scan.title )
        {
            sectionDepth = depthForAdornment( { scan.title->adornChar, scan.title->overlined } );
            sawSection = true;
            indentStack.assign( 1, 0 );

            const std::size_t last = std::min< std::size_t >( i + scan.span, count );
            for( std::size_t k = i; k < last; ++k )
                folds[ k ].level = sectionDepth;
            i = last;
            continue;
        }
        if( scan.span > 0 )
        {
            // 제목이 아니지만 docutils 가 통째로 소비하는 구간. 본문 깊이로 둔다.
            const std::size_t last = std::min< std::size_t >( i + scan.span, count );
            for( std::size_t k = i; k < last; ++k )
                folds[ k ].level = sectionDepth + ( sawSection ? 1 : 0 );
            i = last;
            continue;
        }

        // 제목이 하나도 없는 문서에서도 들여쓰기 접기는 살아 있어야 한다.
        const int width = indentWidth( line );
        while( indentStack.size() > 1 && width < indentStack.back() )
            indentStack.pop_back();
        if( width > indentStack.back() )
            indentStack.push_back( width );

        const int indentDepth = static_cast< int >( indentStack.size() ) - 1;
        folds[ i ].level = sectionDepth + ( sawSection ? 1 : 0 ) + indentDepth;
        ++i;
    }

    // 빈 줄은 앞뒤 중 **더 깊은** 쪽에 붙인다.
    //
    // 앞쪽만 보면(Lexilla 의 fold.compact 방식) 제목 바로 아래 빈 줄이 제목과
    // 같은 깊이가 된다. Scintilla 는 머리 플래그만으로 접기 마커를 그리지
    // 않고 **바로 다음 줄**이 실제로 더 깊은지까지 확인하므로, 그러면 제목에
    // 접기 마커가 아예 나타나지 않는다. reST 는 제목과 본문 사이에 빈 줄이
    // 반드시 들어가므로 이 경우가 곧 모든 섹션이다.
    //
    // 뒤쪽만 보면 반대로 섹션 끝의 빈 줄이 다음 섹션 것으로 넘어가 접었을 때
    // 빈 줄만 덩그러니 남는다. 더 깊은 쪽을 고르면 둘 다 해결된다.
    std::vector< int > forward( count, 0 );
    int                carried = 0;
    for( std::size_t i = 0; i < count; ++i )
    {
        if( folds[ i ].level >= 0 )
            carried = folds[ i ].level;
        forward[ i ] = carried;
    }

    int trailing = carried;
    for( std::size_t i = count; i-- > 0; )
    {
        if( folds[ i ].level >= 0 )
        {
            trailing = folds[ i ].level;
            continue;
        }
        folds[ i ].level = std::max( forward[ i ], trailing );
    }

    // 다음 비어 있지 않은 줄이 더 깊으면 이 줄이 접기 머리다.
    int  nextLevel = 0;
    bool haveNext = false;
    for( std::size_t i = count; i-- > 0; )
    {
        if( folds[ i ].blank )
            continue;
        if( haveNext && nextLevel > folds[ i ].level )
            folds[ i ].header = true;
        nextLevel = folds[ i ].level;
        haveNext = true;
    }

    return folds;
}

// ── 어휘 캐시 ────────────────────────────────────────────

int RstMetadataCache::directiveStyle( const std::string& name ) const
{
    if( !directivesPopulated )
        return STYLE_DIRECTIVE_UNKNOWN;

    return directives.contains( name ) ? STYLE_DIRECTIVE_VALID : STYLE_DIRECTIVE_INVALID;
}

int RstMetadataCache::roleStyle( const std::string& name ) const
{
    if( !rolesPopulated )
        return STYLE_ROLE_UNKNOWN;

    return roles.contains( name ) ? STYLE_ROLE_VALID : STYLE_ROLE_INVALID;
}

void RstMetadataCache::updateFromCompletion( const std::vector< CompletionEntry >& entries )
{
    for( const CompletionEntry& entry : entries )
    {
        const std::string detailLower = toLower( entry.detail );
        const std::string insertTrimmed = trim( entry.insertText );
        if( endsWith( insertTrimmed, "::" ) || detailLower.find( "directive" ) != std::string::npos )
        {
            const std::string name = extractDirectiveName( entry.label, entry.insertText );
            if( !name.empty() )
            {
                directives.insert( name );
                directivesPopulated = true;
            }
        }
        else if( startsWith( entry.label, ":" ) && endsWith( entry.label, ":" ) )
        {
            const std::string name = stripColons( entry.label );
            if( !name.empty() )
            {
                roles.insert( name );
                rolesPopulated = true;
            }
        }
        else if( detailLower.find( "role" ) != std::string::npos )
        {
            const std::string name = stripColons( entry.label );
            if( !name.empty() )
            {
                roles.insert( name );
                rolesPopulated = true;
            }
        }
    }
}

std::string extractDirectiveName( const std::string& label, const std::string& insertText )
{
    std::string text = trim( firstLine( insertText ) );
    if( startsWith( text, ".." ) )
        text = trim( text.substr( 2 ) );

    // 스니펫 placeholder 를 "::" 보다 먼저 제거해야 한다. Python 원본은 순서가
    // 반대라서 "image:: ${1:path}" 가 "image::" 로 남고, 그 이름이 캐시에 들어가면
    // 정작 "image" directive 가 INVALID(빨강)로 표시된다.
    text = trim( stripSnippetPlaceholders( text ) );
    if( endsWith( text, "::" ) )
        text = trim( text.substr( 0, text.size() - 2 ) );

    if( !text.empty() )
        return text;

    std::string fallback = trim( firstLine( label ) );
    if( startsWith( fallback, ".." ) )
        fallback = trim( fallback.substr( 2 ) );
    if( endsWith( fallback, "::" ) )
        fallback = trim( fallback.substr( 0, fallback.size() - 2 ) );

    return fallback;
}

// ── 렉서 ─────────────────────────────────────────────────

RstContainerLexer::RstContainerLexer( RstMetadataCache cache )
    : cache_( std::move( cache ) )
{
}

const RstMetadataCache& RstContainerLexer::metadataCache() const
{
    return cache_;
}

RstMetadataCache& RstContainerLexer::metadataCache()
{
    return cache_;
}

void RstContainerLexer::styleInto( std::string_view utf8Text, std::span< unsigned char > out ) const
{
    std::fill( out.begin(), out.end(), static_cast< unsigned char >( STYLE_DEFAULT ) );
    if( utf8Text.empty() )
        return;

    const LineIndex            lines( utf8Text );
    std::vector< InlineToken > scratch;
    scratch.reserve( 16 );

    // 리터럴 블록은 줄 사이 상태다. 창이 블록 밖에서 시작한다는 보장은 부르는
    // 쪽(handleStyleNeeded)이 창을 0열 줄까지 넓혀서 준다.
    LiteralBlockTracker literal;

    for( std::size_t i = 0; i < lines.size(); )
    {
        const std::string_view line = lines.line( i );

        // 리터럴 본문이 아닐 때만 제목을 찾는다. 상태를 미리 보는 이유는 제목
        // 묶음이 여러 줄을 한꺼번에 가져가기 때문이다 — 먹인 뒤에 되돌릴 수 없다.
        if( !literal.peek( line ) )
        {
            const TitleScan scan = titleScanAt( lines, i );
            if( scan.span > 0 )
            {
                if( scan.title )
                {
                    // 묶음 전체(윗줄·제목·밑줄)를 제목색으로 칠한다. 개행은 남긴다.
                    for( std::uint32_t k = scan.title->firstLine; k <= scan.title->lastLine; ++k )
                    {
                        const std::size_t base = lines.byteStart( k );
                        paint( out, base, base + lines.line( k ).size(), STYLE_TITLE );
                    }
                }
                // 묶음이 가져간 줄도 빠짐없이 상태 기계에 흘려보낸다.
                for( std::uint32_t k = 0; k < scan.span; ++k )
                    literal.consumeAsTitle( lines.line( i + k ) );
                i += scan.span;
                continue;
            }
        }

        if( literal.feed( line ) )
            paint( out, lines.byteStart( i ), lines.byteStart( i ) + line.size(), STYLE_LITERAL );
        else
            paintOrdinaryLine( line, lines.byteStart( i ), cache_, out, scratch );
        ++i;
    }
}

std::vector< Span > RstContainerLexer::tokenizeLine( const std::string& line,
                                                    const std::string& previousLine,
                                                    const std::string& nextLine,
                                                    const std::string& lineAfterNext ) const
{
    // 네 줄짜리 작은 문서를 만들어 같은 경로로 칠한 뒤 가운데 줄만 잘라 낸다.
    // 제목은 세 줄에 걸치므로 줄 하나만 보고는 판정할 수 없고, 판정 규칙을 여기
    // 한 벌 더 두면 그것이 곧 갈라지는 지점이 된다.
    std::string document;
    document.reserve( previousLine.size() + line.size() + nextLine.size() + lineAfterNext.size() + 4 );
    document += previousLine;
    document += '\n';
    const std::size_t base = document.size();
    document += line;
    document += '\n';
    document += nextLine;
    document += '\n';
    document += lineAfterNext;

    std::vector< unsigned char > styles( document.size(), static_cast< unsigned char >( STYLE_DEFAULT ) );
    styleInto( document, styles );

    std::vector< Span > spans;
    std::size_t         runStart = 0;
    for( std::size_t i = 0; i <= line.size(); ++i )
    {
        const int current = ( i < line.size() ) ? static_cast< int >( styles[ base + i ] ) : -1;
        const int previous = static_cast< int >( styles[ base + runStart ] );
        if( i == line.size() || current != previous )
        {
            if( previous != STYLE_DEFAULT )
                spans.push_back( { runStart, i, previous } );
            runStart = i;
        }
    }
    return spans;
}

std::vector< Span > RstContainerLexer::styleText( const std::string& utf8Text ) const
{
    const std::vector< unsigned char > styles = styleBytes( utf8Text );

    std::vector< Span > spans;
    std::size_t         runStart = 0;
    for( std::size_t i = 0; i <= styles.size(); ++i )
    {
        const bool end = ( i == styles.size() );
        if( end || styles[ i ] != styles[ runStart ] )
        {
            if( runStart < styles.size() )
                spans.push_back( { runStart, i, static_cast< int >( styles[ runStart ] ) } );
            runStart = i;
        }
    }
    return spans;
}

std::vector< unsigned char > RstContainerLexer::styleBytes( const std::string& utf8Text ) const
{
    std::vector< unsigned char > result( utf8Text.size(), static_cast< unsigned char >( STYLE_DEFAULT ) );
    styleInto( utf8Text, result );
    return result;
}

}   // namespace hc::rst::syntax
