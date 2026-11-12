#pragma once

/// 모듈

/// UIs

/// 기타 헤더파일

/// 고유 라이브러리

/// 공통 라이브러리

/// 외부 라이브러리

/// 선언 헤더파일들

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QObject>
#include <QString>

class QWebEnginePage;

namespace hc::rst {

/// 미리보기 페이지 안의 스크립트( mrr_preview.js )와 이 프로세스를 잇는 QWebChannel 객체.
///
/// 페이지에는 `bridge` 라는 이름으로 보인다. 페이지가 호출하는 것은 슬롯, 페이지가 듣는 것은
/// "→ 페이지" 신호다. 호스트( HyperEditor )와의 통신은 main.cpp 가 맡는다 — 이 클래스는 페이지 쪽만 안다.
///
/// MultiRoot reST Editor 의 PreviewBridge 를 옮긴 것이다. Markdown 렌더러용 신호는 뺐다.
class PreviewBridge final : public QObject
{
    Q_OBJECT

public:
    explicit PreviewBridge( QObject* Parent = nullptr );

    /// 페이지에 채널을 걸고 qwebchannel.js 와 mrr_preview.js 를 주입 스크립트로 넣는다.
    void                                AttachTo( QWebEnginePage* Page );

    [[nodiscard]] bool                  IsReady() const { return ready_; }
    /// 새 페이지를 띄우기 직전에 부른다. 그 페이지의 스크립트가 ready() 를 부를 때까지 요청을 미룬다.
    void                                ResetReady() { ready_ = false; }

    /// 준비 전이면 마지막 요청 하나를 기억했다가 준비되면 보낸다.
    void                                RequestScrollToLine( int SourceIndex, double Line, double Ratio );
    void                                SuppressScrollFeedback( int Milliseconds );
    void                                RequestHotSwap( const QString& DocumentHtml, const QString& BaseUrl,
                                                        bool AllowMorph, int Token );

public slots:   // 페이지가 부른다
    void                                ready( int ProtocolVersion );
    void                                sourceLocationClicked( int SourceIndex, double Line, double ViewportRatio );
    void                                previewScrolled( int SourceIndex, double Line, double ViewportRatio, bool UserDriven );
    void                                hotSwapResult( int Token, bool Ok, const QString& Message );

signals:
    // → 페이지
    void                                scrollToLineRequested( int SourceIndex, double Line, double Ratio );
    void                                rebindRequested();
    void                                scrollFeedbackSuppressed( int Milliseconds );
    void                                hotSwapRequested( const QString& DocumentHtml, const QString& BaseUrl,
                                                          bool AllowMorph, int Token );

    // → 이 프로세스
    void                                BridgeReady();
    void                                HotSwapCompleted( int Token, bool Ok, const QString& Message );
    void                                EditorNavigationRequested( int SourceIndex, double Line, double ViewportRatio );
    void                                PreviewScrollChanged( int SourceIndex, double Line, double ViewportRatio, bool UserDriven );

private:
    bool                                ready_ = false;
    bool                                hasPendingScroll_ = false;
    int                                 pendingSourceIndex_ = 0;
    double                              pendingLine_ = 1.0;
    double                              pendingRatio_ = 0.5;
};

}  // namespace hc::rst
