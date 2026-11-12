#include "rstTestRunner.hpp"

#include "text/RstContainerLexer.h"

#include <QTest>

#include <algorithm>
#include <string>
#include <vector>

using namespace hc::rst::syntax;

namespace {

/// 사람이 읽기 쉬운 요약: 줄마다 "레벨[H][W]".
QString describe( const std::vector< FoldLine >& folds )
{
    QStringList parts;
    for( const FoldLine& fold : folds )
    {
        QString text = QString::number( fold.level );
        if( fold.header )
            text += QLatin1Char( 'H' );
        if( fold.blank )
            text += QLatin1Char( 'W' );
        parts << text;
    }
    return parts.join( QLatin1Char( ' ' ) );
}

}  // namespace

class TestRstFolding : public QObject
{
    Q_OBJECT

private slots:
    void emptyDocument();
    /// 장식 문자에 고정 깊이를 주면 안 된다. 문서마다 1단계로 쓰는 문자가 다르다.
    void sectionDepthFollowsFirstAppearance();
    void overlinedTitleIsOneUnit();
    void underlineIsTheFoldHeader();
    void siblingSectionClosesPrevious();
    void directiveBodyFoldsUnderItsHeader();
    void blankLinesJoinTheDeeperNeighbour();
    /// Scintilla 는 머리 플래그만으로 마커를 그리지 않고 바로 다음 줄이 더
    /// 깊은지까지 본다. reST 제목 뒤에는 늘 빈 줄이 오므로 이 불변식이 깨지면
    /// 모든 섹션에서 접기 마커가 사라진다.
    void lineAfterHeaderIsDeeper();
    void indentedBlockWithoutAnyTitle();
    void lineCountMatchesDocument_data();
    void lineCountMatchesDocument();
};

void TestRstFolding::emptyDocument()
{
    const std::vector< FoldLine > folds = computeFoldLevels( "" );
    QCOMPARE( folds.size(), std::size_t( 1 ) );
    QCOMPARE( folds[ 0 ].level, 0 );
    QVERIFY( !folds[ 0 ].header );
}

void TestRstFolding::sectionDepthFollowsFirstAppearance()
{
    // '#' 을 1단계로, '=' 를 2단계로 쓰는 문서.
    // 장식은 렉서와 같은 기준으로 4글자 이상이어야 제목으로 인정된다.
    const std::string text =
        "Top\n"
        "#####\n"
        "\n"
        "body\n"
        "\n"
        "Sub\n"
        "=====\n"
        "\n"
        "more\n";

    const std::vector< FoldLine > folds = computeFoldLevels( text );

    QCOMPARE( folds[ 0 ].level, 0 );   // Top
    QCOMPARE( folds[ 1 ].level, 0 );   // ###
    QCOMPARE( folds[ 3 ].level, 1 );   // body
    QCOMPARE( folds[ 5 ].level, 1 );   // Sub — '=' 는 두 번째로 나온 장식
    QCOMPARE( folds[ 6 ].level, 1 );   // ===
    QCOMPARE( folds[ 8 ].level, 2 );   // more
}

void TestRstFolding::overlinedTitleIsOneUnit()
{
    const std::string text =
        "####\n"
        "제목\n"
        "####\n"
        "\n"
        "본문\n";

    const std::vector< FoldLine > folds = computeFoldLevels( text );

    // 세 줄 모두 섹션 자신의 깊이. 접었을 때 제목이 통째로 보여야 한다.
    QCOMPARE( folds[ 0 ].level, 0 );
    QCOMPARE( folds[ 1 ].level, 0 );
    QCOMPARE( folds[ 2 ].level, 0 );
    QCOMPARE( folds[ 4 ].level, 1 );

    // 접기 머리는 마지막 장식 줄이다. 그래야 제목 글자가 접힌 뒤에도 남는다.
    QVERIFY( !folds[ 0 ].header );
    QVERIFY( !folds[ 1 ].header );
    QVERIFY( folds[ 2 ].header );
}

void TestRstFolding::underlineIsTheFoldHeader()
{
    const std::vector< FoldLine > folds = computeFoldLevels( "Title\n=====\n\nbody\n" );

    QVERIFY( !folds[ 0 ].header );
    QVERIFY( folds[ 1 ].header );
}

void TestRstFolding::siblingSectionClosesPrevious()
{
    const std::string text =
        "A\n"
        "====\n"
        "\n"
        "a body\n"
        "\n"
        "B\n"
        "====\n"
        "\n"
        "b body\n";

    const std::vector< FoldLine > folds = computeFoldLevels( text );

    QCOMPARE( folds[ 3 ].level, 1 );   // a body
    QCOMPARE( folds[ 5 ].level, 0 );   // B — 형제 섹션이므로 다시 0
    QCOMPARE( folds[ 8 ].level, 1 );   // b body
}

void TestRstFolding::directiveBodyFoldsUnderItsHeader()
{
    const std::string text =
        ".. code-block:: python\n"
        "   :linenos:\n"
        "\n"
        "   print()\n"
        "\n"
        "다음 문단\n";

    const std::vector< FoldLine > folds = computeFoldLevels( text );

    QVERIFY2( folds[ 0 ].header, qPrintable( describe( folds ) ) );
    QCOMPARE( folds[ 0 ].level, 0 );
    QCOMPARE( folds[ 1 ].level, 1 );   // :linenos:
    QCOMPARE( folds[ 3 ].level, 1 );   // print()
    QCOMPARE( folds[ 5 ].level, 0 );   // 들여쓰기가 풀리면 블록도 끝난다
}

void TestRstFolding::blankLinesJoinTheDeeperNeighbour()
{
    const std::vector< FoldLine > folds = computeFoldLevels( ".. note::\n\n   text\n\nafter\n" );

    // 머리와 본문 사이의 빈 줄은 본문 쪽(깊은 쪽)에 붙는다.
    QVERIFY( folds[ 1 ].blank );
    QCOMPARE( folds[ 1 ].level, folds[ 2 ].level );

    // 블록 끝의 빈 줄도 블록 쪽(깊은 쪽)에 남는다. 다음 문단으로 넘어가면
    // 블록을 접었을 때 빈 줄만 덩그러니 남는다.
    QVERIFY( folds[ 3 ].blank );
    QCOMPARE( folds[ 3 ].level, folds[ 2 ].level );
}

void TestRstFolding::lineAfterHeaderIsDeeper()
{
    const std::string text =
        "#######\n"
        "제목\n"
        "#######\n"
        "\n"
        "본문\n"
        "\n"
        "절\n"
        "====\n"
        "\n"
        "절 본문\n"
        "\n"
        ".. note::\n"
        "\n"
        "   메모\n";

    const std::vector< FoldLine > folds = computeFoldLevels( text );

    bool sawHeader = false;
    for( std::size_t i = 0; i + 1 < folds.size(); ++i )
    {
        if( !folds[ i ].header )
            continue;
        sawHeader = true;
        QVERIFY2( folds[ i + 1 ].level > folds[ i ].level,
                  qPrintable( QStringLiteral( "%1행: 머리 %2, 다음 줄 %3 — %4" )
                                  .arg( i )
                                  .arg( folds[ i ].level )
                                  .arg( folds[ i + 1 ].level )
                                  .arg( describe( folds ) ) ) );
    }
    QVERIFY( sawHeader );
}

void TestRstFolding::indentedBlockWithoutAnyTitle()
{
    // 제목이 하나도 없는 조각 파일에서도 들여쓰기 접기는 살아 있어야 한다.
    const std::vector< FoldLine > folds = computeFoldLevels( "* 항목\n\n  계속\n\n* 다음\n" );

    QCOMPARE( folds[ 0 ].level, 0 );
    QVERIFY( folds[ 0 ].header );
    QCOMPARE( folds[ 2 ].level, 1 );
    QCOMPARE( folds[ 4 ].level, 0 );
}

void TestRstFolding::lineCountMatchesDocument_data()
{
    QTest::addColumn< QByteArray >( "text" );

    QTest::newRow( "empty" ) << QByteArray( "" );
    QTest::newRow( "no trailing newline" ) << QByteArray( "a\nb" );
    QTest::newRow( "trailing newline" ) << QByteArray( "a\nb\n" );
    QTest::newRow( "only blanks" ) << QByteArray( "\n\n\n" );
    QTest::newRow( "title at eof" ) << QByteArray( "Title\n=====" );
    QTest::newRow( "overline at eof" ) << QByteArray( "#####\nTitle\n#####" );
    QTest::newRow( "truncated overline" ) << QByteArray( "#####\nTitle" );
}

void TestRstFolding::lineCountMatchesDocument()
{
    QFETCH( QByteArray, text );

    const std::string source( text.constData(), static_cast< std::size_t >( text.size() ) );
    const std::vector< FoldLine > folds = computeFoldLevels( source );

    // Scintilla 는 줄마다 깊이 하나를 기대한다. 개수가 어긋나면 접기 구조가
    // 문서와 밀려서 엉뚱한 줄이 접힌다.
    const std::size_t expected =
        static_cast< std::size_t >( std::count( source.begin(), source.end(), '\n' ) ) + 1;
    QCOMPARE( folds.size(), expected );

    for( const FoldLine& fold : folds )
        QVERIFY( fold.level >= 0 );
}

HC_RST_REGISTER_TEST( TestRstFolding );

#include "tstRstFolding.moc"
