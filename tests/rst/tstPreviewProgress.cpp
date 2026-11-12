#include "rstTestRunner.hpp"

#include "sphinxPreviewProgress.hpp"

#include <QTest>

using namespace hc::rst;

/// 프리뷰 진행막대가 **되돌아가지 않는지**, 그리고 남의 출력에 걸려 넘어지지
/// 않는지 지킨다.
///
/// 진행률은 사용자 프로젝트의 venv 에서 도는 Sphinx 옆에서 우리 빌더가 흘려보낸다.
/// 그 stdout 은 로그 패널로 그대로 가는 통로이기도 해서, 파싱이 느슨하면 남의
/// 출력을 진행률로 오독하고, 빡빡하면 진행률이 로그에 새어 나온다.
///
/// 단계가 넘어갈 때 막대가 뒤로 가는 것은 특히 눈에 거슬리는 부류다. 각 단계가
/// 0~100% 를 자기 몫으로 쓰면 "읽기 90%" 다음에 "쓰기 5%" 가 와서 막대가
/// 되돌아간다. 구간을 나눠 둔 이유가 그것이고, 여기서 그 성질을 못 박는다.
class PreviewProgressTest : public QObject
{
    Q_OBJECT

private slots:
    void readsTaggedLine();
    void ignoresUntaggedLine_data();
    void ignoresUntaggedLine();
    void ignoresBrokenPayload_data();
    void ignoresBrokenPayload();

    void phaseSpansDoNotOverlapOrGoBackwards();
    void unknownDenominatorGivesThePhaseStart();
    void doneBeyondTotalStaysInsideThePhase();
    void unknownPhaseTagLandsInTheBuild();
};

void PreviewProgressTest::readsTaggedLine()
{
    const PreviewBuildProgress progress = parsePreviewProgressLine(
            QStringLiteral( "%1{\"phase\":\"read\",\"done\":12,\"total\":57}" )
                    .arg( QLatin1String( kPreviewProgressTag ) ) );

    QVERIFY( progress.valid );
    QCOMPARE( progress.phase, QStringLiteral( "read" ) );
    QCOMPARE( progress.done, 12 );
    QCOMPARE( progress.total, 57 );
}

void PreviewProgressTest::ignoresUntaggedLine_data()
{
    QTest::addColumn< QString >( "line" );

    QTest::newRow( "empty" ) << QString();
    QTest::newRow( "sphinx status" ) << QStringLiteral( "reading sources... [ 42%] index" );
    QTest::newRow( "warning" ) << QStringLiteral( "index.rst:3: WARNING: title underline too short" );
    // 표식이 줄 가운데 있으면 우리 것이 아니다 — 남의 로그가 우리 줄을 인용한 것이다.
    QTest::newRow( "tag not at start" )
            << QStringLiteral( "note: %1{}" ).arg( QLatin1String( kPreviewProgressTag ) );
}

void PreviewProgressTest::ignoresUntaggedLine()
{
    QFETCH( QString, line );
    QVERIFY( !parsePreviewProgressLine( line ).valid );
}

void PreviewProgressTest::ignoresBrokenPayload_data()
{
    QTest::addColumn< QString >( "payload" );

    QTest::newRow( "not json" ) << QStringLiteral( "reading 42%" );
    QTest::newRow( "empty object" ) << QStringLiteral( "{}" );
    QTest::newRow( "truncated" ) << QStringLiteral( "{\"phase\":\"read\",\"done\":1" );
    QTest::newRow( "no phase" ) << QStringLiteral( "{\"done\":1,\"total\":2}" );
    QTest::newRow( "negative done" ) << QStringLiteral( "{\"phase\":\"read\",\"done\":-1,\"total\":2}" );
    QTest::newRow( "array" ) << QStringLiteral( "[1,2,3]" );
}

void PreviewProgressTest::ignoresBrokenPayload()
{
    QFETCH( QString, payload );
    QVERIFY( !parsePreviewProgressLine( QLatin1String( kPreviewProgressTag ) + payload ).valid );
}

void PreviewProgressTest::phaseSpansDoNotOverlapOrGoBackwards()
{
    // 프리뷰 한 판이 실제로 지나는 순서대로 훑는다. 진행도가 한 번이라도
    // 줄어들면 화면에서 막대가 되돌아간다.
    const QList< PreviewPhase > order{ PreviewPhase::Prepare, PreviewPhase::BuildRead,
                                       PreviewPhase::BuildWrite, PreviewPhase::Load };

    int previous = -1;
    for( const PreviewPhase phase : order )
    {
        for( int done = 0; done <= 10; ++done )
        {
            const int value = previewOverallPermille( phase, done, 10 );
            QVERIFY2( value >= previous,
                      qPrintable( QStringLiteral( "진행도가 %1 에서 %2 로 되돌아갔다" )
                                          .arg( previous )
                                          .arg( value ) ) );
            QVERIFY( value >= 0 && value <= 1000 );
            previous = value;
        }
    }

    // 마지막 단계를 끝까지 채우면 꽉 찬다. 그러지 않으면 다 끝났는데 막대가
    // 조금 남은 것처럼 보인다.
    QCOMPARE( previewOverallPermille( PreviewPhase::Load, 10, 10 ), 1000 );
}

void PreviewProgressTest::unknownDenominatorGivesThePhaseStart()
{
    // 분모를 모르는 것은 진행도를 모르는 것이 아니다 — 그 단계에 막 들어섰다는 뜻이다.
    const int readStart = previewOverallPermille( PreviewPhase::BuildRead, 0, 0 );
    QCOMPARE( previewOverallPermille( PreviewPhase::BuildRead, 5, 0 ), readStart );
    QCOMPARE( previewOverallPermille( PreviewPhase::BuildRead, 0, -3 ), readStart );
    QVERIFY( readStart > previewOverallPermille( PreviewPhase::Prepare, 0, 0 ) );
}

void PreviewProgressTest::doneBeyondTotalStaysInsideThePhase()
{
    // 빌더의 분모는 어림값이다. html-page-context 는 genindex·search 처럼 문서
    // 목록에 없는 페이지에도 오므로 done 이 total 을 넘는다. 넘긴 값이 다음 단계의
    // 구간을 먹으면 "쓰기" 가 끝나기도 전에 막대가 로딩 구간까지 차오른다.
    const int writeFull = previewOverallPermille( PreviewPhase::BuildWrite, 10, 10 );
    QCOMPARE( previewOverallPermille( PreviewPhase::BuildWrite, 25, 10 ), writeFull );
    QVERIFY( writeFull <= previewOverallPermille( PreviewPhase::Load, 0, 10 ) );
}

void PreviewProgressTest::unknownPhaseTagLandsInTheBuild()
{
    QCOMPARE( previewPhaseFromTag( QStringLiteral( "read" ) ), PreviewPhase::BuildRead );
    QCOMPARE( previewPhaseFromTag( QStringLiteral( "write" ) ), PreviewPhase::BuildWrite );
    // 모르는 값은 빌드 구간에 둔다. 빌드 중에 온 것이므로 그쪽이 진행도를
    // 되돌리지 않는 선택이다.
    QCOMPARE( previewPhaseFromTag( QStringLiteral( "resolving" ) ), PreviewPhase::BuildRead );
    QCOMPARE( previewPhaseFromTag( QString() ), PreviewPhase::BuildRead );
}

HC_RST_REGISTER_TEST( PreviewProgressTest );

#include "tstPreviewProgress.moc"
