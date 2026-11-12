#include "stdafx.h"
#include "previewBridge.hpp"

/// 모듈

/// UIs

/// 기타 헤더파일

/// 고유 라이브러리
#include "UniqueLibs/document/documentStallTrace.hpp"
#include "UniqueLibs/document/documentWebProcess.hpp"
#include "UniqueLibs/fonts/startupFontWarmup.hpp"

/// 공통 라이브러리

/// 외부 라이브러리

/// 선언 헤더파일들

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QAbstractNativeEventFilter>
#include <QDir>
#include <QFileInfo>
#include <QLocalSocket>
#include <QReadWriteLock>
#include <QShortcut>
#include <QTimer>
#include <QWebEngineContextMenuRequest>
#include <QWebEngineDownloadRequest>
#include <QWebEngineNewWindowRequest>
#include <QWebEnginePage>
#include <QWebEngineProfile>
#include <QWebEngineScript>
#include <QWebEngineScriptCollection>
#include <QWebEngineSettings>
#include <QWebEngineUrlRequestInterceptor>
#include <QWebEngineView>
#include <qt_windows.h>

// reStructuredText 미리보기 프로세스.
//
// HyperEditor 는 웹 콘텐츠를 문서 창과 다른 프로세스에 둔다. 이 프로세스는 Sphinx 가 만든 HTML 한 벌을
// 띄우고, 호스트( hc::document::RstPreviewSurface )와 로컬 소켓으로 한 줄에 JSON 하나씩 주고받는다.
// 창은 호스트의 창에 자식으로 붙는다. 틀은 Markdown 미리보기( tools/markdown/render )와 같다.
//
// 호스트 → 여기: attached · focus · copy · selectAll · find · zoom · shortcuts · settings · load ·
//                scrollTo · suppress · hotSwap · close
// 여기 → 호스트: ready · loadProgress · loadFinished · url · bridgeReady · scrolled · clicked ·
//                hotSwapResult · context · link · escape · command · error
//
// 읽을 수 있는 것: load 가 준 뿌리 폴더( 프로젝트의 미리보기 출력 ) 아래의 파일, data:, 그리고 설정이 허락하면
// http(s). 그 밖의 주소로 가는 링크는 여기서 열지 않고 호스트에 넘긴다.

namespace {

// 호스트는 이 창을 자기 스레드에서 직접 크기 조절하지 않고 메시지를 보낸다. 두 GUI 스레드가 입력 큐를
// 함께 쓰므로, 동기 크기 조절은 여기서 페이지를 그리는 동안 편집기 입력을 멈춘다.
class SurfaceResize final : public QAbstractNativeEventFilter
{
public:
    explicit SurfaceResize( QWidget& Window )
        : window_( Window ), message_( RegisterWindowMessageW( hc::document::WebSurfaceResizeMessage ) )
    {
    }
    bool nativeEventFilter( const QByteArray& Type, void* Message, qintptr* ) override
    {
        const auto* Msg = static_cast< const MSG* >( Message );
        if( Type != "windows_generic_MSG" || !message_ || Msg->message != message_ ) return false;
        const auto Window = reinterpret_cast< HWND >( window_.winId() );
        if( Msg->hwnd != Window ) return false;
        RECT Parent{}, Own{};
        const auto Host = GetParent( Window );
        if( Host && GetClientRect( Host, &Parent ) && GetClientRect( Window, &Own )
            && ( Own.right != Parent.right || Own.bottom != Parent.bottom ) )
            SetWindowPos( Window, nullptr, 0, 0, Parent.right, Parent.bottom, SWP_NOACTIVATE | SWP_NOZORDER );
        return true;
    }

private:
    QWidget& window_;
    UINT     message_ = 0;
};

/// 경로가 뿌리 폴더 아래에 있는가. 둘 다 정규 경로여야 한다.
bool isBelow( const QString& Path, const QString& Root )
{
    return !Path.isEmpty() && !Root.isEmpty()
        && ( Path.compare( Root, Qt::CaseInsensitive ) == 0 || Path.startsWith( Root + QLatin1Char( '/' ), Qt::CaseInsensitive ) );
}

/// 페이지가 읽을 수 있는 주소. 인터셉터는 Chromium 의 IO 스레드에서 불린다.
class ResourcePolicy final : public QWebEngineUrlRequestInterceptor
{
public:
    using QWebEngineUrlRequestInterceptor::QWebEngineUrlRequestInterceptor;

    void Set( const QString& Root, const bool Remote )
    {
        QWriteLocker Lock( &mutex_ );
        root_ = Root.isEmpty() ? QString{} : QFileInfo( Root ).canonicalFilePath();
        remote_ = Remote;
    }
    void SetRemote( const bool Remote ) { QWriteLocker Lock( &mutex_ ); remote_ = Remote; }
    [[nodiscard]] bool AllowsFile( const QUrl& Url )
    {
        QReadLocker Lock( &mutex_ );
        return Url.isLocalFile() && isBelow( QFileInfo( Url.toLocalFile() ).canonicalFilePath(), root_ );
    }
    void interceptRequest( QWebEngineUrlRequestInfo& Request ) override
    {
        const QUrl Url = Request.requestUrl();
        const QString Scheme = Url.scheme();
        if( Scheme == QLatin1String( "data" ) || Scheme == QLatin1String( "blob" ) ) return;
        if( Scheme == QLatin1String( "qrc" ) && Url.path() == QLatin1String( "/qtwebchannel/qwebchannel.js" ) ) return;
        if( Url.isLocalFile() ) {
            if( AllowsFile( Url ) ) return;
        } else if( Scheme == QLatin1String( "https" ) || Scheme == QLatin1String( "http" ) ) {
            QReadLocker Lock( &mutex_ );
            if( remote_ ) return;
        }
        Request.block( true );
    }

private:
    QReadWriteLock mutex_;
    QString        root_;
    bool           remote_ = false;
};

class PreviewPage final : public QWebEnginePage
{
    Q_OBJECT

public:
    PreviewPage( QWebEngineProfile* Profile, ResourcePolicy* Policy, QObject* Parent )
        : QWebEnginePage( Profile, Parent ), policy_( Policy )
    {
    }

signals:
    /// 이 창에서 열지 않는 링크( 웹 주소, 메일, 출력 폴더 밖의 파일 ).
    void LinkActivated( const QUrl& Url );

protected:
    bool acceptNavigationRequest( const QUrl& Url, const NavigationType Type, const bool Main ) override
    {
        if( !Main ) return true;   // 프레임 안의 요청은 인터셉터가 거른다.
        if( Url.scheme() == QLatin1String( "about" ) || policy_->AllowsFile( Url ) ) return true;
        // 사용자가 누른 링크만 넘긴다. 스크립트가 일으킨 이동은 조용히 막는다.
        if( Type == NavigationTypeLinkClicked ) emit LinkActivated( Url );
        return false;
    }
    void javaScriptConsoleMessage( JavaScriptConsoleMessageLevel, const QString& Message, const int Line, const QString& Source ) override
    {
        if( qEnvironmentVariableIsEmpty( "HC_RST_PREVIEW_DIAGNOSTICS" ) ) return;
        QFile Log( qEnvironmentVariable( "HC_RST_PREVIEW_DIAGNOSTICS" ) );
        if( Log.open( QIODevice::Append ) )
            Log.write( ( Source + ":" + QString::number( Line ) + ": " + Message + '\n' ).toUtf8() );
    }

private:
    ResourcePolicy* policy_;
};

QString resourceText( const QString& Path )
{
    QFile File( Path );
    return File.open( QIODevice::ReadOnly ) ? QString::fromUtf8( File.readAll() ) : QString{};
}

/// 이름이 Name 인 주입 스크립트를 Source 로 바꾸고 지금 페이지에도 한 번 돌린다. 문서 스타일을 고치는 것이라
/// 페이지의 스크립트와 섞이지 않는 세계에서 돈다.
void applyPageScript( QWebEnginePage* Page, const QString& Name, const QString& Source )
{
    auto& Scripts = Page->scripts();
    for( const auto& Previous : Scripts.find( Name ) ) Scripts.remove( Previous );
    QWebEngineScript Script;
    Script.setName( Name );
    Script.setInjectionPoint( QWebEngineScript::DocumentReady );
    Script.setWorldId( QWebEngineScript::ApplicationWorld );
    Script.setRunsOnSubFrames( false );
    Script.setSourceCode( Source );
    Scripts.insert( Script );
    Page->runJavaScript( Source, QWebEngineScript::ApplicationWorld );
}

/// 본문 컨테이너의 너비 제한만 푼다. 사이드바 · 이미지 · 표의 너비는 그대로다.
QString widthScript( const bool Enabled )
{
    return QString::fromLatin1( R"JS((function () {
    const id = 'mrr-preview-width';
    let style = document.getElementById(id);
    if (!%1) {
        if (style) style.remove();
        return;
    }
    if (!document.documentElement) return;
    if (!style) {
        style = document.createElement('style');
        style.id = id;
        // An early layer also overrides !important rules in layered themes.
        const host = document.head || document.documentElement;
        host.insertBefore(style, host.firstChild);
    }
    const css = '@layer mrr-preview-width {' +
        '.wy-nav-content,div.document,div.body,main,.main .content,' +
        '.bd-page-width,.bd-article-container,.bd-main .bd-content,[role="main"] {' +
        'max-width:none !important;max-inline-size:none !important;' +
        'width:auto !important;inline-size:auto !important;}}';
    if (style.textContent !== css) style.textContent = css;
})();)JS" ).arg( Enabled ? QStringLiteral( "true" ) : QStringLiteral( "false" ) );
}

}  // namespace

int main( int Argc, char** Argv )
{
    hc::fonts::StartupFontWarmup FontWarmup( Argc, Argv );
    // Qt 의 캐시 폴더는 앱 이름을 따른다. 정하지 않으면 %LOCALAPPDATA%\hc-rst-preview 가 생겼다.
    QCoreApplication::setOrganizationName( QStringLiteral( "HyperCommander" ) ); QCoreApplication::setApplicationName( QStringLiteral( "hc-rst-preview" ) );
    QApplication App( Argc, Argv );
    FontWarmup.WatchFirstWindow();
    App.setQuitOnLastWindowClosed( true );
    hc::document::DocumentStallTrace::InstallHelper( QStringLiteral( "rst" ) );
    const auto Args = App.arguments();
    if( Args.size() != 3 ) return 2;

    QLocalSocket Socket;
    QByteArray Input;
    bool Attached = false;
    // 호스트에 붙기 전( 첫 빈 페이지를 띄우는 동안 )의 보고는 보낼 곳이 없다.
    auto Send = [ & ]( const QJsonObject& Value ) {
        if( Socket.state() == QLocalSocket::ConnectedState ) Socket.write( QJsonDocument( Value ).toJson( QJsonDocument::Compact ) + '\n' );
    };

    QWebEngineProfile Profile;
    Profile.setHttpCacheType( QWebEngineProfile::MemoryHttpCache );
    Profile.setPersistentCookiesPolicy( QWebEngineProfile::NoPersistentCookies );
    ResourcePolicy Policy;
    Profile.setUrlRequestInterceptor( &Policy );
    QObject::connect( &Profile, &QWebEngineProfile::downloadRequested, []( QWebEngineDownloadRequest* Download ) { Download->cancel(); } );

    QWebEngineView View;
    View.resize( 800, 600 );
    SurfaceResize Resize( View );
    App.installNativeEventFilter( &Resize );
    auto* Escape = new QShortcut( QKeySequence( Qt::Key_Escape ), &View );
    Escape->setContext( Qt::WindowShortcut );
    QObject::connect( Escape, &QShortcut::activated, &App, [ & ] { Send( { { "type", "escape" } } ); } );
    View.setWindowFlags( Qt::FramelessWindowHint | Qt::Tool );
    View.setAttribute( Qt::WA_ShowWithoutActivating );

    auto* Page = new PreviewPage( &Profile, &Policy, &View );
    View.setPage( Page );
    auto* Settings = Page->settings();
    // 미리보기를 다시 읽어도 소스 편집기의 입력을 가져가지 않는다.
    Settings->setAttribute( QWebEngineSettings::FocusOnNavigationEnabled, false );
    Settings->setAttribute( QWebEngineSettings::LocalContentCanAccessFileUrls, true );
    Settings->setAttribute( QWebEngineSettings::LocalContentCanAccessRemoteUrls, false );
    Settings->setAttribute( QWebEngineSettings::JavascriptCanOpenWindows, false );
    Settings->setAttribute( QWebEngineSettings::JavascriptCanAccessClipboard, false );
    Settings->setAttribute( QWebEngineSettings::PluginsEnabled, false );

    hc::rst::PreviewBridge Bridge;
    Bridge.AttachTo( Page );

    // ── 페이지 → 호스트 ──
    QObject::connect( &Bridge, &hc::rst::PreviewBridge::BridgeReady, &App, [ & ] { Send( { { "type", "bridgeReady" } } ); } );
    QObject::connect( &Bridge, &hc::rst::PreviewBridge::PreviewScrollChanged, &App,
        [ & ]( const int Source, const double Line, const double Ratio, const bool User ) {
            Send( { { "type", "scrolled" }, { "src", Source }, { "line", Line }, { "ratio", Ratio }, { "user", User } } );
        } );
    QObject::connect( &Bridge, &hc::rst::PreviewBridge::EditorNavigationRequested, &App,
        [ & ]( const int Source, const double Line, const double Ratio ) {
            Send( { { "type", "clicked" }, { "src", Source }, { "line", Line }, { "ratio", Ratio } } );
        } );
    QObject::connect( &Bridge, &hc::rst::PreviewBridge::HotSwapCompleted, &App,
        [ & ]( const int Token, const bool Ok, const QString& Message ) {
            Send( { { "type", "hotSwapResult" }, { "token", Token }, { "ok", Ok }, { "message", Message } } );
        } );
    QObject::connect( Page, &PreviewPage::LinkActivated, &App, [ & ]( const QUrl& Url ) {
        Send( { { "type", "link" }, { "url", Url.toString() } } );
    } );
    // target=_blank 와 window.open: 새 창을 만들지 않고 주소만 넘긴다.
    QObject::connect( Page, &QWebEnginePage::newWindowRequested, &App, [ & ]( QWebEngineNewWindowRequest& Request ) {
        Send( { { "type", "link" }, { "url", Request.requestedUrl().toString() } } );
    } );
    QObject::connect( Page, &QWebEnginePage::loadStarted, &App, [ & ] { Bridge.ResetReady(); } );
    QObject::connect( Page, &QWebEnginePage::loadProgress, &App, [ & ]( const int Percent ) {
        Send( { { "type", "loadProgress" }, { "percent", Percent } } );
    } );
    QObject::connect( Page, &QWebEnginePage::loadFinished, &App, [ & ]( const bool Ok ) {
        Send( { { "type", "loadFinished" }, { "ok", Ok }, { "url", Page->url().toString() } } );
    } );
    QObject::connect( Page, &QWebEnginePage::urlChanged, &App, [ & ]( const QUrl& Url ) {
        Send( { { "type", "url" }, { "url", Url.toString() } } );
    } );
    QObject::connect( Page, &QWebEnginePage::renderProcessTerminated, &App, [ & ] {
        Send( { { "type", "error" }, { "message", QObject::tr( "미리보기 프로세스가 종료되었습니다. 다시 빌드로 다시 시도하세요." ) } } );
    } );

    // 메뉴는 호스트가 그린다. 누른 자리의 원본 줄을 페이지에 물어 함께 보낸다.
    View.setContextMenuPolicy( Qt::CustomContextMenu );
    QObject::connect( &View, &QWidget::customContextMenuRequested, &App, [ & ]( const QPoint& Position ) {
        const auto* Request = View.lastContextMenuRequest();
        const double Zoom = qMax( .1, View.zoomFactor() );
        QJsonObject Report{ { "type", "context" },
                            { "x", Position.x() / Zoom },
                            { "y", Position.y() / Zoom },
                            { "selection", Request ? Request->selectedText().left( 1024 ) : QString() },
                            { "link", Request ? Request->linkUrl().toString() : QString() },
                            { "src", -1 },
                            { "line", 0 } };
        Page->runJavaScript( QStringLiteral( "window.__mrrHitAt ? window.__mrrHitAt(%1) : null" ).arg( Position.y() / Zoom ),
            QWebEngineScript::MainWorld, [ &, Report ]( const QVariant& Hit ) mutable {
                const auto Map = Hit.toMap();
                if( !Map.isEmpty() ) { Report[ "src" ] = Map.value( "src" ).toInt(); Report[ "line" ] = Map.value( "line" ).toDouble(); }
                Send( Report );
            } );
    } );

    // ── 호스트 → 페이지 ──
    const QString FontSource = resourceText( QStringLiteral( ":/rst/mrr_fonts.js" ) );
    auto Dispatch = [ & ]( const QJsonObject& Message ) {
        const auto Type = Message[ "type" ].toString();
        if( Type == "attached" ) { Attached = true; View.show(); return; }
        if( Type == "shortcuts" ) {
            for( auto* Shortcut : View.findChildren< QShortcut* >() )
                if( Shortcut != Escape ) delete Shortcut;
            const auto Bindings = Message[ "bindings" ].toObject();
            for( auto At = Bindings.begin(); At != Bindings.end(); ++At ) {
                const auto Key = QKeySequence::fromString( At.value().toString(), QKeySequence::PortableText );
                if( Key.isEmpty() ) continue;
                auto* Shortcut = new QShortcut( Key, &View );
                Shortcut->setContext( Qt::WindowShortcut );
                QObject::connect( Shortcut, &QShortcut::activated, &App, [ &, Id = At.key() ] { Send( { { "type", "command" }, { "id", Id } } ); } );
            }
            return;
        }
        if( Type == "settings" ) {
            const bool Remote = Message[ "allowRemote" ].toBool();
            Policy.SetRemote( Remote );
            Settings->setAttribute( QWebEngineSettings::LocalContentCanAccessRemoteUrls, Remote );
            if( Message.contains( "background" ) ) Page->setBackgroundColor( QColor( Message[ "background" ].toString() ) );
            if( Message.contains( "fonts" ) && !FontSource.isEmpty() )
                applyPageScript( Page, QStringLiteral( "mrr_preview_fonts" ),
                    FontSource + QStringLiteral( "\nwindow.__mrrPreviewFonts.apply(%1);" )
                        .arg( QString::fromUtf8( QJsonDocument( Message[ "fonts" ].toObject() ).toJson( QJsonDocument::Compact ) ) ) );
            if( Message.contains( "unlimitedWidth" ) )
                applyPageScript( Page, QStringLiteral( "mrr_preview_css" ), widthScript( Message[ "unlimitedWidth" ].toBool() ) );
            return;
        }
        if( Type == "load" ) {
            const QUrl Url( Message[ "url" ].toString() );
            Policy.Set( Message[ "root" ].toString(), Message[ "allowRemote" ].toBool() );
            Settings->setAttribute( QWebEngineSettings::LocalContentCanAccessRemoteUrls, Message[ "allowRemote" ].toBool() );
            if( !Policy.AllowsFile( Url ) ) {
                Send( { { "type", "loadFinished" }, { "ok", false }, { "url", Url.toString() } } );
                return;
            }
            Bridge.ResetReady();
            Page->load( Url );
            return;
        }
        if( Type == "scrollTo" ) { Bridge.RequestScrollToLine( Message[ "src" ].toInt(), Message[ "line" ].toDouble( 1 ), Message[ "ratio" ].toDouble( .5 ) ); return; }
        if( Type == "suppress" ) { Bridge.SuppressScrollFeedback( Message[ "ms" ].toInt() ); return; }
        if( Type == "hotSwap" ) {
            Bridge.RequestHotSwap( Message[ "html" ].toString(), Message[ "baseUrl" ].toString(), Message[ "allowMorph" ].toBool( true ), Message[ "token" ].toInt() );
            return;
        }
        if( Type == "probe" ) {
            // 창의 ratio 높이에 놓인 원본 줄과 스크롤 위치. 스크롤 동기화는 눈으로 확인하기 어려워 시험이 이것으로 잰다.
            const double Ratio = qBound( 0., Message[ "ratio" ].toDouble( .5 ), 1. );
            Page->runJavaScript( QStringLiteral( "(function(){var h=window.__mrrHitAt?window.__mrrHitAt(window.innerHeight*%1):null;"
                                                 "return {src:h?h.src:-1,line:h?h.line:0,scrollY:window.scrollY,height:window.innerHeight,"
                                                 "total:document.documentElement.scrollHeight,ready:!!window.__mrrHitAt};})()" ).arg( Ratio ),
                QWebEngineScript::MainWorld, [ & ]( const QVariant& Result ) {
                    auto Report = QJsonObject::fromVariantMap( Result.toMap() );
                    Report[ "type" ] = "probe";
                    Send( Report );
                } );
            return;
        }
        if( Type == "zoom" ) { View.setZoomFactor( qBound( .5, Message[ "value" ].toDouble( 1 ), 3. ) ); return; }
        if( Type == "find" ) { Page->findText( Message[ "text" ].toString() ); return; }
        if( Type == "copy" ) { Page->triggerAction( QWebEnginePage::Copy ); return; }
        if( Type == "selectAll" ) { Page->triggerAction( QWebEnginePage::SelectAll ); return; }
        if( Type == "focus" ) { View.activateWindow(); View.setFocus( Qt::OtherFocusReason ); return; }
    };

    QObject::connect( &Socket, &QLocalSocket::connected, &App, [ & ] {
        Send( { { "type", "ready" }, { "window", QString::number( quintptr( View.winId() ) ) }, { "token", Args[ 2 ] } } );
    } );
    QObject::connect( &Socket, &QLocalSocket::disconnected, &App, &QApplication::quit );
    QObject::connect( &Socket, &QLocalSocket::errorOccurred, &App, [ & ] { App.exit( 3 ); } );
    QObject::connect( &Socket, &QLocalSocket::readyRead, &App, [ & ] {
        Input += Socket.readAll();
        // 핫스왑은 HTML 한 벌을 싣는다( 호스트가 4 MiB 로 제한한다 ).
        if( Input.size() > 32 * 1024 * 1024 ) { App.exit( 4 ); return; }
        while( Input.contains( '\n' ) ) {
            const auto End = Input.indexOf( '\n' );
            const auto Message = QJsonDocument::fromJson( Input.left( End ) ).object();
            Input.remove( 0, End + 1 );
            if( Message[ "type" ] == "close" ) { App.quit(); return; }
            Dispatch( Message );
        }
    } );
    // 창 핸들은 첫 페이지가 뜬 뒤에 알린다. 페이지의 렌더 위젯이 처음 붙을 때 Qt 가 최상위 창을 다시
    // 만들기 때문이다 — 그 전에 알린 핸들을 호스트가 자기 창에 붙여도, 곧 없어지고 새 창이 따로 뜬다.
    bool Started = false;
    QObject::connect( Page, &QWebEnginePage::loadFinished, &App, [ & ] {
        if( Started ) return;
        Started = true;
        Socket.connectToServer( Args[ 1 ] );
    } );
    Page->setHtml( QStringLiteral( "<!doctype html><html><head><meta charset=\"utf-8\"></head><body></body></html>" ), QUrl( QStringLiteral( "about:blank" ) ) );
    // 호스트가 창을 받아 가지 않으면 남지 않는다.
    QTimer::singleShot( 15000, &App, [ & ] { if( !Attached ) App.exit( 5 ); } );
    return App.exec();
}

#include "main.moc"
