#pragma once

/// 모듈
#include "documentStatusBar.hpp"
#include "documentToolPanel.hpp"
#include "rst/rstOutlineService.hpp"
#include "rst/sphinxDiagnostics.hpp"

/// UIs

/// 기타 헤더파일

/// 고유 라이브러리

/// 공통 라이브러리

/// 외부 라이브러리

/// 선언 헤더파일들

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QJsonObject>
#include <QJsonValue>
#include <QMap>
#include <QPointer>
#include <QStringList>
#include <QTimer>
#include <QVector>
#include <QWidget>

#include <functional>

class QAction;
class QFrame;
class QLabel;
class QMenu;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QSplitter;
class QStackedWidget;
class QTabWidget;
class QTextView;
class QToolBar;
class QTreeWidget;
class QTreeWidgetItem;

namespace fm::ui
{
    class Banner;
    class Card;
}

namespace hc::rst
{
    class RstEditor;
    class WorkspaceController;
}

namespace hc::document
{
    class RstPreviewProcess;

    /// reStructuredText 문서 보기: 개요 | 소스 | 미리보기, 아래에 진단 · 빌드 로그.
    ///
    /// 텍스트 편집기( QTextView )를 감싼다. 같은 편집기가 계속 쓰이므로 다른 이름으로 저장해 확장자가 바뀌어도
    /// 내용 · 캐럿 · 실행 취소가 남는다( Markdown 보기와 같은 방식 ).
    ///
    /// 프로젝트 해석, 언어 서버, Sphinx 빌드는 프로세스에 하나인 조율자( hc::rst::RstWorkspace() )가 한다.
    /// 이 보기는 앞에 나올 때 자기를 그 조율자의 "활성 문서" 로 만들고, 조율자가 내는 결과 가운데 자기 것만
    /// 받아 그린다. 미리보기 화면( RstPreviewProcess )은 보기마다 따로 있다.
    class RstDocumentView final : public QWidget
    {
        Q_OBJECT

    public:
        static constexpr int MinZoomPercent = 50, MaxZoomPercent = 300;

        RstDocumentView( QTextView* Source, const QString& Path, bool Editor, QWidget* Parent = nullptr );
        ~RstDocumentView() override;

        /// 한 줄짜리 "rstToolbar". 소스가 보이는 동안에는 텍스트 편집기의 바꾸기 · 줄바꿈 · 공백 · 미니맵을 함께 싣는다.
        [[nodiscard]] QToolBar*         Toolbar() const { return toolbar_; }
        [[nodiscard]] bool              SourceVisible() const;
        [[nodiscard]] bool              PreviewVisible() const;
        [[nodiscard]] bool              CanRun( const QString& Id ) const;
        /// Value 는 rst.zoomSet( 퍼센트 )이 쓴다. 없으면 그 명령이 묻는다.
        bool                            Run( const QString& Id, const QJsonValue& Value = {} );
        /// 미리보기에 포커스가 있을 때의 edit.copy · edit.selectAll · text.find.
        bool                            RunPreviewCommand( const QString& Id );
        void                            ReloadPreferences();
        void                            SetShortcutBindings( const QJsonObject& Bindings );
        void                            SetSourceTools( QToolBar* Tools );
        void                            Focus();
        void                            SetReadOnly( bool ReadOnly );
        /// 문서의 경로가 정해지거나 바뀌었다( 저장 · 다른 이름으로 저장 ). 프로젝트를 다시 찾는다.
        void                            SetPath( const QString& Path );
        /// 저장이 끝났다: 언어 서버에 알리고 용어집 · 치환 · 경로 색인을 새로 한다.
        void                            NotifySaved();
        [[nodiscard]] QWidget*          InputWidget() const;
        [[nodiscard]] QList< DocumentStatusField > StatusFields() const;
        /// 상태 표시줄 칸( rstProject · rstDiag · rstEnv · rstzoom · rstPreview )의 피커를 Anchor 위에 연다.
        void                            OpenStatusPicker( const QString& Id, QWidget* Anchor, std::function< void() > Done );
        /// 소스 편집기의 오른쪽 버튼 메뉴 맨 위에 붙는 항목들( 참조로 이동 · 진단 · 미리보기에서 보기 · 자동 완성 ).
        void                            PrependSourceMenu( QMenu& Menu );
        [[nodiscard]] QJsonObject       State() const;

        [[nodiscard]] int               PreviewZoomPercent() const;
        void                            SetPreviewZoom( int Percent );
        /// 미리보기만 남긴다( 전체 화면 ). 끄면 들어가기 전의 보기 방식과 패널로 돌아간다.
        void                            SetPreviewOnly( bool On );
        /// 이 문서의 줄( 1부터 )로 간다. 소스가 가려져 있으면 나란히 보기로 바꾼다.
        void                            GoToLine( int Line );
        /// 명령을 실행한다. 켜고 끄는 명령은 단추를 누른 것처럼 상태를 바꾼다.
        void                            Trigger( const QString& Id );
        [[nodiscard]] bool              IsPreviewOnly() const { return previewOnly_; }
        /// 도킹 작업 공간: 개요 · 진단 · 빌드 로그 칸을 넘긴다( 한 번만 ). 넘긴 뒤 켜기 · 끄기는 도구 창을 연다 · 닫는다.
        QList< DocumentToolPanelItem >  TakeToolPanels();
        /// 옆에 미리보기 열기: 미리보기 칸을 넘긴다. 보기 방식은 나란히로 두고 바꾸지 못한다. ReturnPreview 로 되돌린다.
        QWidget*                        TakePreview();
        void                            ReturnPreview();
        [[nodiscard]] bool              IsPreviewTaken() const { return previewTaken_; }

    signals:
        void                            Changed();
        /// 넘긴 패널을 보여 달라( "outline" · "problems" · "output" ).
        void                            ToolPanelRequested( const QString& Kind );
        void                            OpenRequested( const QString& Path );
        /// 다른 파일을 열고 그 줄( 1부터 )로 간다.
        void                            OpenAtRequested( const QString& Path, int Line );
        void                            EscapeRequested();
        /// rst.fullScreen: 문서 창이 전체 화면을 켜고 끈다.
        void                            FullScreenToggleRequested();
        /// 창의 명령( 설정 · 새 Sphinx 프로젝트 등 )을 id 로 부른다.
        void                            WorkspaceCommandRequested( const QString& Id );

    protected:
        bool                            eventFilter( QObject* Watched, QEvent* Event ) override;
        void                            showEvent( QShowEvent* Event ) override;
        void                            hideEvent( QHideEvent* Event ) override;

    private:
        enum class Banner { None, MissingPackages, BuildFailed, PreviewStopped, OtherPage, Note };

        [[nodiscard]] bool              isActive() const;
        /// 조율자의 활성 문서를 이 보기로 한다. 보이지 않으면 아무것도 하지 않는다.
        void                            activate();
        void                            mode( const QString& Mode );
        void                            syncPanels();
        void                            saveToggle( const QString& Key, const QVariant& Value );
        void                            focusSource();
        void                            pollFocus();

        // 환경
        void                            ensureEnvironment();
        void                            refreshEnvironmentCard();
        [[nodiscard]] QString           environmentText() const;

        // 띠
        void                            showBanner( Banner Kind, const QString& Text, const QStringList& Buttons = {} );
        void                            hideBanner( Banner Kind );
        void                            bannerButton( int Index );

        // 개요
        void                            fillDocumentOutline();
        void                            fillProjectOutline();
        void                            addOutlineSymbols( QTreeWidgetItem* Parent, QTreeWidget* Tree,
                                                           const QVector< hc::rst::OutlineSymbol >& Symbols, int Depth );
        void                            activateOutlineItem( QTreeWidgetItem* Item );
        void                            showOutlineMenu( QTreeWidget* Tree, const QPoint& Position );
        void                            markCurrentOutlineItem();

        // 진단 · 로그
        void                            refreshDiagnostics();
        void                            showDiagnosticsMenu( const QPoint& Position );
        void                            showLogMenu( const QPoint& Position );
        void                            appendLog( const QString& Text );
        void                            goTo( const QString& Path, int Line );

        // 미리보기
        void                            showPreviewMenu( const QJsonObject& Report );
        void                            openLink( const QUrl& Url );
        /// 미리보기의 주소가 바뀌었다. 링크로 다른 문서의 페이지에 가 있으면 띠로 알린다.
        void                            pageChanged();
        /// 빌드된 HTML 페이지에 해당하는 원본 문서. 없으면 빈 문자열( 색인 · 검색 페이지 등 ).
        [[nodiscard]] QString           sourceForPage( const QString& HtmlPath ) const;
        void                            showLineInPreview( int Line );
        void                            setProblemsVisible( bool Visible, bool Remember );

        // 프로젝트
        void                            buildProject();
        void                            openProjectFile( const QString& Path );
        [[nodiscard]] QString           projectLabel() const;

        QTextView*                      source_;
        QString                         path_, mode_;
        bool                            editorHost_ = false, readOnly_ = false;
        hc::rst::WorkspaceController*   workspace_ = nullptr;
        hc::rst::RstEditor*             editor_ = nullptr;
        RstPreviewProcess*              preview_ = nullptr;

        QToolBar*                       toolbar_ = nullptr;
        QList< QAction* >               sourceActions_;
        QMap< QString, QAction* >       actions_;
        QJsonObject                     bindings_;

        QSplitter*                      splitter_ = nullptr;      // 개요 | 소스 | 미리보기
        QSplitter*                      vertical_ = nullptr;      // 위 | 진단 패널
        QWidget*                        outlinePanel_ = nullptr;
        QTabWidget*                     outlineTabs_ = nullptr;
        QTreeWidget*                    outlineDocument_ = nullptr;
        QTreeWidget*                    outlineProject_ = nullptr;
        QWidget*                        previewPanel_ = nullptr;
        QStackedWidget*                 previewStack_ = nullptr;
        fm::ui::Banner*                 banner_ = nullptr;
        QList< QPushButton* >           bannerButtons_;
        Banner                          bannerKind_ = Banner::None;
        QStringList                     missingPackages_, ignoredPackages_;
        QString                         otherPageSource_;
        QWidget*                        environmentCard_ = nullptr;
        fm::ui::Card*                   environmentBox_ = nullptr;
        QLabel*                         environmentText_ = nullptr;
        QProgressBar*                   environmentProgress_ = nullptr;
        QPushButton*                    environmentPrimary_ = nullptr;
        QPushButton*                    environmentSecondary_ = nullptr;
        QString                         environmentPhase_;
        int                             environmentPercent_ = -1;
        QTabWidget*                     problems_ = nullptr;
        QTreeWidget*                    diagnostics_ = nullptr;
        QPlainTextEdit*                 log_ = nullptr;
        /// 작업 공간에 넘긴 칸( 이 화면이 지울 때 함께 지운다 ).
        QList< QPointer< QWidget > >    takenPanels_;
        bool                            panelsTaken_ = false, previewTaken_ = false;
        QString                         modeBeforeTaken_;

        QVector< hc::rst::OutlineSymbol > documentOutline_;
        QVector< hc::rst::OutlineDocumentEntry > projectOutline_;
        int                             projectOutlineTruncated_ = 0;
        int                             outlineDepth_ = 3;
        QString                         outlineDefaultTab_ = QStringLiteral( "auto" );
        bool                            outlineTabChosen_ = false;

        QVector< hc::rst::DiagnosticEntry > shownDiagnostics_;
        bool                            diagnosticsThisDocument_ = true;
        bool                            diagnosticsQueued_ = false;
        int                             errorCount_ = 0, warningCount_ = 0;
        QString                         problemsPolicy_ = QStringLiteral( "auto" );
        bool                            problemsClosedByUser_ = false;

        QString                         previewStatus_;
        bool                            previewBusy_ = false, previewFailed_ = false, previewShown_ = false;
        bool                            previewActive_ = false, declinedEnvironment_ = false;
        bool                            projectBuilding_ = false;
        double                          zoom_ = 1;
        QTimer                          focusPoll_, bannerTimer_;

        // 전체 화면에 들어가기 전의 모습.
        bool                            previewOnly_ = false;
        QString                         modeBeforePreviewOnly_;
        bool                            outlineBeforePreviewOnly_ = false, problemsBeforePreviewOnly_ = false;
    };
}
