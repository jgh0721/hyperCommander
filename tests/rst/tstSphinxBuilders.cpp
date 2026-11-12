#include "rstTestRunner.hpp"

#include "sphinxBuilders.hpp"

#include <QDir>
#include <QTest>

using namespace hc::rst;

/// 빌드 대화상자의 값이 그대로 명령행이 된다. 이름 검사가 느슨하면 편집 가능한
/// 콤보가 인자 주입 통로가 되고, make 모드 판정이 틀리면 사용자가 가장 먼저
/// 고르는 latexpdf 가 "Builder name not registered" 로 죽는다.
class TestSphinxBuilders : public QObject
{
    Q_OBJECT

private slots:
    void presetsStartWithHtml();
    void acceptsRealBuilderNames_data();
    void acceptsRealBuilderNames();
    void rejectsNamesThatCouldBeArguments_data();
    void rejectsNamesThatCouldBeArguments();
    void latexpdfIsMakeModeAndLandsInLatex();
    void infoIsMakeModeAndLandsInTexinfo();
    void plainBuildersAreNotMakeMode();
    void defaultOutputFollowsQuickstartLayout();
    void defaultOutputFallsBackToHtml();
    void defaultOutputIsEmptyWithoutRoot();
};

void TestSphinxBuilders::presetsStartWithHtml()
{
    const QStringList presets = sphinxBuilderPresets();
    QVERIFY( !presets.isEmpty() );
    // 첫 항목이 콤보의 기본 선택이다.
    QCOMPARE( presets.first(), QStringLiteral( "html" ) );
    QVERIFY( presets.contains( QStringLiteral( "latexpdf" ) ) );
}

void TestSphinxBuilders::acceptsRealBuilderNames_data()
{
    QTest::addColumn< QString >( "name" );
    QTest::newRow( "html" ) << "html";
    QTest::newRow( "dirhtml" ) << "dirhtml";
    QTest::newRow( "singlehtml" ) << "singlehtml";
    QTest::newRow( "하이픈" ) << "some-builder";
    QTest::newRow( "밑줄" ) << "some_builder";
    QTest::newRow( "앞뒤 공백" ) << "  html  ";
}

void TestSphinxBuilders::acceptsRealBuilderNames()
{
    QFETCH( QString, name );
    QVERIFY( isValidSphinxBuilderName( name ) );
}

void TestSphinxBuilders::rejectsNamesThatCouldBeArguments_data()
{
    QTest::addColumn< QString >( "name" );
    QTest::newRow( "빈 문자열" ) << "";
    QTest::newRow( "공백만" ) << "   ";
    QTest::newRow( "가운데 공백" ) << "html -D foo=1";
    QTest::newRow( "옵션처럼 시작" ) << "-D";
    QTest::newRow( "경로 구분자" ) << "../html";
    QTest::newRow( "세미콜론" ) << "html;rm";
}

void TestSphinxBuilders::rejectsNamesThatCouldBeArguments()
{
    QFETCH( QString, name );
    QVERIFY( !isValidSphinxBuilderName( name ) );
}

void TestSphinxBuilders::latexpdfIsMakeModeAndLandsInLatex()
{
    QVERIFY( isSphinxMakeModeTarget( QStringLiteral( "latexpdf" ) ) );
    // 목표 이름과 하위 폴더 이름이 다른 것이 요점이다. 빌드가 끝난 뒤 탐색기로
    // 열어 줄 자리가 이 값에 달려 있다.
    QCOMPARE( sphinxMakeModeSubdirectory( QStringLiteral( "latexpdf" ) ),
             QStringLiteral( "latex" ) );
    QCOMPARE( sphinxMakeModeSubdirectory( QStringLiteral( "LatexPdf" ) ),
             QStringLiteral( "latex" ) );
}

void TestSphinxBuilders::infoIsMakeModeAndLandsInTexinfo()
{
    QVERIFY( isSphinxMakeModeTarget( QStringLiteral( "info" ) ) );
    QCOMPARE( sphinxMakeModeSubdirectory( QStringLiteral( "info" ) ),
             QStringLiteral( "texinfo" ) );
}

void TestSphinxBuilders::plainBuildersAreNotMakeMode()
{
    for( const QString& name : { QStringLiteral( "html" ), QStringLiteral( "dirhtml" ),
                                QStringLiteral( "latex" ), QStringLiteral( "epub" ),
                                QStringLiteral( "text" ), QStringLiteral( "man" ) } )
    {
        QVERIFY2( !isSphinxMakeModeTarget( name ), qPrintable( name ) );
        QVERIFY2( sphinxMakeModeSubdirectory( name ).isEmpty(), qPrintable( name ) );
    }
}

void TestSphinxBuilders::defaultOutputFollowsQuickstartLayout()
{
    const QString output = defaultSphinxOutputDirectory( QStringLiteral( "/w/docs" ),
                                                        QStringLiteral( "dirhtml" ) );
    QCOMPARE( QDir::fromNativeSeparators( output ), QStringLiteral( "/w/docs/_build/dirhtml" ) );
}

void TestSphinxBuilders::defaultOutputFallsBackToHtml()
{
    const QString output = defaultSphinxOutputDirectory( QStringLiteral( "/w/docs" ), QString{} );
    QCOMPARE( QDir::fromNativeSeparators( output ), QStringLiteral( "/w/docs/_build/html" ) );
}

void TestSphinxBuilders::defaultOutputIsEmptyWithoutRoot()
{
    // 프로젝트를 못 찾았을 때 상대 경로를 만들어 내면 엉뚱한 곳에 쓴다.
    QVERIFY( defaultSphinxOutputDirectory( QString{}, QStringLiteral( "html" ) ).isEmpty() );
}

HC_RST_REGISTER_TEST( TestSphinxBuilders );

#include "tstSphinxBuilders.moc"
