#include "rstTestRunner.hpp"

#include "rstSubstitutionIndex.hpp"

#include <QTest>

using namespace hc::rst;

namespace {

const SubstitutionEntry* find( const QVector< SubstitutionEntry >& entries, const QString& name )
{
    for( const SubstitutionEntry& entry : entries )
    {
        if( entry.name == name )
            return &entry;
    }
    return nullptr;
}

}  // namespace

/// `|name|` 자동완성이 낼 수 있는 후보는 전적으로 이 파서에서 나온다.
///
/// 실사용에서 치환을 가장 많이 두는 자리는 .rst 가 아니라 **conf.py 의
/// rst_prolog** 다. 파이썬 문자열을 꺼내는 부분이 조용히 틀리면 목록이 통째로
/// 비고, 그것은 "기능이 없다" 와 구분되지 않는다.
class TestRstSubstitutions : public QObject
{
    Q_OBJECT

private slots:
    // ── .rst 안의 정의 ──
    void noSubstitutionYieldsNothing();
    void parsesReplaceDefinition();
    void parsesIndentedBody();
    void skipsDirectiveOptions();
    void normalizesNameWhitespace();
    void keepsContinuationOfArgument();
    void reportsLineNumberAndPath();
    void appliesLineOffset();
    void ignoresPlainDirective();

    // ── conf.py 의 파이썬 문자열 ──
    void readsTripleQuotedAssignment();
    void readsSingleQuotedAssignment();
    void keepsBackslashInRawString();
    void unescapesNonRawString();
    void ignoresNonLiteralAssignment();
    void ignoresSimilarlyNamedVariable();
    void parsesConfPrologWithFileLineNumbers();
    void parsesConfEpilog();

    // ── Sphinx 기본 치환 ──
    void builtinsAreAlwaysThere();
    void builtinsTakeValuesFromConf();

    // ── 표시 문자열 ──
    void summaryIsOneLine();
    void detailKeepsBody();
};

// ── .rst 안의 정의 ─────────────────────────────────────────

void TestRstSubstitutions::noSubstitutionYieldsNothing()
{
    const QString text = QStringLiteral( "제목\n====\n\n평범한 문단입니다. |있는 것처럼| 보여도 참조입니다.\n" );
    QVERIFY( parseSubstitutions( text ).isEmpty() );
}

void TestRstSubstitutions::parsesReplaceDefinition()
{
    const QString text = QStringLiteral( ".. |제품| replace:: iMonAIT\n" );

    const QVector< SubstitutionEntry > entries = parseSubstitutions( text );
    QCOMPARE( entries.size(), 1 );
    QCOMPARE( entries.at( 0 ).name, QStringLiteral( "제품" ) );
    QCOMPARE( entries.at( 0 ).directive, QStringLiteral( "replace" ) );
    QCOMPARE( entries.at( 0 ).argument, QStringLiteral( "iMonAIT" ) );
    QVERIFY( entries.at( 0 ).body.isEmpty() );
}

void TestRstSubstitutions::parsesIndentedBody()
{
    // conf.py 의 rst_prolog 가 |br| 을 만드는 실제 형태.
    const QString text = QStringLiteral( ".. |br| raw:: html\n"
                                        "\n"
                                        "    <br/>\n"
                                        "\n"
                                        "다음 문단.\n" );

    const QVector< SubstitutionEntry > entries = parseSubstitutions( text );
    QCOMPARE( entries.size(), 1 );
    QCOMPARE( entries.at( 0 ).directive, QStringLiteral( "raw" ) );
    QCOMPARE( entries.at( 0 ).argument, QStringLiteral( "html" ) );
    QCOMPARE( entries.at( 0 ).body, QStringLiteral( "<br/>" ) );
}

void TestRstSubstitutions::skipsDirectiveOptions()
{
    const QString text = QStringLiteral( ".. |로고| image:: logo.png\n"
                                        "   :alt: 회사 로고\n"
                                        "   :width: 120px\n" );

    const QVector< SubstitutionEntry > entries = parseSubstitutions( text );
    QCOMPARE( entries.size(), 1 );
    QCOMPARE( entries.at( 0 ).argument, QStringLiteral( "logo.png" ) );
    // 옵션은 본문이 아니다 — 상세 패널에 ":alt: ..." 가 정의로 보이면 안 된다.
    QVERIFY( entries.at( 0 ).body.isEmpty() );
}

void TestRstSubstitutions::normalizesNameWhitespace()
{
    // docutils 는 치환 이름의 공백을 정규화해서 대조한다.
    const QString text = QStringLiteral( ".. |  긴   이름  | replace:: 값\n" );

    const QVector< SubstitutionEntry > entries = parseSubstitutions( text );
    QCOMPARE( entries.size(), 1 );
    QCOMPARE( entries.at( 0 ).name, QStringLiteral( "긴 이름" ) );
}

void TestRstSubstitutions::keepsContinuationOfArgument()
{
    // 빈 줄 없이 이어지는 줄은 옵션이 아니면 인자의 계속이다.
    const QString text = QStringLiteral( ".. |긴글| replace:: 첫 줄\n"
                                        "   둘째 줄\n" );

    const QVector< SubstitutionEntry > entries = parseSubstitutions( text );
    QCOMPARE( entries.size(), 1 );
    QCOMPARE( entries.at( 0 ).argument, QStringLiteral( "첫 줄" ) );
    QCOMPARE( entries.at( 0 ).body, QStringLiteral( "둘째 줄" ) );
}

void TestRstSubstitutions::reportsLineNumberAndPath()
{
    const QString text = QStringLiteral( "제목\n====\n\n.. |x| replace:: 값\n" );

    const QVector< SubstitutionEntry > entries =
        parseSubstitutions( text, QStringLiteral( "/w/doc.rst" ) );
    QCOMPARE( entries.size(), 1 );
    QCOMPARE( entries.at( 0 ).line, 4 );
    QCOMPARE( entries.at( 0 ).path, QStringLiteral( "/w/doc.rst" ) );
}

void TestRstSubstitutions::appliesLineOffset()
{
    // conf.py 안의 rst_prolog 는 파일의 일부다. 줄 번호가 파일 기준이어야
    // 상세 패널의 "conf.py:9" 가 실제로 그 줄을 가리킨다.
    const QString text = QStringLiteral( ".. |x| replace:: 값\n" );

    const QVector< SubstitutionEntry > entries =
        parseSubstitutions( text, QStringLiteral( "/w/conf.py" ), SubstitutionOrigin::Conf, 20 );
    QCOMPARE( entries.size(), 1 );
    QCOMPARE( entries.at( 0 ).line, 21 );
    QVERIFY( entries.at( 0 ).origin == SubstitutionOrigin::Conf );
}

void TestRstSubstitutions::ignoresPlainDirective()
{
    const QString text = QStringLiteral( ".. note:: 치환이 아니다\n"
                                        ".. role:: raw-html(raw)\n" );
    QVERIFY( parseSubstitutions( text ).isEmpty() );
}

// ── conf.py 의 파이썬 문자열 ───────────────────────────────

void TestRstSubstitutions::readsTripleQuotedAssignment()
{
    const QString source = QStringLiteral( "rst_prolog = \"\"\"\n첫 줄\n둘째 줄\n\"\"\"\n" );

    int           firstLine = 0;
    const QString value =
        pythonStringAssignment( source, QStringLiteral( "rst_prolog" ), &firstLine );
    QCOMPARE( value, QStringLiteral( "\n첫 줄\n둘째 줄\n" ) );
    QCOMPARE( firstLine, 1 );
}

void TestRstSubstitutions::readsSingleQuotedAssignment()
{
    const QString source = QStringLiteral( "project = 'iMonAIT'\nversion = \"3.2\"\n" );

    QCOMPARE( pythonStringAssignment( source, QStringLiteral( "project" ) ),
             QStringLiteral( "iMonAIT" ) );
    QCOMPARE( pythonStringAssignment( source, QStringLiteral( "version" ) ),
             QStringLiteral( "3.2" ) );
}

void TestRstSubstitutions::keepsBackslashInRawString()
{
    const QString source = QStringLiteral( "rst_prolog = r\"\"\"\\n 그대로\"\"\"\n" );
    QCOMPARE( pythonStringAssignment( source, QStringLiteral( "rst_prolog" ) ),
             QStringLiteral( "\\n 그대로" ) );
}

void TestRstSubstitutions::unescapesNonRawString()
{
    const QString source = QStringLiteral( "rst_prolog = \".. |x| replace:: 값\\n\"\n" );
    QCOMPARE( pythonStringAssignment( source, QStringLiteral( "rst_prolog" ) ),
             QStringLiteral( ".. |x| replace:: 값\n" ) );
}

void TestRstSubstitutions::ignoresNonLiteralAssignment()
{
    // 값을 알려면 파이썬을 돌려야 한다. 조용히 틀린 값을 내는 것보다 비는 편이 낫다.
    const QString source = QStringLiteral( "rst_prolog = _build_prolog()\n" );
    QVERIFY( pythonStringAssignment( source, QStringLiteral( "rst_prolog" ) ).isEmpty() );
}

void TestRstSubstitutions::ignoresSimilarlyNamedVariable()
{
    const QString source = QStringLiteral( "mermaid_version = '10.9.1'\n" );
    QVERIFY( pythonStringAssignment( source, QStringLiteral( "version" ) ).isEmpty() );
}

void TestRstSubstitutions::parsesConfPrologWithFileLineNumbers()
{
    // 실사용 conf.py 의 모양 그대로.
    const QString conf = QStringLiteral( R"PY(project = 'iMonAIT'
version = '3.2'

rst_prolog = """
.. role:: raw-html(raw)
    :format: html

.. |br| raw:: html

    <br/>

.. |pb| raw:: pdf

    PageBreak
"""

html_theme = 'sphinx_rtd_theme'
)PY" );

    const QVector< SubstitutionEntry > entries =
        parseConfSubstitutions( conf, QStringLiteral( "/w/conf.py" ) );
    QCOMPARE( entries.size(), 2 );

    const SubstitutionEntry* br = find( entries, QStringLiteral( "br" ) );
    QVERIFY( br != nullptr );
    QCOMPARE( br->directive, QStringLiteral( "raw" ) );
    QCOMPARE( br->argument, QStringLiteral( "html" ) );
    QCOMPARE( br->body, QStringLiteral( "<br/>" ) );
    QCOMPARE( br->path, QStringLiteral( "/w/conf.py" ) );
    // conf.py 기준 줄 번호. rst_prolog 문자열 안의 줄 번호가 아니다.
    QCOMPARE( br->line, 8 );
    QVERIFY( br->origin == SubstitutionOrigin::Conf );

    const SubstitutionEntry* pb = find( entries, QStringLiteral( "pb" ) );
    QVERIFY( pb != nullptr );
    QCOMPARE( pb->line, 12 );
    QCOMPARE( pb->body, QStringLiteral( "PageBreak" ) );
}

void TestRstSubstitutions::parsesConfEpilog()
{
    const QString conf = QStringLiteral( "rst_epilog = \"\"\"\n.. |끝| replace:: 마침\n\"\"\"\n" );

    const QVector< SubstitutionEntry > entries =
        parseConfSubstitutions( conf, QStringLiteral( "/w/conf.py" ) );
    QCOMPARE( entries.size(), 1 );
    QCOMPARE( entries.at( 0 ).name, QStringLiteral( "끝" ) );
    QCOMPARE( entries.at( 0 ).line, 2 );
}

// ── Sphinx 기본 치환 ───────────────────────────────────────

void TestRstSubstitutions::builtinsAreAlwaysThere()
{
    const QVector< SubstitutionEntry > entries = builtinSubstitutions();
    QCOMPARE( entries.size(), 3 );
    QVERIFY( find( entries, QStringLiteral( "version" ) ) != nullptr );
    QVERIFY( find( entries, QStringLiteral( "release" ) ) != nullptr );
    QVERIFY( find( entries, QStringLiteral( "today" ) ) != nullptr );
}

void TestRstSubstitutions::builtinsTakeValuesFromConf()
{
    const QString conf = QStringLiteral( "project = 'iMonAIT'\nversion = '3.2'\nrelease = '3.2.1'\n" );

    const QVector< SubstitutionEntry > entries =
        builtinSubstitutions( conf, QStringLiteral( "/w/conf.py" ) );

    const SubstitutionEntry* version = find( entries, QStringLiteral( "version" ) );
    QVERIFY( version != nullptr );
    QCOMPARE( version->argument, QStringLiteral( "3.2" ) );
    QCOMPARE( version->line, 2 );
    QCOMPARE( version->path, QStringLiteral( "/w/conf.py" ) );

    const SubstitutionEntry* release = find( entries, QStringLiteral( "release" ) );
    QVERIFY( release != nullptr );
    QCOMPARE( release->argument, QStringLiteral( "3.2.1" ) );

    // today 는 빌드 시각이라 conf.py 에 값이 없다. 그래도 이름은 낸다.
    const SubstitutionEntry* today = find( entries, QStringLiteral( "today" ) );
    QVERIFY( today != nullptr );
    QVERIFY( today->path.isEmpty() );
}

// ── 표시 문자열 ────────────────────────────────────────────

void TestRstSubstitutions::summaryIsOneLine()
{
    SubstitutionEntry entry;
    entry.name = QStringLiteral( "br" );
    entry.directive = QStringLiteral( "raw" );
    entry.argument = QStringLiteral( "html" );
    entry.body = QStringLiteral( "<br/>" );
    QCOMPARE( substitutionSummary( entry ), QStringLiteral( "raw:: html" ) );

    // 인자가 없으면 본문 첫머리로 대신한다. 목록 한 행은 한 줄이어야 한다.
    SubstitutionEntry noArgument;
    noArgument.directive = QStringLiteral( "replace" );
    noArgument.body = QStringLiteral( "첫 줄\n둘째 줄" );
    QCOMPARE( substitutionSummary( noArgument ), QStringLiteral( "replace:: 첫 줄 둘째 줄" ) );
}

void TestRstSubstitutions::detailKeepsBody()
{
    SubstitutionEntry entry;
    entry.directive = QStringLiteral( "raw" );
    entry.argument = QStringLiteral( "html" );
    entry.body = QStringLiteral( "<br/>" );
    QCOMPARE( substitutionDetail( entry ), QStringLiteral( "raw:: html\n<br/>" ) );
}

HC_RST_REGISTER_TEST( TestRstSubstitutions );

#include "tstRstSubstitutions.moc"
