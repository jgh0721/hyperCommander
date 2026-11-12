#pragma once

#include "sphinxPreviewInputs.hpp"

#include "sphinxDiagnostics.hpp"
#include "sphinxScanner.hpp"

#include <QLockFile>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>

#include <memory>

class QTimer;

namespace hc::rst {

class UvTask;
class PreviewShadowSnapshot;

/// 한 번의 프리뷰 빌드 요청.
struct PreviewBuildRequest
{
    SphinxProject                       project;
    QString                             pythonExe;          ///< 빌더 스크립트를 돌릴 인터프리터
    /// pythonExe 에 sphinx 가 없을 때 대신 쓸 인터프리터(번들). 비우면 폴백 없음.
    QString                             fallbackPythonExe;
    QString                             builderScript;      ///< mrr_sphinx_preview_build.py 절대 경로
    QString                             sourceFile;         ///< 편집 중인 원본 (htmlPath 계산용)
    QString                             shadowFile;         ///< 미저장 버퍼 사본. 없으면 빈 문자열
    std::shared_ptr<const PreviewShadowSnapshot> shadowSnapshot; ///< 큐·실행 동안 사본을 보유
    /// 직전 빌드의 재파싱 시간이 이 값(ms)을 넘는 문서에는 사본을 적용하지 않는다.
    /// 음수면 제한 없음. 사본을 반영하려면 그 문서를 매번 다시 읽어야 하는데,
    /// Breathe 문서는 그 비용이 수십 초라 편집이 멈춰 버린다.
    int                                 shadowMaxReadMs = -1;
    /// 미저장 사본을 적용할 때 `.. doxygen*::` 블록을 자리표시자로 바꾼다.
    ///
    /// 이 지시어 하나가 doxygen XML 수백 개를 훑어 재파싱이 수십 초가 되므로
    /// (실측: code-libs 38.3초, code-svc 8.3초) 타이핑 중에는 그 비용을 낼 수 없다.
    /// 지금까지는 shadowMaxReadMs 임계값이 그런 문서를 아예 제외해서 **타이핑 중
    /// 프리뷰 갱신을 포기**하고 있었다. 자리표시자로 바꾸면 그 문서도 반영된다.
    bool                                stubDoxygenForShadow = false;
    /// 입력 지문을 남길 파일. 비면 빌더가 남기지 않고, 그러면 게이트가 늘
    /// "바뀌었다" 로 판정해 매번 빌드한다(가상 프로젝트가 그 경우다).
    ///
    /// 경로를 앱이 정해서 넘기는 이유: 이름 규칙을 C++ 과 파이썬 양쪽에 두면
    /// 한쪽만 고쳐졌을 때 조용히 어긋난다.
    QString                             inputsFile;

    /// 두 요청이 "같은 산출물을 낳는가".
    ///
    /// operator== 로 두지 않는 이유: 구조체 동일성이 아니라 도메인 판정이고,
    /// 아래처럼 비대칭 예외가 있어서 이름이 그 사실을 말해야 한다.
    ///
    /// 미저장 사본이 걸린 요청은 절대 같다고 보지 않는다. 빌드별 사본을
    /// 사용하며, 편집 요청은 원문을 복사하기 전에 디바운스한다.
    [[nodiscard]] bool                  sameOutcomeAs( const PreviewBuildRequest& other ) const
    {
        if( !shadowFile.isEmpty() || !other.shadowFile.isEmpty() )
            return false;

        return project.projectId == other.project.projectId
            && pythonExe == other.pythonExe
            && builderScript == other.builderScript
            && sourceFile.compare( other.sourceFile, Qt::CaseInsensitive ) == 0;
    }
};

/// 빌더가 남긴 사이드카 JSON 을 그대로 옮긴 결과.
struct PreviewBuildResult
{
    bool                                ok = false;
    bool                                cancelled = false;
    QString                             projectId;
    QString                             htmlPath;
    /// 이 결과를 만든 빌드의 일련번호. 출력 디렉터리가 고정이라 HTML 경로가
    /// 매번 같으므로, 이 값을 URL 쿼리에 실어 Chromium 캐시를 무효화한다.
    int                                 serial = 0;
    /// 이 결과를 만든 요청의 원본 파일. 요청과 결과를 짝지어야 "요청한 문서가
    /// 정말 렌더됐는가" 를 판정할 수 있다 (빌더는 못 찾으면 root 문서로 물러선다).
    QString                             sourceFile;
    QString                             primaryDocname;
    /// data-mrr-src 인덱스와 순서가 같다. 증분 빌드에서도 인덱스가 흔들리지 않도록
    /// 빌더가 출력 디렉터리에 누적해 두는 대응표라, 이번 빌드가 건드리지 않은
    /// 파일도 들어 있다.
    QStringList                         sources;
    /// 이번 빌드가 실제로 다시 처리한 원본. 진단 교체 범위는 이쪽을 써야 한다.
    QStringList                         processedSources;
    QString                             sphinxVersion;
    QString                             htmlTheme;
    QVector< DiagnosticEntry >          diagnostics;
    QStringList                         missingExtensions;  ///< 배포명 (설치 제안용)
    QStringList                         missingModules;     ///< 모듈명 (표시용)
    QStringList                         missingThemes;
    QString                             traceback;
};

/// 상태 폴더( 환경 폴더의 `state` ) 아래 입력 지문 파일 경로. 프로젝트 폴더에는 아무것도 두지 않는다.
/// 프로젝트마다 하나이므로 projectId 로 갈라 둔다. 상태 폴더가 없으면
/// (가상 프로젝트 등) 빈 문자열을 낸다 — 그러면 지문을 쓰지도 읽지도 않는다.
[[nodiscard]] QString previewInputsFilePath( const QString& stateDirectory,
                                             const QString& projectId );

/// Sphinx HTML 프리뷰 빌드를 관리한다.
///
/// sphinx-build 를 직접 부르지 않고 번들 인터프리터로 mrr_sphinx_preview_build.py
/// 를 돌린다. 그래야 doctree 에서 정확한 원본 줄 범위를 얻을 수 있다.
class SphinxPreviewController final : public QObject
{
    Q_OBJECT

public:
    explicit SphinxPreviewController( QObject* parent = nullptr );
    ~SphinxPreviewController() override;

    [[nodiscard]] bool                  isBuilding() const;
    [[nodiscard]] QString               lastHtmlPath() const;

    /// 지난 빌드가 남긴 산출물을 **빌드 없이** 그대로 읽는다.
    /// 신뢰할 수 없으면 ok=false 를 돌려준다 — 판정은 전부 "의심스러우면
    /// 보여 주지 않는다" 쪽이다(낡은 내용을 새것으로 오인하는 것이 최악이다).
    [[nodiscard]] PreviewBuildResult    cachedResultFor( const PreviewBuildRequest& request ) const;

    /// 프로젝트당 고정된 출력 디렉터리. 회전시키면 Sphinx 의 증분 판정이 죽는다.
    /// active_ 에 의존하지 않고 디렉터리를 만들지도 않으므로, 빌드를 돌리기
    /// **전에** 조회만 하는 쪽에서도 쓸 수 있다.
    [[nodiscard]] static QString        outputDirFor( const SphinxProject& project );

    void                                setShadowDir( const QString& path );
    void                                setDebounceInterval( int milliseconds );

    /// 디바운스 후 빌드한다. 편집 중에는 이쪽을 쓴다.
    void                                requestBuild( const PreviewBuildRequest& request );
    /// 즉시 빌드한다. 저장/탭 전환처럼 사용자가 결과를 기다리는 경우.
    void                                buildNow( const PreviewBuildRequest& request );
    void                                cancel();
    /// 앱 종료 전용. 진행 중인 빌드를 곧바로 죽이고 기다리지 않는다.
    /// cancel() 의 유예 타이머는 종료 스택 안에서 발화하지 못한다.
    void                                cancelImmediately();

signals:
    void                                logMessage( const QString& text );
    /// 이번 빌드에서 실제로 다시 읽힌 원본 파일들. 진단을 이 범위로만
    /// 교체해야 증분 빌드에서 다른 파일의 진단이 사라지지 않는다.
    /// diagnosticsReady 보다 먼저 발신된다.
    void                                processedSourcesKnown( const QStringList& sources );
    void                                buildStarted( const QString& projectId );
    /// 빌드가 어디까지 왔는가. 빌더가 stdout 으로 흘려보낸 것을 그대로 옮긴다.
    ///
    /// `phase` 는 `read`(원본 읽기) 또는 `write`(HTML 쓰기)다. 분모(`total`)는
    /// 어림값이라 `done` 이 그것을 넘을 수 있다 — 받는 쪽이 가둔다
    /// (previewOverallPermille).
    void                                buildProgress( const QString& projectId, const QString& phase,
                                                       int done, int total );
    void                                buildFinished( const hc::rst::PreviewBuildResult& result );
    void                                diagnosticsReady( const QString& source,
                                                          const QVector< DiagnosticEntry >& entries );
    /// Phase 8 (누락 패키지 자동 설치) 로 이어지는 진입점.
    void                                missingDependenciesDetected( const QString& projectId,
                                                                     const QStringList& distributions,
                                                                     const QStringList& themes );
    /// 선택한 프로젝트 Python 자체가 실행 불가능해 내장 환경으로 물러났음.
    void                                pythonEnvironmentDamaged( const QString& projectId,
                                                                  const QString& pythonExe,
                                                                  const QString& reason );

private:
    void                                startBuild();
    void                                finishBuild( int exitCode, bool crashed, bool cancelled,
                                                     const QString& output );
    [[nodiscard]] PreviewBuildResult    readReport( const QString& reportPath ) const;
    /// active_ 의 출력 디렉터리. 없으면 만든다.
    [[nodiscard]] QString               outputDir() const;
    void                                cleanupStaleOutputDirs( const QString& keepDir ) const;
    [[nodiscard]] QString               writeShadowCopy() const;

    PreviewBuildRequest                 pending_;
    PreviewBuildRequest                 active_;
    bool                                hasPending_ = false;
    QTimer*                             debounceTimer_ = nullptr;
    QPointer< UvTask >                  task_;
    QString                             shadowDir_;
    QString                             activeOutDir_;
    QString                             activeReportPath_;
    QString                             lastHtmlPath_;
    int                                 buildSerial_ = 0;
    /// 계측용. preview.build.end 에 이번 빌드의 소요 시간을 실어 보낸다.
    qint64                              buildStartedAtMs_ = 0;
    /// 이번 요청에서 이미 번들로 한 번 물러섰는가 (무한 재시도 방지).
    bool                                usedFallbackPython_ = false;
    /// 출력 디렉터리( 프로젝트당 하나, 여러 프로세스가 공유 )의 빌드 잠금. 빌드하는 동안만 쥔다.
    std::unique_ptr< QLockFile >        buildLock_;
    /// 다른 프로세스가 같은 출력 디렉터리를 빌드하는 중이라 다시 시도를 예약했는가.
    bool                                lockRetryScheduled_ = false;
    bool                                waitingForLock_ = false;
};

}  // namespace hc::rst

Q_DECLARE_METATYPE( hc::rst::PreviewBuildResult )
