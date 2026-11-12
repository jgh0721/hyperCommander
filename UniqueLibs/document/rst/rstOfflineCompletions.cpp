#include "stdafx.h"
#include "rstOfflineCompletions.hpp"

/// 모듈
#include "rstPathCompletion.hpp"
#include "rstLineUtils.hpp"
#include "text/RstStructure.h"

/// UIs

/// 기타 헤더파일

/// 고유 라이브러리

/// 공통 라이브러리

/// 외부 라이브러리

/// 선언 헤더파일들

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QCoreApplication>
#include <QObject>
#include <QRegularExpression>
#include <QSet>

DECLARE_CMNLIBSV2_NAMESPACE
namespace hc::rst::rstcomplete {
namespace {

// LSP CompletionItemKind 중 쓰는 것만.
constexpr int kKindClass = 7;
constexpr int kKindProperty = 10;
constexpr int kKindKeyword = 14;
constexpr int kKindFile = 17;
constexpr int kKindReference = 18;

/// detail 은 자동완성 팝업에 그대로 보이는 문구라 번역 대상이다.
///
/// 정적 표라 tr() 을 쓸 수 없다 — tr() 은 실행 시점에 평가돼야 하는데 여기는
/// 표의 초기화 목록이다. QT_TRANSLATE_NOOP 로 lupdate 에만 알려 두고, 실제
/// 번역은 소비 지점에서 QCoreApplication::translate() 로 한다. translate() 는
/// 소스 문자열을 UTF-8 로 읽고 번역을 못 찾으면 원문을 그대로 돌려주므로,
/// 한국어 동작은 예전 QString::fromUtf8() 과 바이트 단위로 같다.
///
/// ⚠ 컨텍스트 "RstOfflineCompletions" 는 NOOP 과 translate() 양쪽이 글자 하나까지
///   같아야 한다. 다르면 lupdate 는 항목을 만들지만 실행 시점에는 못 찾아
///   조용히 원문이 나온다. QT_TRANSLATE_NOOP 은 컨텍스트를 리터럴로만 받으므로
///   상수로 뺄 수 없다 — 그래서 양쪽에 그대로 적는다.
///
/// options 는 ASCII 옵션명 목록이라 번역하지 않는다.
struct DirectiveSpec
{
    const char* name;
    const char* detail;
    const char* options;   ///< 공백으로 구분
};

/// docutils + Sphinx 기본 directive. Esbonio 가 응답하기 전까지 쓰인다.
const QVector< DirectiveSpec >& directiveTable()
{
    static const QVector< DirectiveSpec > table{
        { "note",         QT_TRANSLATE_NOOP( "RstOfflineCompletions", "참고 admonition" ),        "class name" },
        { "warning",      QT_TRANSLATE_NOOP( "RstOfflineCompletions", "경고 admonition" ),        "class name" },
        { "tip",          QT_TRANSLATE_NOOP( "RstOfflineCompletions", "팁 admonition" ),          "class name" },
        { "important",    QT_TRANSLATE_NOOP( "RstOfflineCompletions", "중요 admonition" ),        "class name" },
        { "caution",      QT_TRANSLATE_NOOP( "RstOfflineCompletions", "주의 admonition" ),        "class name" },
        { "danger",       QT_TRANSLATE_NOOP( "RstOfflineCompletions", "위험 admonition" ),        "class name" },
        { "attention",    QT_TRANSLATE_NOOP( "RstOfflineCompletions", "주목 admonition" ),        "class name" },
        { "error",        QT_TRANSLATE_NOOP( "RstOfflineCompletions", "오류 admonition" ),        "class name" },
        { "hint",         QT_TRANSLATE_NOOP( "RstOfflineCompletions", "힌트 admonition" ),        "class name" },
        { "admonition",   QT_TRANSLATE_NOOP( "RstOfflineCompletions", "제목을 직접 쓰는 admonition" ), "class name" },
        { "code-block",   QT_TRANSLATE_NOOP( "RstOfflineCompletions", "구문 강조 코드 블록" ),
          "linenos lineno-start emphasize-lines caption name dedent force class" },
        { "code",         QT_TRANSLATE_NOOP( "RstOfflineCompletions", "코드 블록 (docutils)" ),   "number-lines class name" },
        { "literalinclude", QT_TRANSLATE_NOOP( "RstOfflineCompletions", "파일 내용 삽입" ),
          "language linenos lines start-after end-before emphasize-lines caption "
          "dedent tab-width encoding pyobject diff class name" },
        { "include",      QT_TRANSLATE_NOOP( "RstOfflineCompletions", "다른 reST 파일 삽입" ),
          "start-line end-line start-after end-before literal code encoding tab-width" },
        { "image",        QT_TRANSLATE_NOOP( "RstOfflineCompletions", "이미지" ),                 "alt height width scale align target class name" },
        { "figure",       QT_TRANSLATE_NOOP( "RstOfflineCompletions", "캡션이 있는 이미지" ),
          "alt height width scale align target figwidth figclass class name" },
        { "toctree",      QT_TRANSLATE_NOOP( "RstOfflineCompletions", "목차 트리" ),
          "maxdepth caption numbered titlesonly glob hidden includehidden reversed name" },
        { "table",        QT_TRANSLATE_NOOP( "RstOfflineCompletions", "제목이 있는 표" ),         "widths width align class name" },
        { "list-table",   QT_TRANSLATE_NOOP( "RstOfflineCompletions", "리스트로 쓰는 표" ),
          "header-rows stub-columns widths width align class name" },
        { "csv-table",    QT_TRANSLATE_NOOP( "RstOfflineCompletions", "CSV 로 쓰는 표" ),
          "header header-rows stub-columns widths file url encoding delim quote escape class name" },
        { "math",         QT_TRANSLATE_NOOP( "RstOfflineCompletions", "수식 블록" ),              "label nowrap class name" },
        { "rubric",       QT_TRANSLATE_NOOP( "RstOfflineCompletions", "제목처럼 보이는 단락" ),   "class name" },
        { "topic",        QT_TRANSLATE_NOOP( "RstOfflineCompletions", "주제 블록" ),              "class name" },
        { "sidebar",      QT_TRANSLATE_NOOP( "RstOfflineCompletions", "사이드바" ),               "subtitle class name" },
        { "parsed-literal", QT_TRANSLATE_NOOP( "RstOfflineCompletions", "마크업이 해석되는 리터럴" ), "class name" },
        { "epigraph",     QT_TRANSLATE_NOOP( "RstOfflineCompletions", "인용구" ),                 "class" },
        { "highlights",   QT_TRANSLATE_NOOP( "RstOfflineCompletions", "요약 인용" ),              "class" },
        { "pull-quote",   QT_TRANSLATE_NOOP( "RstOfflineCompletions", "발췌 인용" ),              "class" },
        { "compound",     QT_TRANSLATE_NOOP( "RstOfflineCompletions", "복합 단락" ),              "class name" },
        { "container",    QT_TRANSLATE_NOOP( "RstOfflineCompletions", "임의 컨테이너" ),          "name" },
        { "raw",          QT_TRANSLATE_NOOP( "RstOfflineCompletions", "가공하지 않은 출력" ),     "file url encoding class name" },
        { "replace",      QT_TRANSLATE_NOOP( "RstOfflineCompletions", "치환 정의 본문" ),         "" },
        { "unicode",      QT_TRANSLATE_NOOP( "RstOfflineCompletions", "유니코드 치환" ),          "trim ltrim rtrim" },
        { "date",         QT_TRANSLATE_NOOP( "RstOfflineCompletions", "날짜 치환" ),              "" },
        { "contents",     QT_TRANSLATE_NOOP( "RstOfflineCompletions", "문서 내 목차" ),           "depth local backlinks class name" },
        { "sectnum",      QT_TRANSLATE_NOOP( "RstOfflineCompletions", "섹션 번호 매기기" ),       "depth start prefix suffix" },
        { "index",        QT_TRANSLATE_NOOP( "RstOfflineCompletions", "색인 항목" ),              "name" },
        { "only",         QT_TRANSLATE_NOOP( "RstOfflineCompletions", "조건부 출력" ),            "" },
        { "versionadded", QT_TRANSLATE_NOOP( "RstOfflineCompletions", "추가된 버전" ),            "" },
        { "versionchanged", QT_TRANSLATE_NOOP( "RstOfflineCompletions", "변경된 버전" ),          "" },
        { "deprecated",   QT_TRANSLATE_NOOP( "RstOfflineCompletions", "폐지 예정" ),              "" },
        { "seealso",      QT_TRANSLATE_NOOP( "RstOfflineCompletions", "참고 자료" ),              "class name" },
        { "centered",     QT_TRANSLATE_NOOP( "RstOfflineCompletions", "가운데 정렬 (구식)" ),     "" },
        { "highlight",    QT_TRANSLATE_NOOP( "RstOfflineCompletions", "이후 코드 블록 언어 지정" ), "linenothreshold force" },
    };
    return table;
}

/// detail 은 DirectiveSpec 과 같다 — QT_TRANSLATE_NOOP 로 표시하고 소비 지점에서
/// QCoreApplication::translate() 로 옮긴다.
struct RoleSpec
{
    const char* name;
    const char* detail;
};

const QVector< RoleSpec >& roleTable()
{
    static const QVector< RoleSpec > table{
        { "ref",       QT_TRANSLATE_NOOP( "RstOfflineCompletions", "레이블 참조" ) },
        { "doc",       QT_TRANSLATE_NOOP( "RstOfflineCompletions", "문서 참조" ) },
        { "download",  QT_TRANSLATE_NOOP( "RstOfflineCompletions", "파일 내려받기 링크" ) },
        { "numref",    QT_TRANSLATE_NOOP( "RstOfflineCompletions", "번호로 참조" ) },
        { "term",      QT_TRANSLATE_NOOP( "RstOfflineCompletions", "용어집 항목" ) },
        { "abbr",      QT_TRANSLATE_NOOP( "RstOfflineCompletions", "약어" ) },
        { "command",   QT_TRANSLATE_NOOP( "RstOfflineCompletions", "명령어" ) },
        { "file",      QT_TRANSLATE_NOOP( "RstOfflineCompletions", "파일 경로" ) },
        { "guilabel",  QT_TRANSLATE_NOOP( "RstOfflineCompletions", "GUI 레이블" ) },
        { "kbd",       QT_TRANSLATE_NOOP( "RstOfflineCompletions", "키 입력" ) },
        { "menuselection", QT_TRANSLATE_NOOP( "RstOfflineCompletions", "메뉴 경로" ) },
        { "mailheader", QT_TRANSLATE_NOOP( "RstOfflineCompletions", "메일 헤더" ) },
        { "manpage",   QT_TRANSLATE_NOOP( "RstOfflineCompletions", "man 페이지" ) },
        { "program",   QT_TRANSLATE_NOOP( "RstOfflineCompletions", "프로그램 이름" ) },
        { "samp",      QT_TRANSLATE_NOOP( "RstOfflineCompletions", "치환 가능한 코드" ) },
        { "math",      QT_TRANSLATE_NOOP( "RstOfflineCompletions", "인라인 수식" ) },
        { "eq",        QT_TRANSLATE_NOOP( "RstOfflineCompletions", "수식 참조" ) },
        { "envvar",    QT_TRANSLATE_NOOP( "RstOfflineCompletions", "환경 변수" ) },
        { "option",    QT_TRANSLATE_NOOP( "RstOfflineCompletions", "명령행 옵션" ) },
        { "regexp",    QT_TRANSLATE_NOOP( "RstOfflineCompletions", "정규식" ) },
        { "subscript", QT_TRANSLATE_NOOP( "RstOfflineCompletions", "아래 첨자" ) },
        { "superscript", QT_TRANSLATE_NOOP( "RstOfflineCompletions", "위 첨자" ) },
        { "title-reference", QT_TRANSLATE_NOOP( "RstOfflineCompletions", "제목 참조" ) },
        { "pep",       QT_TRANSLATE_NOOP( "RstOfflineCompletions", "PEP 참조" ) },
        { "rfc",       QT_TRANSLATE_NOOP( "RstOfflineCompletions", "RFC 참조" ) },
        { "emphasis",  QT_TRANSLATE_NOOP( "RstOfflineCompletions", "강조" ) },
        { "strong",    QT_TRANSLATE_NOOP( "RstOfflineCompletions", "굵게" ) },
        { "literal",   QT_TRANSLATE_NOOP( "RstOfflineCompletions", "리터럴" ) },
        { "code",      QT_TRANSLATE_NOOP( "RstOfflineCompletions", "코드" ) },
    };
    return table;
}

QStringList optionsFor( const QString& directiveName )
{
    for( const DirectiveSpec& spec : directiveTable() )
    {
        if( QLatin1String( spec.name ) == directiveName )
        {
            const QString options = QString::fromLatin1( spec.options );
            return options.isEmpty() ? QStringList{}
                                     : options.split( QLatin1Char( ' ' ), Qt::SkipEmptyParts );
        }
    }
    return {};
}

// 줄 앞 공백 문자 수. 공용 도우미를 쓴다 — 예전에는 이 파일과 조율자에 같은
// 사본이 하나씩 있었다.
using hc::rst::rstline::leadingSpaceCount;

/// 커서가 directive 블록 안(들여쓰기된 본문)이라면 그 directive 이름.
///
/// previousLines 는 역순이다 (바로 앞 줄이 [0]). 위로 거슬러 올라가다가
/// 처음 만나는 "더 얕게 들여쓴 줄" 이 directive 면 그 안에 있는 것이고,
/// 아니면 블록 밖이다.
QString enclosingDirective( const QStringList& previousLines, const int currentIndent )
{
    for( const QString& line : previousLines )
    {
        if( line.trimmed().isEmpty() )
            continue;

        const int indent = leadingSpaceCount( line );
        if( indent >= currentIndent )
            continue;   // 같은 블록 안의 다른 줄. 계속 위로 본다.

        // 더 얕게 들여쓴 첫 줄. 이게 directive 여야 우리가 그 블록 안이다.
        //
        // 판정은 공용 파서에 맡긴다. 예전에는 여기에 정규식이 한 벌 더 있었고
        // 다섯 벌 중 이것만 이름의 `+` 를 막고 있었다.
        const QByteArray utf8 = line.toUtf8();
        const std::optional< syntax::DirectiveParts > parts = syntax::parseDirective(
            std::string_view( utf8.constData(), static_cast< std::size_t >( utf8.size() ) ) );
        if( !parts )
            return {};
        return QString::fromUtf8( utf8.constData() + parts->nameStart,
                                  static_cast< qsizetype >( parts->nameEnd - parts->nameStart ) );
    }
    return {};
}


/// Scintilla 의 lineText() 는 줄 끝 문자까지 준다. `$` 로 끝나는 검사에 그대로
/// 넣으면 조용히 어긋나므로 여기서 뗀다.
QString stripEol( QString line )
{
    while( !line.isEmpty()
           && ( line.endsWith( QLatin1Char( '\n' ) ) || line.endsWith( QLatin1Char( '\r' ) ) ) )
    {
        line.chop( 1 );
    }
    return line;
}

/// 이 줄의 `|` 가 치환이 아니라 **표·줄 블록** 문법인가.
///
/// 격자 표(`| 칸 | 칸 |`)와 줄 블록(`| 한 줄`)은 줄머리에 `|` 를 쓴다. 거기서
/// 후보를 띄우면 표 한 칸마다 목록이 튀어나온다. 가르는 규칙은 docutils 그대로다
/// — 줄머리 `|` 뒤에 공백이 오면 치환 참조가 될 수 없다. 방금 친 `|` 뒤에 아직
/// 아무것도 없어 그 규칙을 쓸 수 없을 때만 앞 줄을 본다.
bool barIsBlockMarkup( const QString& rawLine, const QStringList& previousLines )
{
    const QString lineText = stripEol( rawLine );
    const int     first = leadingSpaceCount( lineText );
    if( first >= lineText.length() || lineText.at( first ) != QLatin1Char( '|' ) )
        return false;   // 줄머리가 `|` 가 아니면 표도 줄 블록도 아니다

    if( first + 1 < lineText.length() && lineText.at( first + 1 ).isSpace() )
        return true;

    // **바로 위 한 줄만** 본다. 표 안이라면 그 줄이 이미 표이고, 빈 줄이면
    // 표도 줄 블록도 거기서 끝난 것이다.
    if( previousLines.isEmpty() )
        return false;

    const QString previous = stripEol( previousLines.constFirst() );
    if( previous.trimmed().isEmpty() )
        return false;

    static const QRegularExpression gridRe( QStringLiteral( R"(^\s*\+[-=+]+\+\s*$)" ) );
    static const QRegularExpression rowRe( QStringLiteral( R"(^\s*\|.*\|\s*$)" ) );
    static const QRegularExpression lineBlockRe( QStringLiteral( R"(^\s*\|(\s|$))" ) );
    return gridRe.match( previous ).hasMatch() || rowRe.match( previous ).hasMatch()
        || lineBlockRe.match( previous ).hasMatch();
}

/// 캐럿 앞에서 치환 참조를 여는 `|` 의 위치. 없으면 -1.
///
/// docutils 의 인라인 마크업 규칙을 그대로 쓴다. 시작 문자열은 줄머리이거나
/// 공백 또는 `-:/'"<([{` 뒤여야 하고, 백슬래시로 이스케이프되지 않아야 하며,
/// 바로 뒤에 공백이 올 수 없다.
///
/// 이 규칙 하나가 `|foo|` 를 닫는 `|` 도 걸러 준다 — 닫는 쪽은 앞 글자가
/// 공백이 아니기 때문이다. 그것이 없으면 참조를 손으로 닫는 순간 목록이 다시 뜬다.
int substitutionBarIndex( const QString& lineText, const QString& before,
                          const QStringList& previousLines )
{
    const int bar = static_cast< int >( before.lastIndexOf( QLatin1Char( '|' ) ) );
    if( bar < 0 )
        return -1;

    // 이스케이프한 `\|` 는 문자 그대로의 세로줄이다.
    if( bar > 0 && before.at( bar - 1 ) == QLatin1Char( '\\' ) )
        return -1;

    static const QString openers = QStringLiteral( " \t-:/'\"<([{" );
    if( bar > 0 && !openers.contains( before.at( bar - 1 ) ) )
        return -1;

    const QString typed = before.mid( bar + 1 );
    if( !typed.isEmpty() && typed.at( 0 ).isSpace() )
        return -1;

    return barIsBlockMarkup( lineText, previousLines ) ? -1 : bar;
}

/// prefix 가 정해진 컨텍스트를 마무리한다.
///
/// replaceLength 는 언제나 **친 것 전체**다. 경로 항목의 insertText 를 문서 기준
/// 전체 상대 경로로 통일했기 때문이다 — 그래야 "현재 디렉터리 한 단계" 후보와
/// "프로젝트 전역 퍼지" 후보가 한 목록에 섞여도 삽입 규칙이 하나로 유지된다.
/// 반면 팝업 필터는 마지막 조각만 봐야 한다. 후보의 라벨이 파일 이름뿐이라
/// "../img/lo" 전체로 거르면 부분수열 검사가 전부 실패한다.
Context finish( Context context )
{
    context.replaceLength = static_cast< int >( context.prefix.length() );
    context.filterPrefix = context.kind == ContextKind::Path
                               ? rstpath::splitTypedPath( context.prefix ).name
                               : context.prefix;
    return context;
}

/// 이 자리에 공백이 올 수 없는데 이미 들어 있으면 완성을 포기한다.
///
/// toctree 항목과 graphviz 인자가 그렇다. 공백이 보인다는 것은 사용자가 경로가
/// 아닌 무언가를 쓰고 있다는 뜻이므로 팝업을 띄우지 않는 편이 맞다.
bool valueFitsSlot( const QString& value, const rstpath::Slot& slot )
{
    return slot.spaces != rstpath::Spaces::Forbidden || !value.contains( QLatin1Char( ' ' ) );
}
}  // namespace

bool fuzzyMatchCompletion( const QString& pattern, const QString& candidate, int* score,
                           QVector< int >* matchedPositions )
{
    if( score != nullptr )
        *score = 0;
    if( matchedPositions != nullptr )
        matchedPositions->clear();

    if( pattern.isEmpty() )
        return true;
    if( pattern.length() > candidate.length() )
        return false;

    const QString foldedPattern = pattern.toCaseFolded();
    const QString foldedCandidate = candidate.toCaseFolded();

    QVector< int > positions;
    positions.reserve( foldedPattern.length() );
    int patternIndex = 0;
    for( int index = 0; index < foldedCandidate.length() && patternIndex < foldedPattern.length(); ++index )
    {
        if( foldedCandidate.at( index ) == foldedPattern.at( patternIndex ) )
        {
            positions.push_back( index );
            ++patternIndex;
        }
    }
    if( patternIndex < foldedPattern.length() )
        return false;

    static const QString boundaryChars = QStringLiteral( "_-/\\ ." );
    int total = 0;
    int previous = -2;
    for( int order = 0; order < positions.size(); ++order )
    {
        const int position = positions.at( order );
        if( position == previous + 1 )
            total += 5;
        if( position == 0 )
            total += 10;
        else if( boundaryChars.contains( candidate.at( position - 1 ) ) )
            total += 8;
        if( order == position )
            total += 3;
        previous = position;
    }
    // 흩어져 있을수록 감점. "cb" 가 "code-block" 보다 "c...b" 를 이기지 않게.
    total -= ( positions.last() - positions.first() + 1 ) - positions.size();

    if( score != nullptr )
        *score = total;
    if( matchedPositions != nullptr )
        *matchedPositions = positions;
    return true;
}

QStringList knownDirectives()
{
    QStringList names;
    for( const DirectiveSpec& spec : directiveTable() )
    {
        const QString name = QString::fromLatin1( spec.name );
        if( !names.contains( name ) )
            names << name;
    }
    return names;
}

QStringList knownRoles()
{
    QStringList names;
    for( const RoleSpec& spec : roleTable() )
        names << QString::fromLatin1( spec.name );
    return names;
}

QStringList substitutionDirectives()
{
    // docutils 가 치환 정의 안에서 받는 것 전부다. raw 가 여기 드는 것이
    // 뜻밖으로 보이지만, 실사용 conf.py 의 rst_prolog 가 `|br|` 을 만들 때 쓰는
    // 것이 정확히 `.. |br| raw:: html` 이다.
    return { QStringLiteral( "replace" ), QStringLiteral( "image" ),
            QStringLiteral( "unicode" ), QStringLiteral( "date" ), QStringLiteral( "raw" ) };
}

QStringList collectPreviousLines( const int caretLine, const int caretIndent,
                                  const std::function< std::optional< QString >( int ) >& lineAt,
                                  const int absoluteLimit )
{
    QStringList lines;
    for( int line = caretLine - 1; line >= 0 && lines.size() < absoluteLimit; --line )
    {
        const std::optional< QString > text = lineAt( line );
        if( !text )
            break;
        lines << *text;
        if( text->trimmed().isEmpty() )
            continue;
        if( leadingSpaceCount( *text ) < caretIndent )
            break;   // 블록 경계
    }
    return lines;   // 역순: 바로 앞 줄이 [0]
}

Context detectContext( const QString& lineText, const int column, const QStringList& previousLines )
{
    Context context;

    const int caret = qBound( 0, column - 1, static_cast< int >( lineText.length() ) );
    const QString before = lineText.left( caret );

    // ".. dir" / ".. |sub| dir" — directive 이름
    static const QRegularExpression directiveRe(
        QStringLiteral( R"(^(\s*)\.\.\s+(?:\|([^|]+)\|\s+)?([a-zA-Z0-9_.-]*)$)" ) );
    if( const QRegularExpressionMatch match = directiveRe.match( before ); match.hasMatch() )
    {
        context.kind = ContextKind::Directive;
        context.prefix = match.captured( 3 );
        // ".. |logo| " 는 치환 정의다. 그 자리에 올 수 있는 directive 는 몇 개뿐이다.
        context.substitutionDefinition = match.capturedStart( 2 ) >= 0;
        return finish( std::move( context ) );
    }

    // 경로 인자: ".. image:: pa" / ".. |logo| image:: pa"
    //
    // 인자를 (\S*) 가 아니라 (.*) 로 잡는다. image/figure/include/literalinclude 는
    // final_argument_whitespace 가 켜져 있어 공백이 든 경로가 **문법적으로 정당**하다.
    // 그리고 "::" 직후 공백이 없어도 받는다 — 사람들은 directive 이름 완성을 쓰지
    // 않고 ".. image::" 를 통째로 치는데, 거기서 Ctrl+Space 가 죽으면 기능이 없는
    // 것처럼 보인다.
    static const QRegularExpression argumentRe( QStringLiteral(
        R"(^\s*\.\.\s+(?:\|[^|]+\|\s+)?([a-zA-Z0-9_.-]+(?::[a-zA-Z0-9_.-]+)?)::([ \t]*)(.*)$)" ) );
    if( const QRegularExpressionMatch match = argumentRe.match( before ); match.hasMatch() )
    {
        const QString name = match.captured( 1 );
        if( const rstpath::Slot* slot = rstpath::slotForArgument( name );
            slot != nullptr && valueFitsSlot( match.captured( 3 ), *slot ) )
        {
            context.kind = ContextKind::Path;
            context.pathSite = PathSlotSite::Argument;
            context.directiveName = name;
            context.prefix = match.captured( 3 );
            context.argumentNeedsSpace = match.captured( 2 ).isEmpty();
            return finish( std::move( context ) );
        }
        // 경로가 아닌 인자(raw 의 포맷 이름, csv-table 의 표 제목 등)는 완성하지
        // 않는다. 아래 롤 검사로 흘려보내도 이 줄에는 걸릴 것이 없다.
    }

    // directive 블록 안 옵션의 **값**: "   :file: da"
    static const QRegularExpression optionValueRe(
        QStringLiteral( R"(^(\s+):([a-zA-Z0-9_-]+):([ \t]*)(.*)$)" ) );
    if( const QRegularExpressionMatch match = optionValueRe.match( before ); match.hasMatch() )
    {
        const QString owner = enclosingDirective( previousLines, match.captured( 1 ).length() );
        const QString option = match.captured( 2 );
        if( const rstpath::Slot* slot = rstpath::slotForOption( owner, option );
            slot != nullptr && valueFitsSlot( match.captured( 4 ), *slot ) )
        {
            context.kind = ContextKind::Path;
            context.pathSite = PathSlotSite::Option;
            context.directiveName = owner;
            context.optionName = option;
            context.prefix = match.captured( 4 );
            context.argumentNeedsSpace = match.captured( 3 ).isEmpty();
            return finish( std::move( context ) );
        }
    }

    // directive 블록 안의 옵션 **이름**: 들여쓰기 + ":opt"
    static const QRegularExpression optionRe( QStringLiteral( R"(^(\s+):([a-zA-Z0-9_-]*)$)" ) );
    if( const QRegularExpressionMatch match = optionRe.match( before ); match.hasMatch() )
    {
        const QString owner = enclosingDirective( previousLines, match.captured( 1 ).length() );
        if( !owner.isEmpty() )
        {
            context.kind = ContextKind::DirectiveOption;
            context.directiveName = owner;
            context.prefix = match.captured( 2 );
            return finish( std::move( context ) );
        }
    }

    // directive **본문**의 각 줄이 경로인 경우 (지금은 toctree 뿐).
    //
    // 옵션 검사 뒤에 와야 한다. "   :maxdepth" 도 아래 정규식에 걸리기 때문이다.
    // 소유 directive 를 반드시 확인하므로 평범한 들여쓴 산문에서는 발화하지 않는다.
    static const QRegularExpression bodyRe( QStringLiteral( R"(^(\s+)(\S*)$)" ) );
    if( const QRegularExpressionMatch match = bodyRe.match( before ); match.hasMatch() )
    {
        const QString owner = enclosingDirective( previousLines, match.captured( 1 ).length() );
        if( const rstpath::Slot* slot = rstpath::slotForBody( owner ); slot != nullptr )
        {
            context.kind = ContextKind::Path;
            context.pathSite = PathSlotSite::Body;
            context.directiveName = owner;
            context.prefix = match.captured( 2 );
            return finish( std::move( context ) );
        }
    }

    // :ref:`tar` — 참조 대상. :download: 와 :doc: 는 대상이 경로다.
    static const QRegularExpression targetRe(
        QStringLiteral( R"(:([a-zA-Z0-9_.-]+):`([^`]*)$)" ) );
    if( const QRegularExpressionMatch match = targetRe.match( before ); match.hasMatch() )
    {
        const QString role = match.captured( 1 );
        const QString target = match.captured( 2 );
        if( const rstpath::Slot* slot = rstpath::slotForRoleTarget( role );
            slot != nullptr && valueFitsSlot( target, *slot ) )
        {
            context.kind = ContextKind::Path;
            context.pathSite = PathSlotSite::RoleTarget;
        }
        else
        {
            context.kind = ContextKind::RoleTarget;
        }
        context.directiveName = role;
        context.prefix = target;
        return finish( std::move( context ) );
    }

    // 치환 참조: "|logo"
    //
    // 정의 자리(".. |na" — 새 이름을 짓는 중)에는 완성할 것이 없다. 참조 검사보다
    // 먼저 걸러 낸다. 닫는 `|` 가 이미 있으면 위의 directive 검사가 가져간다.
    static const QRegularExpression substitutionNameRe( QStringLiteral( R"(^\s*\.\.\s+\|[^|]*$)" ) );
    if( substitutionNameRe.match( before ).hasMatch() )
        return context;   // ContextKind::None

    if( const int bar = substitutionBarIndex( lineText, before, previousLines ); bar >= 0 )
    {
        context.kind = ContextKind::Substitution;
        context.prefix = before.mid( bar + 1 );
        return finish( std::move( context ) );
    }

    // 인라인 role 이름: ":re"
    // 줄 맨 앞의 ":opt:" 필드는 위에서 이미 걸러졌다.
    static const QRegularExpression roleRe( QStringLiteral( R"((?:^|[\s(\[{])(:[a-zA-Z0-9_.-]*)$)" ) );
    if( const QRegularExpressionMatch match = roleRe.match( before ); match.hasMatch() )
    {
        context.kind = ContextKind::Role;
        context.prefix = match.captured( 1 ).mid( 1 );   // 앞의 ':' 제외
        return finish( std::move( context ) );
    }

    return context;
}

QVector< Item > candidatesFor( const Context& context )
{
    QVector< Item > items;

    switch( context.kind )
    {
        case ContextKind::Directive:
        {
            // 치환 정의(".. |logo| ") 안에서는 인라인으로 펼쳐지는 것만 쓸 수 있다.
            const QStringList allowed =
                context.substitutionDefinition ? substitutionDirectives() : QStringList{};

            QStringList seen;
            for( const DirectiveSpec& spec : directiveTable() )
            {
                const QString name = QString::fromLatin1( spec.name );
                if( seen.contains( name ) )
                    continue;
                if( !allowed.isEmpty() && !allowed.contains( name ) )
                    continue;
                seen << name;
                items.push_back( { name, name + QStringLiteral( ":: " ),
                                  QCoreApplication::translate( "RstOfflineCompletions", spec.detail ),
                                  kKindClass } );
            }
            break;
        }
        case ContextKind::Role:
            for( const RoleSpec& spec : roleTable() )
            {
                const QString name = QString::fromLatin1( spec.name );
                items.push_back( { name, name + QStringLiteral( ":`" ),
                                  QCoreApplication::translate( "RstOfflineCompletions", spec.detail ),
                                  kKindKeyword } );
            }
            break;

        case ContextKind::DirectiveOption:
            for( const QString& option : optionsFor( context.directiveName ) )
            {
                items.push_back( { option, option + QStringLiteral( ": " ),
                                  QObject::tr( "%1 옵션" ).arg( context.directiveName ),
                                  kKindProperty } );
            }
            break;

        case ContextKind::RoleTarget:
            // 실제 대상 수집은 LSP 나 워크스페이스 스캔이 맡는다.
            // 오프라인 표에는 후보가 없다.
            break;

        case ContextKind::Path:
            // 경로 후보는 호출 측이 파일 시스템에서 채운다.
            break;

        case ContextKind::Substitution:
            // 치환 후보는 문서·conf.py 를 훑어야 나온다. 호출 측(SubstitutionIndex)이
            // 채운다 — 이 표는 파일 시스템도 프로젝트도 모른다.
            break;

        case ContextKind::None:
            break;
    }

    // 이미 입력한 접두로 거른다.
    if( !context.prefix.isEmpty() )
    {
        QVector< Item > filtered;
        for( const Item& item : items )
        {
            if( item.label.startsWith( context.prefix, Qt::CaseInsensitive ) )
                filtered.push_back( item );
        }
        return filtered;
    }

    return items;
}

QVector< Item > normalizeLspItems( QVector< Item > items, const QString& lineText, const int column )
{
    const int caret = qBound( 0, column - 1, static_cast< int >( lineText.length() ) );
    const QString before = lineText.left( caret );
    // 앞쪽만 잘라야 한다. trimmed() 로 뒤까지 자르면 정작 문제가 되는 상황인
    // ".. " (뒤에 공백 하나) 가 ".." 이 되어 컨텍스트 판정을 놓친다.
    const QStringView stripped = QStringView( before ).mid( leadingSpaceCount( before ) );
    if( !stripped.startsWith( QStringLiteral( ".. " ) ) )
        return items;

    for( Item& item : items )
    {
        // QString::trimmed() 는 뒤쪽도 자른다. 여기서는 앞만 잘라야 한다 —
        // "code-block:: " 의 끝 공백은 커서를 인자 자리로 보내는 의미가 있다.
        qsizetype leading = 0;
        while( leading < item.insertText.length() && item.insertText.at( leading ).isSpace() )
            ++leading;
        item.insertText = item.insertText.mid( leading );

        // 우리 표는 "image:: " 처럼 끝에 공백을 둔다 (커서를 인자 자리로
        // 보낸다). Esbonio 는 "image::" 로 준다. 그대로 두면 **같은
        // directive 가 목록에 두 번** 뜬다 — 중복 제거를 insertText 로
        // 하는데 끝 공백 하나 때문에 다른 문자열이 되기 때문이다.
        if( item.insertText.endsWith( QStringLiteral( "::" ) ) )
            item.insertText += QLatin1Char( ' ' );
    }
    return items;
}

QVector< Item > finalizeItems( QVector< Item > items, const int limit )
{
    QVector< Item > result;
    result.reserve( qMin( items.size(), static_cast< qsizetype >( limit ) ) );

    QSet< QString > seen;
    for( Item& item : items )
    {
        // Esbonio 는 스니펫 경계에 \x01 을 남기기도 한다. 그대로 넣으면 문서에 박힌다.
        item.insertText.remove( QChar( 0x0001 ) );
        if( item.insertText.isEmpty() || seen.contains( item.insertText ) )
            continue;

        seen.insert( item.insertText );
        result.push_back( item );
        if( result.size() >= limit )
            break;
    }
    return result;
}

QVector< Item > mergeItems( QVector< Item > primary, const QVector< Item >& additional )
{
    QSet< QString > seen;
    seen.reserve( primary.size() );
    for( const Item& item : primary )
        seen.insert( item.insertText );

    for( const Item& item : additional )
    {
        if( seen.contains( item.insertText ) )
            continue;
        seen.insert( item.insertText );
        primary.push_back( item );
    }
    return primary;
}

}  // namespace hc::rst::rstcomplete
