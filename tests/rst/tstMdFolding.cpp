#include "rstTestRunner.hpp"

#include "text/MarkdownStructure.h"

#include <QTest>

#include <string>
#include <vector>

using namespace hc::rst::md;
using hc::rst::syntax::FoldLine;

namespace {

/// 사람이 읽기 쉬운 요약: 줄마다 "레벨[H][W]". tst_RstFolding 과 같은 형식이다.
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

class TestMdFolding : public QObject
{
    Q_OBJECT

private slots:
    void emptyDocument();
    void atxHeaderIsFoldHeader();
    /// md 는 해시 개수가 곧 깊이다. reST 의 "처음 나온 순서" 규칙을 옮겨 오면 안 된다.
    void headerLevelMapsToFoldDepth();
    /// Scintilla 는 머리 플래그만으로 마커를 그리지 않고 바로 다음 줄이 더
    /// 깊은지까지 본다. Markdown 제목 뒤에는 거의 늘 빈 줄이 오므로 이 불변식이
    /// 깨지면 모든 섹션에서 접기 마커가 사라진다.
    void lineAfterHeaderIsDeeper();
    void siblingHeaderClosesPrevious();
    void deeperThenShallowerClosesBoth();
    void setextHeaderFolds();
    void fencedCodeBodyStaysInsideItsSection();
    void hashInsideFenceIsNotAHeader();
    void frontMatterIsNotAHeader();
    void blankLinesJoinTheDeeperNeighbour();
    void lineCountMatchesDocument_data();
    void lineCountMatchesDocument();
};

void TestMdFolding::emptyDocument()
{
    const std::vector< FoldLine > folds = computeMarkdownFoldLevels( "" );
    QCOMPARE( folds.size(), std::size_t( 1 ) );
    QCOMPARE( folds[ 0 ].level, 0 );
    QVERIFY( !folds[ 0 ].header );
}

void TestMdFolding::atxHeaderIsFoldHeader()
{
    const std::vector< FoldLine > folds = computeMarkdownFoldLevels( "# 제목\n\n본문\n" );
    QCOMPARE( describe( folds ), QStringLiteral( "0H 1W 1" ) );
}

void TestMdFolding::headerLevelMapsToFoldDepth()
{
    const std::vector< FoldLine > folds =
        computeMarkdownFoldLevels( "# 하나\n\n본문1\n\n## 둘\n\n본문2\n\n### 셋\n\n본문3\n" );

    // 제목 줄의 깊이는 (단계 - 1) 이고 그 본문은 (단계) 다. 그래야 하위 제목이
    // 상위 제목 아래로 접힌다.
    QCOMPARE( folds[ 0 ].level, 0 );   // # 하나
    QCOMPARE( folds[ 2 ].level, 1 );   // 본문1
    QCOMPARE( folds[ 4 ].level, 1 );   // ## 둘
    QCOMPARE( folds[ 6 ].level, 2 );   // 본문2
    QCOMPARE( folds[ 8 ].level, 2 );   // ### 셋
    QCOMPARE( folds[ 10 ].level, 3 );  // 본문3

    QVERIFY( folds[ 0 ].header );
    QVERIFY( folds[ 4 ].header );
    QVERIFY( folds[ 8 ].header );
}

void TestMdFolding::lineAfterHeaderIsDeeper()
{
    const std::vector< FoldLine > folds = computeMarkdownFoldLevels( "## 제목\n\n본문\n" );

    // 제목 바로 뒤가 빈 줄이어도 그 빈 줄은 더 깊은 쪽(본문)에 붙어야 한다.
    QVERIFY( folds[ 1 ].blank );
    QVERIFY( folds[ 1 ].level > folds[ 0 ].level );
    QVERIFY( folds[ 0 ].header );
}

void TestMdFolding::siblingHeaderClosesPrevious()
{
    const std::vector< FoldLine > folds =
        computeMarkdownFoldLevels( "## 가\n\n본문\n\n## 나\n\n본문\n" );

    QCOMPARE( folds[ 0 ].level, 1 );
    QCOMPARE( folds[ 4 ].level, 1 );   // 형제 제목은 같은 깊이
    QVERIFY( folds[ 0 ].header );
    QVERIFY( folds[ 4 ].header );
}

void TestMdFolding::deeperThenShallowerClosesBoth()
{
    const std::vector< FoldLine > folds =
        computeMarkdownFoldLevels( "### 깊게\n\n본문\n\n# 얕게\n\n본문\n" );

    QCOMPARE( folds[ 0 ].level, 2 );
    QCOMPARE( folds[ 4 ].level, 0 );   // 더 얕은 제목이 앞의 것들을 닫는다
    QVERIFY( folds[ 4 ].header );
}

void TestMdFolding::setextHeaderFolds()
{
    const std::vector< FoldLine > folds =
        computeMarkdownFoldLevels( "제목\n====\n\n본문\n" );

    // 제목 글자 줄과 밑줄이 같은 깊이를 갖고, 머리는 밑줄 쪽이다(다음 줄이 더 깊다).
    QCOMPARE( folds[ 0 ].level, 0 );
    QCOMPARE( folds[ 1 ].level, 0 );
    QCOMPARE( folds[ 3 ].level, 1 );
    QVERIFY( folds[ 1 ].header );
}

void TestMdFolding::fencedCodeBodyStaysInsideItsSection()
{
    const std::vector< FoldLine > folds =
        computeMarkdownFoldLevels( "# 제목\n\n```cpp\nint main() {}\n```\n" );

    // 펜스와 그 본문은 제목의 본문 깊이다. 펜스가 별도 접기 단위가 되면
    // 코드 블록 하나 때문에 섹션 접기가 끊긴다.
    QCOMPARE( folds[ 2 ].level, 1 );
    QCOMPARE( folds[ 3 ].level, 1 );
    QCOMPARE( folds[ 4 ].level, 1 );
    QVERIFY( !folds[ 2 ].header );
}

void TestMdFolding::hashInsideFenceIsNotAHeader()
{
    const std::vector< FoldLine > folds =
        computeMarkdownFoldLevels( "# 진짜\n\n```\n# 가짜\n```\n\n본문\n" );

    QCOMPARE( folds[ 0 ].level, 0 );
    QCOMPARE( folds[ 3 ].level, 1 );   // 펜스 안의 해시는 제목이 아니다
    QVERIFY( !folds[ 3 ].header );
}

void TestMdFolding::frontMatterIsNotAHeader()
{
    const std::vector< FoldLine > folds =
        computeMarkdownFoldLevels( "---\ntitle: 무엇\n---\n\n# 제목\n\n본문\n" );

    QCOMPARE( folds[ 0 ].level, 0 );
    QCOMPARE( folds[ 1 ].level, 0 );
    QCOMPARE( folds[ 2 ].level, 0 );
    QCOMPARE( folds[ 4 ].level, 0 );   // 제목은 front matter 뒤에서 시작한다
    QVERIFY( folds[ 4 ].header );
}

void TestMdFolding::blankLinesJoinTheDeeperNeighbour()
{
    const std::vector< FoldLine > folds =
        computeMarkdownFoldLevels( "# 가\n\n본문\n\n# 나\n\n본문\n" );

    // 섹션 끝의 빈 줄(3행)은 앞쪽 본문(깊이 1)에 붙어야 한다. 뒤쪽 제목(깊이 0)에
    // 붙으면 접었을 때 빈 줄만 덩그러니 남는다.
    QVERIFY( folds[ 3 ].blank );
    QCOMPARE( folds[ 3 ].level, 1 );
}

void TestMdFolding::lineCountMatchesDocument_data()
{
    QTest::addColumn< QByteArray >( "text" );
    QTest::addColumn< int >( "lines" );

    QTest::newRow( "빈 문서" ) << QByteArray( "" ) << 1;
    QTest::newRow( "제목만" ) << QByteArray( "# 하나\n" ) << 1;
    QTest::newRow( "끝에 개행 없음" ) << QByteArray( "# 하나\n본문" ) << 2;
    QTest::newRow( "CRLF" ) << QByteArray( "# 하나\r\n\r\n본문\r\n" ) << 3;
    QTest::newRow( "BOM" ) << QByteArray( "\xEF\xBB\xBF# 하나\n\n본문\n" ) << 3;
    QTest::newRow( "닫히지 않은 펜스" ) << QByteArray( "# 하나\n\n```\n본문\n" ) << 4;
}

void TestMdFolding::lineCountMatchesDocument()
{
    QFETCH( QByteArray, text );
    QFETCH( int, lines );

    const std::vector< FoldLine > folds =
        computeMarkdownFoldLevels( std::string( text.constData(), static_cast< std::size_t >( text.size() ) ) );
    QCOMPARE( static_cast< int >( folds.size() ), lines );
}

HC_RST_REGISTER_TEST( TestMdFolding );

#include "tstMdFolding.moc"
