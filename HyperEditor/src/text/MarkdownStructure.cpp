// Qt 비의존 순수 모듈이다. RstContainerLexer.cpp 와 같은 규칙으로 쓴다.
#include "text/MarkdownStructure.h"

#include <algorithm>
#include <cctype>

namespace hc::rst::md {
namespace {

constexpr int kMaxHeadingIndent = 3;   ///< 4칸부터는 들여쓴 코드블록이다 (CommonMark)
constexpr int kTabWidth = 4;

[[nodiscard]] bool isSpace( const char ch )
{
    return ch == ' ' || ch == '\t';
}

/// 줄 끝의 CR 을 떼고 줄 단위로 나눈다.
///
/// 편집기에서 오는 생 UTF-8의 LF·CRLF·독립 CR을 논리적 줄 경계로 처리한다.
/// 이 저장소의 문서는 CRLF 와 LF 가 섞여
/// 있다. CR 을 남기면 "정확히 세 글자 --- 인가" 같은 판정이 전부 어긋난다.
[[nodiscard]] std::vector< std::string > splitLines( const std::string& text )
{
    std::vector< std::string > lines;
    std::string current;
    for( std::size_t i = 0; i < text.size(); ++i )
    {
        const char ch = text[ i ];
        if( ch == '\r' || ch == '\n' )
        {
            lines.push_back( current );
            current.clear();
            if( ch == '\r' && i + 1 < text.size() && text[ i + 1 ] == '\n' )
                ++i;
            continue;
        }
        current.push_back( ch );
    }
    if( !current.empty() || lines.empty() )
        lines.push_back( current );
    return lines;
}

[[nodiscard]] bool isBlank( const std::string& line )
{
    return std::all_of( line.begin(), line.end(), []( const char ch ) { return isSpace( ch ); } );
}

/// 선행 공백 폭. 탭은 4칸으로 센다 (CommonMark).
[[nodiscard]] int indentWidth( const std::string& line )
{
    int width = 0;
    for( const char ch : line )
    {
        if( ch == ' ' )
            width += 1;
        else if( ch == '\t' )
            width += kTabWidth;
        else
            break;
    }
    return width;
}

[[nodiscard]] std::string trimmed( const std::string& text )
{
    std::size_t begin = 0;
    while( begin < text.size() && isSpace( text[ begin ] ) )
        ++begin;
    std::size_t end = text.size();
    while( end > begin && isSpace( text[ end - 1 ] ) )
        --end;
    return text.substr( begin, end - begin );
}

/// 같은 문자가 앞에서 몇 개 연달아 오는가.
[[nodiscard]] std::size_t runLength( const std::string& text, const char ch )
{
    std::size_t count = 0;
    while( count < text.size() && text[ count ] == ch )
        ++count;
    return count;
}

/// 세 글자 이상이 모두 같은 문자인가. 수평선과 setext 밑줄 판정에 쓴다.
[[nodiscard]] bool isRunOf( const std::string& body, const char ch )
{
    return !body.empty() && body.front() == ch && runLength( body, ch ) == body.size();
}

/// 코드펜스 여는 줄인가. 그렇다면 문자와 개수를 낸다.
[[nodiscard]] bool isFenceOpen( const std::string& line, char& outChar, std::size_t& outLength )
{
    if( indentWidth( line ) > kMaxHeadingIndent )
        return false;
    const std::string body = trimmed( line );
    if( body.empty() )
        return false;
    const char ch = body.front();
    if( ch != '`' && ch != '~' )
        return false;
    const std::size_t length = runLength( body, ch );
    if( length < 3 )
        return false;
    // 백틱 펜스의 info string 에는 백틱이 들어갈 수 없다 (CommonMark).
    if( ch == '`' && body.find( '`', length ) != std::string::npos )
        return false;
    outChar = ch;
    outLength = length;
    return true;
}

/// 열려 있는 펜스를 닫는 줄인가. 같은 문자, 개수가 여는 것 이상, 뒤에 아무것도
/// 없어야 한다. 그래서 틸드로 열고 백틱으로 닫을 수 없고, 네 개로 열었으면
/// 세 개로 닫을 수 없다.
[[nodiscard]] bool isFenceClose( const std::string& line, const char fenceChar,
                                 const std::size_t openLength )
{
    if( indentWidth( line ) > kMaxHeadingIndent )
        return false;
    const std::string body = trimmed( line );
    if( body.empty() || body.front() != fenceChar )
        return false;
    const std::size_t length = runLength( body, fenceChar );
    return length >= openLength && trimmed( body.substr( length ) ).empty();
}

/// ATX 제목인가. 해시 1~6개 뒤에 공백이나 줄끝이 와야 한다 (CommonMark).
///
/// 공백 없는 "#foo" 를 제목으로 보지 않는 것이 LexMarkdown 과 갈라지는 지점이다.
/// 렉서는 그것을 제목으로 칠하지만 프리뷰(markdown-it/myst)와 GitHub 는 그렇지
/// 않다. 개요는 프리뷰와 일치해야 항목을 눌렀을 때 같은 곳으로 간다.
[[nodiscard]] bool parseAtxHeading( const std::string& line, int& outLevel, std::string& outText )
{
    if( indentWidth( line ) > kMaxHeadingIndent )
        return false;
    const std::string body = trimmed( line );
    const std::size_t hashes = runLength( body, '#' );
    if( hashes == 0 || hashes > 6 )
        return false;
    if( hashes < body.size() && !isSpace( body[ hashes ] ) )
        return false;

    std::string text = trimmed( body.substr( hashes ) );

    // 닫는 시퀀스를 벗긴다. 단 그 해시 앞이 공백일 때만이다 — "## foo#" 의
    // 해시는 제목 글자의 일부다.
    const std::size_t end = text.size();
    std::size_t trailing = 0;
    while( trailing < end && text[ end - 1 - trailing ] == '#' )
        ++trailing;
    if( trailing > 0 && ( trailing == end || isSpace( text[ end - 1 - trailing ] ) ) )
        text = trimmed( text.substr( 0, end - trailing ) );

    if( text.empty() )
        return false;   // 빈 제목은 개요에 올릴 것이 없다

    outLevel = static_cast< int >( hashes );
    outText = text;
    return true;
}

/// setext 밑줄인가. 등호는 1단계, 붙임표는 2단계다.
///
/// 별표와 밑줄로 된 줄은 절대 setext 가 아니다 — 수평선 전용이다.
[[nodiscard]] bool isSetextUnderline( const std::string& line, int& outLevel )
{
    if( indentWidth( line ) > kMaxHeadingIndent )
        return false;
    const std::string body = trimmed( line );
    if( isRunOf( body, '=' ) )
    {
        outLevel = 1;
        return true;
    }
    if( isRunOf( body, '-' ) )
    {
        outLevel = 2;
        return true;
    }
    return false;
}

/// setext 제목이 되려면 직전 줄이 "문단 줄" 이어야 한다.
///
/// 리스트·인용·펜스·수평선·ATX 제목은 문단이 아니다. 이 판정이 없으면 리스트
/// 바로 뒤의 붙임표 줄이 그 리스트 항목을 제목으로 만든다.
[[nodiscard]] bool isParagraphLine( const std::string& line )
{
    if( isBlank( line ) )
        return false;
    if( indentWidth( line ) > kMaxHeadingIndent )
        return false;

    const std::string body = trimmed( line );
    if( body.empty() )
        return false;
    if( body.front() == '>' || body.front() == '#' )
        return false;

    // 불릿 리스트: 마커 뒤에 공백.
    if( ( body.front() == '-' || body.front() == '*' || body.front() == '+' )
        && body.size() > 1 && isSpace( body[ 1 ] ) )
        return false;

    // 번호 리스트: 숫자 뒤에 점이나 닫는 괄호, 그리고 공백.
    std::size_t digits = 0;
    while( digits < body.size()
           && std::isdigit( static_cast< unsigned char >( body[ digits ] ) ) != 0 )
        ++digits;
    if( digits > 0 && digits + 1 < body.size()
        && ( body[ digits ] == '.' || body[ digits ] == ')' )
        && isSpace( body[ digits + 1 ] ) )
        return false;

    // 수평선과 setext 밑줄 자체.
    if( body.size() >= 3
        && ( isRunOf( body, '-' ) || isRunOf( body, '*' ) || isRunOf( body, '_' )
             || isRunOf( body, '=' ) ) )
        return false;

    char fenceChar = 0;
    std::size_t fenceLength = 0;
    if( isFenceOpen( line, fenceChar, fenceLength ) )
        return false;

    return true;
}

/// front matter 구간의 끝(닫는 구분자가 있는 줄 인덱스)을 찾는다.
///
/// **닫는 구분자를 먼저 확인한다.** 닫히지 않은 세 붙임표는 front matter 가
/// 아니라 수평선이므로, 확인 없이 소비하면 문서 전체가 사라진다.
[[nodiscard]] bool findFrontMatterEnd( const std::vector< std::string >& lines, std::size_t& outEnd )
{
    if( lines.empty() || trimmed( lines[ 0 ] ) != "---" )
        return false;
    for( std::size_t i = 1; i < lines.size(); ++i )
    {
        const std::string body = trimmed( lines[ i ] );
        if( body == "---" || body == "..." )
        {
            outEnd = i;
            return true;
        }
    }
    return false;
}

}   // namespace

MdScan scanMarkdown( const std::string& utf8Text )
{
    // BOM 을 벗긴다. 남기면 첫 줄의 해시가 0열이 아니게 되어 그 문서의 첫 제목만
    // 조용히 사라진다.
    std::string text = utf8Text;
    if( text.size() >= 3 && static_cast< unsigned char >( text[ 0 ] ) == 0xEF
        && static_cast< unsigned char >( text[ 1 ] ) == 0xBB
        && static_cast< unsigned char >( text[ 2 ] ) == 0xBF )
    {
        text.erase( 0, 3 );
    }

    const std::vector< std::string > lines = splitLines( text );
    const std::size_t count = lines.size();

    MdScan scan;
    scan.folds.assign( count, hc::rst::syntax::FoldLine{} );
    if( count == 0 )
        return scan;

    std::size_t frontMatterEnd = 0;
    const bool hasFrontMatter = findFrontMatterEnd( lines, frontMatterEnd );

    char fenceChar = 0;
    std::size_t fenceLength = 0;
    bool inFence = false;

    /// 지금 열려 있는 제목의 단계. 0 이면 아직 제목을 만나지 않았고, 그때의
    /// 본문 깊이도 0 이다.
    int headingLevel = 0;

    for( std::size_t i = 0; i < count; ++i )
    {
        const std::string& line = lines[ i ];

        if( hasFrontMatter && i <= frontMatterEnd )
        {
            // 렌더에서는 숨지만 줄 수는 그대로 센다.
            scan.folds[ i ].level = 0;
            scan.folds[ i ].blank = isBlank( line );
            continue;
        }

        if( isBlank( line ) )
        {
            scan.folds[ i ].blank = true;
            scan.folds[ i ].level = -1;   // 뒤에서 앞뒤 중 더 깊은 쪽으로 채운다
            continue;
        }

        if( inFence )
        {
            scan.folds[ i ].level = headingLevel;
            if( isFenceClose( line, fenceChar, fenceLength ) )
                inFence = false;
            continue;
        }

        char openChar = 0;
        std::size_t openLength = 0;
        if( isFenceOpen( line, openChar, openLength ) )
        {
            inFence = true;
            fenceChar = openChar;
            fenceLength = openLength;
            scan.folds[ i ].level = headingLevel;
            continue;
        }

        // 인용문 안의 제목은 문서 구조가 아니다 (Sphinx toctree 도 잡지 않는다).
        if( trimmed( line ).front() == '>' )
        {
            scan.folds[ i ].level = headingLevel;
            continue;
        }

        int atxLevel = 0;
        std::string atxText;
        if( parseAtxHeading( line, atxLevel, atxText ) )
        {
            headingLevel = atxLevel;
            scan.folds[ i ].level = atxLevel - 1;
            scan.headings.push_back( MdHeading{ atxText, atxLevel, i + 1, false } );
            continue;
        }

        int setextLevel = 0;
        if( isSetextUnderline( line, setextLevel ) && i > 0 && isParagraphLine( lines[ i - 1 ] ) )
        {
            headingLevel = setextLevel;
            // 직전 줄을 본문으로 이미 계산했다. 제목 줄로 고쳐 준다.
            scan.folds[ i - 1 ].level = setextLevel - 1;
            scan.folds[ i ].level = setextLevel - 1;
            // 줄 번호는 밑줄이 아니라 **제목 글자가 있는 줄**을 가리킨다.
            // reST 쪽(parseRstOutline)은 밑줄 줄을 가리키는데, 프리뷰가 제목
            // 글자 줄에 앵커를 붙이므로 여기서 갈라진다. 의도된 차이다.
            scan.headings.push_back( MdHeading{ trimmed( lines[ i - 1 ] ), setextLevel, i, true } );
            continue;
        }

        scan.folds[ i ].level = headingLevel;
    }

    // 빈 줄은 앞뒤 중 **더 깊은** 쪽에 붙인다. 근거는 reST 쪽과 같다 — 앞쪽만
    // 보면 제목 바로 아래 빈 줄이 제목과 같은 깊이가 되고, Scintilla 는 머리
    // 플래그만으로 마커를 그리지 않고 다음 줄이 실제로 더 깊은지까지 보므로
    // 제목에 접기 마커가 아예 나타나지 않는다. Markdown 문서는 제목과 본문
    // 사이에 거의 항상 빈 줄이 있으므로 이 경우가 곧 모든 섹션이다.
    std::vector< int > forward( count, 0 );
    int carried = 0;
    for( std::size_t i = 0; i < count; ++i )
    {
        if( scan.folds[ i ].level >= 0 )
            carried = scan.folds[ i ].level;
        forward[ i ] = carried;
    }

    int trailing = carried;
    for( std::size_t i = count; i-- > 0; )
    {
        if( scan.folds[ i ].level >= 0 )
        {
            trailing = scan.folds[ i ].level;
            continue;
        }
        scan.folds[ i ].level = std::max( forward[ i ], trailing );
    }

    // 다음 비어 있지 않은 줄이 더 깊으면 이 줄이 접기 머리다.
    int nextLevel = 0;
    bool haveNext = false;
    for( std::size_t i = count; i-- > 0; )
    {
        if( scan.folds[ i ].blank )
            continue;
        if( haveNext && nextLevel > scan.folds[ i ].level )
            scan.folds[ i ].header = true;
        nextLevel = scan.folds[ i ].level;
        haveNext = true;
    }

    return scan;
}

std::vector< hc::rst::syntax::FoldLine > computeMarkdownFoldLevels( const std::string& utf8Text )
{
    return scanMarkdown( utf8Text ).folds;
}

}   // namespace hc::rst::md
