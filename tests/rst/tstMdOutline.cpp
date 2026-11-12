#include "rstTestRunner.hpp"

#include "rstOutlineService.hpp"

#include <QTest>

using namespace hc::rst;

namespace {

/// 제목 하나만 있는 개요에서 그 항목을 꺼낸다.
const OutlineSymbol& only( const QVector< OutlineSymbol >& roots )
{
    Q_ASSERT( roots.size() == 1 );
    return roots.first();
}

}  // namespace

/// Markdown 개요 파서. reST 쪽과 같은 자리(Esbonio 가 데워지기 전)를 채운다.
///
/// 계층을 잘못 잡으면 트리가 통째로 어긋나고, 코드펜스·front matter 판정을 놓치면
/// 문서에 없는 제목이 올라온다. 그 둘에 집중한다.
class TestMdOutline : public QObject
{
    Q_OBJECT

private slots:
    void emptyTextHasNoSymbols();
    void findsAtxHeadings();
    /// md 는 해시 개수가 곧 단계다. reST 의 "처음 나온 순서" 규칙을 옮겨 오면 안 된다.
    void atxLevelIsHashCount();
    void skippedLevelNestsUnderNearestAncestor();
    void deeperFirstThenShallowerStartsNewRoot();
    void siblingsAtSameLevelStayFlat();
    void findsSetextHeadings();
    void setextLineNumberPointsAtTitleNotUnderline();
    void thematicBreakIsNotSetext();
    void frontMatterIsSkipped();
    void unclosedFrontMatterIsThematicBreak();
    void fencedCodeHidesHashes();
    void unclosedFenceSwallowsRestOfFile();
    void mismatchedFenceCharDoesNotClose();
    void shorterClosingFenceDoesNotClose();
    void atxRequiresSpaceAfterHash();
    void emptyAtxHeadingIsDropped();
    void closingHashSequenceIsStripped();
    void indentedFourSpacesIsNotHeading();
    void tabIndentedIsNotHeading();
    void blockquotedHeadingIsSkipped();
    void handlesCrlfAndBom();
    void reportsOneBasedTitleLine();
    void detailIsMarkdown();
    void dispatcherPicksParserByExtension();
};

void TestMdOutline::emptyTextHasNoSymbols()
{
    QVERIFY( parseMarkdownOutline( QString() ).isEmpty() );
    QVERIFY( parseMarkdownOutline( QStringLiteral( "제목 없는 본문\n" ) ).isEmpty() );
}

void TestMdOutline::findsAtxHeadings()
{
    const QVector< OutlineSymbol > roots =
        parseMarkdownOutline( QStringLiteral( "# 하나\n\n본문\n\n## 둘\n" ) );

    QCOMPARE( roots.size(), 1 );
    QCOMPARE( roots.first().name, QStringLiteral( "하나" ) );
    QCOMPARE( roots.first().children.size(), 1 );
    QCOMPARE( roots.first().children.first().name, QStringLiteral( "둘" ) );
}

void TestMdOutline::atxLevelIsHashCount()
{
    // reST 라면 처음 나온 '###' 이 1단계가 되지만, md 는 3단계다.
    const QVector< OutlineSymbol > roots =
        parseMarkdownOutline( QStringLiteral( "### 셋\n\n# 하나\n\n## 둘\n" ) );

    QCOMPARE( roots.size(), 2 );                       // ### 과 # 이 각각 루트
    QCOMPARE( roots.at( 0 ).name, QStringLiteral( "셋" ) );
    QCOMPARE( roots.at( 1 ).name, QStringLiteral( "하나" ) );
    QCOMPARE( roots.at( 1 ).children.size(), 1 );      // ## 는 # 아래
}

void TestMdOutline::skippedLevelNestsUnderNearestAncestor()
{
    const QVector< OutlineSymbol > roots =
        parseMarkdownOutline( QStringLiteral( "# 하나\n\n### 셋\n" ) );

    QCOMPARE( roots.size(), 1 );
    QCOMPARE( roots.first().children.size(), 1 );
    QCOMPARE( roots.first().children.first().name, QStringLiteral( "셋" ) );
}

void TestMdOutline::deeperFirstThenShallowerStartsNewRoot()
{
    const QVector< OutlineSymbol > roots =
        parseMarkdownOutline( QStringLiteral( "### 셋\n\n# 하나\n" ) );

    QCOMPARE( roots.size(), 2 );
    QVERIFY( roots.at( 0 ).children.isEmpty() );
}

void TestMdOutline::siblingsAtSameLevelStayFlat()
{
    const QVector< OutlineSymbol > roots =
        parseMarkdownOutline( QStringLiteral( "## 가\n\n## 나\n\n## 다\n" ) );

    QCOMPARE( roots.size(), 3 );
    for( const OutlineSymbol& symbol : roots )
        QVERIFY( symbol.children.isEmpty() );
}

void TestMdOutline::findsSetextHeadings()
{
    const QVector< OutlineSymbol > roots =
        parseMarkdownOutline( QStringLiteral( "제목\n====\n\n본문\n\n버금\n----\n" ) );

    QCOMPARE( roots.size(), 1 );
    QCOMPARE( roots.first().name, QStringLiteral( "제목" ) );
    QCOMPARE( roots.first().children.size(), 1 );      // '---' 는 2단계
    QCOMPARE( roots.first().children.first().name, QStringLiteral( "버금" ) );
}

void TestMdOutline::setextLineNumberPointsAtTitleNotUnderline()
{
    // 프리뷰는 제목 글자 줄에 앵커를 붙인다. 밑줄 줄을 가리키면 개요를 눌렀을 때
    // 한 줄 아래로 간다. reST 쪽과 의도적으로 다른 지점이다.
    const QVector< OutlineSymbol > roots =
        parseMarkdownOutline( QStringLiteral( "머리말\n\n제목\n====\n" ) );

    QCOMPARE( only( roots ).line, 3 );
}

void TestMdOutline::thematicBreakIsNotSetext()
{
    // 빈 줄 뒤의 '---' 는 수평선이다.
    QVERIFY( parseMarkdownOutline( QStringLiteral( "본문\n\n---\n" ) ).isEmpty() );
    // '***' 와 '___' 는 문단 바로 뒤에 와도 setext 가 아니다.
    QVERIFY( parseMarkdownOutline( QStringLiteral( "본문\n***\n" ) ).isEmpty() );
    QVERIFY( parseMarkdownOutline( QStringLiteral( "본문\n___\n" ) ).isEmpty() );
}

void TestMdOutline::frontMatterIsSkipped()
{
    const QVector< OutlineSymbol > roots = parseMarkdownOutline(
        QStringLiteral( "---\ntitle: 무엇\ntags: [가, 나]\n---\n\n# 진짜 제목\n" ) );

    QCOMPARE( roots.size(), 1 );
    QCOMPARE( only( roots ).name, QStringLiteral( "진짜 제목" ) );
    QCOMPARE( only( roots ).line, 6 );   // front matter 의 줄 수가 보존되어야 한다
}

void TestMdOutline::unclosedFrontMatterIsThematicBreak()
{
    // 닫히지 않은 '---' 를 front matter 로 보면 문서 전체가 사라진다.
    const QVector< OutlineSymbol > roots =
        parseMarkdownOutline( QStringLiteral( "---\n\n# 제목\n" ) );

    QCOMPARE( roots.size(), 1 );
    QCOMPARE( only( roots ).name, QStringLiteral( "제목" ) );
}

void TestMdOutline::fencedCodeHidesHashes()
{
    for( const QString& fence : { QStringLiteral( "```" ), QStringLiteral( "~~~" ) } )
    {
        const QVector< OutlineSymbol > roots = parseMarkdownOutline(
            QStringLiteral( "# 진짜\n\n%1\n# 가짜\n%1\n" ).arg( fence ) );

        QCOMPARE( roots.size(), 1 );
        QVERIFY2( roots.first().children.isEmpty(), qPrintable( fence ) );
    }
}

void TestMdOutline::unclosedFenceSwallowsRestOfFile()
{
    const QVector< OutlineSymbol > roots =
        parseMarkdownOutline( QStringLiteral( "# 진짜\n\n```\n# 가짜\n\n## 이것도 가짜\n" ) );

    QCOMPARE( roots.size(), 1 );
    QVERIFY( roots.first().children.isEmpty() );
}

void TestMdOutline::mismatchedFenceCharDoesNotClose()
{
    // 틸드로 열고 백틱으로 닫을 수 없다.
    const QVector< OutlineSymbol > roots =
        parseMarkdownOutline( QStringLiteral( "~~~\n```\n# 가짜\n" ) );

    QVERIFY( roots.isEmpty() );
}

void TestMdOutline::shorterClosingFenceDoesNotClose()
{
    const QVector< OutlineSymbol > roots =
        parseMarkdownOutline( QStringLiteral( "````\n```\n# 가짜\n" ) );

    QVERIFY( roots.isEmpty() );
}

void TestMdOutline::atxRequiresSpaceAfterHash()
{
    // LexMarkdown 은 이것을 제목으로 칠하지만 CommonMark 와 프리뷰는 아니다.
    // 개요는 프리뷰를 따른다 — 그래야 항목을 눌렀을 때 같은 곳으로 간다.
    QVERIFY( parseMarkdownOutline( QStringLiteral( "#제목\n" ) ).isEmpty() );
    QVERIFY( parseMarkdownOutline( QStringLiteral( "#hashtag\n" ) ).isEmpty() );
}

void TestMdOutline::emptyAtxHeadingIsDropped()
{
    QVERIFY( parseMarkdownOutline( QStringLiteral( "#\n" ) ).isEmpty() );
    QVERIFY( parseMarkdownOutline( QStringLiteral( "###   \n" ) ).isEmpty() );
    QVERIFY( parseMarkdownOutline( QStringLiteral( "## ##\n" ) ).isEmpty() );
}

void TestMdOutline::closingHashSequenceIsStripped()
{
    QCOMPARE( only( parseMarkdownOutline( QStringLiteral( "## 제목 ##\n" ) ) ).name,
              QStringLiteral( "제목" ) );
    // 앞이 공백이 아니면 제목 글자의 일부다.
    QCOMPARE( only( parseMarkdownOutline( QStringLiteral( "## 제목#\n" ) ) ).name,
              QStringLiteral( "제목#" ) );
}

void TestMdOutline::indentedFourSpacesIsNotHeading()
{
    QVERIFY( parseMarkdownOutline( QStringLiteral( "    # 코드블록\n" ) ).isEmpty() );
    // 세 칸까지는 제목이다.
    QCOMPARE( only( parseMarkdownOutline( QStringLiteral( "   # 제목\n" ) ) ).name,
              QStringLiteral( "제목" ) );
}

void TestMdOutline::tabIndentedIsNotHeading()
{
    QVERIFY( parseMarkdownOutline( QStringLiteral( "\t# 코드블록\n" ) ).isEmpty() );
}

void TestMdOutline::blockquotedHeadingIsSkipped()
{
    QVERIFY( parseMarkdownOutline( QStringLiteral( "> # 인용 안 제목\n" ) ).isEmpty() );
}

void TestMdOutline::handlesCrlfAndBom()
{
    const QVector< OutlineSymbol > crlf =
        parseMarkdownOutline( QStringLiteral( "# 하나\r\n\r\n## 둘\r\n" ) );
    QCOMPARE( crlf.size(), 1 );
    QCOMPARE( crlf.first().children.size(), 1 );

    // BOM 을 남기면 해시가 0열이 아니게 되어 첫 제목만 조용히 사라진다.
    const QVector< OutlineSymbol > bom =
        parseMarkdownOutline( QStringLiteral( "﻿# 하나\n" ) );
    QCOMPARE( bom.size(), 1 );
    QCOMPARE( bom.first().name, QStringLiteral( "하나" ) );
}

void TestMdOutline::reportsOneBasedTitleLine()
{
    const QVector< OutlineSymbol > roots =
        parseMarkdownOutline( QStringLiteral( "머리말\n\n# 제목\n" ) );

    QCOMPARE( only( roots ).line, 3 );
}

void TestMdOutline::detailIsMarkdown()
{
    const QVector< OutlineSymbol > roots =
        parseMarkdownOutline( QStringLiteral( "# 제목\n" ), QStringLiteral( "/tmp/a.md" ) );

    QCOMPARE( only( roots ).detail, QStringLiteral( "Markdown" ) );
    QCOMPARE( only( roots ).path, QStringLiteral( "/tmp/a.md" ) );
}

void TestMdOutline::dispatcherPicksParserByExtension()
{
    // 같은 글에 두 마크업의 제목을 함께 담아 어느 파서가 돌았는지 구분한다.
    const QString mixed = QStringLiteral( "# 마크다운\n\nreST 제목\n=========\n" );

    const QVector< OutlineSymbol > asMarkdown =
        parseDocumentOutline( mixed, QStringLiteral( "/tmp/a.md" ) );
    QCOMPARE( asMarkdown.first().detail, QStringLiteral( "Markdown" ) );

    for( const QString& path : { QStringLiteral( "/tmp/a.markdown" ), QStringLiteral( "/tmp/a.mdown" ),
                                 QStringLiteral( "/tmp/A.MD" ) } )
    {
        QVERIFY2( parseDocumentOutline( mixed, path ).first().detail
                      == QStringLiteral( "Markdown" ),
                  qPrintable( path ) );
    }

    const QVector< OutlineSymbol > asRst =
        parseDocumentOutline( mixed, QStringLiteral( "/tmp/a.rst" ) );
    QCOMPARE( asRst.first().detail, QStringLiteral( "reST" ) );

    // 경로가 비면 reST 로 본다.
    QCOMPARE( parseDocumentOutline( mixed, QString() ).first().detail,
              QStringLiteral( "reST" ) );
}

HC_RST_REGISTER_TEST( TestMdOutline );

#include "tstMdOutline.moc"
