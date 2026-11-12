#include "stdafx.h"
#include "previewBridge.hpp"

/// 모듈

/// UIs

/// 기타 헤더파일

/// 고유 라이브러리

/// 공통 라이브러리

/// 외부 라이브러리

/// 선언 헤더파일들

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QFile>
#include <QWebChannel>
#include <QWebEnginePage>
#include <QWebEngineScript>
#include <QWebEngineScriptCollection>

namespace hc::rst {
namespace {

QString readResourceText( const QString& Path )
{
    QFile File( Path );
    return File.open( QIODevice::ReadOnly ) ? QString::fromUtf8( File.readAll() ) : QString{};
}

void insertScript( QWebEnginePage* Page, const QString& Name, const QString& Source,
                   const QWebEngineScript::InjectionPoint Point )
{
    if( Source.isEmpty() ) return;
    QWebEngineScript Script;
    Script.setName( Name );
    Script.setInjectionPoint( Point );
    // 페이지의 스크립트와 같은 세계여야 한다: sphinxcontrib-mermaid 의 window.runMermaid 를 부르고,
    // 페이지가 심은 전역을 읽는다.
    Script.setWorldId( QWebEngineScript::MainWorld );
    Script.setRunsOnSubFrames( false );
    Script.setSourceCode( Source );
    Page->scripts().insert( Script );
}

}  // namespace

PreviewBridge::PreviewBridge( QObject* Parent )
    : QObject( Parent )
{
}

void PreviewBridge::AttachTo( QWebEnginePage* Page )
{
    if( Page == nullptr ) return;
    auto* Channel = new QWebChannel( this );
    Channel->registerObject( QStringLiteral( "bridge" ), this );
    Page->setWebChannel( Channel );
    // 전송 계층은 문서가 만들어질 때, 동기화 스크립트는 DOM 이 선 뒤에 넣는다.
    insertScript( Page, QStringLiteral( "mrr_qwebchannel" ), readResourceText( QStringLiteral( ":/qtwebchannel/qwebchannel.js" ) ),
                  QWebEngineScript::DocumentCreation );
    insertScript( Page, QStringLiteral( "mrr_preview" ), readResourceText( QStringLiteral( ":/rst/mrr_preview.js" ) ),
                  QWebEngineScript::DocumentReady );
}

void PreviewBridge::RequestScrollToLine( const int SourceIndex, const double Line, const double Ratio )
{
    if( !ready_ ) {
        hasPendingScroll_ = true;
        pendingSourceIndex_ = SourceIndex;
        pendingLine_ = Line;
        pendingRatio_ = Ratio;
        return;
    }
    emit scrollToLineRequested( SourceIndex, Line, Ratio );
}

void PreviewBridge::SuppressScrollFeedback( const int Milliseconds )
{
    if( ready_ ) emit scrollFeedbackSuppressed( Milliseconds );
}

void PreviewBridge::RequestHotSwap( const QString& DocumentHtml, const QString& BaseUrl, const bool AllowMorph, const int Token )
{
    if( !ready_ ) {
        // 준비되지 않은 페이지는 갈아 끼울 수 없다. 호스트가 전체 로드로 물러서게 실패로 답한다.
        emit HotSwapCompleted( Token, false, QStringLiteral( "page not ready" ) );
        return;
    }
    emit hotSwapRequested( DocumentHtml, BaseUrl, AllowMorph, Token );
}

void PreviewBridge::ready( int /*ProtocolVersion*/ )
{
    ready_ = true;
    emit BridgeReady();
    if( hasPendingScroll_ ) {
        hasPendingScroll_ = false;
        emit scrollToLineRequested( pendingSourceIndex_, pendingLine_, pendingRatio_ );
    }
}

void PreviewBridge::sourceLocationClicked( const int SourceIndex, const double Line, const double ViewportRatio )
{
    emit EditorNavigationRequested( SourceIndex, Line, ViewportRatio );
}

void PreviewBridge::previewScrolled( const int SourceIndex, const double Line, const double ViewportRatio, const bool UserDriven )
{
    emit PreviewScrollChanged( SourceIndex, Line, ViewportRatio, UserDriven );
}

void PreviewBridge::hotSwapResult( const int Token, const bool Ok, const QString& Message )
{
    emit HotSwapCompleted( Token, Ok, Message );
}

}  // namespace hc::rst
