#pragma once

/// 모듈
#include "rst/rstPreviewSurface.hpp"

/// UIs

/// 기타 헤더파일

/// 고유 라이브러리

/// 공통 라이브러리

/// 외부 라이브러리

/// 선언 헤더파일들

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QJsonObject>
#include <QPointer>
#include <QString>
#include <QUrl>

class QWidget;

namespace hc::document
{
    class MarkdownProcessView;

    /// reST 미리보기 화면: 별도 프로세스( hc-rst-preview.exe )의 창을 붙이고 메시지를 주고받는다.
    ///
    /// hc::rst::PreviewSurface 의 구현이다. 조율자( hc::rst::WorkspaceController )는 이 인터페이스로만
    /// 페이지를 띄우고 스크롤을 맞춘다. 그 밖의 것( 오른쪽 버튼 메뉴, 링크, 단축키, 배율, 찾기 )은 보기가
    /// 직접 쓴다.
    ///
    /// 프로세스는 처음 메시지를 보낼 때 뜬다. Widget() 을 레이아웃에 넣어 쓴다.
    class RstPreviewProcess final : public hc::rst::PreviewSurface
    {
        Q_OBJECT

    public:
        explicit RstPreviewProcess( QWidget* Parent );
        ~RstPreviewProcess() override;

        /// 프로세스의 창이 붙는 위젯.
        [[nodiscard]] QWidget*          Widget() const;
        [[nodiscard]] bool              HasFocus() const;
        void                            FocusInput();
        /// 프로세스를 내리고 다시 띄운다. 마지막 설정과 페이지를 다시 보낸다.
        void                            Restart();

        void                            SetZoom( double Factor );
        void                            SetShortcuts( const QJsonObject& Bindings );
        /// 페이지가 뜨기 전의 바탕색( 창 테마 ).
        void                            SetBackground( const QString& Color );
        void                            Find( const QString& Text );
        /// 창의 Ratio( 0 – 1 ) 높이에 놓인 원본 줄과 스크롤 위치를 묻는다. 답은 Probed() 로 온다( 시험 · 진단용 ).
        void                            Probe( double Ratio );
        /// 조율자가 마지막으로 띄운 페이지. 링크를 눌러 다른 페이지로 넘어가도 바뀌지 않는다.
        [[nodiscard]] QUrl              RequestedUrl() const { return requestedUrl_; }
        /// 링크를 눌러 다른 페이지로 넘어가 있는가( 같은 페이지 안의 이동은 아니다 ).
        [[nodiscard]] bool              IsOtherPage() const;
        /// 조율자가 띄운 페이지로 돌아간다.
        void                            ReturnToRequested();
        void                            Copy();
        void                            SelectAll();

        // hc::rst::PreviewSurface
        void                            load( const QUrl& Url ) override;
        [[nodiscard]] QUrl              url() const override { return url_; }
        [[nodiscard]] bool              isReady() const override { return ready_; }
        void                            resetReady() override { ready_ = false; }
        void                            requestScrollToLine( int SourceIndex, double Line, double Ratio ) override;
        void                            suppressScrollFeedback( int Milliseconds ) override;
        void                            requestHotSwap( const QString& DocumentHtml, const QString& BaseUrl,
                                                        bool AllowMorph, int Token ) override;
        void                            setAllowRemoteContent( bool Allow ) override;
        void                            setAppearance( const QJsonObject& Fonts, bool UnlimitedWidth ) override;

    signals:
        /// 오른쪽 버튼: x · y( CSS px ), selection, link, src, line.
        void                            ContextRequested( const QJsonObject& Report );
        /// 이 창에서 열지 않는 링크를 눌렀다.
        void                            LinkActivated( const QUrl& Url );
        void                            EscapePressed();
        /// 미리보기에 포커스가 있을 때 눌린 단축키의 명령 id.
        void                            CommandRequested( const QString& Id );
        void                            Failed( const QString& Message );
        /// Probe() 의 답: src · line · scrollY · height · total( CSS px ) · ready.
        void                            Probed( const QJsonObject& Report );
        /// 보이는 페이지의 주소가 바뀌었다( 조율자의 로드 · 링크 이동 ).
        void                            PageChanged();

    private:
        void                            report( const QJsonObject& Message );
        void                            sendSettings();

        QPointer< MarkdownProcessView > view_;
        QUrl                            url_, requestedUrl_;
        QJsonObject                     fonts_;
        QString                         background_;
        bool                            ready_ = false;
        bool                            allowRemote_ = false;
        bool                            unlimitedWidth_ = false;
    };
}
