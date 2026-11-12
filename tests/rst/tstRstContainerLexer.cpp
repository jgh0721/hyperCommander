#include "rstTestRunner.hpp"

#include "text/RstContainerLexer.h"

#include <QTest>

#include <string>
#include <vector>

using namespace hc::rst::syntax;

namespace {

/// 특정 바이트 위치의 스타일.
int styleAt( const RstContainerLexer& lexer, const std::string& text, std::size_t byteOffset )
{
    const std::vector< unsigned char > styles = lexer.styleBytes( text );
    return byteOffset < styles.size() ? static_cast< int >( styles[ byteOffset ] ) : -1;
}

}  // namespace

class TestRstContainerLexer : public QObject
{
    Q_OBJECT

private slots:
    /// 가장 중요한 불변식: 스타일 배열 길이는 항상 입력 바이트 수와 같아야 한다.
    /// 어긋나면 SCI_SETSTYLINGEX 가 문서를 밀어서 칠한다.
    void byteCountIsConserved_data();
    void byteCountIsConserved();

    void titleUnderlineMakesTitle();
    void overlineTitleIsFullyStyled();
    void nonAsciiTitleComparesCharacterCounts();
    void standaloneTransition();
    void directiveIsUnknownUntilCachePopulated();
    void directiveBecomesValidOrInvalidAfterCache();
    void fieldNameSpansColons();
    void inlineMarkup();
    void commentLine();

    /// 비ASCII 앞선 인라인 마크업의 바이트 오프셋 정렬.
    /// src_cpp 포트는 regex 의 바이트 위치를 문자 인덱스로 오해해 여기서 밀렸다.
    void utf8OffsetsAreByteAccurate();
    void utf8OffsetsWithEmoji();

    void extractsDirectiveName_data();
    void extractsDirectiveName();

    // ── 규칙을 docutils 로 맞추면서 의도적으로 바뀐 동작 ──
    /// `.. |logo| image::` 는 치환 정의다. 예전에는 줄 전체가 주석색이었다.
    void substitutionDefinitionIsDirective();
    /// docutils 는 directive 이름에 `+` 를 허용한다. 예전에는 렉서만 막았다.
    void directiveNameAllowsPlus();
    /// 줄머리의 `:ref:`x`` 는 필드가 아니다. 닫는 콜론 뒤에 공백이 없기 때문이다.
    void leadingRoleIsNotField();
    /// `snake_case` 의 `snake_` 는 하이퍼링크가 아니다 (종료 경계 규칙).
    void snakeCaseIsNotHyperlink();
    /// `char *argv[]` 의 별표는 강조가 아니다 (시작 경계 규칙).
    void pointerStarIsNotEmphasis();
    /// 백틱 하나로 감싼 것은 해석 텍스트다. 예전에는 아무 색도 없었다.
    void interpretedTextIsStyled();
    /// 장식 길이 규칙. docutils 는 표시 열 폭으로 견주고 4자 미만은 특례가 있다.
    void adornmentLengthFollowsColumnWidth();

    // ── 리터럴 블록 ──
    /// `::` 로 끝난 문단 뒤의 들여쓴 블록.
    void doubleColonOpensLiteralBlock();
    /// `.. code-block::` 본문. 옵션 줄은 필드로 남는다.
    void codeBlockBodyIsLiteral();
    /// 본문 안의 `**` 나 `*` 는 인라인 마크업이 아니다.
    void literalBodySuppressesInlineMarkup();
    /// 들여쓰기가 얕아지면 블록이 끝난다.
    void dedentClosesLiteralBlock();
    /// `.. note::` 처럼 본문이 reST 인 directive 는 리터럴이 아니다.
    void ordinaryDirectiveBodyStaysReST();
    /// 제목 밑줄이 `::` 여도 리터럴 도입부로 오인하지 않는다.
    void titleUnderscoreIsNotLiteralIntro();
};

void TestRstContainerLexer::byteCountIsConserved_data()
{
    QTest::addColumn< QByteArray >( "text" );

    QTest::newRow( "empty" ) << QByteArray( "" );
    QTest::newRow( "single line" ) << QByteArray( "hello" );
    QTest::newRow( "trailing newline" ) << QByteArray( "hello\n" );
    QTest::newRow( "title" ) << QByteArray( "Title\n=====\n\nbody\n" );
    QTest::newRow( "directive" ) << QByteArray( ".. note::\n\n   text\n" );
    QTest::newRow( "field" ) << QByteArray( ":author: someone\n" );
    QTest::newRow( "inline" ) << QByteArray( "a **b** c *d* ``e`` |f| g_\n" );
    QTest::newRow( "korean" ) << QByteArray( "\xed\x95\x9c\xea\xb8\x80 **\xea\xb5\xb5\xea\xb2\x8c** \xeb\x81\x9d\n" );
    QTest::newRow( "crlf" ) << QByteArray( "Title\r\n=====\r\n\r\nbody\r\n" );
    QTest::newRow( "blank lines" ) << QByteArray( "\n\n\n" );
}

void TestRstContainerLexer::byteCountIsConserved()
{
    QFETCH( QByteArray, text );

    const RstContainerLexer lexer;
    const std::string input( text.constData(), static_cast< std::size_t >( text.size() ) );
    QCOMPARE( lexer.styleBytes( input ).size(), input.size() );
}

void TestRstContainerLexer::titleUnderlineMakesTitle()
{
    const RstContainerLexer lexer;
    const std::string text = "Title\n=====\nbody\n";

    QCOMPARE( styleAt( lexer, text, 0 ), int( STYLE_TITLE ) );   // "Title"
    QCOMPARE( styleAt( lexer, text, 6 ), int( STYLE_TITLE ) );   // "=====" (제목의 밑줄)
    QCOMPARE( styleAt( lexer, text, 12 ), int( STYLE_DEFAULT ) ); // "body"
}

void TestRstContainerLexer::overlineTitleIsFullyStyled()
{
    const RstContainerLexer lexer;
    // 윗줄/아랫줄로 감싼 제목은 세 줄 모두 제목이다.
    const std::string text = "########\nTitle\n########\n\nbody\n";

    QCOMPARE( styleAt( lexer, text, 0 ), int( STYLE_TITLE ) );    // 윗줄
    QCOMPARE( styleAt( lexer, text, 9 ), int( STYLE_TITLE ) );    // "Title"
    QCOMPARE( styleAt( lexer, text, 15 ), int( STYLE_TITLE ) );   // 아랫줄
    QCOMPARE( styleAt( lexer, text, 25 ), int( STYLE_DEFAULT ) ); // "body"

    // 짝이 맞지 않는 장식. docutils 는 "Title overline & underline mismatch" 오류를
    // 내고 **세 줄을 통째로 소비한다** — 구분선도 제목도 아니다(states.py 의 Line.text).
    // 예전에는 윗줄을 구분선으로 칠했는데, 그러면 가운데 줄이 뒤늦게 밑줄형 제목으로
    // 잡혀 Sphinx 산출물과 어긋났다.
    const std::string mismatched = "====\nTitle\n----\n";
    QCOMPARE( styleAt( lexer, mismatched, 0 ), int( STYLE_DEFAULT ) );
    QCOMPARE( styleAt( lexer, mismatched, 5 ), int( STYLE_DEFAULT ) );
}

void TestRstContainerLexer::nonAsciiTitleComparesCharacterCounts()
{
    const RstContainerLexer lexer;
    // "제목문" 은 3글자지만 UTF-8 로는 9바이트다. 바이트로 견주면 5글자짜리
    // 밑줄이 짧다고 판정되어 제목이 강조되지 않는다.
    const std::string text = "\xEC\xA0\x9C\xEB\xAA\xA9\xEB\xAC\xB8\n=====\nbody\n";

    QCOMPARE( styleAt( lexer, text, 0 ), int( STYLE_TITLE ) );    // 제목 첫 바이트
    QCOMPARE( styleAt( lexer, text, 10 ), int( STYLE_TITLE ) );   // 밑줄
    QCOMPARE( styleAt( lexer, text, 16 ), int( STYLE_DEFAULT ) ); // "body"
}

void TestRstContainerLexer::standaloneTransition()
{
    const RstContainerLexer lexer;
    // 앞 줄이 비어 있으면 제목 밑줄이 아니라 구분선이다.
    const std::string text = "\n----\n\n";
    QCOMPARE( styleAt( lexer, text, 1 ), int( STYLE_TRANSITION ) );
}

void TestRstContainerLexer::directiveIsUnknownUntilCachePopulated()
{
    const RstContainerLexer lexer;
    const std::string text = ".. note:: hi\n";

    QCOMPARE( styleAt( lexer, text, 0 ), int( STYLE_EXPLICIT_MARKUP ) );  // ".. "
    QCOMPARE( styleAt( lexer, text, 3 ), int( STYLE_DIRECTIVE_UNKNOWN ) ); // "note"
    QCOMPARE( styleAt( lexer, text, 7 ), int( STYLE_DIRECTIVE_UNKNOWN ) ); // "::"
}

void TestRstContainerLexer::directiveBecomesValidOrInvalidAfterCache()
{
    RstMetadataCache cache;
    cache.updateFromCompletion( { { "note", "note::", "directive" } } );
    QVERIFY( cache.directivesPopulated );

    const RstContainerLexer lexer( cache );
    QCOMPARE( styleAt( lexer, ".. note:: hi\n", 3 ), int( STYLE_DIRECTIVE_VALID ) );
    QCOMPARE( styleAt( lexer, ".. bogus:: hi\n", 3 ), int( STYLE_DIRECTIVE_INVALID ) );
}

void TestRstContainerLexer::fieldNameSpansColons()
{
    const RstContainerLexer lexer;
    const std::string text = ":author: someone\n";

    QCOMPARE( styleAt( lexer, text, 0 ), int( STYLE_FIELD_NAME ) );   // ':'
    QCOMPARE( styleAt( lexer, text, 7 ), int( STYLE_FIELD_NAME ) );   // 닫는 ':'
    QCOMPARE( styleAt( lexer, text, 9 ), int( STYLE_DEFAULT ) );      // "someone"
}

void TestRstContainerLexer::inlineMarkup()
{
    const RstContainerLexer lexer;
    //             0123456789...
    const std::string text = "a **b** c ``d``\n";

    QCOMPARE( styleAt( lexer, text, 0 ), int( STYLE_DEFAULT ) );        // 'a'
    QCOMPARE( styleAt( lexer, text, 2 ), int( STYLE_STRONG ) );         // "**b**"
    QCOMPARE( styleAt( lexer, text, 6 ), int( STYLE_STRONG ) );
    QCOMPARE( styleAt( lexer, text, 8 ), int( STYLE_DEFAULT ) );        // 'c'
    QCOMPARE( styleAt( lexer, text, 10 ), int( STYLE_INLINE_LITERAL ) ); // "``d``"
}

void TestRstContainerLexer::commentLine()
{
    const RstContainerLexer lexer;
    // "::" 로 끝나는 directive 가 아닌 명시적 마크업은 주석이다.
    QCOMPARE( styleAt( lexer, ".. this is a comment\n", 0 ), int( STYLE_COMMENT ) );
}

void TestRstContainerLexer::utf8OffsetsAreByteAccurate()
{
    const RstContainerLexer lexer;
    // "한글 " = 7바이트 (3 + 3 + 공백 1), 그 뒤에 "**굵게**" 가 온다.
    const std::string text = "\xed\x95\x9c\xea\xb8\x80 **\xea\xb5\xb5\xea\xb2\x8c** end\n";

    // 바이트 배치: 한(0-2) 글(3-5) 공백(6) *(7) *(8) 굵(9-11) 게(12-14) *(15) *(16) 공백(17) e(18)
    QCOMPARE( styleAt( lexer, text, 0 ), int( STYLE_DEFAULT ) );  // 한
    QCOMPARE( styleAt( lexer, text, 6 ), int( STYLE_DEFAULT ) );  // 공백
    QCOMPARE( styleAt( lexer, text, 7 ), int( STYLE_STRONG ) );   // 여는 '*'
    QCOMPARE( styleAt( lexer, text, 8 ), int( STYLE_STRONG ) );   // 여는 '*'
    QCOMPARE( styleAt( lexer, text, 14 ), int( STYLE_STRONG ) );  // 게 마지막 바이트
    QCOMPARE( styleAt( lexer, text, 15 ), int( STYLE_STRONG ) );  // 닫는 '*'
    QCOMPARE( styleAt( lexer, text, 16 ), int( STYLE_STRONG ) );  // 닫는 '*'
    QCOMPARE( styleAt( lexer, text, 17 ), int( STYLE_DEFAULT ) ); // 공백
    QCOMPARE( styleAt( lexer, text, 18 ), int( STYLE_DEFAULT ) ); // 'e'
}

void TestRstContainerLexer::utf8OffsetsWithEmoji()
{
    const RstContainerLexer lexer;
    // U+1F600 = 4바이트. 그 뒤 "``code``".
    const std::string text = "\xf0\x9f\x98\x80 ``code``\n";

    QCOMPARE( styleAt( lexer, text, 0 ), int( STYLE_DEFAULT ) );
    QCOMPARE( styleAt( lexer, text, 4 ), int( STYLE_DEFAULT ) );        // 공백
    QCOMPARE( styleAt( lexer, text, 5 ), int( STYLE_INLINE_LITERAL ) ); // '`'
    QCOMPARE( styleAt( lexer, text, 12 ), int( STYLE_INLINE_LITERAL ) ); // 닫는 '`'
}

void TestRstContainerLexer::extractsDirectiveName_data()
{
    QTest::addColumn< QString >( "label" );
    QTest::addColumn< QString >( "insertText" );
    QTest::addColumn< QString >( "expected" );

    QTest::newRow( "plain" ) << "note" << "note::" << "note";
    QTest::newRow( "with dots" ) << "note" << ".. note::" << "note";
    QTest::newRow( "snippet" ) << "image" << "image:: ${1:path}" << "image";
    QTest::newRow( "label fallback" ) << ".. code-block::" << "" << "code-block";
    QTest::newRow( "domain" ) << "py:function" << "py:function::" << "py:function";
}

void TestRstContainerLexer::extractsDirectiveName()
{
    QFETCH( QString, label );
    QFETCH( QString, insertText );
    QFETCH( QString, expected );

    const std::string actual = extractDirectiveName( label.toStdString(), insertText.toStdString() );
    QCOMPARE( QString::fromStdString( actual ), expected );
}

void TestRstContainerLexer::substitutionDefinitionIsDirective()
{
    RstMetadataCache cache;
    cache.directives = { "image" };
    cache.directivesPopulated = true;
    const RstContainerLexer lexer( cache );

    const std::string text = ".. |logo| image:: logo.png\n";
    QCOMPARE( styleAt( lexer, text, 0 ), int( STYLE_EXPLICIT_MARKUP ) );   // ".."
    QCOMPARE( styleAt( lexer, text, 3 ), int( STYLE_SUBSTITUTION ) );      // "|logo|"
    QCOMPARE( styleAt( lexer, text, 10 ), int( STYLE_DIRECTIVE_VALID ) );  // "image"
    QCOMPARE( styleAt( lexer, text, 20 ), int( STYLE_DEFAULT ) );          // 인자
}

void TestRstContainerLexer::directiveNameAllowsPlus()
{
    RstMetadataCache cache;
    cache.directives = { "c+ext" };
    cache.directivesPopulated = true;
    const RstContainerLexer lexer( cache );

    const std::string text = ".. c+ext:: arg\n";
    QCOMPARE( styleAt( lexer, text, 3 ), int( STYLE_DIRECTIVE_VALID ) );
}

void TestRstContainerLexer::leadingRoleIsNotField()
{
    const RstContainerLexer lexer;
    const std::string       text = ":ref:`target` 은 링크다\n";
    // 예전에는 kFieldRegex 가 "ref" 를 필드 이름으로 잡았다.
    QCOMPARE( styleAt( lexer, text, 0 ), int( STYLE_ROLE_UNKNOWN ) );

    // 진짜 필드는 그대로 필드다.
    const std::string field = ":author: 홍길동\n";
    QCOMPARE( styleAt( lexer, field, 0 ), int( STYLE_FIELD_NAME ) );
}

void TestRstContainerLexer::snakeCaseIsNotHyperlink()
{
    const RstContainerLexer lexer;
    const std::string       text = "call snake_case now\n";
    for( std::size_t i = 0; i < text.size() - 1; ++i )
        QCOMPARE( styleAt( lexer, text, i ), int( STYLE_DEFAULT ) );

    // 경계를 만족하는 단순 참조는 그대로 링크다.
    const std::string reference = "see intro_ here\n";
    QCOMPARE( styleAt( lexer, reference, 4 ), int( STYLE_HYPERLINK ) );
}

void TestRstContainerLexer::pointerStarIsNotEmphasis()
{
    const RstContainerLexer lexer;
    const std::string       text = "char *argv[] here\n";
    for( std::size_t i = 0; i < text.size() - 1; ++i )
        QCOMPARE( styleAt( lexer, text, i ), int( STYLE_DEFAULT ) );

    // 경계를 만족하는 강조는 그대로 강조다.
    const std::string emphasis = "a *word* b\n";
    QCOMPARE( styleAt( lexer, emphasis, 2 ), int( STYLE_EMPHASIS ) );
}

void TestRstContainerLexer::interpretedTextIsStyled()
{
    const RstContainerLexer lexer;
    const std::string       text = "a `text` b\n";
    QCOMPARE( styleAt( lexer, text, 2 ), int( STYLE_INTERPRETED ) );

    // 뒤에 밑줄이 붙으면 하이퍼링크다.
    const std::string link = "a `text`_ b\n";
    QCOMPARE( styleAt( lexer, link, 2 ), int( STYLE_HYPERLINK ) );
}

void TestRstContainerLexer::adornmentLengthFollowsColumnWidth()
{
    const RstContainerLexer lexer;

    // 두 칸짜리 제목에는 두 자짜리 밑줄이면 된다 (docutils 실측).
    const std::string shortAscii = "AB\n==\n\nbody\n";
    QCOMPARE( styleAt( lexer, shortAscii, 0 ), int( STYLE_TITLE ) );

    // 한글 두 글자는 네 칸이다. 두 자짜리 밑줄로는 제목이 되지 않는다.
    const std::string shortHangul = "\xEC\xA0\x9C\xEB\xAA\xA9\n==\n\nbody\n";
    QCOMPARE( styleAt( lexer, shortHangul, 0 ), int( STYLE_DEFAULT ) );

    // 네 자면 된다.
    const std::string wideEnough = "\xEC\xA0\x9C\xEB\xAA\xA9\n====\n\nbody\n";
    QCOMPARE( styleAt( lexer, wideEnough, 0 ), int( STYLE_TITLE ) );

    // 같은 문자가 아니면 장식이 아니다.
    const std::string mixed = "\xEC\xA0\x9C\xEB\xAA\xA9\n=-=-\n\nbody\n";
    QCOMPARE( styleAt( lexer, mixed, 0 ), int( STYLE_DEFAULT ) );
}

void TestRstContainerLexer::doubleColonOpensLiteralBlock()
{
    const RstContainerLexer lexer;
    const std::string       text = "\xEB\xB3\xB8\xEB\xAC\xB8::\n\n    literal here\n\n\xEB\x81\x9D\n";

    // 도입부 줄 자체는 본문이다.
    QCOMPARE( styleAt( lexer, text, 0 ), int( STYLE_DEFAULT ) );
    QCOMPARE( styleAt( lexer, text, text.find( "literal" ) ), int( STYLE_LITERAL ) );
    // 0열로 돌아온 줄은 블록 밖이다.
    QCOMPARE( styleAt( lexer, text, text.find( "\xEB\x81\x9D" ) ), int( STYLE_DEFAULT ) );
}

void TestRstContainerLexer::codeBlockBodyIsLiteral()
{
    const RstContainerLexer lexer;
    const std::string       text =
        ".. code-block:: cpp\n   :linenos:\n\n   int a = *p;\n\nafter\n";

    // 옵션 줄은 옵션으로 남는다. 그것까지 리터럴로 칠하면 필드 색이 사라진다.
    QCOMPARE( styleAt( lexer, text, text.find( ":linenos:" ) ), int( STYLE_FIELD_NAME ) );
    QCOMPARE( styleAt( lexer, text, text.find( "int a" ) ), int( STYLE_LITERAL ) );
    QCOMPARE( styleAt( lexer, text, text.find( "after" ) ), int( STYLE_DEFAULT ) );
}

void TestRstContainerLexer::literalBodySuppressesInlineMarkup()
{
    const RstContainerLexer lexer;
    const std::string       text = ".. code-block:: python\n\n   f(**kwargs)\n   c = a * b\n";

    // 예전에는 `**kwargs)` 가 굵게, `a * b` 의 별표 구간이 강조로 칠해졌다.
    const std::size_t stars = text.find( "**kwargs" );
    QCOMPARE( styleAt( lexer, text, stars ), int( STYLE_LITERAL ) );
    QCOMPARE( styleAt( lexer, text, text.find( "a * b" ) + 2 ), int( STYLE_LITERAL ) );
}

void TestRstContainerLexer::dedentClosesLiteralBlock()
{
    const RstContainerLexer lexer;
    // 두 칸 들여쓴 도입부 — 본문은 그보다 깊어야 한다.
    const std::string text = "  intro::\n\n      deep\n  shallow **b**\n";

    QCOMPARE( styleAt( lexer, text, text.find( "deep" ) ), int( STYLE_LITERAL ) );
    // 도입부와 같은 깊이로 돌아왔으므로 블록이 끝났고, 인라인이 다시 산다.
    QCOMPARE( styleAt( lexer, text, text.find( "**b**" ) ), int( STYLE_STRONG ) );
}

void TestRstContainerLexer::ordinaryDirectiveBodyStaysReST()
{
    const RstContainerLexer lexer;
    const std::string       text = ".. note::\n\n   **strong** stays strong\n";

    QCOMPARE( styleAt( lexer, text, text.find( "**strong**" ) ), int( STYLE_STRONG ) );
}

void TestRstContainerLexer::titleUnderscoreIsNotLiteralIntro()
{
    const RstContainerLexer lexer;
    // 제목 글이 `::` 로 끝난다. 밑줄이 붙었으므로 제목이지 리터럴 도입부가 아니다.
    const std::string text = "usage::\n=======\n\n   **still strong**\n";

    QCOMPARE( styleAt( lexer, text, 0 ), int( STYLE_TITLE ) );
    QCOMPARE( styleAt( lexer, text, text.find( "**still" ) ), int( STYLE_STRONG ) );
}

HC_RST_REGISTER_TEST( TestRstContainerLexer );

#include "tstRstContainerLexer.moc"
