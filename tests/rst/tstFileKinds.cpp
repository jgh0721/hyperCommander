#include "rstTestRunner.hpp"

#include "rstFileKinds.hpp"

#include <QTest>

using namespace hc::rst;

/// 확장자 목록은 앱 전체가 한 곳에서 읽는다(solFileKinds.hpp 의 머리 주석 참고).
/// 목록이 어긋나면 증상이 고약해서 — `_build/` 를 한 곳에서만 빼면 그 경로가
/// 자동완성 후보로 올라오고, 사용자가 그것을 고르면 다음 빌드에서 산출물이
/// 지워져 링크가 깨진다 — 목록 자체를 못으로 박아 둔다.
class TestFileKinds : public QObject
{
    Q_OBJECT

private slots:
    void restructuredTextIsRstAndRestOnly();
    void documentExtensionsCoverRstTxtAndMarkdown();
    void textLikeContainsEveryDocumentExtension();
    void hasExtensionIgnoresCase_data();
    void hasExtensionIgnoresCase();
};

void TestFileKinds::restructuredTextIsRstAndRestOnly()
{
    // `.txt` 가 여기 새어 들어오면 "실제 프로젝트가 없는 .md/.rst" 규칙
    // (WorkspaceController::activeDocumentIsStandalone) 이 평범한 메모까지
    // 문서로 세어 요약 패널의 탭을 흔든다.
    QCOMPARE( filekinds::restructuredTextExtensions(),
              QStringList( { QStringLiteral( "rst" ), QStringLiteral( "rest" ) } ) );
}

void TestFileKinds::documentExtensionsCoverRstTxtAndMarkdown()
{
    const QStringList documents = filekinds::documentExtensions();
    for( const QString& suffix : filekinds::restructuredTextExtensions() )
        QVERIFY2( documents.contains( suffix ), qPrintable( suffix ) );
    for( const QString& suffix : filekinds::markdownExtensions() )
        QVERIFY2( documents.contains( suffix ), qPrintable( suffix ) );
    // Sphinx 는 source_suffix 에 따라 `.txt` 도 문서로 읽는다. toctree / :doc:
    // 후보에서 빠지면 그런 프로젝트의 자동완성이 조용히 반쪽이 된다.
    QVERIFY( documents.contains( QStringLiteral( "txt" ) ) );
}

void TestFileKinds::textLikeContainsEveryDocumentExtension()
{
    const QStringList textLike = filekinds::textLikeExtensions();
    for( const QString& suffix : filekinds::documentExtensions() )
        QVERIFY2( textLike.contains( suffix ), qPrintable( suffix ) );
}

void TestFileKinds::hasExtensionIgnoresCase_data()
{
    QTest::addColumn< QString >( "path" );
    QTest::addColumn< bool >( "isReST" );
    QTest::newRow( "소문자" )       << QStringLiteral( "a/index.rst" )   << true;
    QTest::newRow( "대문자" )       << QStringLiteral( "a/INDEX.RST" )   << true;
    QTest::newRow( "rest" )         << QStringLiteral( "a/index.rest" )  << true;
    QTest::newRow( "markdown" )     << QStringLiteral( "a/readme.md" )   << false;
    QTest::newRow( "txt" )          << QStringLiteral( "a/notes.txt" )   << false;
    QTest::newRow( "확장자 없음" )  << QStringLiteral( "a/Makefile" )    << false;
    // 디렉터리 이름의 점에 걸리지 않아야 한다. 워크스페이스에는 `본편.rst/` 처럼
    // 확장자를 닮은 폴더가 실제로 있다.
    QTest::newRow( "폴더 이름의 점" ) << QStringLiteral( "a.rst/notes.md" ) << false;
}

void TestFileKinds::hasExtensionIgnoresCase()
{
    QFETCH( QString, path );
    QFETCH( bool, isReST );
    QCOMPARE( filekinds::hasExtension( path, filekinds::restructuredTextExtensions() ), isReST );
}

HC_RST_REGISTER_TEST( TestFileKinds );

#include "tstFileKinds.moc"
