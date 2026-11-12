#pragma once

/// 모듈

/// UIs

/// 기타 헤더파일

/// 고유 라이브러리

/// 공통 라이브러리

/// 외부 라이브러리

/// 선언 헤더파일들

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QUrl>

namespace hc::rst {

/// Sphinx 가 만든 HTML 을 보여 주는 화면. WorkspaceController 가 이것을 통해 페이지를 띄우고 스크롤을 맞춘다.
///
/// MultiRoot reST Editor 는 창 하나에 QWebEngineView 하나와 QWebChannel 브리지를 두었다. HyperEditor 는
/// 웹 콘텐츠를 문서 탭마다 별도 프로세스( hc-rst-preview.exe )에 둔다. 그래서 그 둘( 뷰 + 브리지 )의 표면만
/// 여기 남기고, 구현( hc::document::RstPreviewSurface )이 메시지를 프로세스로 건넨다.
///
/// 활성 문서가 바뀌면 컨트롤러는 그 문서의 화면으로 갈아탄다. 페이지별 상태는 컨트롤러가 화면마다 따로 둔다.
class PreviewSurface : public QObject
{
    Q_OBJECT

public:
    using QObject::QObject;

    /// 페이지를 띄운다. 로컬 파일( Sphinx 출력 )만 받는다. 끝나면 loadFinished().
    virtual void                        load( const QUrl& Url ) = 0;
    /// 지금 떠 있는 페이지의 주소. 사용자가 미리보기 안에서 링크로 옮겨 갔으면 load() 로 준 것과 다르다.
    [[nodiscard]] virtual QUrl          url() const = 0;

    /// 페이지의 동기화 스크립트가 준비되었는가.
    [[nodiscard]] virtual bool          isReady() const = 0;
    /// 새 페이지를 띄우기 직전에 부른다. 준비 신호를 다시 기다린다.
    virtual void                        resetReady() = 0;
    /// 원본 SourceIndex 의 Line( 소수 )이 창 높이의 Ratio 자리에 오게 한다. 준비 전이면 마지막 요청 하나를 기억했다
    /// 준비되면 보낸다.
    virtual void                        requestScrollToLine( int SourceIndex, double Line, double Ratio ) = 0;
    /// 이 시간 동안 페이지가 스크롤 위치를 보고하지 않게 한다( 우리가 굴린 것이 되돌아오지 않게 ).
    virtual void                        suppressScrollFeedback( int Milliseconds ) = 0;
    /// 같은 문서를 다시 빌드했고 <head> 가 그대로일 때: 본문만 갈아 끼운다. 결과는 hotSwapCompleted().
    virtual void                        requestHotSwap( const QString& DocumentHtml, const QString& BaseUrl,
                                                        bool AllowMorph, int Token ) = 0;

    /// 페이지가 원격 주소( CDN 의 스크립트·스타일·이미지 )를 읽게 할지.
    virtual void                        setAllowRemoteContent( bool Allow ) = 0;
    /// 미리보기 글꼴( mrr_fonts.js 의 apply() 인자 )과 본문 너비 제한 해제.
    virtual void                        setAppearance( const QJsonObject& Fonts, bool UnlimitedWidth ) = 0;

signals:
    void                                loadProgress( int Percent );
    void                                loadFinished( bool Ok );
    void                                bridgeReady();
    /// 페이지가 스크롤되었다. UserDriven 이 거짓이면 페이지가 스스로 움직인 것이다( 이미지·글꼴이 자리를 잡는 중 ).
    void                                previewScrollChanged( int SourceIndex, double Line, double ViewportRatio, bool UserDriven );
    /// 본문을 눌렀다: 그 원본 줄로 편집기를 옮긴다.
    void                                editorNavigationRequested( int SourceIndex, double Line, double ViewportRatio );
    void                                hotSwapCompleted( int Token, bool Ok, const QString& Message );
};

}  // namespace hc::rst
