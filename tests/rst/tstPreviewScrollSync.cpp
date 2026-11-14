#include "rstTestRunner.hpp"
#include "UniqueLibs/document/previewScrollSync.hpp"

#include <QElapsedTimer>
#include <QTest>

using hc::document::PreviewScrollSync;

/// 편집기 쪽 스크롤 동기화 규칙( Markdown · reST · EPUB 공통 ): 언제 미리보기의 보고를 따르고, 따른 뒤 언제 편집기
/// 위치를 다시 보내는가.
class TestPreviewScrollSync : public QObject
{
    Q_OBJECT
private slots:
    void followsOnlyTheUsersScroll();
    void reportRightAfterARequestIsNotFollowed();
    void editorWaitsAfterFollowing();
    void resetDropsBothGuards();
};

void TestPreviewScrollSync::followsOnlyTheUsersScroll()
{
    PreviewScrollSync sync;
    QVERIFY( sync.Follows( true ) );
    // 페이지가 스스로 움직인 것( 이미지 · 글꼴이 자리를 잡는 중 )은 편집기를 옮기지 않는다.
    QVERIFY( !sync.Follows( false ) );
    QCOMPARE( sync.SendDelayMs(), 0 );
}

void TestPreviewScrollSync::reportRightAfterARequestIsNotFollowed()
{
    PreviewScrollSync sync;
    sync.EditorSent();
    // 요청보다 먼저 떠난 보고일 수 있다. 따르면 편집기가 방금 떠난 자리로 되돌아간다.
    QVERIFY( !sync.Follows( true ) );
    // 보내는 쪽은 막지 않는다: 편집기를 계속 굴리면 요청이 이어진다.
    QCOMPARE( sync.SendDelayMs(), 0 );
    QTest::qWait( PreviewScrollSync::GuardMs + 40 );
    QVERIFY( sync.Follows( true ) );
}

void TestPreviewScrollSync::editorWaitsAfterFollowing()
{
    PreviewScrollSync sync;
    sync.EditorFollowed();
    // 따라 옮긴 편집기의 스크롤을 곧바로 돌려보내면 왕복이 된다. 남은 시간 뒤에 한 번 더 시도한다.
    const int delay = sync.SendDelayMs();
    QVERIFY2( delay > 0 && delay <= PreviewScrollSync::GuardMs, qPrintable( QString::number( delay ) ) );
    // 따르는 쪽은 막지 않는다: 미리보기를 계속 굴리면 보고가 이어진다.
    QVERIFY( sync.Follows( true ) );
    QTest::qWait( delay + 40 );
    QCOMPARE( sync.SendDelayMs(), 0 );
}

void TestPreviewScrollSync::resetDropsBothGuards()
{
    PreviewScrollSync sync;
    sync.EditorSent();
    sync.EditorFollowed();
    sync.Reset();
    QVERIFY( sync.Follows( true ) );
    QCOMPARE( sync.SendDelayMs(), 0 );
}

HC_RST_REGISTER_TEST( TestPreviewScrollSync );

#include "tstPreviewScrollSync.moc"
