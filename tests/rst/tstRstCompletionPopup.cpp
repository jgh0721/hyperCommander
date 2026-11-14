#include "rstTestRunner.hpp"

#include "rstCompletionCoordinator.hpp"
#include "rstCompletionDetailPopup.hpp"
#include "rstCompletionPopup.hpp"
#include "rstEditor.hpp"
#include "text/ITextEditorBackend.h"
#include "text/QTextView.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QKeyEvent>
#include <QLineEdit>
#include <QListWidget>
#include <QScreen>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QVBoxLayout>

using namespace hc::rst;

/// reST 자동완성 팝업의 지금 동작을 고정한다(SQLite 설계 261113 10.1). 팝업을 SQLite 쿼리 편집기와 함께 쓰도록
/// 공용 자리로 옮기기 전에 쓰고, 옮긴 뒤에도 그대로 통과해야 한다.
///
/// * 편집기 포커스를 가져가지 않는다.
/// * 입력(접두)에 따라 거르고, 남는 것이 없으면 숨는다. 같은 접두로 다시 채우면 고른 항목을 지킨다.
/// * 위 · 아래는 돌고, PgUp · PgDn 은 끝에서 멈춘다.
/// * Enter · Tab 은 확정(itemSelected), Esc 는 닫기. 확정 · 닫기 모두 popupHidden 을 낸다.
/// * 상세 창은 목록 옆에 붙고, 화면 가장자리에서는 목록이 안쪽 · 위로 옮겨 간다.
/// * reST 편집기에서 디렉티브 · 역할 · 경로 완성을 끝까지 해 본다(삽입 결과).
class TestRstCompletionPopup : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();
    void neverTakesEditorFocus();
    void filtersByPrefixAndHidesWhenEmpty();
    void keepsSelectionWhenSamePrefixRefills();
    void arrowsWrapPagesClamp();
    void enterTabAcceptEscCloses();
    void detailPanelSitsBesideList();
    void screenEdgeMovesListInside();
    void directiveRoleAndPathEndToEnd();

private:
    static QList< CompletionDisplayItem > items( const QStringList& labels );
    static bool press( CompletionPopupWidget& popup, int key );

    QTemporaryDir config_;
    QVariant previousConfig_;
};

QList< CompletionDisplayItem > TestRstCompletionPopup::items( const QStringList& labels )
{
    QList< CompletionDisplayItem > result;
    for( const QString& label : labels )
    {
        CompletionDisplayItem item;
        item.label = label;
        item.insertText = label + QStringLiteral( "::" );
        item.detail = QStringLiteral( "directive" );
        item.kind = 7;
        result.append( item );
    }
    return result;
}

bool TestRstCompletionPopup::press( CompletionPopupWidget& popup, const int key )
{
    QKeyEvent event( QEvent::KeyPress, key, Qt::NoModifier );
    return popup.handleKeyPress( &event );
}

void TestRstCompletionPopup::initTestCase()
{
    // 편집기는 설정 파일에서 글꼴 · 눈금자를 읽는다. 사용자 프로필 대신 빈 임시 폴더를 쓴다.
    previousConfig_ = qApp->property( "documentViewsConfigDir" );
    qApp->setProperty( "documentViewsConfigDir", config_.path() );
}

void TestRstCompletionPopup::cleanupTestCase()
{
    qApp->setProperty( "documentViewsConfigDir", previousConfig_ );
}

void TestRstCompletionPopup::neverTakesEditorFocus()
{
    QWidget window;
    auto* layout = new QVBoxLayout( &window );
    auto* editor = new QLineEdit( &window );
    layout->addWidget( editor );
    window.resize( 400, 200 );
    window.show();
    window.activateWindow();
    QVERIFY( QTest::qWaitForWindowActive( &window ) );
    editor->setFocus();
    QTRY_COMPARE( QApplication::focusWidget(), static_cast< QWidget* >( editor ) );

    CompletionPopupWidget popup( &window );
    QCOMPARE( popup.focusPolicy(), Qt::NoFocus );
    QVERIFY( popup.testAttribute( Qt::WA_ShowWithoutActivating ) );
    QVERIFY( popup.windowFlags().testFlag( Qt::Tool ) );
    popup.setItems( items( { QStringLiteral( "image" ), QStringLiteral( "include" ) } ) );
    popup.showAt( editor->mapToGlobal( QPoint( 0, editor->height() ) ) );
    QVERIFY( popup.isActive() );
    QCoreApplication::processEvents();
    // offscreen 플랫폼은 WA_ShowWithoutActivating 을 따르지 않고 보이는 창을 모두 활성화한다. 실제 창에서만 본다.
    if( QGuiApplication::platformName() != QLatin1String( "offscreen" ) )
        QCOMPARE( QApplication::focusWidget(), static_cast< QWidget* >( editor ) );
}

void TestRstCompletionPopup::filtersByPrefixAndHidesWhenEmpty()
{
    QWidget window;
    window.show();
    CompletionPopupWidget popup( &window );
    popup.setItems( items( { QStringLiteral( "image" ), QStringLiteral( "include" ), QStringLiteral( "index" ),
                             QStringLiteral( "code-block" ) } ) );
    popup.showAt( window.mapToGlobal( QPoint( 10, 10 ) ) );
    QCOMPARE( popup.visibleCount(), 4 );
    // 접두가 없으면 공급자가 준 순서 그대로.
    QCOMPARE( popup.currentItem().label, QStringLiteral( "image" ) );

    popup.updateFilter( QStringLiteral( "in" ) );
    QCOMPARE( popup.visibleCount(), 2 );
    popup.updateFilter( QStringLiteral( "inc" ) );
    QCOMPARE( popup.visibleCount(), 1 );
    QCOMPARE( popup.currentItem().label, QStringLiteral( "include" ) );
    QCOMPARE( popup.currentItem().insertText, QStringLiteral( "include::" ) );
    // 흩어진 글자도 찾되, 붙어 있는 쪽이 앞선다.
    popup.updateFilter( QStringLiteral( "cb" ) );
    QCOMPARE( popup.visibleCount(), 1 );
    QCOMPARE( popup.currentItem().label, QStringLiteral( "code-block" ) );

    QSignalSpy hidden( &popup, &CompletionPopupWidget::popupHidden );
    popup.updateFilter( QStringLiteral( "zzz" ) );
    QCOMPARE( popup.visibleCount(), 0 );
    QVERIFY( !popup.isVisible() );
    QVERIFY( !popup.isActive() );
    QCOMPARE( hidden.count(), 1 );
}

void TestRstCompletionPopup::keepsSelectionWhenSamePrefixRefills()
{
    QWidget window;
    window.show();
    CompletionPopupWidget popup( &window );
    popup.setItems( items( { QStringLiteral( "image" ), QStringLiteral( "include" ), QStringLiteral( "index" ) } ) );
    popup.showAt( window.mapToGlobal( QPoint( 10, 10 ) ) );
    popup.updateFilter( QStringLiteral( "i" ) );
    popup.selectNext();
    const QString chosen = popup.currentItem().label;
    QSignalSpy changed( &popup, &CompletionPopupWidget::currentItemChanged );
    popup.updateFilter( QStringLiteral( "i" ) );
    QCOMPARE( popup.currentItem().label, chosen );
    QCOMPARE( changed.count(), 0 );
}

void TestRstCompletionPopup::arrowsWrapPagesClamp()
{
    QWidget window;
    window.show();
    CompletionPopupWidget popup( &window );
    QStringList labels;
    for( int index = 0; index < 30; ++index )
        labels << QStringLiteral( "item%1" ).arg( index, 2, 10, QLatin1Char( '0' ) );
    popup.setItems( items( labels ) );
    popup.showAt( window.mapToGlobal( QPoint( 10, 10 ) ) );
    QCOMPARE( popup.currentItem().label, QStringLiteral( "item00" ) );

    QVERIFY( press( popup, Qt::Key_Up ) );
    QCOMPARE( popup.currentItem().label, QStringLiteral( "item29" ) );
    QVERIFY( press( popup, Qt::Key_Down ) );
    QCOMPARE( popup.currentItem().label, QStringLiteral( "item00" ) );
    QVERIFY( press( popup, Qt::Key_Down ) );
    QCOMPARE( popup.currentItem().label, QStringLiteral( "item01" ) );

    // 한 화면씩 내려가다 끝에서 멈춘다(돌지 않는다).
    QVERIFY( press( popup, Qt::Key_PageDown ) );
    QVERIFY( popup.currentItem().label > QStringLiteral( "item01" ) );
    for( int step = 0; step < 5; ++step )
        QVERIFY( press( popup, Qt::Key_PageDown ) );
    QCOMPARE( popup.currentItem().label, QStringLiteral( "item29" ) );
    for( int step = 0; step < 5; ++step )
        QVERIFY( press( popup, Qt::Key_PageUp ) );
    QCOMPARE( popup.currentItem().label, QStringLiteral( "item00" ) );

    // 처리하지 않는 키는 편집기로 보낸다.
    QVERIFY( !press( popup, Qt::Key_A ) );
    QVERIFY( !press( popup, Qt::Key_Left ) );
}

void TestRstCompletionPopup::enterTabAcceptEscCloses()
{
    QWidget window;
    window.show();
    CompletionPopupWidget popup( &window );
    QSignalSpy selected( &popup, &CompletionPopupWidget::itemSelected );
    QSignalSpy hidden( &popup, &CompletionPopupWidget::popupHidden );

    for( const int key : { int( Qt::Key_Return ), int( Qt::Key_Enter ), int( Qt::Key_Tab ) } )
    {
        popup.setItems( items( { QStringLiteral( "image" ), QStringLiteral( "include" ) } ) );
        popup.showAt( window.mapToGlobal( QPoint( 10, 10 ) ) );
        popup.selectNext();
        QVERIFY( press( popup, key ) );
        QVERIFY( !popup.isVisible() );
        QCOMPARE( selected.count(), 1 );
        QCOMPARE( selected.takeFirst().at( 0 ).toString(), QStringLiteral( "include::" ) );
    }
    QCOMPARE( hidden.count(), 3 );

    // insertText 가 비면 label 을 넣는다.
    CompletionDisplayItem plain;
    plain.label = QStringLiteral( "note" );
    popup.setItems( { plain } );
    popup.showAt( window.mapToGlobal( QPoint( 10, 10 ) ) );
    QVERIFY( press( popup, Qt::Key_Return ) );
    QCOMPARE( selected.takeFirst().at( 0 ).toString(), QStringLiteral( "note" ) );

    popup.setItems( items( { QStringLiteral( "image" ) } ) );
    popup.showAt( window.mapToGlobal( QPoint( 10, 10 ) ) );
    QVERIFY( press( popup, Qt::Key_Escape ) );
    QVERIFY( !popup.isVisible() );
    QCOMPARE( selected.count(), 0 );
    // 숨은 팝업은 키를 받지 않는다.
    QVERIFY( !press( popup, Qt::Key_Return ) );
    QVERIFY( !press( popup, Qt::Key_Escape ) );
}

void TestRstCompletionPopup::detailPanelSitsBesideList()
{
    QWidget window;
    window.resize( 300, 200 );
    window.move( 20, 20 );
    window.show();
    CompletionPopupWidget popup( &window );
    popup.setItems( items( { QStringLiteral( "image" ), QStringLiteral( "include" ) } ) );
    popup.showAt( window.mapToGlobal( QPoint( 10, 10 ) ) );

    CompletionDetailPopup detail( &window );
    QVERIFY( !detail.setContent( QString{}, QString{} ) );
    QVERIFY( !detail.hasContent() );
    QVERIFY( detail.setContent( QStringLiteral( "image" ), QStringLiteral( "Embed a picture.\nOptions: alt, width." ),
                                QStringLiteral( "docutils" ) ) );
    QVERIFY( detail.hasContent() );
    QCOMPARE( detail.focusPolicy(), Qt::NoFocus );
    detail.showBesideAnchor( popup.geometry() );
    QVERIFY( detail.isVisible() );
    const QRect list = popup.geometry();
    const QRect panel = detail.geometry();
    // 오른쪽에 자리가 있으면 목록 오른쪽 바깥, 위쪽을 맞춘다.
    QVERIFY2( panel.left() >= list.right(), qPrintable( QStringLiteral( "list %1,%2 %3x%4 panel %5,%6" )
                                                           .arg( list.x() ).arg( list.y() ).arg( list.width() ).arg( list.height() )
                                                           .arg( panel.x() ).arg( panel.y() ) ) );
    QVERIFY( qAbs( panel.top() - list.top() ) <= 2 );
    QVERIFY( !panel.intersects( list.adjusted( 1, 1, -1, -1 ) ) );
}

void TestRstCompletionPopup::screenEdgeMovesListInside()
{
    QWidget window;
    window.show();
    CompletionPopupWidget popup( &window );
    popup.setItems( items( { QStringLiteral( "image" ), QStringLiteral( "include" ), QStringLiteral( "index" ) } ) );
    const QRect available = QGuiApplication::primaryScreen()->availableGeometry();
    const QPoint corner( available.right() - 5, available.bottom() - 5 );
    popup.showAt( corner );
    const QRect placed = popup.geometry();
    QVERIFY( placed.right() <= available.right() );
    QVERIFY( placed.bottom() <= available.bottom() );
    // 아래로 넘치면 캐럿 위로 올린다.
    QVERIFY( placed.bottom() < corner.y() );
}

void TestRstCompletionPopup::directiveRoleAndPathEndToEnd()
{
    QTemporaryDir folder;
    QDir( folder.path() ).mkpath( QStringLiteral( "images" ) );
    {
        QFile picture( folder.filePath( QStringLiteral( "images/logo.png" ) ) );
        QVERIFY( picture.open( QIODevice::WriteOnly ) );
        picture.write( QByteArray::fromHex( "89504E470D0A1A0A" ) );
    }
    const QString path = folder.filePath( QStringLiteral( "index.rst" ) );

    QWidget window;
    auto* layout = new QVBoxLayout( &window );
    auto* view = new QTextView( &window );
    layout->addWidget( view );
    window.resize( 800, 500 );
    view->LoadContent( QStringLiteral( "Title\n=====\n\n.. ima\n" ), path, QStringLiteral( "UTF-8" ) );
    window.show();
    window.activateWindow();
    QVERIFY( QTest::qWaitForWindowActive( &window ) );
    RstEditor editor( view );
    QVERIFY( editor.widget() != nullptr );
    editor.setFilePath( path );
    editor.widget()->setFocus();

    CompletionCoordinator coordinator;
    coordinator.attachEditor( &editor );
    coordinator.setActiveEditor( &editor );
    coordinator.setActiveProject( QStringLiteral( "test" ), folder.path(), folder.path() );

    const auto finish = [ & ]( const int line, const int column ) {
        editor.goToLine( line, column );
        editor.widget()->setFocus();
        coordinator.requestExplicit();
    };
    const auto lineAt = [ & ]( const int line ) { return editor.lineText( line ).trimmed(); };

    // 디렉티브: ".. ima" → image, Enter.
    finish( 4, 7 );
    QTRY_VERIFY( coordinator.isPopupVisible() );
    QCOMPARE( QApplication::focusWidget(), editor.widget() );
    QTest::keyClick( editor.widget(), Qt::Key_Return );
    QTRY_VERIFY( !coordinator.isPopupVisible() );
    QVERIFY2( lineAt( 4 ).startsWith( QStringLiteral( ".. image::" ) ), qPrintable( lineAt( 4 ) ) );

    // 경로: ".. image:: images/" → logo.png, Tab.
    view->Backend()->setText( QStringLiteral( "Title\n=====\n\n.. image:: images/\n" ) );
    finish( 4, 19 );
    QTRY_VERIFY( coordinator.isPopupVisible() );
    // 하위 폴더 안에서는 상위 폴더(..)가 먼저, 그다음 파일.
    QStringList labels;
    for( auto* list : window.findChildren< QListWidget* >() )
        for( int row = 0; row < list->count(); ++row )
            labels << list->item( row )->data( Qt::UserRole + 1 ).toString();
    QCOMPARE( labels, ( QStringList{ QStringLiteral( ".." ), QStringLiteral( "logo.png" ) } ) );
    QTest::keyClick( editor.widget(), Qt::Key_Down );
    QTest::keyClick( editor.widget(), Qt::Key_Tab );
    QTRY_VERIFY( !coordinator.isPopupVisible() );
    QCOMPARE( lineAt( 4 ), QStringLiteral( ".. image:: images/logo.png" ) );

    // 역할: "See :re" → ref, Enter. Esc 로 닫으면 아무것도 넣지 않는다.
    view->Backend()->setText( QStringLiteral( "Title\n=====\n\nSee :re\n" ) );
    finish( 4, 8 );
    QTRY_VERIFY( coordinator.isPopupVisible() );
    QTest::keyClick( editor.widget(), Qt::Key_Escape );
    QTRY_VERIFY( !coordinator.isPopupVisible() );
    QCOMPARE( lineAt( 4 ), QStringLiteral( "See :re" ) );
    finish( 4, 8 );
    QTRY_VERIFY( coordinator.isPopupVisible() );
    QTest::keyClick( editor.widget(), Qt::Key_Return );
    QTRY_VERIFY( !coordinator.isPopupVisible() );
    QVERIFY2( lineAt( 4 ).startsWith( QStringLiteral( "See :ref:" ) ), qPrintable( lineAt( 4 ) ) );
    coordinator.detachEditor( &editor );
}

HC_RST_REGISTER_TEST( TestRstCompletionPopup );
#include "tstRstCompletionPopup.moc"
