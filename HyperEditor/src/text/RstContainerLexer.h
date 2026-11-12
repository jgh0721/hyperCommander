#pragma once

#include <cstddef>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace hc::rst::syntax {

/// Scintilla 스타일 ID. Lexilla 에는 reStructuredText 렉서가 없어서 컨테이너
/// 렉싱(ILexer 를 비우고 SCN_STYLENEEDED 를 직접 처리)으로 칠한다.
enum Style : int
{
    STYLE_DEFAULT = 0,
    STYLE_COMMENT = 1,
    STYLE_TITLE = 2,
    STYLE_TRANSITION = 3,
    STYLE_DIRECTIVE_VALID = 4,
    STYLE_DIRECTIVE_INVALID = 5,
    STYLE_DIRECTIVE_UNKNOWN = 6,
    STYLE_ROLE_VALID = 7,
    STYLE_ROLE_INVALID = 8,
    STYLE_ROLE_UNKNOWN = 9,
    /// 리터럴 블록. `::` 로 끝난 문단 뒤의 들여쓴 블록과 `.. code-block::` 류의 본문이다.
    ///
    /// 줄 사이 상태가 필요해 syntax::LiteralBlockTracker 가 들고 다닌다. 창 단위로
    /// 불리는 렉서가 그 상태를 이을 수 있도록, 부르는 쪽이 창을 **비어 있지 않은
    /// 0열 줄**까지 위로 넓혀서 준다(ScintillaQtDirectBackend::handleStyleNeeded).
    STYLE_LITERAL = 10,
    STYLE_EMPHASIS = 11,
    STYLE_STRONG = 12,
    STYLE_INTERPRETED = 13,
    STYLE_INLINE_LITERAL = 14,
    STYLE_HYPERLINK = 15,
    STYLE_SUBSTITUTION = 16,
    STYLE_FIELD_NAME = 17,
    STYLE_EXPLICIT_MARKUP = 18,

    STYLE_COUNT = 19,
};

struct CompletionEntry
{
    std::string label;
    std::string insertText;
    std::string detail;
};

/// Esbonio 자동완성 결과에서 수확한 directive/role 이름.
///
/// 3-state 로 동작한다: 캐시가 비어 있으면 UNKNOWN, 채워진 뒤에는 목록에
/// 있으면 VALID, 없으면 INVALID. 덕분에 LSP 가 준비되기 전에 멀쩡한 문서가
/// 빨갛게 물드는 일이 없다.
class RstMetadataCache
{
public:
    std::set< std::string > directives;
    std::set< std::string > roles;
    bool directivesPopulated = false;
    bool rolesPopulated = false;

    [[nodiscard]] int directiveStyle( const std::string& name ) const;
    [[nodiscard]] int roleStyle( const std::string& name ) const;
    void updateFromCompletion( const std::vector< CompletionEntry >& entries );
};

/// UTF-8 바이트 오프셋 기준 구간. Scintilla 위치와 단위가 같다.
struct Span
{
    std::size_t start = 0;
    std::size_t end = 0;
    int style = STYLE_DEFAULT;
};

[[nodiscard]] std::string extractDirectiveName( const std::string& label, const std::string& insertText );

/// 한 줄의 접기 정보.
///
/// Scintilla 는 렉서가 줄마다 알려 준 깊이만으로 접기 구조를 만든다
/// (SCI_SETFOLDLEVEL). 어떤 줄의 깊이가 다음 줄보다 얕으면 그 줄이 머리가 되고
/// 더 깊은 줄들이 그 아래로 접힌다.
struct FoldLine
{
    int  level = 0;        ///< 0 이 문서 최상위. SC_FOLDLEVELBASE 를 더해서 쓴다.
    bool header = false;   ///< 이 줄에서 접기가 시작된다 (SC_FOLDLEVELHEADERFLAG)
    bool blank = false;    ///< 빈 줄 (SC_FOLDLEVELWHITEFLAG)
};

/// reST 문서 구조를 접기 깊이로 옮긴다.
///
/// 두 가지 축을 함께 쓴다.
///  * **섹션**: 제목 장식 양식이 처음 나온 순서가 곧 깊이다 (docutils 규칙).
///    문서마다 `#`, `*`, `=` 중 무엇을 1단계로 쓸지 자유롭기 때문에 문자 자체에
///    고정 깊이를 줄 수 없다.
///  * **들여쓰기**: directive 본문, 리스트 계속 줄, 리터럴 블록이 여기 걸린다.
///
/// 제목 판정은 RstStructure::titleScanAt 이 한다 — 개요·자동완성과 같은 규칙이다.
/// Lexilla 에 reST 렉서가 없어 이 계산도 우리가 한다. Qt 비의존이라 단위
/// 테스트에서 그대로 돌릴 수 있다.
[[nodiscard]] std::vector< FoldLine > computeFoldLevels( const std::string& utf8Text );

class RstContainerLexer
{
public:
    explicit RstContainerLexer( RstMetadataCache cache = {} );

    [[nodiscard]] const RstMetadataCache& metadataCache() const;
    [[nodiscard]] RstMetadataCache& metadataCache();

    /// 바이트당 스타일 1개를 out 에 채운다. out.size() 는 utf8Text.size() 와 같아야
    /// 한다 — SCI_SETSTYLINGEX 가 바이트 단위로 요구하기 때문이다.
    ///
    /// **힙 할당을 하지 않는다.** 호출자가 버퍼를 재사용하면 화면 한 장을 칠하는
    /// 경로에서 할당이 사라진다. styleBytes()/styleText() 는 이 함수의 어댑터다.
    void styleInto( std::string_view utf8Text, std::span< unsigned char > out ) const;

    /// lineAfterNext 는 윗줄/아랫줄로 감싼 제목(overline)을 알아보는 데 쓴다.
    ///     ########
    ///     제목
    ///     ########
    /// 첫 줄만 보면 단독 구분선과 구별할 수 없어 두 줄 아래까지 필요하다.
    ///
    /// 네 줄짜리 작은 문서를 만들어 styleInto() 로 칠한 뒤 가운데 줄만 잘라 낸다.
    /// 판정 규칙을 여기 한 벌 더 두면 그것이 곧 갈라지는 지점이 되기 때문이다.
    [[nodiscard]] std::vector< Span > tokenizeLine( const std::string& line,
                                                    const std::string& previousLine,
                                                    const std::string& nextLine,
                                                    const std::string& lineAfterNext = {} ) const;

    /// 문서 전체를 스팬 목록으로. 기본 스타일 구간도 빠짐없이 들어간다.
    [[nodiscard]] std::vector< Span > styleText( const std::string& utf8Text ) const;

    /// 바이트당 스타일 1개. 길이는 항상 utf8Text.size() 와 같다.
    [[nodiscard]] std::vector< unsigned char > styleBytes( const std::string& utf8Text ) const;

private:
    RstMetadataCache cache_;
};

}  // namespace hc::rst::syntax
