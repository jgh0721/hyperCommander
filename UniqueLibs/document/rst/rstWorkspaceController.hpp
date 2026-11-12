#pragma once

#include "rstOutlineService.hpp"
#include "sphinxPreviewRequestState.hpp"
#include "rstDeferredWork.hpp"
#include "sphinxDiagnostics.hpp"
#include "sphinxScanner.hpp"

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVector>
#include <memory>

#include "rstEditor.hpp"
#include "rstPreviewSurface.hpp"
class QTimer;

namespace hc::rst {

class CompletionCoordinator;
class DiagnosticsStore;
class GlossaryIndex;
class LspClient;
class LspServerPool;
class PathIndex;
class PreviewSurface;
class PreviewShadowSnapshot;
class ProjectRegistry;
class PythonEnvManager;
class PythonEnvResolver;
struct ResolvedPythonEnv;
class SphinxPreviewController;
class SubstitutionIndex;
class UvTask;
class VirtualProjectManager;
struct PreviewBuildResult;
struct PreviewBuildRequest;

/// 열린 문서 하나에 대한 Sphinx 관점의 상태.
///
/// MainWindow 는 탭/툴바/로딩 표시를 관리하고, 이 구조체는 "이 문서가 어느
/// 프로젝트에 속하며 LSP 서버와 동기화되었는가"만 담는다.
struct DocumentContext
{
    QPointer< RstEditor >               view;
    QString                             path;                    ///< 정규화된 절대경로
    QString                             projectId;               ///< 해석 전에는 빈 문자열
    bool                                isVirtual = false;
    bool                                syncedToServer = false;
    /// 초기 빌드를 유발하는 synthetic didSave 를 이미 보냈는가.
    /// Esbonio 는 didOpen 만으로는 빌드하지 않아 진단이 나오지 않는다.
    bool                                nudgedInitialBuild = false;
};

/// reST 보기( hc::document::RstDocumentView )와 Sphinx/Esbonio 서비스 계층 사이의 조율자.
///
/// 의존 방향은 단방향이다: 보기 -> WorkspaceController -> 서비스들. 보기는 LSP 풀이나 빌드 프로세스를
/// 직접 알지 못한다.
///
/// **프로세스에 하나**다( RstWorkspace() ). 열린 .rst 탭 모두가 같은 프로젝트 목록 · 언어 서버 풀 · 진단
/// 저장소 · Python 환경을 쓴다. 미리보기·자동완성·개요는 "활성 문서" 하나를 따라간다 — 보기가 앞에 나올 때
/// setPreviewSurface() 와 setActiveDocument() 로 자기를 활성으로 만든다. 미리보기 화면은 탭마다 따로이고
/// ( 프로세스도 따로 ), 화면별 페이지 상태는 여기서 화면마다 보관한다.
///
/// 워크스페이스 폴더는 없다. 프로젝트는 파일에서 위로 올라가며 찾는다( ProjectRegistry ).
class WorkspaceController final : public QObject
{
    Q_OBJECT

public:
    explicit WorkspaceController( QObject* parent = nullptr );
    ~WorkspaceController() override;

    /// 활성 문서의 미리보기 화면. nullptr 이면 미리보기가 닫혀 있다( 소스만 보는 중 ) — 빌드하지 않는다.
    /// 컨트롤러는 소유권을 갖지 않는다. 화면이 사라질 때는 forgetPreviewSurface() 를 부른다.
    void                                setPreviewSurface( PreviewSurface* surface );
    void                                forgetPreviewSurface( PreviewSurface* surface );
    [[nodiscard]] PreviewSurface*       previewSurface() const { return previewView_; }
    [[nodiscard]] RstEditor*            activeDocument() const { return activeView_; }
    [[nodiscard]] PythonEnvManager*     pythonEnvironment() const { return pythonEnv_; }
    /// 문서가 속한 프로젝트. 실제 프로젝트가 아니면( 가상 · 미정 ) nullptr.
    [[nodiscard]] const SphinxProject*  projectOf( RstEditor* view ) const;
    [[nodiscard]] bool                  isVirtualDocument( RstEditor* view ) const;
    [[nodiscard]] QString               projectIdOf( RstEditor* view ) const;
    /// 지금 미리보기 상태 문구. 비어 있으면 진행 중인 것이 없다.
    [[nodiscard]] QString               previewStatusText() const { return previewStatus_; }
    /// 미리보기 페이지의 data-mrr-src 색인이 가리키는 원본 경로. 모르는 색인이면 빈 문자열.
    [[nodiscard]] QString               previewSourcePath( int sourceIndex ) const { return pathForSourceIndex( sourceIndex ); }
    [[nodiscard]] bool                  isLanguageServerRunning( const QString& projectId ) const;
    /// 그 프로젝트의 언어 서버를 내리고 다시 띄운다.
    void                                restartLanguageServer( const QString& projectId );
    /// 언어 서버를 모두 끝낸다( 환경을 지우거나 다시 구성하기 전 ). 문서는 다음에 쓸 때 다시 붙는다.
    void                                stopLanguageServers();
    /// Python 런타임 제공자를 주입한다. 소유권 없음.
    void                                setPythonEnvironment( PythonEnvManager* manager );

    [[nodiscard]] ProjectRegistry*      projectRegistry() const;
    /// 워크스페이스 전역 파일 인덱스. 빠른 파일 열기와 경로 자동완성이 공유한다.
    [[nodiscard]] PathIndex*            pathIndex();

    void                                rescanProjects();
    void                                reloadSettings();
    void                                shutdown();

    /// 종료 의사만 표시한다. 실제 정리는 shutdown() 이 한다.
    ///
    /// closeEvent 의 저장 확인 루프가 setCurrentIndex() 로 onTabChanged 를
    /// 일으켜 **종료 도중에** 프리뷰 빌드와 LSP 프로세스를 새로 띄우고,
    /// 개요(수백 문서)·용어집(수천 문서) 스캔까지 던진다. shuttingDown_ 가
    /// 그것을 전부 막지만 지금은 shutdown() 에서야 켜져서 이미 늦다.
    void                                beginShutdown();
    /// 사용자가 저장 확인에서 취소를 눌러 종료가 되돌아갔을 때.
    void                                endShutdown();

    // ── 문서 수명주기 (MainWindow 의 탭 배관에서 호출) ──
    void                                attachDocument( RstEditor* view );
    void                                detachDocument( RstEditor* view );
    void                                setActiveDocument( RstEditor* view );
    void                                notifyDocumentSaved( RstEditor* view );

    /// 세션 복원처럼 탭이 무더기로 열리는 구간을 감싼다. 그 사이의
    /// setActiveDocument() 는 대상만 기억해 두고, endBatchRestore() 에서
    /// **마지막 한 번만** 실제로 반영한다.
    ///
    /// 왜 필요한가: MainWindow::addViewTab() 이 탭마다 setActiveDocument() 를
    /// 부르므로, 복원 순서상 **첫 프리뷰 빌드가 0번 탭 것**이 되고 사용자가 볼
    /// 활성 탭 빌드는 그것이 끝난 뒤에야 시작한다. 순수 낭비 한 벌이다.
    /// setActiveDocument() 의 기존 중복 가드는 "같은 문서 재호출" 만 잡아서
    /// 이 상황을 표현할 수 없다.
    void                                beginBatchRestore();
    void                                endBatchRestore();

    [[nodiscard]] QString               activeProjectId() const;
    [[nodiscard]] DiagnosticsStore*     diagnostics() const;

    /// 상태 표시줄에서 사용자가 확인한 손상 프로젝트 환경을 안전하게 복구한다.
    [[nodiscard]] bool                  repairPythonEnvironment( const QString& projectKey );

    /// 활성 문서의 프리뷰를 다시 빌드한다.
    /// immediate=false 면 디바운스(편집 중), true 면 즉시(저장/탭 전환).
    ///
    /// forceRebuild=true 는 "입력이 안 바뀌었어도 반드시 빌드한다" 는 뜻이다.
    /// 변경 감지 게이트(preview/skipUnchangedBuild)를 우회하는 유일한 통로이고,
    /// 사용자가 메뉴로 명시적으로 요청했을 때만 쓴다. 게이트가 놓치는 입력이
    /// 있을 수 있으므로 이 탈출구가 반드시 있어야 한다.
    void                                requestPreviewBuild( bool immediate = false,
                                                             bool forceRebuild = false );

    /// Ctrl+Space. 트리거 문자 없이 지금 캐럿 위치에서 자동완성을 연다.
    void                                requestCompletion();
    void                                requestReferenceNavigation();

    // ── 사용자가 요청한 1회성 빌드 ──
    /// 탐색기에서 프로젝트 루트를 오른쪽 클릭해 부르는 빌드.
    ///
    /// 프리뷰 빌드와는 완전히 다른 길이다. 그쪽은 우리 빌더 스크립트를 우리
    /// 산출물 디렉터리에 돌리고 결과를 프리뷰에 싣지만, 이쪽은 사용자가 고른
    /// 빌더와 디렉터리로 `sphinx-build` 를 그대로 돌리고 출력만 로그에 흘린다.
    ///
    /// 실제로 시작했으면 true. 시작하지 못한 이유(프로젝트를 못 찾음, 런타임
    /// 미준비 등)는 로그로 나간다. **돌려주는 값이 필요한 이유**: 부르는 쪽이
    /// projectBuildFinished 를 한 번만 받으려고 연결해 두는데, 시작하지 않으면
    /// 그 연결이 남아 **다음 빌드**의 결과에 반응한다.
    [[nodiscard]] bool                  buildProject( const QString& projectId, const QString& builder,
                                                      const QString& outputDirectory );
    [[nodiscard]] bool                  isProjectBuildRunning() const;
    void                                cancelProjectBuild();

signals:
    void                                logMessage( const QString& text );
    void                                projectsChanged( int count );
    void                                activeProjectChanged( const QString& projectId, bool isVirtual );
    /// 활성 문서와 그 소속이 정해졌다. standalone 이면 conf.py 를 가진 실제
    /// Sphinx 프로젝트 밖에 홀로 있는 `.md`/`.rst` 다 — 그 문서에는 프로젝트
    /// 개요가 보여 줄 것이 없다.
    ///
    /// activeProjectChanged 로는 이것을 알 수 없어 따로 둔다. 그 신호는
    /// projectId 가 **바뀔** 때만 나오는데, 단독 `.md` 는 가상 프로젝트조차
    /// 만들지 않아(VirtualProjectManager::isSupported) projectId 가 처음부터
    /// 끝까지 빈 문자열이고, 그래서 한 번도 나오지 않는다.
    void                                activeDocumentResolved( bool standalone );
    void                                navigateRequested( const QString& path, int line, int column );
    void                                referenceNavigateRequested( const QString& path, int line );
    void                                referenceNavigationStatus( const QString& text );
    void                                diagnosticsChanged( const QString& source,
                                                            const QVector< DiagnosticEntry >& entries );
    void                                missingDependenciesDetected( const QString& projectId,
                                                                     const QStringList& distributions,
                                                                     const QStringList& themes );
    void                                pythonEnvironmentDamaged( const QString& projectKey,
                                                                  const QString& projectId,
                                                                  const QString& environmentPath,
                                                                  const QString& reason );
    void                                pythonEnvironmentDamageCleared( const QString& projectKey );
    void                                pythonEnvironmentRepairStarted( const QString& projectKey );
    void                                pythonEnvironmentRepairProgress( const QString& projectKey,
                                                                         int percent,
                                                                         const QString& phase );
    void                                pythonEnvironmentRepairFinished( const QString& projectKey,
                                                                         bool success,
                                                                         const QString& message );
    void                                lspStatusChanged( const QString& projectId, const QString& state );

    // ── 사용자가 요청한 1회성 빌드 ──
    void                                projectBuildStarted( const QString& projectId,
                                                             const QString& builder );
    /// 산출물이 실제로 놓인 디렉터리를 함께 준다 — make 모드(`-M`)는 사용자가
    /// 고른 자리 **아래**에 쓰므로 요청 값과 다를 수 있다.
    void                                projectBuildFinished( const QString& projectId,
                                                              const QString& builder,
                                                              const QString& outputDirectory,
                                                              bool ok, bool cancelled );
    /// 프리뷰가 준비되는 동안의 상태. busy=false 면 text 는 비어 있고 표시를 지운다.
    /// 빌드뿐 아니라 HTML 로드 구간까지 덮는다 — 큰 문서는 빌드가 끝난 뒤에도
    /// WebEngine 이 읽는 데 시간이 걸려서, 그 사이가 비어 있으면 고장으로 보인다.
    ///
    /// `permille` 은 프리뷰 한 판 전체에 대한 진행도(0..1000)다. 준비 · 빌드 읽기 ·
    /// 빌드 쓰기 · HTML 로딩이 각자 구간을 나눠 쓰므로 단계가 넘어가도 값이
    /// 뒤로 가지 않는다. 음수면 진행도를 알 수 없다는 뜻이다(왕복 막대).
    void                                previewStatusChanged( const QString& text, bool busy, int permille );
    /// 활성 문서의 미리보기 빌드가 실패했다 / 성공했다. 실패하면 화면은 마지막으로 성공한 결과에 머문다.
    void                                previewBuildFailed( const QString& sourceFile );
    void                                previewBuildSucceeded( const QString& sourceFile );

    // ── 개요 ──
    /// 활성 문서의 섹션 개요. 먼저 정규식 폴백으로 한 번, LSP 응답이 오면 다시.
    void                                documentOutlineReady( const QString& path,
                                                              const QVector< OutlineSymbol >& symbols );
    /// 프로젝트의 문서 목록과 각 문서의 개요. truncated 는 상한에 걸려 잘린 개수.
    void                                projectOutlineReady( const QString& projectId,
                                                             const QVector< OutlineDocumentEntry >& documents,
                                                             int truncated );
    void                                outlineCleared( const QString& reason );

private:
    [[nodiscard]] DocumentContext*      contextFor( RstEditor* view );
    void                                resolveProject( DocumentContext& context );
    /// 실제 프로젝트를 먼저 찾고, 없으면 가상 프로젝트에서 찾는다.
    [[nodiscard]] const SphinxProject*  lookupProject( const QString& projectId ) const;
    /// 경로 자동완성이 파일을 찾는 뿌리: 프로젝트 폴더, 단독 문서면 그 파일의 폴더.
    [[nodiscard]] QString               pathIndexRootFor( const DocumentContext& context ) const;
    /// 활성 문서가 실제 프로젝트 밖에 홀로 있는 `.md`/`.rst` 인가.
    [[nodiscard]] bool                  activeDocumentIsStandalone() const;
    /// 가상 프로젝트의 합성 conf.py 가 쓸 `html_theme`.
    ///
    /// 설정이 비어 있으면("다른 프로젝트와 동일") 워크스페이스의 실제 프로젝트
    /// conf.py 에서 찾아 온다. 아무것도 못 찾으면 빈 문자열 —
    /// VirtualProjectManager 가 alabaster 로 물러선다.
    [[nodiscard]] QString               resolveVirtualProjectTheme() const;
    /// 위 값을 VirtualProjectManager 에 밀어 넣는다. 값이 바뀌었으면 가상
    /// 프로젝트로 렌더 중이던 문서를 새 conf.py 로 다시 잡는다.
    void                                applyVirtualProjectTheme();
    void                                logProjectList();
    void                                onPreviewFinished( const PreviewBuildResult& result );
    [[nodiscard]] std::shared_ptr<const PreviewShadowSnapshot> createShadowSnapshot( RstEditor* view ) const;
    /// 프리뷰가 **우리가 띄운** 페이지인가.
    ///
    /// 사용자가 프리뷰 안에서 외부 링크를 눌러 이동한 경우를 제외하려는 판정이다.
    /// qrc: 를 반드시 포함해야 한다 — Markdown 셸이 qrc 에서 오고,
    /// QUrl::isLocalFile() 은 scheme=="file" 만 참이다. 그것만 보면 md 프리뷰는
    /// allowRemoteContent 를 껐다 켜도 다시 읽히지 않아, 한 번 차단된 CDN
    /// 스크립트가 영영 안 살아난다.
    [[nodiscard]] bool                  previewUrlIsOurs() const;
    void                                showPreviewHtml( const QString& htmlPath, const QString& documentKey,
                                                          int buildSerial );
    /// 같은 문구를 반복 발신하지 않는다. text 가 비면 표시를 지운다.
    ///
    /// 문구가 같아도 진행도가 달라지면 발신한다 — 그러지 않으면 분모를 모르는
    /// 구간에서 막대가 멈춘 채로 남는다.
    void                                setPreviewStatus( const QString& text, int permille = -1 );
    /// 프리뷰 페이지의 원격 리소스 접근 허용 여부를 설정에서 읽어 적용한다.
    /// 값이 바뀌었으면 이미 떠 있는 페이지를 다시 읽는다 (실패한 스크립트는
    /// 설정만 바꿔서는 다시 실행되지 않는다).
    void                                applyPreviewWebSettings();
    void                                applyPreviewAppearanceSettings();
    /// 입력이 안 바뀌었는지 워커 스레드에서 판정하고, 결과에 따라 빌드하거나
    /// 지난 산출물을 그대로 올린다.
    void                                tryServeFromLastBuild( const PreviewBuildRequest& request,
                                                               bool immediate );
    void                                onPreviewGateDecided( const PreviewBuildRequest& request,
                                                              bool immediate, bool changed );
    void                                refreshDiagnosticMarks( const QString& normalizedPath );
    /// pathChanged 를 이벤트 루프 회전당 한 덩어리로 모아 refreshDiagnosticMarks 로 넘긴다.
    void                                scheduleDiagnosticMarksRefresh( const QString& normalizedPath );

    // ── Esbonio ──
    // 지금은 활성 프로젝트 하나만 띄운다. 프로젝트당 하나씩 유지하는 풀은
    // 다음 단계에서 이 자리를 대체한다.
    void                                ensureLspForActiveDocument();
    void                                syncDocumentToServer( DocumentContext& context, bool forceOpen );
    void                                setLspStatus( const QString& state );
    void                                nudgeInitialBuild( const QString& projectId );
    /// 서버가 새로 떴을 때 그 프로젝트의 **모든** 열린 탭을 다시 didOpen 한다.
    /// 축출 후 재기동하면 서버에 문서 상태가 전혀 없으므로, 활성 탭 하나만
    /// 열면 나머지 탭의 진단이 조용히 사라진다.
    void                                reopenDocumentsForProject( const QString& projectId );

    // ── 개요 ──
    /// 활성 문서의 개요를 정규식 폴백으로 즉시 내보내고 LSP 에도 물어본다.
    void                                refreshDocumentOutline();
    /// 프로젝트 문서 목록을 작업 스레드에서 훑는다.
    /// force=false 면 같은 프로젝트에 대해 다시 돌지 않는다.
    void                                refreshProjectOutline( bool force );
    void                                applyProjectOutline( QVector< OutlineDocumentEntry > documents,
                                                             const QString& projectId, int truncated,
                                                             quint64 generation );

    // ── 용어집 ──
    /// 활성 프로젝트의 `.. glossary::` 를 다시 훑는다.
    /// force=false 면 같은 프로젝트에 대해 다시 돌지 않는다.
    void                                refreshGlossary( bool force );

    // ── 치환 ──
    /// 활성 프로젝트의 conf.py(rst_prolog/rst_epilog)와 문서의 `.. |name| ...` 를
    /// 다시 훑는다. force=false 면 같은 프로젝트에 대해 다시 돌지 않는다.
    void                                refreshSubstitutions( bool force );

    // ── 사용자가 요청한 1회성 빌드 ──
    /// 지금 돌고 있는 빌드 한 벌. 한 번에 하나만 돈다.
    struct ProjectBuildJob
    {
        QString                         projectId;
        QString                         builder;
        /// 사용자가 고른 자리.
        QString                         outputDirectory;
        /// 산출물이 실제로 놓이는 자리. make 모드면 outputDirectory 아래다.
        QString                         resultDirectory;
        QString                         sourceDir;
        QString                         confDir;
        /// 프로젝트 환경에 Sphinx 가 없을 때 물러설 인터프리터.
        QString                         fallbackPython;
        QString                         pythonExe;
        bool                            usedFallback = false;
    };

    struct DamagedPythonEnvironment
    {
        QString                         projectId;
        QString                         projectRoot;
        QString                         environmentPath;
        QString                         reason;
    };

    /// 실제로 프로세스를 띄운다. buildProject() 와 폴백 재시도가 함께 쓴다.
    void                                startProjectBuild( const QString& pythonExe );
    void                                finishProjectBuild( int exitCode, bool crashed, bool cancelled,
                                                            const QString& output );

    // ── 스크롤 동기화 ──
    // 에디터 -> 프리뷰, 프리뷰 -> 에디터 양방향. 어느 쪽이든 "기준 비율 위치에
    // 있는 줄" 을 상대편의 같은 비율 위치로 보낸다.
    void                                syncPreviewFromEditor();
    /// userInitiated 는 프리뷰를 직접 클릭한 이동. 왕복 방지 가드를 넘긴다.
    void                                syncEditorFromPreview( int sourceIndex, double line, double ratio,
                                                               bool userInitiated = false );
    [[nodiscard]] int                   sourceIndexForPath( const QString& path ) const;
    [[nodiscard]] QString               pathForSourceIndex( int sourceIndex ) const;

    ProjectRegistry*                    registry_ = nullptr;
    PythonEnvManager*                   pythonEnv_ = nullptr;
    PythonEnvResolver*                  envResolver_ = nullptr;
    QHash< QString, DamagedPythonEnvironment > damagedPythonEnvironments_;
    /// 화면 하나에 떠 있는 페이지의 상태. 활성 화면의 것은 아래 preview* 멤버에 펼쳐져 있고, 나머지는
    /// parkedPages_ 에 접혀 있다.
    struct PageState
    {
        QStringList                     sources;
        QHash< QString, int >           sourceIndex;
        QStringList                     processedSources;
        QString                         requestedPath;
        QString                         primaryPath;
        QString                         documentKey;
        QString                         headSignature;
        qint64                          shownSize = -1;
        qint64                          shownMTimeMs = -1;
        QUrl                            url;
        bool                            loadedOk = false;
        bool                            loadInFlight = false;
        QString                         pendingFullLoadPath;
        QUrl                            pendingFullLoadUrl;
        int                             allowRemote = -1;
    };
    [[nodiscard]] PageState             capturePageState() const;
    void                                restorePageState( const PageState& page );
    void                                connectSurface( PreviewSurface* surface );

    QPointer< PreviewSurface >          previewView_;
    QHash< PreviewSurface*, PageState > parkedPages_;
    QSet< PreviewSurface* >             knownSurfaces_;
    SphinxPreviewController*            previewController_ = nullptr;
    DeferredPreviewWork                 previewBuildWork_;
    VirtualProjectManager*              virtualProjects_ = nullptr;
    DiagnosticsStore*                   diagnosticsStore_ = nullptr;
    LspServerPool*                      lspPool_ = nullptr;
    CompletionCoordinator*              completions_ = nullptr;
    GlossaryIndex*                      glossary_ = nullptr;
    /// 활성 프로젝트의 치환 목록. `|` 자동완성이 여기서 나온다.
    SubstitutionIndex*                  substitutions_ = nullptr;
    /// 워크스페이스 전역 경로 인덱스. 경로 완성이 처음 필요할 때 채워진다.
    PathIndex*                          pathIndex_ = nullptr;
    ProjectBuildJob                     projectBuild_;
    /// 돌고 있는 sphinx-build. 끝나면 스스로 deleteLater 한다.
    QPointer< UvTask >                  projectBuildTask_;
    QString                             lspState_;
    /// 프리뷰의 원격 리소스 허용 상태. -1 은 아직 한 번도 적용하지 않은 것으로,
    /// 첫 적용에서 불필요한 리로드를 하지 않기 위해 구분한다.
    int                                 previewAllowRemote_ = -1;
    /// 저장하지 않은 편집을 프리뷰에 반영할지 (설정 preview/applyUnsavedEdits).
    bool                                previewApplyUnsavedEdits_ = true;
    /// 편집할 때마다 미리보기를 다시 만들지( 설정 autoPreview ), 그 지연( debounceMs ).
    bool                                previewAutoBuild_ = true;
    int                                 previewDebounceMs_ = 600;
    /// 편집기와 미리보기의 스크롤을 서로 맞출지( 설정 scrollSync ). 본문 클릭 이동은 언제나 된다.
    bool                                scrollSyncEnabled_ = true;
    /// Esbonio 를 쓸지( 설정 languageServer ). 끄면 진단은 Sphinx 빌드 결과만 나온다.
    bool                                languageServerEnabled_ = true;
    /// 직전 빌드의 재파싱 시간이 이 값을 넘는 문서는 반영에서 제외한다.
    /// 음수면 제한 없음. Breathe 문서처럼 재파싱이 수십 초인 경우를 막는다.
    int                                 previewUnsavedMaxReadMs_ = 2000;
    QStringList                         previewSources_;      ///< data-mrr-src 인덱스 -> 원본 경로
    /// previewSources_ 의 역인덱스. 정규화·접은 경로 -> 인덱스.
    ///
    /// 없을 때 sourceIndexForPath() 는 목록을 선형 순회하며 **항목마다**
    /// `QFileInfo(...).absoluteFilePath()` 를 새로 만들었다. 그 함수는
    /// sigViewportScrolled 마다 불리고, Sphinx 프로젝트에서 이 목록은 수백 개다.
    QHash< QString, int >               previewSourceIndex_;
    /// scheduleDiagnosticMarksRefresh() 가 모아 두는 경로들. 비어 있지 않으면
    /// 예약된 타이머가 하나 떠 있다는 뜻이다.
    QSet< QString >                     pendingDiagnosticMarkPaths_;
    /// previewSources_ 를 바꿀 때 함께 부른다. 둘이 어긋나면 프리뷰가 엉뚱한
    /// 문서로 스크롤하므로 대입 지점을 이 함수 하나로 모은다.
    void                                setPreviewSources( const QStringList& sources );

    /// 프리뷰로 마지막에 보낸 (소스 인덱스, 줄). 같은 값을 다시 보내지 않는다.
    int                                 lastSyncedSourceIndex_ = -1;
    double                              lastSyncedLine_ = -1.0;
    QStringList                         previewProcessedSources_;  ///< 이번 빌드가 다시 읽은 파일들
    /// 마지막으로 빌드를 **요청한** 문서. 탭을 옮겼는데 프리뷰가 따라오지
    /// 않은 상태를 알아채는 기준이다.
    QString                             previewRequestedPath_;
    /// 프리뷰에 실제로 **표시된** 문서.
    QString                             previewPrimaryPath_;
    /// 한쪽이 유발한 스크롤이 되돌아와 무한 왕복하는 것을 막는다.
    qint64                              suppressSyncUntilMs_ = 0;
    /// 에디터가 주도권을 쥔 구간. 이 동안 프리뷰의 스크롤 보고는 무시한다.
    ///
    /// **페이지가 스스로 움직인 스크롤을 막는 것은 이 값이 아니다.** 그 판정은
    /// 입력 이벤트를 받는 페이지 쪽에서 하고 `previewScrolled` 의 `userDriven`
    /// 으로 온다. 여기 남은 것은 "우리가 방금 프리뷰를 굴렸다" 는 왕복 차단뿐이다.
    qint64                              previewDrivenIgnoreUntilMs_ = 0;
    /// 가드에 걸려 버려진 에디터->프리뷰 동기화를 가드 해제 후 되살린다.
    QTimer*                             previewSyncRetry_ = nullptr;

    // ── 프리뷰 핫스왑 상태 ──
    // 같은 문서를 다시 빌드했고 <head> 가 그대로면 전체 리로드 대신 body 만
    // 갈아끼워 깜빡임을 없앤다.
    QString                             previewDocumentKey_;
    QString                             previewHeadSignature_;
    /// 지금 화면에 띄워 둔 HTML 의 크기와 mtime. 재빌드 결과가 이것과 같으면
    /// 파일 내용이 그대로라는 뜻이므로 다시 로드하지 않는다.
    qint64                              previewShownSize_ = -1;
    qint64                              previewShownMTimeMs_ = -1;
    /// 설정 preview/skipUnchangedBuild. 입력이 안 바뀌었으면 빌드를 건너뛴다.
    bool                                previewSkipUnchangedBuild_ = true;
    /// 설정 preview/stubDoxygenWhileTyping. 타이핑 중에는 doxygen 지시어를
    /// 자리표시자로 두어, 재파싱이 수십 초인 문서도 편집이 반영되게 한다.
    bool                                previewStubDoxygenWhileTyping_ = true;
    /// 렌더러 전환·직접 빌드·문서 해제도 이전 캐시 판정과 빌드 결과를 무효화한다.
    PreviewRequestState                 previewRequests_;
    QUrl                                previewUrl_;
    bool                                previewLoadedOk_ = false;
    /// load() 를 부르고 아직 loadFinished 를 못 받았는가.
    ///
    /// previewLoadedOk_ 로는 이것을 알 수 없다 — 로드 중과 "끝났지만 실패" 가
    /// 둘 다 false 라서, 그것만 보고 같은 URL 재요청을 막으면 실패한 로드를
    /// 다시 시도할 수 없게 된다.
    bool                                previewLoadInFlight_ = false;
    int                                 hotSwapToken_ = 0;
    QString                             pendingFullLoadPath_;
    /// 핫스왑이 실패했을 때 되돌아갈 URL. 캐시 무효화 쿼리까지 포함해야 한다.
    QUrl                                pendingFullLoadUrl_;
    /// 마지막으로 내보낸 프리뷰 상태 문구. 비어 있으면 "표시 없음".
    QString                             previewStatus_;
    /// 마지막으로 내보낸 진행도(0..1000, 음수 = 모름).
    int                                 previewPermille_ = -1;

    // ── 개요 상태 ──
    QTimer*                             outlineDebounce_ = nullptr;
    QString                             projectOutlineProjectId_;
    /// 늦게 도착한 이전 프로젝트의 순회 결과를 버리기 위한 세대 번호.
    quint64                             outlineGeneration_ = 0;

    QHash< RstEditor*, DocumentContext > documents_;
    QPointer< RstEditor >               activeView_;
    quint64                             referenceNavigationGeneration_ = 0;
    QString                             activeProjectId_;
    bool                                shuttingDown_ = false;

    /// 세션 복원 중인가. 켜져 있으면 setActiveDocument() 가 반영을 미룬다.
    bool                                batchRestoring_ = false;
    /// 복원이 끝나면 활성화할 뷰.
    QPointer< RstEditor >               batchPendingActive_;
};

/// 프로세스의 reST 조율자. 처음 부를 때 만들고 Python 환경 관리자를 붙인다. qApp 이 소유한다.
[[nodiscard]] WorkspaceController* RstWorkspace();

}  // namespace hc::rst
