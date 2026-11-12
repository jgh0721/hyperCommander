#include "stdafx.h"
#include "rstPreviewProcess.hpp"

/// 모듈
#include "markdownProcessView.hpp"

/// UIs

/// 기타 헤더파일

/// 고유 라이브러리

/// 공통 라이브러리

/// 외부 라이브러리

/// 선언 헤더파일들

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QDir>
#include <QFileInfo>

DECLARE_CMNLIBSV2_NAMESPACE

namespace hc::document
{
    namespace
    {
        /// 페이지가 읽을 수 있는 뿌리 폴더. 미리보기 출력은 `<빌드 폴더>/preview-html/html/…` 아래에 놓인다.
        /// 그 `preview-html` 을 뿌리로 삼고, 그런 조상이 없으면 파일이 있는 폴더다.
        /// 조각( #... )과 질의를 뺀 파일 경로가 같은가.
        bool SamePage( const QUrl& Left, const QUrl& Right )
        {
            if( !Left.isLocalFile() || !Right.isLocalFile() ) return Left.adjusted( QUrl::RemoveFragment ) == Right.adjusted( QUrl::RemoveFragment );
            return QDir::cleanPath( Left.toLocalFile() ).compare( QDir::cleanPath( Right.toLocalFile() ), Qt::CaseInsensitive ) == 0;
        }

        QString RootFor( const QUrl& Url )
        {
            const QFileInfo File( Url.toLocalFile() );
            QDir Directory = File.absoluteDir();
            for( int Depth = 0; Depth < 32; ++Depth ) {
                if( Directory.dirName().compare( QLatin1String( "preview-html" ), Qt::CaseInsensitive ) == 0 ) return Directory.absolutePath();
                if( !Directory.cdUp() ) break;
            }
            return File.absolutePath();
        }
    }

    RstPreviewProcess::RstPreviewProcess( QWidget* Parent )
        : hc::rst::PreviewSurface( Parent )
    {
        // 프로세스가 다시 붙으면 설정, 페이지, 단축키, 배율 순으로 다시 보낸다.
        view_ = new MarkdownProcessView( QStringLiteral( "hc-rst-preview" ), { "settings", "load", "shortcuts", "zoom" }, Parent );
        view_->setObjectName( "rstPreview" );
        connect( view_, &MarkdownProcessView::Reported, this, &RstPreviewProcess::report );
    }

    RstPreviewProcess::~RstPreviewProcess() = default;

    QWidget* RstPreviewProcess::Widget() const { return view_; }
    bool RstPreviewProcess::HasFocus() const { return !view_.isNull() && view_->HasFocus(); }
    void RstPreviewProcess::FocusInput() { if( !view_.isNull() ) view_->FocusInput(); }

    void RstPreviewProcess::Restart()
    {
        ready_ = false;
        if( !view_.isNull() ) view_->Restart();
    }

    void RstPreviewProcess::SetZoom( const double Factor ) { if( !view_.isNull() ) view_->Send( { { "type", "zoom" }, { "value", Factor } } ); }
    void RstPreviewProcess::SetShortcuts( const QJsonObject& Bindings ) { if( !view_.isNull() ) view_->Send( { { "type", "shortcuts" }, { "bindings", Bindings } } ); }
    void RstPreviewProcess::Find( const QString& Text ) { if( !view_.isNull() ) view_->Send( { { "type", "find" }, { "text", Text } } ); }
    void RstPreviewProcess::Probe( const double Ratio ) { if( !view_.isNull() ) view_->Send( { { "type", "probe" }, { "ratio", Ratio } } ); }
    void RstPreviewProcess::Copy() { if( !view_.isNull() ) view_->Send( { { "type", "copy" } } ); }
    void RstPreviewProcess::SelectAll() { if( !view_.isNull() ) view_->Send( { { "type", "selectAll" } } ); }

    bool RstPreviewProcess::IsOtherPage() const
    {
        return requestedUrl_.isValid() && url_.isValid() && !SamePage( requestedUrl_, url_ );
    }

    void RstPreviewProcess::ReturnToRequested()
    {
        if( requestedUrl_.isValid() ) load( requestedUrl_ );
    }

    void RstPreviewProcess::SetBackground( const QString& Color )
    {
        if( background_ == Color ) return;
        background_ = Color;
        sendSettings();
    }

    void RstPreviewProcess::sendSettings()
    {
        if( view_.isNull() ) return;
        QJsonObject Message{ { "type", "settings" }, { "allowRemote", allowRemote_ }, { "unlimitedWidth", unlimitedWidth_ } };
        if( !fonts_.isEmpty() ) Message[ "fonts" ] = fonts_;
        if( !background_.isEmpty() ) Message[ "background" ] = background_;
        view_->Send( Message );
    }

    void RstPreviewProcess::load( const QUrl& Url )
    {
        if( view_.isNull() ) return;
        ready_ = false;
        requestedUrl_ = Url;
        view_->Send( { { "type", "load" }, { "url", Url.toString() }, { "root", RootFor( Url ) }, { "allowRemote", allowRemote_ } } );
    }

    void RstPreviewProcess::requestScrollToLine( const int SourceIndex, const double Line, const double Ratio )
    {
        // 링크로 넘어간 다른 문서의 페이지는 이 편집기의 줄과 상관이 없다. 그 페이지를 움직이지 않는다.
        if( IsOtherPage() ) return;
        // 준비 전의 요청은 프로세스가 마지막 하나를 기억했다가 페이지가 준비되면 보낸다.
        if( !view_.isNull() ) view_->Send( { { "type", "scrollTo" }, { "src", SourceIndex }, { "line", Line }, { "ratio", Ratio } } );
    }

    void RstPreviewProcess::suppressScrollFeedback( const int Milliseconds )
    {
        if( !view_.isNull() ) view_->Send( { { "type", "suppress" }, { "ms", Milliseconds } } );
    }

    void RstPreviewProcess::requestHotSwap( const QString& DocumentHtml, const QString& BaseUrl, const bool AllowMorph, const int Token )
    {
        if( view_.isNull() ) return;
        view_->Send( { { "type", "hotSwap" }, { "html", DocumentHtml }, { "baseUrl", BaseUrl }, { "allowMorph", AllowMorph }, { "token", Token } } );
    }

    void RstPreviewProcess::setAllowRemoteContent( const bool Allow )
    {
        allowRemote_ = Allow;
        sendSettings();
    }

    void RstPreviewProcess::setAppearance( const QJsonObject& Fonts, const bool UnlimitedWidth )
    {
        if( fonts_ == Fonts && unlimitedWidth_ == UnlimitedWidth ) return;
        fonts_ = Fonts;
        unlimitedWidth_ = UnlimitedWidth;
        sendSettings();
    }

    void RstPreviewProcess::report( const QJsonObject& Message )
    {
        const auto Type = Message[ "type" ].toString();
        if( Type == "loadProgress" ) emit loadProgress( Message[ "percent" ].toInt() );
        else if( Type == "loadFinished" ) {
            const QUrl Before = std::exchange( url_, QUrl( Message[ "url" ].toString() ) );
            emit loadFinished( Message[ "ok" ].toBool() );
            if( Before != url_ ) emit PageChanged();
        }
        else if( Type == "url" ) {
            const QUrl Before = std::exchange( url_, QUrl( Message[ "url" ].toString() ) );
            if( Before != url_ ) emit PageChanged();
        }
        else if( Type == "bridgeReady" ) { ready_ = true; emit bridgeReady(); }
        else if( Type == "scrolled" ) emit previewScrollChanged( Message[ "src" ].toInt(), Message[ "line" ].toDouble(), Message[ "ratio" ].toDouble(), Message[ "user" ].toBool() );
        else if( Type == "clicked" ) emit editorNavigationRequested( Message[ "src" ].toInt(), Message[ "line" ].toDouble(), Message[ "ratio" ].toDouble() );
        else if( Type == "hotSwapResult" ) emit hotSwapCompleted( Message[ "token" ].toInt(), Message[ "ok" ].toBool(), Message[ "message" ].toString() );
        else if( Type == "context" ) emit ContextRequested( Message );
        else if( Type == "link" ) emit LinkActivated( QUrl( Message[ "url" ].toString() ) );
        else if( Type == "escape" ) emit EscapePressed();
        else if( Type == "probe" ) emit Probed( Message );
        else if( Type == "command" ) emit CommandRequested( Message[ "id" ].toString() );
        else if( Type == "error" ) {
            // 프로세스가 죽었다. 떠 있던 페이지는 없다 — 조율자가 다시 띄울 수 있게 실패한 로드로 알린다.
            ready_ = false;
            url_.clear();
            emit PageChanged();
            emit loadFinished( false );
            emit Failed( Message[ "message" ].toString() );
        }
    }
}
