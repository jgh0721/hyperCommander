#include "rstTestRunner.hpp"

#include "rstGlossaryIndex.hpp"
#include "rstReferenceNavigation.hpp"

#include <QTest>

using namespace hc::rst;

namespace {

const GlossaryEntry* find( const QVector< GlossaryEntry >& entries, const QString& term )
{
    for( const GlossaryEntry& entry : entries )
    {
        if( entry.term == term )
            return &entry;
    }
    return nullptr;
}

}  // namespace

/// `:term:` 팝업에 보여 줄 정의 본문은 전적으로 이 파서에서 나온다.
/// 들여쓰기 규칙을 잘못 잡으면 정의가 통째로 비거나 옆 항목이 섞인다.
class TestGlossaryParsing : public QObject
{
    Q_OBJECT

private slots:
    void noGlossaryYieldsNothing();
    void parsesSingleTermAndDefinition();
    void keepsMultilineDefinition();
    void treatsConsecutiveTermLinesAsSynonyms();
    void stripsSortKey();
    void ignoresDirectiveOptions();
    void stopsAtDedentedContent();
    void handlesIndentedGlossary();
    void referenceTargets_data();
    void referenceTargets();
    void referenceDefinitions();
};

void TestGlossaryParsing::referenceTargets_data()
{
    QTest::addColumn<QString>( "marked" );
    QTest::addColumn<bool>( "rst" );
    QTest::addColumn<int>( "kind" );
    QTest::addColumn<QString>( "target" );
    using reference::Kind;
    QTest::newRow("term") << QStringLiteral(":term:`doc|utils`") << true << int(Kind::Term) << QStringLiteral("docutils");
    QTest::newRow("ref-title") << QStringLiteral(":ref:`표시 |이름 <section-label>`") << true << int(Kind::Ref) << QStringLiteral("section-label");
    QTest::newRow("unicode-tab") << QStringLiteral("\t한글 😀 :term:`|용어`") << true << int(Kind::Term) << QStringLiteral("용어");
    QTest::newRow("std-domain") << QStringLiteral(":std:ref:`ab|c`") << true << int(Kind::Ref) << QStringLiteral("abc");
    QTest::newRow("role-end") << QStringLiteral(":ref:`abc`|") << true << int(Kind::Ref) << QStringLiteral("abc");
    QTest::newRow("literal") << QStringLiteral("``:ref:`ab|c` ``") << true << -1 << QStringLiteral("");
    QTest::newRow("escaped") << QStringLiteral("\\:ref:`ab|c`") << true << -1 << QStringLiteral("");
    QTest::newRow("role-in-md") << QStringLiteral(":ref:`ab|c`") << false << -1 << QStringLiteral("");
    QTest::newRow("plain") << QStringLiteral("nothing |here") << true << -1 << QStringLiteral("");
    QTest::newRow("bare-url") << QStringLiteral("See https://exa|mple.org/a?q=1&b=2#part.") << true << int(Kind::Url) << QStringLiteral("https://example.org/a?q=1&b=2#part");
    QTest::newRow("md-url") << QStringLiteral("[link](https://exa|mple.org/a_(b))") << false << int(Kind::Url) << QStringLiteral("https://example.org/a_(b)");
    QTest::newRow("rst-link") << QStringLiteral("`link <http://exam|ple.org/a>`_") << true << int(Kind::Url) << QStringLiteral("http://example.org/a");
    QTest::newRow("autolink") << QStringLiteral("<https://exa|mple.org/>") << false << int(Kind::Url) << QStringLiteral("https://example.org/");
    QTest::newRow("url-end") << QStringLiteral("https://example.org/|") << false << int(Kind::Url) << QStringLiteral("https://example.org/");
    QTest::newRow("multiple") << QStringLiteral("http://first.org https://sec|ond.org") << false << int(Kind::Url) << QStringLiteral("https://second.org");
    QTest::newRow("non-http") << QStringLiteral("file:///C:/ex|ample.txt") << false << -1 << QStringLiteral("");
    QTest::newRow("outside-url") << QStringLiteral("|see https://example.org/") << false << -1 << QStringLiteral("");
}

void TestGlossaryParsing::referenceTargets()
{
    QFETCH(QString, marked);
    QFETCH(bool, rst);
    QFETCH(int, kind);
    QFETCH(QString, target);
    const int column = static_cast<int>( marked.indexOf( QLatin1Char('|') ) );
    QVERIFY( column >= 0 );
    marked.remove( column, 1 );
    const auto result = reference::targetAt( marked, column, rst );
    QCOMPARE( result.has_value(), kind >= 0 );
    if( result )
    {
        QCOMPARE( int(result->kind), kind );
        QCOMPARE( result->value, target );
    }
}

void TestGlossaryParsing::referenceDefinitions()
{
    using reference::Kind;
    const QString source = QStringLiteral(".. _section-label:\r\n\r\n제목\r\n====\r\n\r\n"
        ".. glossary::\r\n\r\n   용어\r\n      정의 본문.\r\n\r\n.. _`Label With Spaces`:\r\n");
    QCOMPARE( reference::definitionLine( source, {Kind::Ref, "SECTION-label"} ), 1 );
    QCOMPARE( reference::definitionLine( QChar(0xfeff) + source, {Kind::Ref, "section-label"} ), 1 );
    QCOMPARE( reference::definitionLine( source, {Kind::Term, QStringLiteral("용어")} ), 8 );
    QCOMPARE( reference::definitionLine( source, {Kind::Ref, "label   with spaces"} ), 11 );
    QCOMPARE( reference::definitionLine( source, {Kind::Ref, "missing"} ), 0 );
    QCOMPARE( reference::definitionLine( source, {Kind::Ref, QStringLiteral("용어")} ), 0 );
    QCOMPARE( reference::definitionLine( source, {Kind::Term, "section-label"} ), 0 );
}

void TestGlossaryParsing::noGlossaryYieldsNothing()
{
    const QString text = QStringLiteral( "제목\n====\n\n본문이다.\n" );
    QVERIFY( parseGlossary( text ).isEmpty() );
}

void TestGlossaryParsing::parsesSingleTermAndDefinition()
{
    const QString text = QStringLiteral(
        ".. glossary::\n"
        "\n"
        "   docutils\n"
        "      reStructuredText 를 처리하는 파이썬 라이브러리.\n" );

    const QVector< GlossaryEntry > entries = parseGlossary( text, QStringLiteral( "/docs/g.rst" ) );
    QCOMPARE( entries.size(), 1 );
    QCOMPARE( entries.at( 0 ).term, QStringLiteral( "docutils" ) );
    QCOMPARE( entries.at( 0 ).definition,
             QStringLiteral( "reStructuredText 를 처리하는 파이썬 라이브러리." ) );
    QCOMPARE( entries.at( 0 ).path, QStringLiteral( "/docs/g.rst" ) );
    // 용어 줄은 1-based. `.. glossary::` 가 1행, 빈 줄이 2행.
    QCOMPARE( entries.at( 0 ).line, 3 );
}

void TestGlossaryParsing::keepsMultilineDefinition()
{
    const QString text = QStringLiteral(
        ".. glossary::\n"
        "\n"
        "   Sphinx\n"
        "      문서 생성기.\n"
        "\n"
        "      docutils 위에 확장을 올린다.\n"
        "\n"
        "   role\n"
        "      인라인 마크업.\n" );

    const QVector< GlossaryEntry > entries = parseGlossary( text );
    QCOMPARE( entries.size(), 2 );

    const GlossaryEntry* sphinx = find( entries, QStringLiteral( "Sphinx" ) );
    QVERIFY( sphinx != nullptr );
    // 문단 사이 빈 줄은 유지하되, 옆 항목이 섞이면 안 된다.
    QCOMPARE( sphinx->definition,
             QStringLiteral( "문서 생성기.\n\ndocutils 위에 확장을 올린다." ) );

    const GlossaryEntry* role = find( entries, QStringLiteral( "role" ) );
    QVERIFY( role != nullptr );
    QCOMPARE( role->definition, QStringLiteral( "인라인 마크업." ) );
}

void TestGlossaryParsing::treatsConsecutiveTermLinesAsSynonyms()
{
    const QString text = QStringLiteral(
        ".. glossary::\n"
        "\n"
        "   role\n"
        "   롤\n"
        "      인라인 마크업을 만드는 이름 있는 해석기.\n" );

    const QVector< GlossaryEntry > entries = parseGlossary( text );
    QCOMPARE( entries.size(), 2 );
    QCOMPARE( entries.at( 0 ).definition, entries.at( 1 ).definition );
    QVERIFY( find( entries, QStringLiteral( "롤" ) ) != nullptr );
    // 동의어라도 각자 자기 줄 번호를 가져야 이동이 정확하다.
    QCOMPARE( entries.at( 0 ).line, 3 );
    QCOMPARE( entries.at( 1 ).line, 4 );
}

void TestGlossaryParsing::stripsSortKey()
{
    const QString text = QStringLiteral(
        ".. glossary::\n"
        "\n"
        "   docutils : docutils\n"
        "      정렬 키가 붙은 용어.\n" );

    const QVector< GlossaryEntry > entries = parseGlossary( text );
    QCOMPARE( entries.size(), 1 );
    QCOMPARE( entries.at( 0 ).term, QStringLiteral( "docutils" ) );
}

void TestGlossaryParsing::ignoresDirectiveOptions()
{
    const QString text = QStringLiteral(
        ".. glossary::\n"
        "   :sorted:\n"
        "\n"
        "   term\n"
        "      정의.\n" );

    const QVector< GlossaryEntry > entries = parseGlossary( text );
    QCOMPARE( entries.size(), 1 );
    QCOMPARE( entries.at( 0 ).term, QStringLiteral( "term" ) );
}

void TestGlossaryParsing::stopsAtDedentedContent()
{
    const QString text = QStringLiteral(
        ".. glossary::\n"
        "\n"
        "   term\n"
        "      정의.\n"
        "\n"
        "이건 용어집 밖의 본문이다.\n"
        "\n"
        "   이건 들여썼지만 용어집과 무관한 인용문이다.\n" );

    const QVector< GlossaryEntry > entries = parseGlossary( text );
    QCOMPARE( entries.size(), 1 );
    QCOMPARE( entries.at( 0 ).term, QStringLiteral( "term" ) );
}

void TestGlossaryParsing::handlesIndentedGlossary()
{
    // 용어집이 다른 directive 안에 들어 있어도 상대 들여쓰기로 판단해야 한다.
    const QString text = QStringLiteral(
        ".. only:: html\n"
        "\n"
        "   .. glossary::\n"
        "\n"
        "      nested\n"
        "         중첩된 용어집.\n" );

    const QVector< GlossaryEntry > entries = parseGlossary( text );
    QCOMPARE( entries.size(), 1 );
    QCOMPARE( entries.at( 0 ).term, QStringLiteral( "nested" ) );
    QCOMPARE( entries.at( 0 ).definition, QStringLiteral( "중첩된 용어집." ) );
}

HC_RST_REGISTER_TEST( TestGlossaryParsing );

#include "tstGlossaryParsing.moc"
