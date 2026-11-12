#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "UniqueLibs/document/rstPreviewProcess.hpp"
#include "UniqueLibs/document/rstDocumentView.hpp"
#include "UniqueLibs/document/documentWorkspaceWidget.hpp"
#include "UniqueLibs/settings/appSettings.hpp"
#include "UniqueLibs/document/rst/rstWorkspaceController.hpp"
#include "text/QTextView.h"
#include "text/ScintillaQtDirectBackend.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QJsonDocument>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScopeGuard>
#include <cstdio>
#include <QVBoxLayout>
#include <QWidget>

#include <windows.h>
#include <Scintilla.h>

using hc::document::RstPreviewProcess;

namespace
{

bool put( const QString& path, const QByteArray& bytes )
{
    QDir().mkpath( QFileInfo( path ).absolutePath() );
    QFile file( path );
    return file.open( QIODevice::WriteOnly ) && file.write( bytes ) == bytes.size();
}

/// Sphinx 빌더가 심는 것과 같은 표식을 가진 페이지. 문단 하나가 40 px 이고 원본 줄은 step 씩 는다.
QByteArray page( const int source, const int step, const int count, const QByteArray& head )
{
    QByteArray html = "<!doctype html><html><head><meta charset=\"utf-8\"><title>fixture</title>"
                      "<style>html,body{margin:0;padding:0}p{margin:0;height:40px;overflow:hidden}"
                      "a.top{display:block;height:60px;background:#dde}h2{margin:0;height:40px}</style></head><body>";
    html += head;
    for( int index = 1; index <= count; ++index )
    {
        const QByteArray line = QByteArray::number( index * step );
        html += "<p data-mrr-src=\"" + QByteArray::number( source ) + "\" data-mrr-start-line=\"" + line
                + "\" data-mrr-end-line=\"" + line + "\">line " + line + "</p>";
        if( source == 1 && index == 60 )
            html += "<h2 id=\"sec\" data-mrr-src=\"1\" data-mrr-start-line=\"" + QByteArray::number( index * step + 1 )
                    + "\" data-mrr-end-line=\"" + QByteArray::number( index * step + 1 ) + "\">section</h2>";
    }
    html += "</body></html>";
    return html;
}

}  // namespace

/// 미리보기 프로세스( hc-rst-preview.exe )를 실제로 띄워 스크롤 동기화와 링크 이동을 확인한다.
///
/// 조율자가 쓰는 것과 같은 인터페이스( hc::rst::PreviewSurface )로 페이지를 띄우고, 편집기 줄 → 미리보기
/// 위치, 미리보기를 굴린 것 → 편집기 줄, 링크로 다른 문서 페이지에 갔다가 돌아오는 것을 본다.
/// 화면이 없는 환경( offscreen )에서는 창을 붙일 수 없어 건너뛴다.
class TestPreviewProcess : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void pageLoadsAndBridgeAttaches();
    void editorLineMovesThePreview();
    void topOfDocumentStaysAtTop();
    void wheelIsReportedAsUserScroll();
    void scrollRequestsAreNotReportedBack();
    void linkOpensAnotherDocumentPage();
    void clickOnOtherPageNamesItsSource();
    void returnsToTheDocumentPage();
    void filesOutsideTheOutputAreRefused();
    void sphinxPreviewEndToEnd();
    void missingExtensionFromProject();
    void cleanupTestCase();

private:
    [[nodiscard]] QJsonObject           probe( double ratio );
    [[nodiscard]] HWND                  child() const;
    void                                click( int x, int y ) const;

    QTemporaryDir                       root_;
    QWidget*                            host_ = nullptr;
    RstPreviewProcess*                  surface_ = nullptr;
    QUrl                                pageA_, pageB_;
    bool                                usable_ = false;
};

void TestPreviewProcess::initTestCase()
{
    if( QGuiApplication::platformName() != QLatin1String( "windows" ) )
        QSKIP( "미리보기 프로세스의 창을 붙이려면 실제 화면이 필요합니다." );
    if( !QFileInfo::exists( QCoreApplication::applicationDirPath() + QStringLiteral( "/hc-rst-preview.exe" ) ) )
        QSKIP( "hc-rst-preview.exe 가 시험 실행 파일 옆에 없습니다." );
    QVERIFY( root_.isValid() );

    // <빌드 폴더>/preview-html/html/…  — 미리보기 출력과 같은 배치. preview-html 이 페이지가 읽을 수 있는 뿌리다.
    const QString html = root_.filePath( QStringLiteral( "_build/hypereditor/preview-html/html" ) );
    QVERIFY( put( html + QStringLiteral( "/a.html" ),
                  page( 0, 10, 200, "<a class=\"top\" id=\"next\" href=\"guide/b.html#sec\">next</a>"
                                    "<a class=\"top\" id=\"out\" href=\"../../../../outside.html\">outside</a>" ) ) );
    QVERIFY( put( html + QStringLiteral( "/guide/b.html" ), page( 1, 5, 200, "<a class=\"top\" href=\"../a.html\">back</a>" ) ) );
    QVERIFY( put( root_.filePath( QStringLiteral( "outside.html" ) ), "<html><body>outside</body></html>" ) );
    pageA_ = QUrl::fromLocalFile( html + QStringLiteral( "/a.html" ) );
    pageB_ = QUrl::fromLocalFile( html + QStringLiteral( "/guide/b.html" ) );

    host_ = new QWidget;
    host_->setWindowTitle( QStringLiteral( "hc_rst_tests preview" ) );
    host_->resize( 900, 700 );
    auto* layout = new QVBoxLayout( host_ );
    layout->setContentsMargins( 0, 0, 0, 0 );
    surface_ = new RstPreviewProcess( host_ );
    layout->addWidget( surface_->Widget() );
    host_->show();
    QVERIFY( QTest::qWaitForWindowExposed( host_ ) );
    host_->raise();
    host_->activateWindow();
    usable_ = true;
}

void TestPreviewProcess::cleanupTestCase()
{
    delete host_;
    host_ = nullptr;
    surface_ = nullptr;
}

QJsonObject TestPreviewProcess::probe( const double ratio )
{
    QSignalSpy spy( surface_, &RstPreviewProcess::Probed );
    surface_->Probe( ratio );
    if( spy.isEmpty() && !spy.wait( 5000 ) )
        return {};
    return spy.last().first().toJsonObject();
}

HWND TestPreviewProcess::child() const
{
    // 호스트 창의 자손 가운데 다른 프로세스의 첫 창이 미리보기 프로세스가 붙인 창이다.
    struct Find { DWORD self; HWND found; } find{ GetCurrentProcessId(), nullptr };
    EnumChildWindows( reinterpret_cast< HWND >( host_->winId() ), []( HWND window, LPARAM state ) -> BOOL {
        auto* into = reinterpret_cast< Find* >( state );
        DWORD process = 0;
        GetWindowThreadProcessId( window, &process );
        if( process == into->self )
            return TRUE;
        into->found = window;
        return FALSE;
    }, reinterpret_cast< LPARAM >( &find ) );
    return find.found;
}

void TestPreviewProcess::click( const int x, const int y ) const
{
    // x · y 는 페이지의 CSS px 이다. 창 메시지는 장치 픽셀로 간다.
    const HWND window = child();
    const double scale = host_->devicePixelRatioF();
    const LPARAM where = MAKELPARAM( qRound( x * scale ), qRound( y * scale ) );
    PostMessageW( window, WM_MOUSEMOVE, 0, where );
    PostMessageW( window, WM_LBUTTONDOWN, MK_LBUTTON, where );
    PostMessageW( window, WM_LBUTTONUP, 0, where );
}

void TestPreviewProcess::pageLoadsAndBridgeAttaches()
{
    if( !usable_ ) QSKIP( "화면 없음" );
    QSignalSpy ready( surface_, &hc::rst::PreviewSurface::bridgeReady );
    QSignalSpy finished( surface_, &hc::rst::PreviewSurface::loadFinished );
    surface_->load( pageA_ );
    // 처음에는 WebEngine 프로세스가 뜨는 시간이 든다.
    QVERIFY2( ready.wait( 60000 ), "미리보기 페이지의 스크립트가 붙지 않았습니다." );
    QVERIFY( !finished.isEmpty() );
    QVERIFY( finished.last().first().toBool() );
    QVERIFY( surface_->isReady() );
    QVERIFY( !surface_->IsOtherPage() );
    QCOMPARE( QFileInfo( surface_->url().toLocalFile() ).fileName(), QStringLiteral( "a.html" ) );
    // 미리보기 프로세스의 창은 문서 창 안에 자식으로 붙어 있어야 한다. 프로세스가 창 핸들을 너무 일찍 알리면
    // ( 첫 페이지의 렌더 위젯이 붙기 전 ) Qt 가 창을 다시 만들면서 미리보기가 따로 뜬 창이 된다.
    const HWND window = child();
    QVERIFY2( window != nullptr, "미리보기 프로세스의 창이 문서 창에 붙지 않았습니다." );
    QCOMPARE( quintptr( GetParent( window ) ), quintptr( surface_->Widget()->winId() ) );
    QVERIFY( ( GetWindowLongPtrW( window, GWL_STYLE ) & WS_CHILD ) != 0 );
    QVERIFY( IsWindowVisible( window ) );
    // 붙은 창은 붙인 자리를 채운다.
    RECT inner{}, outer{};
    QVERIFY( GetClientRect( window, &inner ) && GetClientRect( reinterpret_cast< HWND >( surface_->Widget()->winId() ), &outer ) );
    QTRY_VERIFY_WITH_TIMEOUT( GetClientRect( window, &inner ) && inner.right == outer.right && inner.bottom == outer.bottom, 3000 );

    const QJsonObject at = probe( 0.5 );
    QVERIFY2( at[ "ready" ].toBool(), "동기화 스크립트( mrr_preview.js )가 페이지에 없습니다." );
    QCOMPARE( at[ "src" ].toInt(), 0 );
    QVERIFY( at[ "total" ].toDouble() > 8000 );
}

void TestPreviewProcess::editorLineMovesThePreview()
{
    if( !usable_ ) QSKIP( "화면 없음" );
    // 조율자가 하는 순서 그대로: 되돌아오는 보고를 막고, 편집기 창 가운데의 줄을 미리보기 가운데로.
    for( const double line : { 1000.0, 1500.0, 400.0, 1234.5 } )
    {
        surface_->suppressScrollFeedback( 250 );
        surface_->requestScrollToLine( 0, line, 0.5 );
        QTRY_VERIFY2_WITH_TIMEOUT( qAbs( probe( 0.5 )[ "line" ].toDouble() - line ) < 1.5,
                                   qPrintable( QStringLiteral( "줄 %1 을 가운데로 보냈지만 가운데에는 줄 %2" )
                                                   .arg( line ).arg( probe( 0.5 )[ "line" ].toDouble() ) ),
                                   5000 );
        QCOMPARE( probe( 0.5 )[ "src" ].toInt(), 0 );
    }
    // 창의 다른 높이( 클릭한 자리의 비율 )로도 맞는다.
    surface_->suppressScrollFeedback( 250 );
    surface_->requestScrollToLine( 0, 800.0, 0.25 );
    QTRY_VERIFY_WITH_TIMEOUT( qAbs( probe( 0.25 )[ "line" ].toDouble() - 800.0 ) < 1.5, 5000 );
}

void TestPreviewProcess::topOfDocumentStaysAtTop()
{
    if( !usable_ ) QSKIP( "화면 없음" );
    // 편집기가 문서 맨 위면 미리보기도 맨 위다( 첫 기준점을 가운데로 올려 제목을 밀어내지 않는다 ).
    surface_->suppressScrollFeedback( 250 );
    surface_->requestScrollToLine( 0, 1.0, 0.5 );
    QTRY_COMPARE_WITH_TIMEOUT( probe( 0.5 )[ "scrollY" ].toDouble(), 0.0, 5000 );
}

void TestPreviewProcess::wheelIsReportedAsUserScroll()
{
    if( !usable_ ) QSKIP( "화면 없음" );
    surface_->suppressScrollFeedback( 250 );
    surface_->requestScrollToLine( 0, 600.0, 0.5 );
    QTRY_VERIFY_WITH_TIMEOUT( qAbs( probe( 0.5 )[ "line" ].toDouble() - 600.0 ) < 1.5, 5000 );
    QTest::qWait( 400 );   // 되돌아오는 보고를 막는 구간이 지나야 한다.

    QSignalSpy scrolled( surface_, &hc::rst::PreviewSurface::previewScrollChanged );
    const HWND window = child();
    RECT client{};
    QVERIFY( GetClientRect( window, &client ) );
    POINT centre{ client.right / 2, client.bottom / 2 };
    ClientToScreen( window, &centre );
    // 아래로 세 칸. 휠은 화면 좌표로 온다.
    for( int notch = 0; notch < 3; ++notch )
        PostMessageW( window, WM_MOUSEWHEEL, MAKEWPARAM( 0, static_cast< WORD >( -WHEEL_DELTA ) ), MAKELPARAM( centre.x, centre.y ) );

    QTRY_VERIFY2_WITH_TIMEOUT( !scrolled.isEmpty(), "미리보기를 굴렸지만 위치 보고가 오지 않았습니다.", 8000 );
    const QList< QVariant > last = scrolled.last();
    QCOMPARE( last.at( 0 ).toInt(), 0 );
    // 사용자가 굴린 것으로 판정돼야 편집기가 따라간다. 페이지가 스스로 움직인 것은 편집기를 옮기지 않는다.
    QVERIFY2( last.at( 3 ).toBool(), "휠로 굴린 스크롤이 사용자 입력으로 판정되지 않았습니다." );
    QVERIFY2( last.at( 1 ).toDouble() > 600.0, qPrintable( QString::number( last.at( 1 ).toDouble() ) ) );
    // 보고된 줄은 지금 가운데에 놓인 줄이다.
    QTRY_VERIFY_WITH_TIMEOUT( qAbs( probe( 0.5 )[ "line" ].toDouble() - scrolled.last().at( 1 ).toDouble() ) < 2.0, 5000 );
}

void TestPreviewProcess::scrollRequestsAreNotReportedBack()
{
    if( !usable_ ) QSKIP( "화면 없음" );
    QTest::qWait( 700 );   // 앞 시험의 휠 여운( 사용자 입력 판정 구간 )이 지나야 한다.
    QSignalSpy scrolled( surface_, &hc::rst::PreviewSurface::previewScrollChanged );
    surface_->suppressScrollFeedback( 250 );
    surface_->requestScrollToLine( 0, 1700.0, 0.5 );
    QTRY_VERIFY_WITH_TIMEOUT( qAbs( probe( 0.5 )[ "line" ].toDouble() - 1700.0 ) < 1.5, 5000 );
    QTest::qWait( 600 );
    // 편집기가 옮긴 스크롤이 "사용자가 굴렸다" 로 되돌아오면 왕복이 생겨 스크롤이 튄다.
    for( const QList< QVariant >& report : scrolled )
        QVERIFY2( !report.at( 3 ).toBool(), "편집기가 만든 스크롤이 사용자 스크롤로 보고됐습니다." );
}

void TestPreviewProcess::linkOpensAnotherDocumentPage()
{
    if( !usable_ ) QSKIP( "화면 없음" );
    surface_->suppressScrollFeedback( 250 );
    surface_->requestScrollToLine( 0, 1.0, 0.5 );
    QTRY_COMPARE_WITH_TIMEOUT( probe( 0.5 )[ "scrollY" ].toDouble(), 0.0, 5000 );

    QSignalSpy changed( surface_, &RstPreviewProcess::PageChanged );
    QSignalSpy ready( surface_, &hc::rst::PreviewSurface::bridgeReady );
    QSignalSpy outside( surface_, &RstPreviewProcess::LinkActivated );
    // 맨 위의 링크( guide/b.html#sec ): 같은 출력 폴더 안의 다른 문서 페이지.
    click( 40, 20 );
    QTRY_VERIFY2_WITH_TIMEOUT( surface_->IsOtherPage(), "링크를 눌렀지만 다른 문서 페이지로 넘어가지 않았습니다.", 10000 );
    QVERIFY( !changed.isEmpty() );
    QVERIFY( outside.isEmpty() );
    QCOMPARE( QFileInfo( surface_->url().toLocalFile() ).fileName(), QStringLiteral( "b.html" ) );
    QCOMPARE( surface_->url().fragment(), QStringLiteral( "sec" ) );
    // 조율자가 띄운 페이지는 그대로 기억한다( 돌아갈 곳 ).
    QCOMPARE( QFileInfo( surface_->RequestedUrl().toLocalFile() ).fileName(), QStringLiteral( "a.html" ) );
    // 넘어간 페이지에도 동기화 스크립트가 붙는다.
    QTRY_VERIFY_WITH_TIMEOUT( !ready.isEmpty(), 10000 );
    QJsonObject at;
    QTRY_VERIFY_WITH_TIMEOUT( ( at = probe( 0.5 ) )[ "ready" ].toBool(), 5000 );
    QCOMPARE( at[ "src" ].toInt(), 1 );
    // #sec 로 내려와 있다.
    QVERIFY( at[ "scrollY" ].toDouble() > 1000 );
}

void TestPreviewProcess::clickOnOtherPageNamesItsSource()
{
    if( !usable_ ) QSKIP( "화면 없음" );
    QVERIFY( surface_->IsOtherPage() );
    // 다른 문서 페이지에 있는 동안 이 편집기의 줄로 그 페이지를 움직이지 않는다.
    const double before = probe( 0.5 )[ "scrollY" ].toDouble();
    surface_->suppressScrollFeedback( 250 );
    surface_->requestScrollToLine( 0, 1.0, 0.5 );
    QTest::qWait( 500 );
    QCOMPARE( probe( 0.5 )[ "scrollY" ].toDouble(), before );

    // 내용을 누르면 그 자리의 원본( 색인 1 = 다른 문서 )과 줄이 온다. 보기가 그 파일을 그 줄로 연다.
    QSignalSpy clicked( surface_, &hc::rst::PreviewSurface::editorNavigationRequested );
    click( 60, 300 );
    QTRY_VERIFY2_WITH_TIMEOUT( !clicked.isEmpty(), "넘어간 페이지의 내용을 눌렀지만 원본 위치가 오지 않았습니다.", 8000 );
    QCOMPARE( clicked.last().at( 0 ).toInt(), 1 );
    QVERIFY( clicked.last().at( 1 ).toDouble() > 1.0 );
}

void TestPreviewProcess::returnsToTheDocumentPage()
{
    if( !usable_ ) QSKIP( "화면 없음" );
    QSignalSpy ready( surface_, &hc::rst::PreviewSurface::bridgeReady );
    surface_->ReturnToRequested();
    QTRY_VERIFY_WITH_TIMEOUT( !surface_->IsOtherPage() && !ready.isEmpty(), 10000 );
    QCOMPARE( QFileInfo( surface_->url().toLocalFile() ).fileName(), QStringLiteral( "a.html" ) );
    // 돌아온 뒤에는 다시 편집기의 줄을 따른다.
    surface_->suppressScrollFeedback( 250 );
    surface_->requestScrollToLine( 0, 900.0, 0.5 );
    QTRY_VERIFY_WITH_TIMEOUT( qAbs( probe( 0.5 )[ "line" ].toDouble() - 900.0 ) < 1.5, 5000 );
}

void TestPreviewProcess::filesOutsideTheOutputAreRefused()
{
    if( !usable_ ) QSKIP( "화면 없음" );
    // 미리보기는 Sphinx 출력 폴더( preview-html )만 읽는다. 그 밖의 파일로 가는 링크는 이 창에서 열지 않고
    // 주소만 호스트에 넘긴다( 보기가 문서로 열지 정한다 ).
    surface_->suppressScrollFeedback( 250 );
    surface_->requestScrollToLine( 0, 1.0, 0.5 );
    QTRY_COMPARE_WITH_TIMEOUT( probe( 0.5 )[ "scrollY" ].toDouble(), 0.0, 5000 );
    QSignalSpy outside( surface_, &RstPreviewProcess::LinkActivated );
    click( 40, 90 );   // 둘째 링크( 60 – 120 px ): ../../../../outside.html
    QTRY_VERIFY2_WITH_TIMEOUT( !outside.isEmpty(), "출력 폴더 밖의 링크가 호스트로 넘어오지 않았습니다.", 8000 );
    QCOMPARE( QFileInfo( outside.last().first().toUrl().toLocalFile() ).fileName(), QStringLiteral( "outside.html" ) );
    QTest::qWait( 300 );
    QCOMPARE( QFileInfo( surface_->url().toLocalFile() ).fileName(), QStringLiteral( "a.html" ) );
    QVERIFY( !surface_->IsOtherPage() );
}

/// 전체 흐름: uv 를 공식 배포처에서 내려받아 환경을 구성하고, Sphinx 로 미리보기를 만들고, 조율자가 편집기와
/// 미리보기의 스크롤을 서로 맞추는지 본다. 수백 MB 를 내려받으므로 HC_RST_E2E=1 일 때만 돈다. 환경은
/// HC_RST_RUNTIME_DIR( 시험용 폴더 )에 만든다 — 사용자의 실제 환경을 건드리지 않도록 그 변수가 없으면 건너뛴다.
void TestPreviewProcess::sphinxPreviewEndToEnd()
{
    using namespace hc::document;
    if( qEnvironmentVariableIsEmpty( "HC_RST_E2E" ) )
        QSKIP( "네트워크로 uv · Python · Sphinx 를 내려받는 시험입니다. HC_RST_E2E=1 로 켭니다." );
    if( qEnvironmentVariableIsEmpty( "HC_RST_RUNTIME_DIR" ) )
        QSKIP( "HC_RST_RUNTIME_DIR 로 시험용 환경 폴더를 지정해야 합니다." );
    if( !usable_ ) QSKIP( "화면 없음" );

    // 프로젝트: conf.py + index.rst( 줄 수가 많은 문서 ) + 다른 문서 하나.
    QTemporaryDir project;
    QVERIFY( project.isValid() );
    QVERIFY( put( project.filePath( "conf.py" ), "project = 'E2E'\nextensions = []\nhtml_theme = 'alabaster'\n" ) );
    QByteArray index = "E2E\n===\n\n.. toctree::\n\n   guide/install\n\n";
    for( int section = 1; section <= 40; ++section )
    {
        index += "Section " + QByteArray::number( section ) + "\n" + QByteArray( 12, '-' ) + "\n\n";
        for( int paragraph = 1; paragraph <= 4; ++paragraph )
            index += "Paragraph " + QByteArray::number( section ) + "." + QByteArray::number( paragraph )
                     + " with enough words to be a real line of text.\n\n";
    }
    QVERIFY( put( project.filePath( "index.rst" ), index ) );
    QVERIFY( put( project.filePath( "guide/install.rst" ), "Install\n=======\n\nSee :doc:`../index`.\n" ) );
    const int totalLines = index.count( '\n' );

    // 설정: 묻지 않고 uv 를 받는다( 시험이 대화상자에서 멈추지 않게 ).
    QTemporaryDir config;
    QVERIFY( config.isValid() );
    const QVariant previous = qApp->property( "documentViewsConfigDir" );
    qApp->setProperty( "documentViewsConfigDir", config.path() );
    const auto restore = qScopeGuard( [ previous ] { qApp->setProperty( "documentViewsConfigDir", previous ); } );
    QVERIFY( put( hc::settings::UnifiedConfig::FileName( config.path() ),
                  R"({"schemaVersion":2,"documentViews":{"rst":{"python":{"uvAcquire":"auto"}}}})" ) );

    DocumentWorkspaceWidget window( config.filePath( "recovery" ) );
    window.resize( 1300, 800 );
    window.show();
    QVERIFY( QTest::qWaitForWindowExposed( &window ) );
    const QString id = window.Handle( { { "action", "open" }, { "path", project.filePath( "index.rst" ) } } )[ "documentId" ].toString();
    QVERIFY( !id.isEmpty() );
    RstDocumentView* view = nullptr;
    QTRY_VERIFY_WITH_TIMEOUT( ( view = window.findChild< RstDocumentView* >() ) != nullptr, 20000 );

    // 오래 걸리는 시험이다. 어디까지 왔는지 표준 오류로 남긴다( 시험 로그는 끝나야 써진다 ).
    QTimer progress;
    QString lastState;
    connect( &progress, &QTimer::timeout, this, [ & ] {
        const QString state = QString::fromUtf8( QJsonDocument( view->State() ).toJson( QJsonDocument::Compact ) );
        if( state == lastState )
            return;
        lastState = state;
        std::fprintf( stderr, "[e2e] %s\n", qPrintable( state ) );
        std::fflush( stderr );
    } );
    progress.start( 3000 );

    // 1) 환경: uv 내려받기( 해시 확인 ) → Python → Sphinx · Esbonio.
    QTRY_VERIFY2_WITH_TIMEOUT( view->State()[ "environmentReady" ].toBool(),
                               qPrintable( QStringLiteral( "환경이 구성되지 않았습니다: %1" ).arg( view->State()[ "environment" ].toString() ) ),
                               30 * 60 * 1000 );
    const QString runtime = qEnvironmentVariable( "HC_RST_RUNTIME_DIR" );
    QVERIFY( QFileInfo::exists( runtime + QStringLiteral( "/uv.exe" ) ) );
    QVERIFY( QFileInfo::exists( runtime + QStringLiteral( "/uv.version" ) ) );

    qInfo() << "환경 준비됨:" << view->State()[ "environment" ].toString();
    // 2) 미리보기: Sphinx 가 만든 이 문서의 페이지가 뜬다.
    QTRY_VERIFY2_WITH_TIMEOUT( view->State()[ "previewShown" ].toBool() && QUrl( view->State()[ "previewUrl" ].toString() ).fileName() == QLatin1String( "index.html" ),
                               qPrintable( QStringLiteral( "미리보기가 뜨지 않았습니다: %1 / %2" )
                                               .arg( view->State()[ "previewStatus" ].toString(), view->State()[ "banner" ].toString() ) ),
                               4 * 60 * 1000 );
    qInfo() << "미리보기 표시:" << view->State()[ "previewUrl" ].toString();
    // 빌드 산출물은 프로젝트의 내부 빌드 폴더에만 생긴다.
    QVERIFY( QFileInfo( project.filePath( "_build/hypereditor/preview-html" ) ).isDir() );
    QVERIFY( !QFileInfo::exists( project.filePath( ".multiroot" ) ) );

    auto* surface = window.findChild< RstPreviewProcess* >();
    QVERIFY( surface );
    const auto probeSurface = [ surface ]( const double ratio ) {
        QSignalSpy spy( surface, &RstPreviewProcess::Probed );
        surface->Probe( ratio );
        if( spy.isEmpty() && !spy.wait( 5000 ) )
            return QJsonObject{};
        return spy.last().first().toJsonObject();
    };
    QTRY_VERIFY_WITH_TIMEOUT( probeSurface( 0.5 )[ "ready" ].toBool(), 30000 );

    // 3) 편집기 → 미리보기: 편집기 창 가운데의 줄이 미리보기 가운데에 온다.
    QTextView* text = window.findChild< QTextView* >();
    QVERIFY( text );
    auto* backend = static_cast< ScintillaQtDirectBackend* >( text->Backend() );
    for( const double line : { totalLines * 0.5, totalLines * 0.8, totalLines * 0.25 } )
    {
        backend->scrollFractionalLineToViewportRatio( line, 0.5 );
        const double wanted = backend->fractionalLineAtViewportRatio( 0.5 );
        QTRY_VERIFY2_WITH_TIMEOUT( qAbs( probeSurface( 0.5 )[ "line" ].toDouble() - wanted ) < 4.0,
                                   qPrintable( QStringLiteral( "편집기 가운데 줄 %1, 미리보기 가운데 줄 %2" )
                                                   .arg( wanted ).arg( probeSurface( 0.5 )[ "line" ].toDouble() ) ),
                                   10000 );
    }

    qInfo() << "편집기 → 미리보기 확인";
    // 4) 미리보기 → 편집기: 미리보기를 굴리면 편집기가 따라온다.
    QTest::qWait( 800 );
    const double before = backend->fractionalLineAtViewportRatio( 0.5 );
    HWND helper = nullptr;
    {
        struct Find { DWORD self; HWND found; } find{ GetCurrentProcessId(), nullptr };
        EnumChildWindows( reinterpret_cast< HWND >( window.winId() ), []( HWND candidate, LPARAM state ) -> BOOL {
            auto* into = reinterpret_cast< Find* >( state );
            DWORD process = 0;
            GetWindowThreadProcessId( candidate, &process );
            if( process == into->self )
                return TRUE;
            into->found = candidate;
            return FALSE;
        }, reinterpret_cast< LPARAM >( &find ) );
        helper = find.found;
    }
    QVERIFY2( helper != nullptr, "미리보기 프로세스의 창이 문서 창에 붙지 않았습니다." );
    RECT client{};
    QVERIFY( GetClientRect( helper, &client ) );
    POINT centre{ client.right / 2, client.bottom / 2 };
    ClientToScreen( helper, &centre );
    for( int notch = 0; notch < 4; ++notch )
        PostMessageW( helper, WM_MOUSEWHEEL, MAKEWPARAM( 0, static_cast< WORD >( -WHEEL_DELTA ) ), MAKELPARAM( centre.x, centre.y ) );
    QTRY_VERIFY2_WITH_TIMEOUT( backend->fractionalLineAtViewportRatio( 0.5 ) > before + 2.0,
                               "미리보기를 굴렸지만 편집기가 따라오지 않았습니다.", 10000 );
    // 따라온 편집기의 가운데 줄이 미리보기의 가운데 줄이다( 왕복해서 튀지 않고 머문다 ).
    QTest::qWait( 1200 );
    QVERIFY2( qAbs( probeSurface( 0.5 )[ "line" ].toDouble() - backend->fractionalLineAtViewportRatio( 0.5 ) ) < 4.0,
              qPrintable( QStringLiteral( "편집기 %1, 미리보기 %2" ).arg( backend->fractionalLineAtViewportRatio( 0.5 ) )
                              .arg( probeSurface( 0.5 )[ "line" ].toDouble() ) ) );

    qInfo() << "미리보기 → 편집기 확인";
    // 5) 편집: 없는 지시어를 넣으면 다시 빌드되고 진단이 생긴다.
    QCOMPARE( view->State()[ "errors" ].toInt() + view->State()[ "warnings" ].toInt(), 0 );
    const QByteArray added = "\n.. nosuchdirectivexyz::\n\n   body\n";
    backend->Send( SCI_DOCUMENTEND );
    backend->Send( SCI_ADDTEXT, static_cast< uptr_t >( added.size() ), reinterpret_cast< sptr_t >( added.constData() ) );
    QTRY_VERIFY2_WITH_TIMEOUT( view->State()[ "errors" ].toInt() + view->State()[ "warnings" ].toInt() > 0,
                               "잘못된 지시어를 넣었지만 진단이 생기지 않았습니다.", 3 * 60 * 1000 );
}

/// 프로젝트가 쓰는 확장이 HyperEditor 환경에 없을 때( 2026-11-12, breathe ): 빌드 실패 안내가 설치 안내를 덮지
/// 않고, 프로젝트가 밝힌 버전으로 HyperEditor 환경에 설치한 뒤 다시 빌드한다. 프로젝트 pyproject.toml 의
/// [tool.esbonio.sphinx] pythonCommand( uv run python )는 따르지 않는다 — 따랐다면 uv 가 프로젝트에 .venv 를 만든다.
void TestPreviewProcess::missingExtensionFromProject()
{
    using namespace hc::document;
    if( qEnvironmentVariableIsEmpty( "HC_RST_E2E" ) )
        QSKIP( "네트워크로 uv · Python · Sphinx 와 확장을 내려받는 시험입니다. HC_RST_E2E=1 로 켭니다." );
    if( qEnvironmentVariableIsEmpty( "HC_RST_RUNTIME_DIR" ) )
        QSKIP( "HC_RST_RUNTIME_DIR 로 시험용 환경 폴더를 지정해야 합니다." );
    if( !usable_ ) QSKIP( "화면 없음" );

    QTemporaryDir project;
    QVERIFY( project.isValid() );
    QVERIFY( put( project.filePath( "pyproject.toml" ),
                  "[project]\nname = \"missing-extension\"\nversion = \"0.1.0\"\ndependencies = [\n    \"sphinx>=8\",\n    \"breathe>=4.36\",\n]\n\n"
                  "[tool.esbonio.sphinx]\npythonCommand = [ \"uv\", \"run\", \"python\" ]\n" ) );
    QVERIFY( put( project.filePath( "conf.py" ), "project = 'Missing'\nextensions = ['breathe']\nbreathe_projects = {}\nhtml_theme = 'alabaster'\n" ) );
    QVERIFY( put( project.filePath( "index.rst" ), "Missing\n=======\n\ntext\n" ) );

    QTemporaryDir config;
    QVERIFY( config.isValid() );
    const QVariant previous = qApp->property( "documentViewsConfigDir" );
    qApp->setProperty( "documentViewsConfigDir", config.path() );
    const auto restore = qScopeGuard( [ previous ] { qApp->setProperty( "documentViewsConfigDir", previous ); } );
    QVERIFY( put( hc::settings::UnifiedConfig::FileName( config.path() ),
                  R"({"schemaVersion":2,"documentViews":{"rst":{"python":{"uvAcquire":"auto"}}}})" ) );

    DocumentWorkspaceWidget window( config.filePath( "recovery" ) );
    window.resize( 1200, 800 );
    window.show();
    QVERIFY( QTest::qWaitForWindowExposed( &window ) );
    QVERIFY( !window.Handle( { { "action", "open" }, { "path", project.filePath( "index.rst" ) } } )[ "documentId" ].toString().isEmpty() );
    RstDocumentView* view = nullptr;
    QTRY_VERIFY_WITH_TIMEOUT( ( view = window.findChild< RstDocumentView* >() ) != nullptr, 20000 );
    QTRY_VERIFY_WITH_TIMEOUT( view->State()[ "environmentReady" ].toBool(), 30 * 60 * 1000 );
    const QString runtime = qEnvironmentVariable( "HC_RST_RUNTIME_DIR" );
    const QDir packages( runtime + QStringLiteral( "/.venv/Lib/site-packages" ) );
    QVERIFY2( packages.entryList( { QStringLiteral( "breathe-*.dist-info" ) }, QDir::Dirs ).isEmpty(), "시험용 환경에 breathe 가 이미 있습니다." );

    // 빌드는 breathe 를 못 찾고 실패한다. 설치 안내가 남고, 프로젝트가 밝힌 버전 조건을 싣는다.
    QTRY_VERIFY_WITH_TIMEOUT( view->State()[ "previewFailed" ].toBool(), 4 * 60 * 1000 );
    QTest::qWait( 1500 );
    // 안내 글은 HTML 로 이스케이프돼 있다.
    const QString banner = view->State()[ "banner" ].toString().replace( QLatin1String( "&gt;" ), QLatin1String( ">" ) );
    if( !banner.contains( QLatin1String( "breathe>=4.36" ) ) )
        if( auto* log = view->findChild< QPlainTextEdit* >( QStringLiteral( "rstLog" ) ) )
            std::fprintf( stderr, "[log]\n%s\n", log->toPlainText().toUtf8().constData() );
    QVERIFY2( banner.contains( QLatin1String( "breathe>=4.36" ) ), qPrintable( QStringLiteral( "안내: %1" ).arg( banner ) ) );
    auto* install = view->findChild< QPushButton* >( QStringLiteral( "rstBannerButton0" ) );
    QVERIFY( install && install->isVisible() );

    // 언어 서버는 HyperEditor 환경으로 돈다.
    const QString projectId = view->State()[ "projectId" ].toString();
    QTRY_VERIFY_WITH_TIMEOUT( hc::rst::RstWorkspace()->isLanguageServerRunning( projectId ), 120000 );
    QTest::qWait( 8000 );
    QVERIFY2( !QFileInfo::exists( project.filePath( ".venv" ) ), "언어 서버가 프로젝트 pyproject.toml 의 uv run python 을 따랐습니다." );

    // 설치: HyperEditor 환경에 들어가고 미리보기를 다시 빌드한다.
    install->click();
    QTRY_VERIFY2_WITH_TIMEOUT( view->State()[ "previewShown" ].toBool() && !view->State()[ "previewFailed" ].toBool(),
                               qPrintable( QStringLiteral( "설치 뒤 미리보기: %1 / %2" ).arg( view->State()[ "previewStatus" ].toString(), view->State()[ "banner" ].toString() ) ),
                               5 * 60 * 1000 );
    QVERIFY( !packages.entryList( { QStringLiteral( "breathe-*.dist-info" ) }, QDir::Dirs ).isEmpty() );
    QVERIFY( !QFileInfo::exists( project.filePath( ".venv" ) ) );
}

QTEST_MAIN( TestPreviewProcess )

#include "tstRstPreviewProcess.moc"
