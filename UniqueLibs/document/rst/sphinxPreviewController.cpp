#include "stdafx.h"
#include "sphinxPreviewController.hpp"

/// 모듈
#include "sphinxPreviewProgress.hpp"
#include "pythonEnvHealth.hpp"
#include "uvTaskRunner.hpp"
#include "rstBackgroundWork.hpp"
#include "rstPhaseTrace.hpp"

/// UIs

/// 기타 헤더파일

/// 고유 라이브러리

/// 공통 라이브러리

/// 외부 라이브러리

/// 선언 헤더파일들

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QThreadPool>
#include <QTimer>

DECLARE_CMNLIBSV2_NAMESPACE
namespace hc::rst {
namespace {

constexpr int kDefaultDebounceMs = 350;

DiagnosticEntry diagnosticFromJson( const QJsonObject& object )
{
    DiagnosticEntry entry;
    entry.path = object.value( QStringLiteral( "path" ) ).toString();
    entry.uri = pathToFileUri( entry.path );
    entry.line = qMax( 1, object.value( QStringLiteral( "line" ) ).toInt( 1 ) );
    entry.character = 1;
    entry.endLine = entry.line;
    entry.endCharacter = 2;
    entry.message = object.value( QStringLiteral( "message" ) ).toString();
    entry.source = QStringLiteral( "sphinx-build" );

    const QString severity = object.value( QStringLiteral( "severity" ) ).toString();
    if( severity == QStringLiteral( "error" ) || severity == QStringLiteral( "critical" )
        || severity == QStringLiteral( "severe" ) )
        entry.severity = 1;
    else
        entry.severity = 2;

    return entry;
}

}  // namespace

SphinxPreviewController::SphinxPreviewController( QObject* parent )
    : QObject( parent )
    , debounceTimer_( new QTimer( this ) )
{
    debounceTimer_->setSingleShot( true );
    debounceTimer_->setInterval( kDefaultDebounceMs );
    connect( debounceTimer_, &QTimer::timeout, this, [this] {
        if( hasPending_ )
            startBuild();
    } );
}

SphinxPreviewController::~SphinxPreviewController()
{
    if( task_ )
        task_->cancel();
}

bool SphinxPreviewController::isBuilding() const
{
    return task_ != nullptr && task_->isRunning();
}

QString SphinxPreviewController::lastHtmlPath() const
{
    return lastHtmlPath_;
}

void SphinxPreviewController::setShadowDir( const QString& path )
{
    shadowDir_ = path;
}

void SphinxPreviewController::setDebounceInterval( const int milliseconds )
{
    debounceTimer_->setInterval( qMax( 0, milliseconds ) );
}

void SphinxPreviewController::requestBuild( const PreviewBuildRequest& request )
{
    usedFallbackPython_ = false;
    pending_ = request;
    hasPending_ = true;
    debounceTimer_->start();
}

void SphinxPreviewController::buildNow( const PreviewBuildRequest& request )
{
    usedFallbackPython_ = false;
    pending_ = request;
    hasPending_ = true;
    debounceTimer_->stop();
    startBuild();
}

void SphinxPreviewController::cancel()
{
    debounceTimer_->stop();
    hasPending_ = false;
    if( task_ )
        task_->cancel();
}

void SphinxPreviewController::cancelImmediately()
{
    debounceTimer_->stop();
    hasPending_ = false;
    if( task_ )
        task_->killNow();
}

QString SphinxPreviewController::writeShadowCopy() const
{
    if( pending_.shadowFile.isEmpty() || shadowDir_.isEmpty() )
        return {};

    return pending_.shadowFile;   // 호출 측이 이미 파일로 만들어 넘긴다.
}

QString previewInputsFilePath( const QString& stateDirectory, const QString& projectId )
{
    if( stateDirectory.isEmpty() || projectId.isEmpty() )
        return {};

    // 지문은 사용자의 프로젝트 폴더가 아니라 런타임 폴더의 state/ 에 둔다. 프로젝트마다 하나이므로
    // projectId 로 이름을 갈라 둔다 — 한 파일에 모으면 프로젝트 여러 개가 서로의 항목을 지울 수 있다.
    return QDir( stateDirectory ).filePath( QStringLiteral( "preview-inputs-%1.json" ).arg( projectId ) );
}

PreviewBuildResult SphinxPreviewController::cachedResultFor( const PreviewBuildRequest& request ) const
{
    PreviewBuildResult result;
    const QString outDir = outputDirFor( request.project );

    // 중단된 빌드가 남긴 표식. 그때는 쓰다 만 HTML 이 최신 mtime 으로 남아 있다
    // (빌더의 BUILDING_SENTINEL / _claim_out_dir 참고). 절대 보여 주면 안 된다.
    if( QFileInfo::exists( QDir( outDir ).filePath( QStringLiteral( ".mrr-building" ) ) ) )
    {
        traceP( "preview.stale.miss", QStringLiteral( "build-sentinel" ) );
        return result;
    }

    result = readReport( QDir( outDir ).filePath( QStringLiteral( ".mrr-build.json" ) ) );
    if( !result.ok || result.htmlPath.isEmpty() )
    {
        traceP( "preview.stale.miss", QStringLiteral( "report-not-ok" ) );
        result.ok = false;
        return result;
    }

    // 리포트에는 "무엇을 요청했는가" 가 없다. docname 으로 짝을 확인한다.
    // 소스 루트 기준 상대 경로에서 확장자를 떼면 Sphinx 의 docname 과 같아진다.
    // source_suffix 가 기본이 아닌 프로젝트에서는 계산이 어긋나는데, 그때는
    // 불일치로 판정되어 **틀리는 쪽이 아니라 안 보여 주는 쪽으로** 실패한다.
    const QDir sourceRoot( toCanonicalQString( request.project.sourcePath ) );
    QString docname = sourceRoot.relativeFilePath( request.sourceFile );
    docname.replace( QLatin1Char( '\\' ), QLatin1Char( '/' ) );
    const qsizetype dot = docname.lastIndexOf( QLatin1Char( '.' ) );
    if( dot > 0 )
        docname.truncate( dot );

    if( docname.compare( result.primaryDocname, Qt::CaseInsensitive ) != 0 )
    {
        traceP( "preview.stale.miss",
               QStringLiteral( "docname %1 != %2" ).arg( docname, result.primaryDocname ) );
        result.ok = false;
        return result;
    }

    const QFileInfo htmlInfo( result.htmlPath );
    if( !htmlInfo.exists() || htmlInfo.size() <= 0 )
    {
        traceP( "preview.stale.miss", QStringLiteral( "html-missing" ) );
        result.ok = false;
        return result;
    }

    // 원본이 HTML 보다 새로우면 그 사이 외부 도구(git pull 등)가 바꾼 것이다.
    // 입력 지문이 이미 이것을 잡지만, 지문이 놓치는 경로(지문 파일만 지워진
    // 경우 등)를 위해 한 겹 더 둔다.
    const QFileInfo sourceInfo( request.sourceFile );
    if( sourceInfo.exists() && sourceInfo.lastModified() > htmlInfo.lastModified() )
    {
        traceP( "preview.stale.miss", QStringLiteral( "source-newer" ) );
        result.ok = false;
        return result;
    }

    result.projectId = QString::fromStdWString( request.project.projectId );
    result.sourceFile = request.sourceFile;
    // 이 결과는 새 빌드가 아니다. 일련번호를 올리지 않아야 showPreviewHtml() 의
    // "이미 같은 것을 띄웠다" 판정이 살아 있다.
    result.serial = buildSerial_;
    return result;
}

QString SphinxPreviewController::outputDirFor( const SphinxProject& project )
{
    const QString buildRoot = toCanonicalQString( project.buildPath );
    const QString previewRoot = QDir( buildRoot ).filePath( QStringLiteral( "preview-html" ) );
    return QDir( previewRoot ).filePath( QStringLiteral( "html" ) );
}

QString SphinxPreviewController::outputDir() const
{
    // 출력 디렉터리를 빌드마다 새로 만들면 Sphinx 의 HTML 빌더가 mtime 을 비교할
    // <outdir>/<docname>.html 이 없어 **매번 전 문서를 다시 쓴다**. doctree 를
    // 공유해 읽기를 증분으로 만들어도 쓰기가 전량이면 소용이 없다. Breathe 로
    // API 문서를 싣는 프로젝트에서는 그 재작성만 60초를 넘긴다(실측: HTML 36MB,
    // 그중 한 파일이 22MB). 디렉터리를 고정해야 증분 판정이 살아난다.
    const QString outDir = outputDirFor( active_.project );
    QDir().mkpath( outDir );
    return outDir;
}

void SphinxPreviewController::cleanupStaleOutputDirs( const QString& keepDir ) const
{
    const QString buildRoot = toCanonicalQString( active_.project.buildPath );
    QDir previewRoot( QDir( buildRoot ).filePath( QStringLiteral( "preview-html" ) ) );
    if( !previewRoot.exists() )
        return;

    // 지울 목록은 GUI 스레드에서 만든다. 여기까지는 디렉터리 한 겹만 읽으므로 싸다.
    const QString keepName = QFileInfo( keepDir ).fileName();
    const QFileInfoList entries = previewRoot.entryInfoList( QDir::Dirs | QDir::NoDotAndDotDot );

    QStringList doomed;
    for( const QFileInfo& entry : entries )
    {
        if( entry.fileName() == keepName )
            continue;
        doomed << entry.absoluteFilePath();
    }
    if( doomed.isEmpty() )
        return;

    // 실제 삭제는 워커로 넘긴다.
    //
    // 이 디렉터리 하나가 HTML 36 MB / 파일 수천 개다(위 주석의 실측). 지금까지는
    // 빌드가 끝나는 순간 GUI 스레드에서 재귀 삭제했다 — 즉 "프리뷰 로딩 중"
    // 구간에 파일 시스템 작업 수천 건이 얹혀 있었다.
    //
    // 결과를 기다릴 이유가 없다. 실패해도 그냥 두고 다음 빌드에서 다시 시도하는
    // 것이 원래 규칙이고(아래 주석), 남아 있어도 프리뷰 동작에는 영향이 없다.
    QThreadPool::globalInstance()->start( [ doomed ] {
        for( const QString& path : doomed )
        {
            // 종료 중이면 그만둔다. 수천 개 파일을 다 지우고 나서야 결과가
            // 버려지는 것을 알게 되면 그 시간만큼 프로세스가 살아 있다.
            if( isShuttingDown() )
                return;
            // QWebEngine 이 이전 HTML 을 물고 있으면 지워지지 않는다. 실패해도
            // 무시하고 다음 기회에 다시 시도한다.
            QDir( path ).removeRecursively();
        }
    } );
}

void SphinxPreviewController::startBuild()
{
    if( !hasPending_ )
        return;

    // 이미 도는 중이면 **취소하지 않고** 그대로 둔다. hasPending_ 가 남아 있으므로
    // finishBuild() 가 끝난 뒤 이어서 처리한다.
    //
    // 예전에는 여기서 취소했다. 빌드가 몇 초로 끝나던 시절에는 최신 요청을 빨리
    // 반영하는 쪽이 이득이었지만, 세션 복원으로 탭 여러 개가 순차로 열리거나
    // 편집·탭 전환이 이어지면 진행 중인 빌드가 계속 리셋되어 프리뷰가 영영 뜨지
    // 않는다. 게다가 고정 출력 디렉터리에서는 중단된 빌드가 **쓰다 만 HTML** 을
    // 남긴다(회수는 빌더 쪽 sentinel 이 한다).
    if( isBuilding() )
    {
        // 지금 도는 빌드와 완전히 같은 요청이면 큐에 남길 이유가 없다.
        // 세션 복원 중에는 setActiveDocument 와 sigFileOpened 람다가 같은 문서로
        // 두 번 요청하므로, 그대로 두면 finishBuild() 가 끝난 빌드를 그대로 다시
        // 돌린다(실측 기준 빌드 한 벌 + 산출 HTML 재로드 한 벌이 통째로 낭비다).
        if( hasPending_ && pending_.sameOutcomeAs( active_ ) )
        {
            hasPending_ = false;
            traceP( "preview.build.dedup",
                   QFileInfo( active_.sourceFile ).fileName() );
        }
        return;
    }

    const bool continuingFallback = usedFallbackPython_;
    active_ = pending_;
    hasPending_ = false;
    if( !continuingFallback )
        usedFallbackPython_ = false;

    if( active_.pythonExe.isEmpty() || !QFileInfo::exists( active_.pythonExe ) )
    {
        emit logMessage( tr( "Python 런타임이 아직 준비되지 않아 프리뷰를 건너뜁니다." ) );
        return;
    }
    if( active_.builderScript.isEmpty() || !QFileInfo::exists( active_.builderScript ) )
    {
        emit logMessage( tr( "프리뷰 빌더 스크립트를 찾을 수 없습니다: %1" )
                            .arg( QDir::toNativeSeparators( active_.builderScript ) ) );
        return;
    }

    const QString buildRoot = toCanonicalQString( active_.project.buildPath );
    activeOutDir_ = outputDir();

    // 출력 디렉터리는 프로젝트마다 하나라 HyperEditor 와 HyperViewer 가 같은 프로젝트를 미리보면 함께 쓴다.
    // 빌더는 남아 있는 .mrr-building 을 "중단된 빌드의 잔해" 로 보고 HTML 을 지우므로, 두 빌드가 겹치면
    // 서로의 결과를 지운다. 잠금을 쥔 쪽만 빌드하고, 다른 쪽은 잠금이 풀린 뒤 이어서 빌드한다
    // ( 같은 입력이면 증분이라 금방 끝나고, 미저장 버퍼가 다르면 그 내용으로 다시 만든다 ).
    auto lock = std::make_unique< QLockFile >( QDir( activeOutDir_ ).filePath( QStringLiteral( ".mrr-build.lock" ) ) );
    lock->setStaleLockTime( 0 );   // 끝난 프로세스의 잠금은 PID 로 바로 회수한다.
    if( !lock->tryLock( 0 ) )
    {
        pending_ = active_;
        hasPending_ = true;
        if( !waitingForLock_ )
        {
            waitingForLock_ = true;
            emit logMessage( tr( "다른 창에서 이 프로젝트의 미리보기를 빌드하고 있습니다. 끝나면 이어서 빌드합니다." ) );
        }
        if( !lockRetryScheduled_ )
        {
            lockRetryScheduled_ = true;
            QTimer::singleShot( 500, this, [ this ] { lockRetryScheduled_ = false; startBuild(); } );
        }
        return;
    }
    waitingForLock_ = false;
    buildLock_ = std::move( lock );

    activeReportPath_ = QDir( activeOutDir_ ).filePath( QStringLiteral( ".mrr-build.json" ) );
    ++buildSerial_;

    // doctree 는 프로젝트당 하나로 공유한다. 원본은 회전하는 out-dir 안에 두어
    // 매 빌드마다 전체 재파싱을 했다. 빌드는 직렬화되므로 공유가 안전하다.
    const QString doctreeDir = QDir( buildRoot ).filePath( QStringLiteral( "preview/doctrees" ) );
    QDir().mkpath( doctreeDir );

    QStringList arguments{
        active_.builderScript,
        QStringLiteral( "--conf-dir" ), toCanonicalQString( active_.project.confPath.parent_path() ),
        QStringLiteral( "--source-dir" ), toCanonicalQString( active_.project.sourcePath ),
        QStringLiteral( "--out-dir" ), activeOutDir_,
        QStringLiteral( "--doctree-dir" ), doctreeDir,
        QStringLiteral( "--report" ), activeReportPath_,
        QStringLiteral( "--auto-fix-legacy-conf" ),
    };
    // 비면 넘기지 않는다. 빌더는 인자가 없으면 지문을 남기지 않고, 그러면
    // 게이트가 늘 "바뀌었다" 로 판정한다 (안전한 방향).
    if( !active_.inputsFile.isEmpty() )
        arguments << QStringLiteral( "--inputs-file" ) << active_.inputsFile;
    if( !active_.sourceFile.isEmpty() )
        arguments << QStringLiteral( "--primary" ) << active_.sourceFile;
    if( !active_.shadowFile.isEmpty() && !active_.sourceFile.isEmpty() )
    {
        arguments << QStringLiteral( "--shadow" )
                  << QStringLiteral( "%1=%2" ).arg( active_.sourceFile, active_.shadowFile )
                  << QStringLiteral( "--shadow-max-read-ms" )
                  << QString::number( active_.shadowMaxReadMs );
        if( active_.stubDoxygenForShadow )
            arguments << QStringLiteral( "--stub-doxygen" );
    }

    UvTask::Request request;
    request.program = active_.pythonExe;
    request.arguments = arguments;
    request.workingDirectory = toCanonicalQString( active_.project.rootPath );
    request.environment = utf8ProcessEnvironment();
    request.tag = QStringLiteral( "sphinx preview" );

    auto* task = new UvTask( std::move( request ), this );
    task_ = task;

    const QString projectId = QString::fromStdWString( active_.project.projectId );
    traceP( "preview.build.begin", QFileInfo( active_.sourceFile ).fileName() );
    buildStartedAtMs_ = QDateTime::currentMSecsSinceEpoch();
    emit buildStarted( projectId );

    // 빌더의 stdout 은 로그 패널로 가는 통로다. 진행률 줄만 걷어내 신호로 바꾼다 —
    // 그러지 않으면 문서 하나마다 한 줄씩 로그가 쌓여 정작 볼 것이 묻힌다.
    connect( task, &UvTask::outputLine, this, [this, projectId]( const QString& line ) {
        const PreviewBuildProgress progress = parsePreviewProgressLine( line );
        if( progress.valid )
        {
            emit buildProgress( projectId, progress.phase, progress.done, progress.total );
            return;
        }
        emit logMessage( line );
    } );
    connect( task, &UvTask::failedToStart, this, [this, task]( const QString& message ) {
        emit logMessage( message );
        task->deleteLater();
        finishBuild( -1, true, false, message );
    } );
    connect( task, &UvTask::finished, this, [this, task]( const int exitCode, const bool crashed ) {
        const bool cancelled = task->wasCancelled();
        const QString output = task->collectedOutput();
        task->deleteLater();
        finishBuild( exitCode, crashed, cancelled, output );
    } );

    task->start();
}

void SphinxPreviewController::finishBuild( const int exitCode, const bool crashed, const bool cancelled,
                                           const QString& output )
{
    // 빌더 프로세스가 끝났으므로 출력 디렉터리를 놓는다. 아래에서 바로 다시 빌드하면 다시 쥔다.
    buildLock_.reset();
    const bool brokenPython = !cancelled && !usedFallbackPython_
        && !active_.fallbackPythonExe.isEmpty()
        && active_.fallbackPythonExe != active_.pythonExe
        && pythonFailureIndicatesBrokenEnvironment( exitCode, crashed, output );

    // 종료 코드 4 = 빌더가 sphinx/docutils 를 임포트하지 못했다.
    // 프로젝트 venv 가 문서용이 아니라 애플리케이션용인 경우가 흔하다
    // (저장소 루트의 .venv 에는 Sphinx 가 없는 식). 이때 그냥 실패시키면
    // 프리뷰가 영영 안 뜨므로 번들 런타임으로 한 번 물러선다.
    if( !cancelled && ( exitCode == 4 || brokenPython ) && !usedFallbackPython_
        && !active_.fallbackPythonExe.isEmpty()
        && active_.fallbackPythonExe != active_.pythonExe )
    {
        usedFallbackPython_ = true;
        if( brokenPython )
        {
            const QString reason = output.trimmed().isEmpty()
                                       ? tr( "프로젝트 Python을 실행할 수 없습니다 (종료 코드 %1)." )
                                             .arg( exitCode )
                                       : output.trimmed();
            emit pythonEnvironmentDamaged( QString::fromStdWString( active_.project.projectId ),
                                           active_.pythonExe, reason );
            emit logMessage( tr( "프로젝트 Python 환경이 손상되어 내장 환경으로 다시 시도합니다." ) );
        }
        else
        {
            emit logMessage( tr( "프로젝트 환경에 Sphinx 가 없어 내장 환경으로 다시 시도합니다." ) );
        }

        pending_ = active_;
        pending_.pythonExe = active_.fallbackPythonExe;
        hasPending_ = true;
        startBuild();
        return;
    }

    const bool processOk = !crashed && exitCode == 0;
    PreviewBuildResult result = readReport( activeReportPath_ );
    result.projectId = QString::fromStdWString( active_.project.projectId );
    result.sourceFile = active_.sourceFile;
    result.serial = buildSerial_;
    result.cancelled = cancelled;
    if( !processOk )
        result.ok = false;

    if( result.ok && !result.htmlPath.isEmpty() && QFileInfo::exists( result.htmlPath ) )
    {
        lastHtmlPath_ = result.htmlPath;
        // 회전하던 시절의 build-NNNN-xxxx 디렉터리를 첫 성공 빌드에서 걷어낸다.
        cleanupStaleOutputDirs( activeOutDir_ );
    }
    else if( !cancelled )
    {
        result.ok = false;
    }

    if( !cancelled )
    {
        // 진단보다 먼저 "무엇이 다시 읽혔는지" 를 알려야 소비자가 교체 범위를
        // 정할 수 있다.
        emit processedSourcesKnown( result.processedSources );
        emit diagnosticsReady( QStringLiteral( "sphinx-build" ), result.diagnostics );

        if( !result.missingExtensions.isEmpty() || !result.missingThemes.isEmpty() )
        {
            emit missingDependenciesDetected( result.projectId, result.missingExtensions,
                                             result.missingThemes );
        }

        emit logMessage( result.ok ? tr( "프리뷰 빌드 완료" ) : tr( "프리뷰 빌드 실패" ) );
    }

    traceP( "preview.build.end",
           QStringLiteral( "%1ms ok=%2 processed=%3 %4" )
               .arg( QDateTime::currentMSecsSinceEpoch() - buildStartedAtMs_ )
               .arg( result.ok ? 1 : 0 )
               .arg( result.processedSources.size() )
               .arg( QFileInfo( active_.sourceFile ).fileName() ) );

    emit buildFinished( result );

    // 방금 끝난 것과 같은 요청은 다시 돌리지 않는다 — 결과가 이미 화면에 있다.
    // 실패한 빌드는 예외로 남긴다(다시 시도할 기회가 있어야 한다).
    if( hasPending_ && result.ok && pending_.sameOutcomeAs( active_ ) )
    {
        hasPending_ = false;
        traceP( "preview.build.dedup", QFileInfo( active_.sourceFile ).fileName() );
    }

    // 빌드 중 새 요청이 들어왔으면 이어서 처리한다.
    if( hasPending_ )
        debounceTimer_->start();
}

PreviewBuildResult SphinxPreviewController::readReport( const QString& reportPath ) const
{
    PreviewBuildResult result;

    QFile file( reportPath );
    if( !file.open( QIODevice::ReadOnly ) )
        return result;

    const QJsonDocument document = QJsonDocument::fromJson( file.readAll() );
    file.close();
    if( !document.isObject() )
        return result;

    const QJsonObject root = document.object();
    result.ok = root.value( QStringLiteral( "ok" ) ).toBool();
    result.htmlPath = root.value( QStringLiteral( "htmlPath" ) ).toString();
    result.primaryDocname = root.value( QStringLiteral( "primaryDocname" ) ).toString();
    result.sphinxVersion = root.value( QStringLiteral( "sphinxVersion" ) ).toString();
    result.htmlTheme = root.value( QStringLiteral( "htmlTheme" ) ).toString();
    result.traceback = root.value( QStringLiteral( "traceback" ) ).toString();

    const QJsonArray sources = root.value( QStringLiteral( "sources" ) ).toArray();
    for( const QJsonValue& value : sources )
        result.sources << value.toString();

    const QJsonArray processed = root.value( QStringLiteral( "processedSources" ) ).toArray();
    for( const QJsonValue& value : processed )
        result.processedSources << value.toString();
    // 예전 스키마의 리포트에는 processedSources 가 없다. 그때는 sources 가 곧
    // "이번에 처리한 것" 이었으므로 그대로 쓴다.
    if( result.processedSources.isEmpty() )
        result.processedSources = result.sources;

    const QJsonArray diagnostics = root.value( QStringLiteral( "diagnostics" ) ).toArray();
    for( const QJsonValue& value : diagnostics )
    {
        if( value.isObject() )
            result.diagnostics.push_back( diagnosticFromJson( value.toObject() ) );
    }

    const QJsonArray missing = root.value( QStringLiteral( "missingExtensions" ) ).toArray();
    for( const QJsonValue& value : missing )
    {
        const QJsonObject entry = value.toObject();
        const QString distribution = entry.value( QStringLiteral( "distribution" ) ).toString();
        const QString module = entry.value( QStringLiteral( "module" ) ).toString();
        if( !distribution.isEmpty() )
            result.missingExtensions << distribution;
        if( !module.isEmpty() )
            result.missingModules << module;
    }

    const QJsonArray themes = root.value( QStringLiteral( "missingThemes" ) ).toArray();
    for( const QJsonValue& value : themes )
        result.missingThemes << value.toString();

    return result;
}

}  // namespace hc::rst
