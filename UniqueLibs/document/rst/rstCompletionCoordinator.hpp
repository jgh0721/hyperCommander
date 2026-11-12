#pragma once

#include "rstCompletionDetailPopup.hpp"
#include "rstFilePreviewLoader.hpp"
#include "rstCompletionPopup.hpp"
#include "esbonioLspClient.hpp"
#include "rstOfflineCompletions.hpp"
#include "rstPathCompletion.hpp"
#include "rstSubstitutionIndex.hpp"

#include <QObject>
#include <QPointer>
#include <QHash>
#include <QSet>
#include <QString>
#include <QStringList>

#include "rstEditor.hpp"
class QTimer;

namespace hc::rst {

class GlossaryIndex;
class PathIndex;

/// 자동완성 조율자.
///
/// Esbonio 2.x 는 프로젝트의 내부 Sphinx 빌드가 끝나기 전까지 completion 에
/// 빈 결과를 돌려준다. 콜드 스타트에서 그 시간은 수십 초다. 그래서 이 조율자는
/// **항상 오프라인 표를 먼저 띄우고**, LSP 응답이 오면 그 위에 덮어쓴다.
///
/// LSP 풀을 직접 알지 않는다. 요청은 시그널로 내보내고 결과는 메서드로 받는다.
/// 라우팅은 WorkspaceController 의 일이다.
class CompletionCoordinator final : public QObject
{
    Q_OBJECT

public:
    explicit CompletionCoordinator( QObject* parent = nullptr );
    ~CompletionCoordinator() override;

    /// 설정( documentViews/rst/completion… )을 다시 읽는다.
    void                                reloadSettings();
    void                                attachEditor( RstEditor* view );
    void                                detachEditor( RstEditor* view );
    void                                setActiveEditor( RstEditor* view );
    /// 활성 프로젝트가 바뀌었다.
    ///
    /// sourceRoot / workspaceRoot 는 **값으로** 받는다. ProjectRegistry 가
    /// 돌려주는 SphinxProject 포인터는 다음 스캔에 무효화되므로 조율자가
    /// 그것을 들고 있으면 안 된다.
    void                                setActiveProject( const QString& projectId,
                                                         const QString& sourceRoot,
                                                         const QString& workspaceRoot );
    /// 워크스페이스 전역 경로 인덱스를 주입한다. 소유권 없음.
    void                                setPathIndex( PathIndex* index );
    /// 전역 경로 인덱스가 갱신됐다. 경로 후보를 띄우고 있으면 다시 채운다.
    void                                notifyPathIndexReady( const QString& root );
    /// 용어집 인덱스를 주입한다. 소유권 없음.
    /// `:term:` 후보와 상세 패널의 정의 본문이 여기서 온다.
    void                                setGlossaryIndex( GlossaryIndex* glossary );
    /// 용어집 수집이 끝났다. 지금 `:term:` 후보를 띄우고 있으면 다시 채운다.
    void                                notifyGlossaryReady( const QString& projectId );

    /// 치환 인덱스를 주입한다. 소유권 없음.
    /// `|name|` 후보와 상세 패널의 정의 본문이 여기서 온다.
    void                                setSubstitutionIndex( SubstitutionIndex* substitutions );
    /// 치환 수집이 끝났다. 지금 `|` 후보를 띄우고 있으면 다시 채운다.
    void                                notifySubstitutionsReady( const QString& projectId );

    /// 본문 호버로 뜨는 상세 팝업. 자동완성이 떠 있으면 무시한다.
    void                                showHoverDetail( const QString& role, const QString& target,
                                                         const QPoint& globalPos );
    void                                hideHoverDetail();

    /// Ctrl+Space. 트리거 문자 없이도 지금 위치에서 후보를 낸다.
    void                                requestExplicit();
    void                                hidePopup();
    [[nodiscard]] bool                  isPopupVisible() const;

    /// lspCompletionRequested 를 받은 쪽이 발급된 id 를 곧바로 알려준다. 0 이면 실패.
    void                                registerRequestId( int requestId );
    /// LSP 응답 도착. 기다리던 요청이 아니면 버린다.
    void                                applyLspItems( const QString& projectId, int requestId,
                                                       const QList< LspCompletionItem >& items );
    /// 해당 프로젝트의 Sphinx 빌드가 끝났다. 빌드 전에 빈 응답을 받았다면
    /// 한 번 더 물어본다 (Esbonio 는 빌드 전에는 후보를 만들지 못한다).
    void                                notifyBuildComplete( const QString& projectId );

signals:
    void                                logMessage( const QString& text );
    /// LSP 완성을 요청해 달라. 받은 쪽은 registerRequestId() 로 id 를 돌려준다.
    void                                lspCompletionRequested( const QString& path, int line, int column,
                                                                const QString& triggerCharacter );
    /// LSP 가 알려준 directive/role 이름. 렉서의 3-state 캐시를 채운다.
    void                                vocabularyHarvested( const QStringList& directives,
                                                             const QStringList& roles );

protected:
    bool                                eventFilter( QObject* watched, QEvent* event ) override;

private:
    struct PendingRequest
    {
        QString                         projectId;
        QString                         path;
        int                             line = 0;
        int                             column = 0;
        int                             requestId = 0;
        QString                         triggerCharacter;
        bool                            retried = false;
    };

    void                                onCharAdded( int character );
    /// 문서가 편집됐다 (삽입·삭제 둘 다).
    ///
    /// sigCharAdded 는 **넣은 글자**만 알려 준다. 지우는 것은 통지가 없어서
    /// ".. image" 를 ".. " 로 지워도 목록이 image 로 좁혀진 채 남아 있었다.
    void                                onTextEdited();
    /// 캐럿 위치를 다시 보고 떠 있는 목록을 지금 내용에 맞춘다.
    void                                refreshShownContext();
    void                                flushTrigger();
    /// 지금 캐럿 위치의 컨텍스트를 판정하고 오프라인 후보를 띄운 뒤 LSP 에 묻는다.
    void                                trigger( const QString& triggerCharacter, bool explicitInvoke );
    void                                askLsp( const QString& triggerCharacter );
    void                                insertCompletion( const QString& insertText );
    void                                harvestVocabulary( const QList< LspCompletionItem >& items );
    /// 팝업을 현재 편집기의 창에 붙이고 표시한다.
    /// 부모가 없는 Tool 창은 Windows 에서 메인 창 뒤로 숨을 수 있다.
    void                                showPopupAtCaret();
    /// 이미 떠 있으면 자리를 지킨 채 크기만 다시 잡고, 아니면 캐럿 밑에 새로 띄운다.
    void                                showOrRefreshPopup();
    /// 강조된 항목에 맞춰 목록 오른쪽 상세 패널을 갱신한다.
    void                                refreshDetailPopup( const CompletionDisplayItem& item );
    /// 컨텍스트와 항목으로 상세 패널에 넣을 제목/본문/출처를 만든다.
    /// 보여 줄 것이 없으면 false.
    [[nodiscard]] bool                  buildDetailContent( const CompletionDisplayItem& item,
                                                            QString* title, QString* body,
                                                            QString* source ) const;
    /// 경로 후보라면 상세 패널을 **파일 모드**로 채운다. 채웠으면 true.
    bool                                showPathDetail( const CompletionDisplayItem& item );
    /// 오프라인 표가 채우지 못하는 컨텍스트를 워크스페이스 인덱스로 메운다.
    [[nodiscard]] QList< CompletionDisplayItem >
                                        localCandidatesFor( const rstcomplete::Context& context );
    /// 파일 시스템에서 경로 후보를 만든다. 만든 것은 상세 패널이 쓸 수 있게
    /// insertText 를 키로 남겨 둔다.
    [[nodiscard]] QList< CompletionDisplayItem >
                                        pathCandidatesFor( const rstcomplete::Context& context );
    /// 치환 후보를 모은다. **지금 편집 중인 버퍼를 먼저 읽는다** — 방금 정의한
    /// `|x|` 는 아직 디스크에 없고, 그것이 가장 쓰고 싶은 후보다.
    [[nodiscard]] QList< CompletionDisplayItem > substitutionCandidates();
    /// 경로 컨텍스트에서 글자가 하나 더 들어왔을 때 후보를 다시 만든다.
    void                                recollectPathItems();
    /// 후보 생성기에 넘길 바깥 사정(문서 디렉터리·소스 루트·워크스페이스 루트).
    [[nodiscard]] rstpath::Query        pathQueryFor( const rstcomplete::Context& context ) const;
    /// 경로 인자 안에서 평범한 글자를 쳤을 때도 후보를 다시 모을 것인가.
    ///
    /// 트리거 문자만 보면 안 된다. ".. image:: M" 에서 한 단계 후보가
    /// 전부 걸러져 팝업이 닫히면, 뒤에 무엇을 쳐도 다시 열릴 길이 없어
    /// "이름만 알면 프로젝트 어디서든 찾아 준다" 는 길이 막힌다.
    [[nodiscard]] bool                  shouldRearmForPath();

    [[nodiscard]] rstcomplete::Context  contextAtCaret() const;
    [[nodiscard]] QStringList           previousLinesAtCaret() const;
    [[nodiscard]] QString               editorPath() const;

    /// 편집기 창의 자식이 되므로 창이 먼저 죽을 수 있다.
    QPointer< CompletionPopupWidget >   popup_;
    /// 목록 오른쪽(또는 마우스 옆)에 뜨는 상세 패널. 목록과 같은 수명.
    QPointer< CompletionDetailPopup >   detail_;
    GlossaryIndex*                      glossary_ = nullptr;
    PathIndex*                          pathIndex_ = nullptr;
    SubstitutionIndex*                  substitutions_ = nullptr;
    QTimer*                             debounce_ = nullptr;
    QPointer< RstEditor >               activeView_;
    /// 설정: 입력 중 자동 제안 · 경로 후보의 이미지 미리보기 · :term: 호버.
    bool                                autoTrigger_ = true;
    bool                                imagePreview_ = true;
    bool                                termHover_ = true;
    QString                             activeProjectId_;
    /// Sphinx srcdir. "/" 로 시작하는 경로의 기준이다.
    QString                             sourceRoot_;
    /// 워크스페이스 루트. 전역 경로 인덱스의 뿌리다.
    QString                             workspaceRoot_;
    /// 지금 팝업에 올라간 경로 후보. insertText -> 후보.
    /// 상세 패널이 절대 경로와 종류를 여기서 가져간다.
    QHash< QString, rstpath::Candidate > pathCandidates_;
    /// 지금 팝업에 올라간 치환 후보. insertText -> 정의.
    /// 상세 패널이 정의 본문과 출처를 여기서 가져간다.
    QHash< QString, SubstitutionEntry > substitutionCandidates_;
    /// Esc 로 닫은 경로 컨텍스트. null 이면 닫은 적이 없다.
    QString                             dismissedPathPrefix_;
    int                                 dismissedPathLine_ = 0;
    /// 상세 패널이 지금 무엇을 보여 주는가. 뒤늦게 오는 프리뷰를 거른다.
    quint64                             detailToken_ = 0;
    /// 상세 패널의 이미지 프리뷰. 디스크는 전부 이쪽 워커가 만진다.
    FilePreviewLoader*                  previewLoader_ = nullptr;

    QString                             pendingTrigger_;
    bool                                pendingExplicit_ = false;
    /// hidePopup() 재진입 가드. 목록을 숨기면 popupHidden 이 다시 이리로 온다.
    bool                                hidingPopup_ = false;
    /// 갱신 예약이 이미 걸려 있는가. 한 번의 편집이 통지를 여러 번 낼 수 있다.
    bool                                refreshQueued_ = false;
    /// 완성 세션이 살아 있는가. 목록이 잠깐 사라져도 같은 줄을 계속 고치는
    /// 동안에는 다시 열 수 있게 둔다 (".. image:" 처럼 지우는 도중의 한
    /// 순간은 아무 컨텍스트도 아니다).
    bool                                sessionArmed_ = false;
    int                                 sessionLine_ = 0;

    PendingRequest                      inFlight_;
    /// 팝업을 띄운 시점의 컨텍스트. 확정할 때 몇 글자를 지울지 여기서 온다.
    rstcomplete::Context                shownContext_;
    /// 팝업에 올라간 오프라인 후보. LSP 응답과 합칠 때 쓴다.
    QList< CompletionDisplayItem >      offlineItems_;

    /// 빌드가 끝나면 다시 물어볼 프로젝트. 빈 응답을 받은 뒤에만 채워진다.
    QSet< QString >                     retryAfterBuild_;
    /// 한 번이라도 후보를 준 프로젝트. 이후로는 재시도하지 않는다.
    QSet< QString >                     warmProjects_;
};

}  // namespace hc::rst
