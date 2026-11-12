// Qt 비의존 순수 모듈이다. stdafx.h 를 포함하지 않으므로 테스트 타깃에서
// Qt Widgets/WebEngine 없이 그대로 컴파일할 수 있다.
#include "text/RstStructure.h"

#include <algorithm>

namespace hc::rst::syntax {
namespace {

// ── 동아시아 폭 표 ───────────────────────────────────────
//
// UCD 15.0.0 의 EastAsianWidth.txt 에서 W 또는 F 인 구간을 뽑았다(182,516자).
// docutils 의 column_width() 가 이 분류로 두 칸을 센다.
//
// 다시 만들려면 unicode.org/Public/15.0.0/ucd/EastAsianWidth.txt 를 받아 두 번째
// 칸이 W·F 인 코드포인트를 모아 연속 구간으로 묶는다. 아래 kCombiningRanges 도
// 같은 판(UnicodeData.txt)에서 나왔으므로 판을 올릴 때는 둘을 함께 올린다.
struct CodepointRange
{
    std::uint32_t first;
    std::uint32_t last;
};

constexpr CodepointRange kWideRanges[] = {
    {0x01100,0x0115F}, {0x0231A,0x0231B}, {0x02329,0x0232A}, {0x023E9,0x023EC}, {0x023F0,0x023F0},
    {0x023F3,0x023F3}, {0x025FD,0x025FE}, {0x02614,0x02615}, {0x02648,0x02653}, {0x0267F,0x0267F},
    {0x02693,0x02693}, {0x026A1,0x026A1}, {0x026AA,0x026AB}, {0x026BD,0x026BE}, {0x026C4,0x026C5},
    {0x026CE,0x026CE}, {0x026D4,0x026D4}, {0x026EA,0x026EA}, {0x026F2,0x026F3}, {0x026F5,0x026F5},
    {0x026FA,0x026FA}, {0x026FD,0x026FD}, {0x02705,0x02705}, {0x0270A,0x0270B}, {0x02728,0x02728},
    {0x0274C,0x0274C}, {0x0274E,0x0274E}, {0x02753,0x02755}, {0x02757,0x02757}, {0x02795,0x02797},
    {0x027B0,0x027B0}, {0x027BF,0x027BF}, {0x02B1B,0x02B1C}, {0x02B50,0x02B50}, {0x02B55,0x02B55},
    {0x02E80,0x02E99}, {0x02E9B,0x02EF3}, {0x02F00,0x02FD5}, {0x02FF0,0x02FFB}, {0x03000,0x0303E},
    {0x03041,0x03096}, {0x03099,0x030FF}, {0x03105,0x0312F}, {0x03131,0x0318E}, {0x03190,0x031E3},
    {0x031F0,0x0321E}, {0x03220,0x03247}, {0x03250,0x04DBF}, {0x04E00,0x0A48C}, {0x0A490,0x0A4C6},
    {0x0A960,0x0A97C}, {0x0AC00,0x0D7A3}, {0x0F900,0x0FAFF}, {0x0FE10,0x0FE19}, {0x0FE30,0x0FE52},
    {0x0FE54,0x0FE66}, {0x0FE68,0x0FE6B}, {0x0FF01,0x0FF60}, {0x0FFE0,0x0FFE6}, {0x16FE0,0x16FE4},
    {0x16FF0,0x16FF1}, {0x17000,0x187F7}, {0x18800,0x18CD5}, {0x18D00,0x18D08}, {0x1AFF0,0x1AFF3},
    {0x1AFF5,0x1AFFB}, {0x1AFFD,0x1AFFE}, {0x1B000,0x1B122}, {0x1B132,0x1B132}, {0x1B150,0x1B152},
    {0x1B155,0x1B155}, {0x1B164,0x1B167}, {0x1B170,0x1B2FB}, {0x1F004,0x1F004}, {0x1F0CF,0x1F0CF},
    {0x1F18E,0x1F18E}, {0x1F191,0x1F19A}, {0x1F200,0x1F202}, {0x1F210,0x1F23B}, {0x1F240,0x1F248},
    {0x1F250,0x1F251}, {0x1F260,0x1F265}, {0x1F300,0x1F320}, {0x1F32D,0x1F335}, {0x1F337,0x1F37C},
    {0x1F37E,0x1F393}, {0x1F3A0,0x1F3CA}, {0x1F3CF,0x1F3D3}, {0x1F3E0,0x1F3F0}, {0x1F3F4,0x1F3F4},
    {0x1F3F8,0x1F43E}, {0x1F440,0x1F440}, {0x1F442,0x1F4FC}, {0x1F4FF,0x1F53D}, {0x1F54B,0x1F54E},
    {0x1F550,0x1F567}, {0x1F57A,0x1F57A}, {0x1F595,0x1F596}, {0x1F5A4,0x1F5A4}, {0x1F5FB,0x1F64F},
    {0x1F680,0x1F6C5}, {0x1F6CC,0x1F6CC}, {0x1F6D0,0x1F6D2}, {0x1F6D5,0x1F6D7}, {0x1F6DC,0x1F6DF},
    {0x1F6EB,0x1F6EC}, {0x1F6F4,0x1F6FC}, {0x1F7E0,0x1F7EB}, {0x1F7F0,0x1F7F0}, {0x1F90C,0x1F93A},
    {0x1F93C,0x1F945}, {0x1F947,0x1F9FF}, {0x1FA70,0x1FA7C}, {0x1FA80,0x1FA88}, {0x1FA90,0x1FABD},
    {0x1FABF,0x1FAC5}, {0x1FACE,0x1FADB}, {0x1FAE0,0x1FAE8}, {0x1FAF0,0x1FAF8}, {0x20000,0x2FFFD},
    {0x30000,0x3FFFD},
};

/// 결합 문자. docutils 는 폭 계산에서 이만큼을 뺀다.
///
/// 기준은 정규 결합 클래스(UnicodeData.txt 세 번째 칸)가 0 이 아닌 것이다 — docutils 의
/// find_combining_chars() 가 unicodedata.combining() 으로 가르는 것과 같다. 범주 Mn/Me 와는
/// 다르다: 에워싸는 표시(U+0488)처럼 클래스가 0 인 표시 문자는 docutils 도 빼지 않으므로
/// 여기에도 없다.
///
/// UCD 15.0.0 전체다(922자 / 192구간). kWideRanges 와 같은 판을 쓴다.
constexpr CodepointRange kCombiningRanges[] = {
    {0x00300,0x0034E}, {0x00350,0x0036F}, {0x00483,0x00487}, {0x00591,0x005BD}, {0x005BF,0x005BF},
    {0x005C1,0x005C2}, {0x005C4,0x005C5}, {0x005C7,0x005C7}, {0x00610,0x0061A}, {0x0064B,0x0065F},
    {0x00670,0x00670}, {0x006D6,0x006DC}, {0x006DF,0x006E4}, {0x006E7,0x006E8}, {0x006EA,0x006ED},
    {0x00711,0x00711}, {0x00730,0x0074A}, {0x007EB,0x007F3}, {0x007FD,0x007FD}, {0x00816,0x00819},
    {0x0081B,0x00823}, {0x00825,0x00827}, {0x00829,0x0082D}, {0x00859,0x0085B}, {0x00898,0x0089F},
    {0x008CA,0x008E1}, {0x008E3,0x008FF}, {0x0093C,0x0093C}, {0x0094D,0x0094D}, {0x00951,0x00954},
    {0x009BC,0x009BC}, {0x009CD,0x009CD}, {0x009FE,0x009FE}, {0x00A3C,0x00A3C}, {0x00A4D,0x00A4D},
    {0x00ABC,0x00ABC}, {0x00ACD,0x00ACD}, {0x00B3C,0x00B3C}, {0x00B4D,0x00B4D}, {0x00BCD,0x00BCD},
    {0x00C3C,0x00C3C}, {0x00C4D,0x00C4D}, {0x00C55,0x00C56}, {0x00CBC,0x00CBC}, {0x00CCD,0x00CCD},
    {0x00D3B,0x00D3C}, {0x00D4D,0x00D4D}, {0x00DCA,0x00DCA}, {0x00E38,0x00E3A}, {0x00E48,0x00E4B},
    {0x00EB8,0x00EBA}, {0x00EC8,0x00ECB}, {0x00F18,0x00F19}, {0x00F35,0x00F35}, {0x00F37,0x00F37},
    {0x00F39,0x00F39}, {0x00F71,0x00F72}, {0x00F74,0x00F74}, {0x00F7A,0x00F7D}, {0x00F80,0x00F80},
    {0x00F82,0x00F84}, {0x00F86,0x00F87}, {0x00FC6,0x00FC6}, {0x01037,0x01037}, {0x01039,0x0103A},
    {0x0108D,0x0108D}, {0x0135D,0x0135F}, {0x01714,0x01715}, {0x01734,0x01734}, {0x017D2,0x017D2},
    {0x017DD,0x017DD}, {0x018A9,0x018A9}, {0x01939,0x0193B}, {0x01A17,0x01A18}, {0x01A60,0x01A60},
    {0x01A75,0x01A7C}, {0x01A7F,0x01A7F}, {0x01AB0,0x01ABD}, {0x01ABF,0x01ACE}, {0x01B34,0x01B34},
    {0x01B44,0x01B44}, {0x01B6B,0x01B73}, {0x01BAA,0x01BAB}, {0x01BE6,0x01BE6}, {0x01BF2,0x01BF3},
    {0x01C37,0x01C37}, {0x01CD0,0x01CD2}, {0x01CD4,0x01CE0}, {0x01CE2,0x01CE8}, {0x01CED,0x01CED},
    {0x01CF4,0x01CF4}, {0x01CF8,0x01CF9}, {0x01DC0,0x01DFF}, {0x020D0,0x020DC}, {0x020E1,0x020E1},
    {0x020E5,0x020F0}, {0x02CEF,0x02CF1}, {0x02D7F,0x02D7F}, {0x02DE0,0x02DFF}, {0x0302A,0x0302F},
    {0x03099,0x0309A}, {0x0A66F,0x0A66F}, {0x0A674,0x0A67D}, {0x0A69E,0x0A69F}, {0x0A6F0,0x0A6F1},
    {0x0A806,0x0A806}, {0x0A82C,0x0A82C}, {0x0A8C4,0x0A8C4}, {0x0A8E0,0x0A8F1}, {0x0A92B,0x0A92D},
    {0x0A953,0x0A953}, {0x0A9B3,0x0A9B3}, {0x0A9C0,0x0A9C0}, {0x0AAB0,0x0AAB0}, {0x0AAB2,0x0AAB4},
    {0x0AAB7,0x0AAB8}, {0x0AABE,0x0AABF}, {0x0AAC1,0x0AAC1}, {0x0AAF6,0x0AAF6}, {0x0ABED,0x0ABED},
    {0x0FB1E,0x0FB1E}, {0x0FE20,0x0FE2F}, {0x101FD,0x101FD}, {0x102E0,0x102E0}, {0x10376,0x1037A},
    {0x10A0D,0x10A0D}, {0x10A0F,0x10A0F}, {0x10A38,0x10A3A}, {0x10A3F,0x10A3F}, {0x10AE5,0x10AE6},
    {0x10D24,0x10D27}, {0x10EAB,0x10EAC}, {0x10EFD,0x10EFF}, {0x10F46,0x10F50}, {0x10F82,0x10F85},
    {0x11046,0x11046}, {0x11070,0x11070}, {0x1107F,0x1107F}, {0x110B9,0x110BA}, {0x11100,0x11102},
    {0x11133,0x11134}, {0x11173,0x11173}, {0x111C0,0x111C0}, {0x111CA,0x111CA}, {0x11235,0x11236},
    {0x112E9,0x112EA}, {0x1133B,0x1133C}, {0x1134D,0x1134D}, {0x11366,0x1136C}, {0x11370,0x11374},
    {0x11442,0x11442}, {0x11446,0x11446}, {0x1145E,0x1145E}, {0x114C2,0x114C3}, {0x115BF,0x115C0},
    {0x1163F,0x1163F}, {0x116B6,0x116B7}, {0x1172B,0x1172B}, {0x11839,0x1183A}, {0x1193D,0x1193E},
    {0x11943,0x11943}, {0x119E0,0x119E0}, {0x11A34,0x11A34}, {0x11A47,0x11A47}, {0x11A99,0x11A99},
    {0x11C3F,0x11C3F}, {0x11D42,0x11D42}, {0x11D44,0x11D45}, {0x11D97,0x11D97}, {0x11F41,0x11F42},
    {0x16AF0,0x16AF4}, {0x16B30,0x16B36}, {0x16FF0,0x16FF1}, {0x1BC9E,0x1BC9E}, {0x1D165,0x1D169},
    {0x1D16D,0x1D172}, {0x1D17B,0x1D182}, {0x1D185,0x1D18B}, {0x1D1AA,0x1D1AD}, {0x1D242,0x1D244},
    {0x1E000,0x1E006}, {0x1E008,0x1E018}, {0x1E01B,0x1E021}, {0x1E023,0x1E024}, {0x1E026,0x1E02A},
    {0x1E08F,0x1E08F}, {0x1E130,0x1E136}, {0x1E2AE,0x1E2AE}, {0x1E2EC,0x1E2EF}, {0x1E4EC,0x1E4EF},
    {0x1E8D0,0x1E8D6}, {0x1E944,0x1E94A},
};

[[nodiscard]] bool inRanges( const CodepointRange* ranges, const std::size_t count,
                             const std::uint32_t codepoint ) noexcept
{
    std::size_t low = 0;
    std::size_t high = count;
    while( low < high )
    {
        const std::size_t mid = low + ( high - low ) / 2;
        if( codepoint < ranges[ mid ].first )
            high = mid;
        else if( codepoint > ranges[ mid ].last )
            low = mid + 1;
        else
            return true;
    }
    return false;
}

/// pos 에서 UTF-8 코드포인트 하나를 읽고 pos 를 다음으로 옮긴다.
/// 깨진 바이트는 그 바이트 하나를 소비하고 값으로 돌려준다 — 진행은 보장한다.
[[nodiscard]] std::uint32_t decodeUtf8( std::string_view text, std::size_t& pos ) noexcept
{
    const auto byteAt = [ & ]( const std::size_t index ) {
        return static_cast< std::uint32_t >( static_cast< unsigned char >( text[ index ] ) );
    };

    const std::uint32_t lead = byteAt( pos );
    std::size_t         extra = 0;
    std::uint32_t       value = lead;

    if( lead < 0x80 )
        extra = 0;
    else if( ( lead & 0xE0 ) == 0xC0 )
    {
        extra = 1;
        value = lead & 0x1F;
    }
    else if( ( lead & 0xF0 ) == 0xE0 )
    {
        extra = 2;
        value = lead & 0x0F;
    }
    else if( ( lead & 0xF8 ) == 0xF0 )
    {
        extra = 3;
        value = lead & 0x07;
    }
    else
    {
        ++pos;
        return lead;
    }

    if( pos + extra >= text.size() )
    {
        // 잘린 시퀀스. 한 바이트만 소비한다 — 진행은 보장해야 한다.
        ++pos;
        return lead;
    }

    for( std::size_t i = 1; i <= extra; ++i )
    {
        const std::uint32_t continuation = byteAt( pos + i );
        if( ( continuation & 0xC0 ) != 0x80 )
        {
            ++pos;
            return lead;
        }
        value = ( value << 6 ) | ( continuation & 0x3F );
    }
    pos += extra + 1;
    return value;
}

// ── 인라인 마크업 경계 ───────────────────────────────────
//
// docutils 의 인식 규칙(restructuredtext.html#inline-markup) 중 두 항이다.
// 이것이 없으면 `snake_case` 의 `snake_` 가 하이퍼링크로, `char *argv[]` 의 별표
// 구간이 강조로 칠해진다.
//
// 비ASCII 바이트는 경계로 **허용**한다. docutils 는 유니코드 범주로 판정하지만
// 그 표를 들이는 것은 이 모듈의 전제(외부 의존 없음)와 충돌하고, 허용 쪽이
// 지금까지의 동작이라 한국어 문서에서 회귀가 생기지 않는다.

[[nodiscard]] bool isOpenerBoundary( const char ch ) noexcept
{
    switch( ch )
    {
        case ' ': case '\t': case '\f': case '\v':
        case '-': case ':': case '/': case '\'': case '"':
        case '<': case '(': case '[': case '{':
            return true;
        default:
            return static_cast< unsigned char >( ch ) >= 0x80;
    }
}

[[nodiscard]] bool isCloserBoundary( const char ch ) noexcept
{
    switch( ch )
    {
        case ' ': case '\t': case '\f': case '\v':
        case '-': case '.': case ',': case ':': case ';':
        case '!': case '?': case '\\': case '/': case '\'': case '"':
        case ')': case ']': case '}': case '>':
            return true;
        default:
            return static_cast< unsigned char >( ch ) >= 0x80;
    }
}

/// 여는 구분자가 start 에 놓일 수 있는가.
[[nodiscard]] bool canOpenAt( std::string_view line, const std::size_t start ) noexcept
{
    return start == 0 || isOpenerBoundary( line[ start - 1 ] );
}

/// 닫는 구분자가 [closeStart, closeEnd) 에 놓일 수 있는가.
[[nodiscard]] bool canCloseAt( std::string_view line, const std::size_t closeStart,
                               const std::size_t closeEnd ) noexcept
{
    if( closeStart == 0 || isInlineSpace( line[ closeStart - 1 ] ) )
        return false;   // 닫는 문자열 앞은 공백일 수 없다
    return closeEnd >= line.size() || isCloserBoundary( line[ closeEnd ] );
}

[[nodiscard]] bool isAsciiAlnum( const char ch ) noexcept
{
    return ( ch >= '0' && ch <= '9' ) || ( ch >= 'a' && ch <= 'z' ) || ( ch >= 'A' && ch <= 'Z' );
}

}   // namespace

// ── 문자 분류 ────────────────────────────────────────────

bool isNameChar( const char ch ) noexcept
{
    return isAsciiAlnum( ch ) || ch == '_' || ch == '.' || ch == '-' || ch == '+';
}

bool isAdornmentChar( const char ch ) noexcept
{
    // docutils 의 [!-/:-@[-`{-~] — ASCII 문장부호 32자.
    const unsigned char value = static_cast< unsigned char >( ch );
    return ( value >= '!' && value <= '/' ) || ( value >= ':' && value <= '@' )
        || ( value >= '[' && value <= '`' ) || ( value >= '{' && value <= '~' );
}

bool isInlineSpace( const char ch ) noexcept
{
    return ch == ' ' || ch == '\t' || ch == '\v' || ch == '\f' || ch == '\r';
}

std::size_t scanQualifiedName( std::string_view line, std::size_t pos ) noexcept
{
    const std::size_t begin = pos;
    while( pos < line.size() && isNameChar( line[ pos ] ) )
        ++pos;
    if( pos == begin )
        return begin;

    // 선택적 두 번째 마디. ':' 다음이 이름 문자일 때만 취한다 — 그래야 `a::b` 에서
    // 첫 콜론을 마디 구분자로 오해하지 않는다.
    if( pos + 1 < line.size() && line[ pos ] == ':' && isNameChar( line[ pos + 1 ] ) )
    {
        std::size_t second = pos + 1;
        while( second < line.size() && isNameChar( line[ second ] ) )
            ++second;
        return second;
    }
    return pos;
}

// ── 줄 단위 원시 함수 ────────────────────────────────────

int indentWidth( std::string_view line ) noexcept
{
    int width = 0;
    for( const char ch : line )
    {
        if( ch == ' ' )
            ++width;
        else if( ch == '\t' )
            width += 8 - ( width % 8 );
        else
            break;
    }
    return width;
}

bool isBlank( std::string_view line ) noexcept
{
    return std::all_of( line.begin(), line.end(), isInlineSpace );
}

std::string_view trimView( std::string_view line ) noexcept
{
    std::size_t begin = 0;
    std::size_t end = line.size();
    while( begin < end && isInlineSpace( line[ begin ] ) )
        ++begin;
    while( end > begin && isInlineSpace( line[ end - 1 ] ) )
        --end;
    return line.substr( begin, end - begin );
}

std::size_t utf8Length( std::string_view value ) noexcept
{
    std::size_t count = 0;
    for( const char raw : value )
    {
        if( ( static_cast< unsigned char >( raw ) & 0xC0 ) != 0x80 )
            ++count;
    }
    return count;
}

std::size_t columnWidth( std::string_view value ) noexcept
{
    constexpr std::size_t kWideCount = sizeof( kWideRanges ) / sizeof( kWideRanges[ 0 ] );
    constexpr std::size_t kCombiningCount = sizeof( kCombiningRanges ) / sizeof( kCombiningRanges[ 0 ] );

    std::size_t width = 0;
    std::size_t pos = 0;
    while( pos < value.size() )
    {
        const std::uint32_t codepoint = decodeUtf8( value, pos );
        if( codepoint < 0x0300 )
        {
            ++width;   // 흔한 경우를 표 조회 없이 지나간다
            continue;
        }
        if( inRanges( kCombiningRanges, kCombiningCount, codepoint ) )
            continue;
        width += inRanges( kWideRanges, kWideCount, codepoint ) ? 2u : 1u;
    }
    return width;
}

// ── 줄 경계 색인 ─────────────────────────────────────────

LineIndex::LineIndex( std::string_view text )
{
    reset( text );
}

void LineIndex::reset( std::string_view text )
{
    text_ = text;
    starts_.clear();
    starts_.reserve( text.size() / 40 + 4 );
    starts_.push_back( 0 );
    for( std::size_t i = 0; i < text.size(); ++i )
    {
        if( text[ i ] == '\r' || text[ i ] == '\n' )
        {
            if( text[ i ] == '\r' && i + 1 < text.size() && text[ i + 1 ] == '\n' )
                ++i;
            starts_.push_back( static_cast< std::uint32_t >( i + 1 ) );
        }
    }
    // 본문이 개행으로 끝나면 마지막 원소가 빈 줄을 가리킨다. 그것도 한 줄로 센다
    // — Scintilla 의 줄 수와 맞아야 접기 깊이 주입이 어긋나지 않는다.
}

std::string_view LineIndex::line( const std::size_t index ) const noexcept
{
    if( index >= starts_.size() )
        return {};
    const std::size_t begin = starts_[ index ];
    const std::size_t end = ( index + 1 < starts_.size() ) ? starts_[ index + 1 ] : text_.size();
    std::string_view raw = text_.substr( begin, end - begin );
    if( !raw.empty() && raw.back() == '\n' )
        raw.remove_suffix( 1 );
    if( !raw.empty() && raw.back() == '\r' )
        raw.remove_suffix( 1 );
    return raw;
}

std::size_t LineIndex::byteStart( const std::size_t index ) const noexcept
{
    return index < starts_.size() ? starts_[ index ] : text_.size();
}

std::size_t LineIndex::byteLength( const std::size_t index ) const noexcept
{
    if( index >= starts_.size() )
        return 0;
    const std::size_t begin = starts_[ index ];
    const std::size_t end = ( index + 1 < starts_.size() ) ? starts_[ index + 1 ] : text_.size();
    return end - begin;
}

std::size_t LineIndex::lineAt( const std::size_t byteOffset ) const noexcept
{
    if( starts_.empty() )
        return 0;
    const auto it = std::upper_bound( starts_.begin(), starts_.end(),
                                      static_cast< std::uint32_t >( byteOffset ) );
    return static_cast< std::size_t >( it - starts_.begin() ) - 1;
}

// ── 구성 요소 인식 ───────────────────────────────────────

std::optional< DirectiveParts > parseDirective( std::string_view line ) noexcept
{
    const std::size_t size = line.size();
    std::size_t       pos = 0;
    while( pos < size && isInlineSpace( line[ pos ] ) )
        ++pos;
    if( pos + 1 >= size || line[ pos ] != '.' || line[ pos + 1 ] != '.' )
        return std::nullopt;

    pos += 2;
    const std::size_t spaceStart = pos;
    while( pos < size && isInlineSpace( line[ pos ] ) )
        ++pos;
    if( pos == spaceStart )
        return std::nullopt;   // ".." 뒤에 공백이 하나는 있어야 한다

    DirectiveParts parts;
    parts.prefixEnd = static_cast< std::uint32_t >( pos );
    parts.subStart = parts.prefixEnd;
    parts.subEnd = parts.prefixEnd;

    // 선택적 치환 이름. `.. |logo| image:: logo.png` 형태다.
    // 예전 렉서는 이것을 몰라서 치환 정의 줄 전체를 주석 색으로 칠했다.
    if( pos < size && line[ pos ] == '|' )
    {
        const std::size_t barEnd = line.find( '|', pos + 1 );
        if( barEnd != std::string_view::npos && barEnd > pos + 1 )
        {
            std::size_t afterBar = barEnd + 1;
            const std::size_t gapStart = afterBar;
            while( afterBar < size && isInlineSpace( line[ afterBar ] ) )
                ++afterBar;
            if( afterBar > gapStart )
            {
                parts.subStart = static_cast< std::uint32_t >( pos );
                parts.subEnd = static_cast< std::uint32_t >( barEnd + 1 );
                pos = afterBar;
            }
        }
    }

    const std::size_t nameStart = pos;
    const std::size_t nameEnd = scanQualifiedName( line, pos );
    if( nameEnd == nameStart )
        return std::nullopt;
    if( nameEnd + 1 >= size || line[ nameEnd ] != ':' || line[ nameEnd + 1 ] != ':' )
        return std::nullopt;

    parts.nameStart = static_cast< std::uint32_t >( nameStart );
    parts.nameEnd = static_cast< std::uint32_t >( nameEnd );
    parts.colonsEnd = static_cast< std::uint32_t >( nameEnd + 2 );
    return parts;
}

std::optional< FieldParts > parseField( std::string_view line ) noexcept
{
    const std::size_t size = line.size();
    std::size_t       pos = 0;
    while( pos < size && isInlineSpace( line[ pos ] ) )
        ++pos;
    if( pos >= size || line[ pos ] != ':' )
        return std::nullopt;

    const std::size_t indentEnd = pos;
    const std::size_t nameStart = pos + 1;
    if( nameStart >= size || line[ nameStart ] == ':' || isInlineSpace( line[ nameStart ] ) )
        return std::nullopt;   // 여는 ':' 다음이 ':' 나 공백이면 필드가 아니다

    std::size_t nameEnd = nameStart;
    while( nameEnd < size && line[ nameEnd ] != ':' )
    {
        if( line[ nameEnd ] == '\\' && nameEnd + 1 < size )
            ++nameEnd;   // 이스케이프된 문자는 통째로 넘긴다
        ++nameEnd;
    }
    if( nameEnd >= size || nameEnd == nameStart )
        return std::nullopt;
    if( isInlineSpace( line[ nameEnd - 1 ] ) )
        return std::nullopt;   // 필드 이름은 공백으로 끝날 수 없다

    // docutils 는 닫는 콜론 뒤에 공백 하나 이상 또는 줄끝을 요구한다.
    // 이 조건이 줄머리의 `:ref:`x`` 를 필드로 오인하는 것을 막는다.
    const std::size_t markerEnd = nameEnd + 1;
    if( markerEnd < size && !isInlineSpace( line[ markerEnd ] ) )
        return std::nullopt;

    FieldParts parts;
    parts.indentEnd = static_cast< std::uint32_t >( indentEnd );
    parts.nameStart = static_cast< std::uint32_t >( nameStart );
    parts.nameEnd = static_cast< std::uint32_t >( nameEnd );
    parts.markerEnd = static_cast< std::uint32_t >( markerEnd );
    return parts;
}

bool hasExplicitMarkupPrefix( std::string_view line ) noexcept
{
    const std::size_t size = line.size();
    std::size_t       pos = 0;
    while( pos < size && isInlineSpace( line[ pos ] ) )
        ++pos;
    if( pos + 1 >= size || line[ pos ] != '.' || line[ pos + 1 ] != '.' )
        return false;
    pos += 2;
    const std::size_t spaceStart = pos;
    while( pos < size && isInlineSpace( line[ pos ] ) )
        ++pos;
    return pos > spaceStart;
}

char adornmentRun( std::string_view line, std::size_t* outLength ) noexcept
{
    if( outLength != nullptr )
        *outLength = 0;
    if( line.empty() || !isAdornmentChar( line[ 0 ] ) )
        return '\0';   // 들여쓴 줄은 장식으로 보지 않는다

    const char  marker = line[ 0 ];
    std::size_t run = 0;
    while( run < line.size() && line[ run ] == marker )
        ++run;
    for( std::size_t i = run; i < line.size(); ++i )
    {
        if( !isInlineSpace( line[ i ] ) )
            return '\0';
    }
    if( outLength != nullptr )
        *outLength = run;
    return marker;
}

bool isTransitionLine( std::string_view line ) noexcept
{
    std::size_t length = 0;
    return adornmentRun( line, &length ) != '\0' && length >= 4;
}

// ── 리터럴 블록 ──────────────────────────────────────────

namespace {

/// 본문을 그대로 싣는 directive 이름. 오름차순 — 이분 탐색을 쓴다.
///
/// 고르는 기준은 「본문이 reST 인가」다. `parsed-literal` 은 모양만 리터럴이고
/// 인라인 마크업을 그대로 해석하므로 여기 없다.
constexpr std::string_view kLiteralBodyDirectives[] = {
    "code", "code-block", "csv-table", "doctest", "graphviz", "math", "mermaid", "plantuml",
    "raw", "sourcecode", "testcleanup", "testcode", "testoutput", "testsetup", "uml",
};

[[nodiscard]] char asciiLower( const char ch ) noexcept
{
    return ( ch >= 'A' && ch <= 'Z' ) ? static_cast< char >( ch - 'A' + 'a' ) : ch;
}

/// ASCII 대소문자만 무시하는 비교. directive 이름은 ASCII 다.
[[nodiscard]] int compareFolded( std::string_view left, std::string_view right ) noexcept
{
    const std::size_t shared = std::min( left.size(), right.size() );
    for( std::size_t i = 0; i < shared; ++i )
    {
        const char a = asciiLower( left[ i ] );
        const char b = asciiLower( right[ i ] );
        if( a != b )
            return ( a < b ) ? -1 : 1;
    }
    if( left.size() == right.size() )
        return 0;
    return ( left.size() < right.size() ) ? -1 : 1;
}

}   // namespace

bool hasLiteralBody( std::string_view directiveName ) noexcept
{
    constexpr std::size_t kCount =
        sizeof( kLiteralBodyDirectives ) / sizeof( kLiteralBodyDirectives[ 0 ] );

    std::size_t low = 0;
    std::size_t high = kCount;
    while( low < high )
    {
        const std::size_t mid = low + ( high - low ) / 2;
        const int         order = compareFolded( directiveName, kLiteralBodyDirectives[ mid ] );
        if( order == 0 )
            return true;
        if( order < 0 )
            high = mid;
        else
            low = mid + 1;
    }
    return false;
}

bool LiteralBlockTracker::advance( std::string_view line, const bool allowOpen ) noexcept
{
    // 블록이 끝나는 줄은 두 번 본다 — 한 번은 닫으려고, 한 번은 그 줄 자체를
    // 블록 밖 규칙으로 읽으려고. 그 이상은 돌 수 없다.
    for( int pass = 0; pass < 2; ++pass )
    {
        switch( phase_ )
        {
            case Phase::Outside:
            {
                if( !allowOpen )
                    return false;

                if( const std::optional< DirectiveParts > parts = parseDirective( line ) )
                {
                    const std::string_view name =
                        line.substr( parts->nameStart, parts->nameEnd - parts->nameStart );
                    if( hasLiteralBody( name ) )
                    {
                        phase_ = Phase::Options;
                        introIndent_ = indentWidth( line );
                        sawBlankInOptions_ = false;
                    }
                    return false;
                }

                // `::` 로 끝난 문단은 뒤따르는 들여쓴 블록을 리터럴로 만든다.
                // directive 를 먼저 걸러 냈으므로 `.. note::` 는 여기 오지 않는다.
                const std::string_view trimmed = trimView( line );
                if( trimmed.size() >= 2 && trimmed.substr( trimmed.size() - 2 ) == "::"
                    && !hasExplicitMarkupPrefix( line ) )
                {
                    phase_ = Phase::Pending;
                    introIndent_ = indentWidth( line );
                    sawBlankInOptions_ = false;
                }
                return false;
            }

            case Phase::Pending:
            case Phase::Options:
            {
                if( isBlank( line ) )
                {
                    sawBlankInOptions_ = true;
                    return false;
                }
                if( indentWidth( line ) <= introIndent_ )
                {
                    // 본문이 오지 않았다. 이 줄은 블록 밖이다.
                    phase_ = Phase::Outside;
                    continue;
                }
                if( phase_ == Phase::Options && !sawBlankInOptions_ && parseField( line ) )
                    return false;   // directive 옵션. 필드 이름으로 칠해야 한다
                phase_ = Phase::Inside;
                return true;
            }

            case Phase::Inside:
            {
                if( isBlank( line ) )
                    return false;   // 블록 안의 빈 줄. 칠할 것이 없다
                if( indentWidth( line ) > introIndent_ )
                    return true;
                phase_ = Phase::Outside;
                continue;
            }
        }
    }
    return false;
}

bool LiteralBlockTracker::feed( std::string_view line ) noexcept
{
    return advance( line, true );
}

bool LiteralBlockTracker::peek( std::string_view line ) const noexcept
{
    LiteralBlockTracker copy = *this;
    return copy.advance( line, true );
}

void LiteralBlockTracker::consumeAsTitle( std::string_view line ) noexcept
{
    advance( line, false );
}

// ── 섹션 제목 ────────────────────────────────────────────

namespace {

/// docutils 의 길이 조건. 장식이 제목보다 짧아도 4자 이상이면 경고만 하고 인정한다.
[[nodiscard]] bool adornmentLongEnough( std::string_view title, const std::size_t adornLength ) noexcept
{
    return columnWidth( title ) <= adornLength || adornLength >= 4;
}

}   // namespace

TitleScan titleScanAt( const LineIndex& lines, const std::size_t index )
{
    const std::string_view first = lines.line( index );
    std::size_t            firstRun = 0;
    const char             firstChar = adornmentRun( first, &firstRun );

    if( firstChar != '\0' )
    {
        // 윗줄 시도. 다음 줄이 없거나 비어 있으면 홀로 선 장식이므로 여기서 손을 뗀다
        // — 구분선인지 아닌지는 호출자가 isTransitionLine() 으로 판단한다.
        if( index + 1 >= lines.size() )
            return {};
        const std::string_view titleLine = lines.line( index + 1 );
        if( isBlank( titleLine ) )
            return {};

        // 장식이 두 줄 연속이면 docutils 는 "Invalid section title or transition
        // marker" 로 두 줄을 소비한다.
        if( adornmentRun( titleLine ) != '\0' )
            return { 2, std::nullopt };

        std::size_t underRun = 0;
        const char  underChar =
            ( index + 2 < lines.size() ) ? adornmentRun( lines.line( index + 2 ), &underRun ) : '\0';

        const std::string_view title = trimView( titleLine );
        const bool             matched = ( underChar == firstChar && underRun == firstRun
                               && adornmentLongEnough( title, firstRun ) );
        if( matched )
        {
            TitleRun run;
            run.firstLine = static_cast< std::uint32_t >( index );
            run.textLine = static_cast< std::uint32_t >( index + 1 );
            run.lastLine = static_cast< std::uint32_t >( index + 2 );
            run.adornChar = firstChar;
            run.overlined = true;
            run.text = title;
            return { 3, run };
        }

        // 어긋났다. 윗줄이 4자 이상이면 docutils 는 오류를 내고 세 줄을 통째로
        // 소비하므로 가운데 줄이 밑줄형 제목으로 다시 잡히지 않는다. 4자 미만이면
        // 되돌려서 평범한 본문으로 다시 읽으므로 우리도 소비하지 않는다.
        if( firstRun >= 4 )
            return { ( index + 2 < lines.size() ) ? 3u : 2u, std::nullopt };
        return {};
    }

    // 아랫줄만 있는 형식.
    const std::string_view title = trimView( first );
    if( title.empty() || index + 1 >= lines.size() )
        return {};

    std::size_t underRun = 0;
    const char  underChar = adornmentRun( lines.line( index + 1 ), &underRun );
    if( underChar == '\0' || !adornmentLongEnough( title, underRun ) )
        return {};

    TitleRun run;
    run.firstLine = static_cast< std::uint32_t >( index );
    run.textLine = static_cast< std::uint32_t >( index );
    run.lastLine = static_cast< std::uint32_t >( index + 1 );
    run.adornChar = underChar;
    run.overlined = false;
    run.text = title;
    return { 2, run };
}

std::optional< TitleRun > titleAt( const LineIndex& lines, const std::size_t index )
{
    return titleScanAt( lines, index ).title;
}

// ── 인라인 마크업 ────────────────────────────────────────

void scanInline( std::string_view line, std::vector< InlineToken >& out )
{
    const std::size_t size = line.size();

    // 종류별 '소진' 표시. 닫는 구분자를 줄 끝까지 못 찾았으면 그보다 뒤에서도 못 찾는다
    // (닫힘 후보 집합이 접미사 집합이므로). 이 다섯 개가 최악 O(n²) 를 O(n) 으로 만든다.
    bool exhaustedLiteral = false;
    bool exhaustedStrong = false;
    bool exhaustedEmphasis = false;
    bool exhaustedPipe = false;
    bool exhaustedBacktick = false;

    // 이름을 emit 로 두면 안 된다 — 앱 타깃은 stdafx.h 를 PCH 로 강제 포함하고
    // 거기서 Qt 의 emit 매크로가 들어온다.
    const auto addToken = [ &out ]( const std::size_t start, const std::size_t end,
                                const InlineKind kind, const std::size_t nameStart = 0,
                                const std::size_t nameEnd = 0 ) {
        InlineToken token;
        token.start = static_cast< std::uint32_t >( start );
        token.end = static_cast< std::uint32_t >( end );
        token.kind = kind;
        token.nameStart = static_cast< std::uint32_t >( nameStart );
        token.nameEnd = static_cast< std::uint32_t >( nameEnd );
        out.push_back( token );
    };

    /// open 다음부터 needle 을 찾되 내용이 비어 있지 않고 닫는 경계를 만족해야 한다.
    const auto findClosing = [ & ]( const std::size_t contentStart, const char marker,
                                    const std::size_t markerLength ) -> std::size_t {
        std::size_t probe = contentStart;
        while( probe + markerLength <= size )
        {
            if( line[ probe ] == marker
                && ( markerLength == 1 || line[ probe + 1 ] == marker ) )
            {
                if( canCloseAt( line, probe, probe + markerLength ) )
                    return probe;
            }
            ++probe;
        }
        return std::string_view::npos;
    };

    std::size_t pos = 0;
    while( pos < size )
    {
        const char ch = line[ pos ];

        if( ch == '`' )
        {
            const bool doubled = ( pos + 1 < size && line[ pos + 1 ] == '`' );
            if( doubled && !exhaustedLiteral && canOpenAt( line, pos )
                && pos + 2 < size && !isInlineSpace( line[ pos + 2 ] ) )
            {
                const std::size_t close = findClosing( pos + 2, '`', 2 );
                if( close != std::string_view::npos )
                {
                    addToken( pos, close + 2, InlineKind::Literal );
                    pos = close + 2;
                    continue;
                }
                exhaustedLiteral = true;
            }
            if( !doubled && !exhaustedBacktick && canOpenAt( line, pos )
                && pos + 1 < size && !isInlineSpace( line[ pos + 1 ] ) )
            {
                std::size_t close = pos + 1;
                while( close < size && line[ close ] != '`' )
                    ++close;
                if( close < size && close > pos + 1 )
                {
                    // `text`_ 는 하이퍼링크, `text` 는 해석 텍스트.
                    const bool trailingUnderscore = ( close + 1 < size && line[ close + 1 ] == '_' );
                    const std::size_t end = trailingUnderscore ? close + 2 : close + 1;
                    if( canCloseAt( line, close, end ) )
                    {
                        addToken( pos, end,
                              trailingUnderscore ? InlineKind::Hyperlink : InlineKind::Interpreted );
                        pos = end;
                        continue;
                    }
                }
                if( close >= size )
                    exhaustedBacktick = true;
            }
            ++pos;
            continue;
        }

        if( ch == '*' )
        {
            const bool doubled = ( pos + 1 < size && line[ pos + 1 ] == '*' );
            if( doubled )
            {
                if( !exhaustedStrong && canOpenAt( line, pos ) && pos + 2 < size
                    && !isInlineSpace( line[ pos + 2 ] ) )
                {
                    const std::size_t close = findClosing( pos + 2, '*', 2 );
                    if( close != std::string_view::npos )
                    {
                        addToken( pos, close + 2, InlineKind::Strong );
                        pos = close + 2;
                        continue;
                    }
                    exhaustedStrong = true;
                }
                pos += 2;
                continue;
            }
            if( !exhaustedEmphasis && canOpenAt( line, pos ) && pos + 1 < size
                && !isInlineSpace( line[ pos + 1 ] ) && line[ pos + 1 ] != '*' )
            {
                std::size_t close = pos + 1;
                while( close < size && line[ close ] != '*' )
                    ++close;
                if( close < size && close > pos + 1 && canCloseAt( line, close, close + 1 ) )
                {
                    addToken( pos, close + 1, InlineKind::Emphasis );
                    pos = close + 1;
                    continue;
                }
                if( close >= size )
                    exhaustedEmphasis = true;
            }
            ++pos;
            continue;
        }

        if( ch == '|' )
        {
            if( !exhaustedPipe && canOpenAt( line, pos ) && pos + 1 < size
                && !isInlineSpace( line[ pos + 1 ] ) )
            {
                std::size_t close = pos + 1;
                while( close < size && line[ close ] != '|' )
                    ++close;
                if( close < size && close > pos + 1 )
                {
                    // 치환 참조 뒤에는 `_` 나 `__` 가 붙을 수 있다.
                    std::size_t end = close + 1;
                    if( end < size && line[ end ] == '_' )
                        ++end;
                    if( end < size && line[ end ] == '_' )
                        ++end;
                    if( canCloseAt( line, close, end ) )
                    {
                        addToken( pos, end, InlineKind::Substitution );
                        pos = end;
                        continue;
                    }
                }
                if( close >= size )
                    exhaustedPipe = true;
            }
            ++pos;
            continue;
        }

        if( ch == ':' && canOpenAt( line, pos ) )
        {
            const std::size_t nameEnd = scanQualifiedName( line, pos + 1 );
            if( nameEnd > pos + 1 && nameEnd + 1 < size && line[ nameEnd ] == ':'
                && line[ nameEnd + 1 ] == '`' )
            {
                std::size_t close = nameEnd + 2;
                while( close < size && line[ close ] != '`' )
                    ++close;
                if( close < size && canCloseAt( line, close, close + 1 ) )
                {
                    addToken( pos, close + 1, InlineKind::Role, pos + 1, nameEnd );
                    pos = close + 1;
                    continue;
                }
            }
            ++pos;
            continue;
        }

        if( isAsciiAlnum( ch ) )
        {
            // 단순 참조 이름 `word_`. 연속 전체를 한 번에 판정하고 넘어간다 —
            // 실패했다면 더 짧은 접미사도 같은 이유로 실패하기 때문이다.
            std::size_t runEnd = pos;
            while( runEnd < size && ( isAsciiAlnum( line[ runEnd ] ) || line[ runEnd ] == '-'
                                      || line[ runEnd ] == '.' || line[ runEnd ] == '_' ) )
                ++runEnd;

            // 말미의 `_` 또는 `__` 가 참조 표시다. 그 앞은 영숫자여야 한다.
            std::size_t underscores = 0;
            while( underscores < 2 && runEnd - underscores > pos
                   && line[ runEnd - underscores - 1 ] == '_' )
                ++underscores;

            if( underscores > 0 && canOpenAt( line, pos )
                && isAsciiAlnum( line[ runEnd - underscores - 1 ] )
                && ( runEnd >= size || isCloserBoundary( line[ runEnd ] ) ) )
            {
                addToken( pos, runEnd, InlineKind::Hyperlink );
            }
            pos = runEnd;
            continue;
        }

        ++pos;
    }
}

}   // namespace hc::rst::syntax
