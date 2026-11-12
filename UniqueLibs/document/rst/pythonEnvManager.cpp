#include "stdafx.h"
#include "pythonEnvManager.hpp"
#include "UniqueLibs/scratchSession.hpp"

/// 모듈
#include "rstSettings.hpp"
#include "uvDownloader.hpp"
#include "uvTaskRunner.hpp"

/// UIs

/// 기타 헤더파일

/// 고유 라이브러리
#include "UniqueLibs/ProductDefaults_Edit.hpp"

/// 공통 라이브러리

/// 외부 라이브러리

/// 선언 헤더파일들

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QCryptographicHash>
#include <QSettings>
#include <QTimer>

DECLARE_CMNLIBSV2_NAMESPACE
namespace hc::rst {
namespace {

constexpr auto kSettingsUseExternalUv = "python/useExternalUv";
constexpr auto kSettingsExternalUvPath = "python/externalUvPath";
constexpr auto kSettingsUvAcquire = "python/uvAcquire";
constexpr auto kSettingsInstallThemes = "python/installOptionalThemes";
constexpr auto kSettingsInstallExtensions = "python/installOptionalExtensions";
constexpr auto kSettingsExtraPackages = "python/extraPackages";
/// 환경 정의와 빌더 스크립트( tools/rst/runtime ). qrc 로 hc_rst_core 에 들어 있다.
constexpr auto kResourceRoot = ":/rst/runtime/";

QString nativePath( const QString& path )
{
    return QDir::toNativeSeparators( path );
}

QString scriptExe( const QString& venvDir, const QString& name )
{
#ifdef Q_OS_WIN
    return QDir( venvDir ).filePath( QStringLiteral( "Scripts/%1.exe" ).arg( name ) );
#else
    return QDir( venvDir ).filePath( QStringLiteral( "bin/%1" ).arg( name ) );
#endif
}

QString sha1Of( const QByteArray& bytes )
{
    return QString::fromLatin1( QCryptographicHash::hash( bytes, QCryptographicHash::Sha1 ).toHex() );
}

/// 리소스를 대상 경로로 복사한다. 내용이 같으면 건드리지 않는다.
bool copyResourceFile( const QString& resourcePath, const QString& destinationPath,
                      const bool executable, QString* errorMessage )
{
    QFile source( resourcePath );
    if( !source.open( QIODevice::ReadOnly ) )
    {
        if( errorMessage != nullptr )
            *errorMessage = PythonEnvManager::tr( "리소스를 열 수 없습니다: %1" ).arg( resourcePath );
        return false;
    }
    const QByteArray payload = source.readAll();
    source.close();

    QFile existing( destinationPath );
    if( existing.exists() && existing.open( QIODevice::ReadOnly ) )
    {
        const QByteArray current = existing.readAll();
        existing.close();
        if( current == payload )
            return true;   // 이미 최신
    }

    QDir().mkpath( QFileInfo( destinationPath ).absolutePath() );
    QFile target( destinationPath );
    if( !target.open( QIODevice::WriteOnly | QIODevice::Truncate ) )
    {
        if( errorMessage != nullptr )
            *errorMessage = PythonEnvManager::tr( "파일을 쓸 수 없습니다: %1" ).arg( nativePath( destinationPath ) );
        return false;
    }
    target.write( payload );
    target.close();

    if( executable )
        target.setPermissions( target.permissions() | QFileDevice::ExeOwner | QFileDevice::ExeUser );

    return true;
}

/// 워커 스레드에서 수행할 리소스 추출에 필요한 값 묶음.
/// (스레드에서 PythonEnvManager 멤버를 건드리지 않기 위해 값으로 넘긴다.)
struct ExtractPlan
{
    QString runtimeRoot;
    QString projectDir;
    QString uvTarget;
    bool    useExternalUv = false;
    QString externalUvPath;
};

bool runExtractPlan( const ExtractPlan& plan, QString* errorMessage )
{
    if( !QDir().mkpath( plan.runtimeRoot ) )
    {
        if( errorMessage != nullptr )
            *errorMessage = PythonEnvManager::tr( "환경 폴더를 만들 수 없습니다: %1" )
                                .arg( nativePath( plan.runtimeRoot ) );
        return false;
    }

    if( !copyResourceFile( QLatin1String( kResourceRoot ) + QStringLiteral( "pyproject.toml" ),
                          QDir( plan.projectDir ).filePath( QStringLiteral( "pyproject.toml" ) ), false, errorMessage ) )
        return false;

    // uv.lock 이 있어야 --frozen 으로 재현 가능한 설치가 된다.
    if( !copyResourceFile( QLatin1String( kResourceRoot ) + QStringLiteral( "uv.lock" ),
                          QDir( plan.projectDir ).filePath( QStringLiteral( "uv.lock" ) ), false, errorMessage ) )
        return false;

    QFile pythonVersion( QDir( plan.projectDir ).filePath( QStringLiteral( ".python-version" ) ) );
    if( !pythonVersion.open( QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate ) )
    {
        if( errorMessage != nullptr )
            *errorMessage = PythonEnvManager::tr( ".python-version 파일을 쓸 수 없습니다: %1" )
                                .arg( nativePath( pythonVersion.fileName() ) );
        return false;
    }
    pythonVersion.write( "3.12\n" );
    pythonVersion.close();

    if( plan.useExternalUv )
    {
        if( plan.externalUvPath.isEmpty() || !QFileInfo::exists( plan.externalUvPath ) )
        {
            if( errorMessage != nullptr )
                *errorMessage = PythonEnvManager::tr( "외부 UV 파일을 찾을 수 없습니다: %1" )
                                    .arg( nativePath( plan.externalUvPath ) );
            return false;
        }
    }
    // uv 는 리소스가 아니다. 필요하면 이 뒤에 UvDownloader 가 받는다.
    return true;
}

}  // namespace

PythonEnvManager::PythonEnvManager( QObject* parent )
    : QObject( parent )
{
    reloadSettings();

    refreshState();
    // 환경이 이미 있으면 앱과 함께 온 도우미 스크립트로 맞춘다. 환경이 없을 때는 아무것도 쓰지 않는다 —
    // 설정창을 열거나 .rst 를 한 번 연 것만으로 사용자 폴더에 파일이 생기지 않게 한다( 구성할 때 푼다 ).
    if( isReady() )
        ensureHelperScripts();
}

void PythonEnvManager::reloadSettings()
{
    const Settings settings;
    useExternalUv_ = settings.value( kSettingsUseExternalUv, false ).toBool();
    externalUvPath_ = settings.value( kSettingsExternalUvPath ).toString();
    uvAcquire_ = settings.value( kSettingsUvAcquire, QStringLiteral( "ask" ) ).toString();
    if( uvAcquire_ != QLatin1String( "auto" ) && uvAcquire_ != QLatin1String( "never" ) )
        uvAcquire_ = QStringLiteral( "ask" );
    installOptionalThemes_ = settings.value( kSettingsInstallThemes, true ).toBool();
    installOptionalExtensions_ = settings.value( kSettingsInstallExtensions, true ).toBool();
    extraPackages_.clear();
    for( const QString& name : settings.value( kSettingsExtraPackages ).toStringList() )
        if( !name.trimmed().isEmpty() )
            extraPackages_ << name.trimmed();
}

bool PythonEnvManager::removeEnvironment( QString* errorMessage )
{
    const auto fail = [errorMessage]( const QString& message ) {
        if( errorMessage != nullptr )
            *errorMessage = message;
        return false;
    };
    if( isBusy() )
        return fail( tr( "환경을 구성하는 중에는 삭제할 수 없습니다." ) );

    // 이 폴더는 제품이 만든 것만 지운다: 이름이 rst-runtime 이거나 제품이 둔 표식이 있어야 한다.
    const QString root = QDir::cleanPath( runtimeRoot() );
    const QFileInfo info( root );
    if( root.isEmpty() || info.isRoot() )
        return fail( tr( "환경 폴더 경로가 올바르지 않습니다." ) );
    if( !info.exists() )
    {
        refreshState();
        return true;
    }
    const bool ours = info.fileName().compare( QLatin1String( "rst-runtime" ), Qt::CaseInsensitive ) == 0
                      || QFileInfo::exists( readyMarker() ) || QFileInfo( projectDir() ).isDir();
    if( !ours )
        return fail( tr( "제품이 만든 환경 폴더가 아닙니다: %1" ).arg( nativePath( root ) ) );

    if( !QDir( root ).removeRecursively() )
    {
        refreshState();
        return fail( tr( "일부 파일을 지우지 못했습니다. 미리보기나 언어 서버가 쓰는 중일 수 있습니다: %1" ).arg( nativePath( root ) ) );
    }
    emit bootstrapLog( tr( "환경 폴더를 삭제했습니다: %1" ).arg( nativePath( root ) ) );
    refreshState();
    return true;
}

PythonEnvManager::~PythonEnvManager() = default;

// ── 경로 ────────────────────────────────────────────────────

QString PythonEnvManager::runtimeRoot() const
{
    // 시험과 진단용: 다른 자리에 환경을 둔다.
    const QString overridden = qEnvironmentVariable( "HC_RST_RUNTIME_DIR" );
    return overridden.isEmpty() ? hc::defaults::edit::RstRuntimeDirectory() : QDir::cleanPath( overridden );
}

QString PythonEnvManager::projectDir() const
{
    return runtimeRoot();
}

QString PythonEnvManager::venvDir() const
{
    return QDir( projectDir() ).filePath( QStringLiteral( ".venv" ) );
}

QString PythonEnvManager::pythonExe() const
{
#ifdef Q_OS_WIN
    return QDir( venvDir() ).filePath( QStringLiteral( "Scripts/python.exe" ) );
#else
    return QDir( venvDir() ).filePath( QStringLiteral( "bin/python" ) );
#endif
}

QString PythonEnvManager::sphinxBuildExe() const
{
    return scriptExe( venvDir(), QStringLiteral( "sphinx-build" ) );
}

QString PythonEnvManager::esbonioExe() const
{
    return scriptExe( venvDir(), QStringLiteral( "esbonio" ) );
}

QString PythonEnvManager::scriptsDir() const
{
    return QDir( runtimeRoot() ).filePath( QStringLiteral( "scripts" ) );
}

QString PythonEnvManager::previewBuilderScript() const
{
    return QDir( scriptsDir() ).filePath( QStringLiteral( "mrr_sphinx_preview_build.py" ) );
}

QString PythonEnvManager::shadowDir() const
{
    // 저장하지 않은 버퍼의 사본이다. 프로세스의 임시 폴더( 소유 잠금 )에 두어 빌드 중 강제 종료되어도
    // 다음 실행이 지운다. 예전 위치 rst-runtime/shadow 에 남은 것은 시작 시 정리( temporaryCleanup )가 지운다.
    return hc::scratch::Session::Current().Folder( QStringLiteral( "rst-shadow" ) );
}

QString PythonEnvManager::cacheDir() const
{
    return QDir( runtimeRoot() ).filePath( QStringLiteral( "cache" ) );
}

void PythonEnvManager::ensureHelperScripts()
{
    QString errorMessage;
    if( !copyResourceFile( QLatin1String( kResourceRoot ) + QStringLiteral( "mrr_sphinx_preview_build.py" ),
                          previewBuilderScript(), false, &errorMessage ) )
    {
        emit bootstrapLog( errorMessage );
    }
}

QString PythonEnvManager::embeddedUvTarget() const
{
#ifdef Q_OS_WIN
    return QDir( runtimeRoot() ).filePath( QStringLiteral( "uv.exe" ) );
#else
    return QDir( runtimeRoot() ).filePath( QStringLiteral( "uv" ) );
#endif
}

QString PythonEnvManager::readyMarker() const
{
    return QDir( runtimeRoot() ).filePath( QStringLiteral( "ready.marker" ) );
}

QString PythonEnvManager::uvExecutable() const
{
    return useExternalUv_ ? externalUvPath_ : embeddedUvTarget();
}

// ── 상태 ────────────────────────────────────────────────────

EnvState PythonEnvManager::state() const
{
    return state_;
}

bool PythonEnvManager::isReady() const
{
    return state_ == EnvState::Ready;
}

bool PythonEnvManager::isBusy() const
{
    return state_ == EnvState::Preparing || state_ == EnvState::Syncing || state_ == EnvState::Verifying;
}

QString PythonEnvManager::stateText() const
{
    switch( state_ )
    {
        case EnvState::Unknown:        return tr( "확인 전" );
        case EnvState::Checking:       return tr( "확인 중" );
        case EnvState::NotConfigured:  return tr( "구성 안 됨" );
        case EnvState::Preparing:      return tr( "준비 중" );
        case EnvState::Syncing:        return tr( "패키지 설치 중" );
        case EnvState::Verifying:      return tr( "검증 중" );
        case EnvState::Ready:          return tr( "준비됨" );
        case EnvState::Failed:         return tr( "구성 실패" );
        case EnvState::Cancelled:      return tr( "구성 취소됨" );
    }
    return {};
}

QString PythonEnvManager::lastError() const
{
    return lastError_;
}

bool PythonEnvManager::isProjectRepairing() const
{
    return !projectRepairKey_.isEmpty();
}

QDateTime PythonEnvManager::configuredDate() const
{
    const QFileInfo markerInfo( readyMarker() );
    if( !markerInfo.exists() )
        return {};

    const QDateTime created = markerInfo.birthTime();
    return created.isValid() ? created : markerInfo.lastModified();
}

QString PythonEnvManager::configuredDateText() const
{
    const QDateTime configured = configuredDate();
    if( !configured.isValid() )
        return tr( "구성되지 않음" );
    return configured.toLocalTime().toString( QStringLiteral( "yyyy-MM-dd HH:mm:ss" ) );
}

QString PythonEnvManager::uvDescription() const
{
    const QString uvPath = uvExecutable();

    QStringList lines;
    lines << ( useExternalUv_ ? tr( "직접 지정한 uv" )
                              : tr( "내려받은 uv %1" ).arg( downloadedUvVersion().isEmpty() ? tr( "(아직 없음)" ) : downloadedUvVersion() ) );
    lines << tr( "경로: %1" ).arg( nativePath( uvPath ) );
    if( !QFileInfo::exists( uvPath ) )
    {
        lines << ( useExternalUv_ ? tr( "상태: 지정한 uv 파일을 찾을 수 없습니다." )
                                  : uvAcquire_ == QLatin1String( "never" )
                                        ? tr( "상태: 내려받지 않도록 설정되어 있습니다. uv 경로를 지정하세요." )
                                        : tr( "상태: 구성할 때 공식 배포처에서 %1 을(를) 내려받습니다." ).arg( QLatin1String( UvRelease::Version ) ) );
    }
    return lines.join( QLatin1Char( '\n' ) );
}

// ── 설정 ────────────────────────────────────────────────────

bool PythonEnvManager::useExternalUv() const           { return useExternalUv_; }
QString PythonEnvManager::externalUvPath() const       { return externalUvPath_; }
bool PythonEnvManager::autoBootstrap() const           { return uvAcquire_ == QLatin1String( "auto" ) || ( useExternalUv_ && uvAcquire_ != QLatin1String( "ask" ) ); }
QString PythonEnvManager::uvAcquire() const            { return uvAcquire_; }
bool PythonEnvManager::needsUvDownload() const         { return !useExternalUv_ && !UvDownloader::IsInstalled( embeddedUvTarget() ); }
QString PythonEnvManager::downloadedUvVersion() const  { return UvDownloader::InstalledVersion( embeddedUvTarget() ); }
bool PythonEnvManager::installOptionalThemes() const   { return installOptionalThemes_; }
bool PythonEnvManager::installOptionalExtensions() const { return installOptionalExtensions_; }
QStringList PythonEnvManager::extraPackages() const    { return extraPackages_; }

void PythonEnvManager::setUseExternalUv( const bool enabled )
{
    useExternalUv_ = enabled;
}

void PythonEnvManager::setExternalUvPath( const QString& path )
{
    externalUvPath_ = QDir::fromNativeSeparators( path.trimmed() );
}

void PythonEnvManager::setUvAcquire( const QString& policy )
{
    uvAcquire_ = ( policy == QLatin1String( "auto" ) || policy == QLatin1String( "never" ) ) ? policy : QStringLiteral( "ask" );
}

void PythonEnvManager::setInstallOptionalThemes( const bool enabled )
{
    installOptionalThemes_ = enabled;
}

void PythonEnvManager::setInstallOptionalExtensions( const bool enabled )
{
    installOptionalExtensions_ = enabled;
}

void PythonEnvManager::saveUvSettings() const
{
    Settings settings;
    settings.setValue( kSettingsUseExternalUv, useExternalUv_ );
    settings.setValue( kSettingsExternalUvPath, externalUvPath_ );
    settings.setValue( kSettingsUvAcquire, uvAcquire_ );
    settings.setValue( kSettingsInstallThemes, installOptionalThemes_ );
    settings.setValue( kSettingsInstallExtensions, installOptionalExtensions_ );
    settings.setValue( kSettingsExtraPackages, extraPackages_ );
}

// ── 상태 전이 ───────────────────────────────────────────────

void PythonEnvManager::setState( const EnvState next )
{
    if( state_ == next )
        return;

    state_ = next;
    emit stateChanged( state_ );

    const bool ready = isReady();
    if( ready != lastReadyState_ )
    {
        lastReadyState_ = ready;
        emit readyChanged( ready );
    }
}

void PythonEnvManager::setLastError( const QString& message )
{
    lastError_ = message;
}

bool PythonEnvManager::installedFilesPresent() const
{
    return QFileInfo::exists( pythonExe() )
        && QFileInfo::exists( sphinxBuildExe() )
        && QFileInfo::exists( esbonioExe() );
}

QString PythonEnvManager::embeddedPyprojectHash() const
{
    QFile source( QLatin1String( kResourceRoot ) + QStringLiteral( "pyproject.toml" ) );
    if( !source.open( QIODevice::ReadOnly ) )
        return {};
    return sha1Of( source.readAll() );
}

bool PythonEnvManager::markerMatchesEmbeddedManifest() const
{
    QSettings marker( readyMarker(), QSettings::IniFormat );
    if( marker.value( QStringLiteral( "schema" ) ).toInt() != RuntimeSchema )
        return false;

    const QString expected = embeddedPyprojectHash();
    return !expected.isEmpty() && marker.value( QStringLiteral( "pyprojectHash" ) ).toString() == expected;
}

void PythonEnvManager::writeReadyMarker() const
{
    QSettings marker( readyMarker(), QSettings::IniFormat );
    marker.setValue( QStringLiteral( "schema" ), RuntimeSchema );
    marker.setValue( QStringLiteral( "pyprojectHash" ), embeddedPyprojectHash() );
    marker.setValue( QStringLiteral( "configuredAt" ), QDateTime::currentDateTimeUtc().toString( Qt::ISODate ) );
    marker.sync();
}

void PythonEnvManager::refreshState()
{
    if( isBusy() )
        return;

    setState( EnvState::Checking );

    // 파일만 있는 게 아니라 매니페스트 해시까지 맞아야 준비된 것으로 본다.
    // 앱 업데이트로 의존성이 바뀌면 stale venv 로 조용히 돌지 않고 재동기화한다.
    if( QFileInfo::exists( readyMarker() ) && installedFilesPresent() && markerMatchesEmbeddedManifest() )
        setState( EnvState::Ready );
    else
        setState( EnvState::NotConfigured );
}

// ── 부트스트랩 ──────────────────────────────────────────────

void PythonEnvManager::ensureEnvironmentAsync()
{
    if( isBusy() || isProjectRepairing() )
        return;

    if( isReady() )
    {
        // ready.marker 와 파일 존재만으로는 기반 Python 손상을 알 수 없다.
        // 첫 사용 때 실제 import 를 비동기로 확인하고 실패하면 한 번 재구성한다.
        repairBundledAfterVerifyFailure_ = true;
        startVerifyTask();
        return;
    }

    configureEnvironmentAsync( false );
}

void PythonEnvManager::configureEnvironmentAsync( const bool forceRebuild )
{
    if( isBusy() || isProjectRepairing() )
        return;

    setLastError( {} );
    repairBundledAfterVerifyFailure_ = false;
    setState( EnvState::Preparing );
    emit progressChanged( -1, tr( "준비 중" ) );
    emit bootstrapLog( tr( "Python/Sphinx/Esbonio 환경 구성을 시작합니다." ) );

    if( forceRebuild && QFileInfo::exists( readyMarker() ) )
        QFile::remove( readyMarker() );

    startExtractTask();
}

void PythonEnvManager::startExtractTask()
{
    ExtractPlan plan;
    plan.runtimeRoot = runtimeRoot();
    plan.projectDir = projectDir();
    plan.uvTarget = embeddedUvTarget();
    plan.useExternalUv = useExternalUv_;
    plan.externalUvPath = externalUvPath_;

    QPointer< PythonEnvManager > guard( this );
    QThreadPool::globalInstance()->start( [guard, plan] {
        QString errorMessage;
        const bool ok = runExtractPlan( plan, &errorMessage );

        QMetaObject::invokeMethod(
            guard,
            [guard, ok, errorMessage] {
                if( guard )
                    guard->onResourcesExtracted( ok, errorMessage );
            },
            Qt::QueuedConnection );
    } );
}

void PythonEnvManager::onResourcesExtracted( const bool ok, const QString& errorMessage )
{
    if( state_ != EnvState::Preparing )
        return;   // 그 사이 취소되었거나 상태가 바뀌었다.

    if( !ok )
    {
        setLastError( errorMessage );
        emit bootstrapLog( errorMessage );
        emit failed( errorMessage );
        setState( EnvState::Failed );
        return;
    }

    if( needsUvDownload() )
        startUvDownload();
    else
        startSyncTask();
}

void PythonEnvManager::startUvDownload()
{
    if( uvAcquire_ == QLatin1String( "never" ) )
    {
        const QString message = tr( "uv를 내려받지 않도록 설정되어 있습니다. 설정에서 uv 경로를 지정하거나 받는 방법을 바꾸세요." );
        setLastError( message );
        emit bootstrapLog( message );
        emit failed( message );
        setState( EnvState::Failed );
        return;
    }

    if( !uvDownloader_ )
    {
        uvDownloader_ = new UvDownloader( this );
        connect( uvDownloader_, &UvDownloader::Log, this, &PythonEnvManager::bootstrapLog );
        connect( uvDownloader_, &UvDownloader::Progress, this, [this]( const qint64 received, const qint64 total ) {
            // 전체 구성에서 uv 받기는 앞쪽 8% 다. 그 뒤를 uv 가 CPython 과 패키지로 채운다.
            emit progressChanged( total > 0 ? int( received * 8 / total ) : -1, tr( "uv 내려받는 중" ) );
        } );
        connect( uvDownloader_, &UvDownloader::Finished, this, [this]( const bool ok, const QString& message ) {
            if( state_ != EnvState::Preparing )
                return;   // 그 사이 취소되었다.
            if( !ok )
            {
                setLastError( message );
                emit bootstrapLog( message );
                emit failed( message );
                setState( EnvState::Failed );
                return;
            }
            startSyncTask();
        } );
    }
    emit progressChanged( 0, tr( "uv 내려받는 중" ) );
    uvDownloader_->Start( embeddedUvTarget() );
}

QProcessEnvironment PythonEnvManager::uvEnvironment() const
{
    QProcessEnvironment env = utf8ProcessEnvironment();

    // uv 가 쓰는 것을 모두 환경 폴더 아래로 모은다. 그래야 환경 삭제가 폴더 하나를 지우는 일이 되고,
    // 사용자의 다른 uv 설정·캐시와 섞이지 않는다.
    env.insert( QStringLiteral( "UV_PYTHON_INSTALL_DIR" ), QDir( runtimeRoot() ).filePath( QStringLiteral( "python" ) ) );
    env.insert( QStringLiteral( "UV_CACHE_DIR" ), QDir( cacheDir() ).filePath( QStringLiteral( "uv" ) ) );
    env.insert( QStringLiteral( "UV_PROJECT_ENVIRONMENT" ), venvDir() );
    env.insert( QStringLiteral( "UV_PYTHON_PREFERENCE" ), QStringLiteral( "only-managed" ) );
    env.insert( QStringLiteral( "UV_NO_CONFIG" ), QStringLiteral( "1" ) );
    return env;
}

QStringList PythonEnvManager::syncArguments() const
{
    QStringList arguments{ QStringLiteral( "sync" ), QStringLiteral( "--frozen" ) };
    if( installOptionalThemes_ )
        arguments << QStringLiteral( "--extra" ) << QStringLiteral( "themes" );
    if( installOptionalExtensions_ )
        arguments << QStringLiteral( "--extra" ) << QStringLiteral( "extensions" );
    return arguments;
}

void PythonEnvManager::startSyncTask()
{
    setState( EnvState::Syncing );
    emit progressChanged( -1, tr( "패키지 확인 중" ) );

    UvTask::Request request;
    request.program = uvExecutable();
    request.arguments = syncArguments();
    request.workingDirectory = projectDir();
    request.environment = uvEnvironment();
    request.tag = QStringLiteral( "uv sync" );

    auto* task = new UvTask( std::move( request ), this );
    activeTask_ = task;

    connect( task, &UvTask::outputLine, this, [this]( const QString& line ) {
        emit bootstrapLog( line );
        updateProgressFromUvLine( line );
    } );
    connect( task, &UvTask::failedToStart, this, [this, task]( const QString& message ) {
        setLastError( message );
        emit bootstrapLog( message );
        emit failed( message );
        setState( EnvState::Failed );
        task->deleteLater();
    } );
    connect( task, &UvTask::finished, this, [this, task]( const int exitCode, const bool crashed ) {
        const bool cancelled = task->wasCancelled();
        task->deleteLater();

        if( cancelled )
        {
            emit bootstrapLog( tr( "환경 구성을 취소했습니다." ) );
            // ready.marker 는 맨 마지막에 쓰므로 부분 설치는 그대로 두어도 안전하다.
            setState( EnvState::Cancelled );
            return;
        }

        if( crashed || exitCode != 0 )
        {
            const QString message = tr( "uv sync 실패 (종료 코드 %1)" ).arg( exitCode );
            setLastError( message );
            emit bootstrapLog( message );
            emit failed( message );
            setState( EnvState::Failed );
            return;
        }

        startVerifyTask();
    } );

    task->start();
}

void PythonEnvManager::startVerifyTask()
{
    setState( EnvState::Verifying );
    emit progressChanged( 95, tr( "검증 중" ) );

    if( !installedFilesPresent() )
    {
        const bool repair = repairBundledAfterVerifyFailure_;
        repairBundledAfterVerifyFailure_ = false;
        const QString message = tr( "설치는 끝났지만 python/sphinx-build/esbonio 를 찾을 수 없습니다." );
        setLastError( message );
        emit bootstrapLog( message );
        emit failed( message );
        setState( EnvState::Failed );
        if( repair )
            QTimer::singleShot( 0, this, [this] { configureEnvironmentAsync( true ); } );
        return;
    }

    UvTask::Request request;
    request.program = pythonExe();
    request.arguments = { QStringLiteral( "-c" ),
                         QStringLiteral( "import sphinx, esbonio, docutils; print(sphinx.__version__)" ) };
    request.workingDirectory = projectDir();
    request.environment = utf8ProcessEnvironment();
    request.tag = QStringLiteral( "verify" );

    auto* task = new UvTask( std::move( request ), this );
    activeTask_ = task;

    connect( task, &UvTask::outputLine, this, [this]( const QString& line ) {
        emit bootstrapLog( tr( "Sphinx %1" ).arg( line ) );
    } );
    connect( task, &UvTask::failedToStart, this, [this, task]( const QString& message ) {
        const bool repair = repairBundledAfterVerifyFailure_;
        repairBundledAfterVerifyFailure_ = false;
        setLastError( message );
        emit failed( message );
        setState( EnvState::Failed );
        task->deleteLater();
        if( repair )
            QTimer::singleShot( 0, this, [this] { configureEnvironmentAsync( true ); } );
    } );
    connect( task, &UvTask::finished, this, [this, task]( const int exitCode, const bool crashed ) {
        const bool cancelled = task->wasCancelled();
        task->deleteLater();

        if( cancelled )
        {
            repairBundledAfterVerifyFailure_ = false;
            setState( EnvState::Cancelled );
            return;
        }

        if( crashed || exitCode != 0 )
        {
            const bool repair = repairBundledAfterVerifyFailure_;
            repairBundledAfterVerifyFailure_ = false;
            const QString message = tr( "설치된 Python 환경에서 sphinx/esbonio 를 가져올 수 없습니다." );
            setLastError( message );
            emit bootstrapLog( message );
            emit failed( message );
            setState( EnvState::Failed );
            if( repair )
                QTimer::singleShot( 0, this, [this] { configureEnvironmentAsync( true ); } );
            return;
        }

        repairBundledAfterVerifyFailure_ = false;
        // 검증까지 통과한 뒤에야 준비 표식을 남긴다.
        ensureHelperScripts();
        writeReadyMarker();
        emit progressChanged( 100, tr( "완료" ) );
        emit bootstrapLog( tr( "환경 구성 완료: %1" ).arg( nativePath( pythonExe() ) ) );
        setState( EnvState::Ready );
    } );

    task->start();
}

void PythonEnvManager::cancel()
{
    if( uvDownloader_ && uvDownloader_->IsRunning() )
    {
        uvDownloader_->Cancel();
        setLastError( tr( "구성을 취소했습니다." ) );
        setState( EnvState::Cancelled );
        return;
    }
    if( activeTask_ )
        activeTask_->cancel();
    if( projectRepairTask_ )
        projectRepairTask_->cancel();
}

void PythonEnvManager::cancelImmediately()
{
    if( uvDownloader_ )
        uvDownloader_->Cancel();
    if( activeTask_ )
        activeTask_->killNow();
    if( projectRepairTask_ )
        projectRepairTask_->killNow();
}

void PythonEnvManager::updateProgressFromUvLine( const QString& line )
{
    // uv 출력에서 대략적인 단계만 읽는다. 정확한 비율은 노리지 않는다.
    if( line.startsWith( QStringLiteral( "Downloading cpython" ) ) )
        emit progressChanged( 10, tr( "Python 내려받는 중" ) );
    else if( line.startsWith( QStringLiteral( "Resolved" ) ) )
        emit progressChanged( 30, tr( "의존성 해석 완료" ) );
    else if( line.startsWith( QStringLiteral( "Prepared" ) ) )
        emit progressChanged( 70, tr( "패키지 준비 완료" ) );
    else if( line.startsWith( QStringLiteral( "Installed" ) ) )
        emit progressChanged( 90, tr( "설치 완료" ) );
}

void PythonEnvManager::installPackagesAsync( const QStringList& distributions,
                                            const QString& targetPythonExe )
{
    if( distributions.isEmpty() || isProjectRepairing() )
        return;

    const QString target = targetPythonExe.isEmpty() ? pythonExe() : targetPythonExe;
    if( !QFileInfo::exists( target ) )
    {
        emit packageInstallFinished( false, distributions );
        return;
    }

    UvTask::Request request;
    request.program = uvExecutable();
    request.arguments = QStringList{ QStringLiteral( "pip" ), QStringLiteral( "install" ),
                                    QStringLiteral( "--python" ), target }
                      + distributions;
    request.workingDirectory = projectDir();
    request.environment = uvEnvironment();
    request.tag = QStringLiteral( "uv pip install" );

    auto* task = new UvTask( std::move( request ), this );
    const bool intoBundled = ( target == pythonExe() );

    connect( task, &UvTask::outputLine, this, &PythonEnvManager::bootstrapLog );
    connect( task, &UvTask::failedToStart, this, [this, task, distributions]( const QString& message ) {
        emit bootstrapLog( message );
        emit packageInstallFinished( false, distributions );
        task->deleteLater();
    } );
    connect( task, &UvTask::finished, this,
            [this, task, distributions, intoBundled]( const int exitCode, const bool crashed ) {
                const bool ok = !crashed && exitCode == 0 && !task->wasCancelled();
                task->deleteLater();

                if( ok && intoBundled )
                {
                    // uv sync 는 lock 에 없는 패키지를 prune 한다. 여기 남겨 두지
                    // 않으면 다음 환경 재구성 때 조용히 사라진다.
                    for( const QString& distribution : distributions )
                    {
                        if( !extraPackages_.contains( distribution ) )
                            extraPackages_ << distribution;
                    }
                    saveUvSettings();
                }

                emit packageInstallFinished( ok, distributions );
            } );

    emit bootstrapLog( tr( "패키지 설치: %1" ).arg( distributions.join( QStringLiteral( ", " ) ) ) );
    task->start();
}

bool PythonEnvManager::repairProjectEnvironmentAsync( const QString& projectKey,
                                                       const QString& projectRoot,
                                                       const QString& venvDir )
{
    const auto reject = [this, &projectKey]( const QString& message ) {
        emit bootstrapLog( message );
        emit projectRepairFinished( projectKey, false, message );
        return false;
    };

    if( projectKey.isEmpty() || projectRoot.trimmed().isEmpty() || venvDir.trimmed().isEmpty() )
        return reject( tr( "복구할 프로젝트 Python 환경 정보가 올바르지 않습니다." ) );
    if( isBusy() || isProjectRepairing() )
        return reject( tr( "다른 Python 환경 작업이 진행 중입니다." ) );
    if( !QFileInfo::exists( uvExecutable() ) )
        return reject( tr( "환경 복구에 사용할 uv를 찾을 수 없습니다: %1" )
                           .arg( nativePath( uvExecutable() ) ) );

    const QString root = QDir::cleanPath( QFileInfo( projectRoot ).absoluteFilePath() );
    const QString original = QDir::cleanPath( QFileInfo( venvDir ).absoluteFilePath() );
    const QFileInfo originalInfo( original );
    const QString environmentName = originalInfo.fileName();
    const bool acceptedName = environmentName == QStringLiteral( ".venv" )
                           || environmentName == QStringLiteral( "venv" )
                           || environmentName == QStringLiteral( "env" );
    if( !acceptedName || !originalInfo.isDir()
        || QDir::cleanPath( originalInfo.absolutePath() ).compare( root, Qt::CaseInsensitive ) != 0
        || !QFileInfo::exists( QDir( original ).filePath( QStringLiteral( "pyvenv.cfg" ) ) ) )
    {
        return reject( tr( "안전하게 복구할 수 있는 프로젝트 가상환경이 아닙니다: %1" )
                           .arg( nativePath( original ) ) );
    }
    if( !QFileInfo::exists( QDir( root ).filePath( QStringLiteral( "pyproject.toml" ) ) ) )
    {
        return reject( tr( "프로젝트 의존성을 복원할 pyproject.toml을 찾을 수 없습니다: %1" )
                           .arg( nativePath( root ) ) );
    }

    const QString stamp = QDateTime::currentDateTimeUtc().toString( QStringLiteral( "yyyyMMdd-HHmmsszzz" ) );
    projectRepairKey_ = projectKey;
    projectRepairRoot_ = root;
    projectRepairOriginalDir_ = original;
    projectRepairCandidateDir_ = QDir( root ).filePath(
        QStringLiteral( "%1.mrst-repair-%2" ).arg( environmentName, stamp ) );
    projectRepairBackupDir_ = QDir( root ).filePath(
        QStringLiteral( "%1.mrst-broken-%2" ).arg( environmentName, stamp ) );

    const QFileInfo cfgInfo( QDir( original ).filePath( QStringLiteral( "pyvenv.cfg" ) ) );
    projectRepairOriginalCfgSize_ = cfgInfo.size();
    projectRepairOriginalCfgMTimeMs_ = cfgInfo.lastModified().toMSecsSinceEpoch();

    emit bootstrapLog( tr( "프로젝트 Python 환경 복구를 시작합니다: %1" )
                           .arg( nativePath( original ) ) );
    emit projectRepairStarted( projectRepairKey_ );
    emit projectRepairProgress( projectRepairKey_, 0, tr( "교체 환경 준비 중" ) );
    startProjectRepairSync();
    return true;
}

void PythonEnvManager::startProjectRepairSync()
{
    UvTask::Request request;
    request.program = uvExecutable();
    request.arguments = { QStringLiteral( "sync" ) };
    if( QFileInfo::exists( QDir( projectRepairRoot_ ).filePath( QStringLiteral( "uv.lock" ) ) ) )
        request.arguments << QStringLiteral( "--frozen" );
    request.workingDirectory = projectRepairRoot_;
    request.environment = uvEnvironment();
    request.environment.insert( QStringLiteral( "UV_PROJECT_ENVIRONMENT" ), projectRepairCandidateDir_ );
    request.tag = QStringLiteral( "uv sync (project repair)" );

    auto* task = new UvTask( std::move( request ), this );
    projectRepairTask_ = task;
    connect( task, &UvTask::outputLine, this, [this]( const QString& line ) {
        emit bootstrapLog( line );
        int percent = -1;
        QString phase = tr( "프로젝트 패키지 구성 중" );
        if( line.startsWith( QStringLiteral( "Downloading cpython" ) ) )
        {
            percent = 10;
            phase = tr( "프로젝트 Python 내려받는 중" );
        }
        else if( line.startsWith( QStringLiteral( "Resolved" ) ) )
            percent = 30;
        else if( line.startsWith( QStringLiteral( "Prepared" ) ) )
            percent = 70;
        else if( line.startsWith( QStringLiteral( "Installed" ) ) )
            percent = 90;
        emit projectRepairProgress( projectRepairKey_, percent, phase );
    } );
    connect( task, &UvTask::failedToStart, this, [this, task]( const QString& message ) {
        projectRepairTask_ = nullptr;
        task->deleteLater();
        finishProjectRepair( false, message );
    } );
    connect( task, &UvTask::finished, this, [this, task]( const int exitCode, const bool crashed ) {
        const bool cancelled = task->wasCancelled();
        projectRepairTask_ = nullptr;
        task->deleteLater();
        if( cancelled )
        {
            finishProjectRepair( false, tr( "프로젝트 Python 환경 복구를 취소했습니다." ) );
            return;
        }
        if( crashed || exitCode != 0 )
        {
            finishProjectRepair( false,
                                 tr( "프로젝트 환경 uv sync 실패 (종료 코드 %1). 원래 환경은 변경하지 않았습니다."
                                     ).arg( exitCode ) );
            return;
        }
        startProjectRepairVerify();
    } );
    task->start();
}

void PythonEnvManager::startProjectRepairVerify()
{
    emit projectRepairProgress( projectRepairKey_, 95, tr( "교체 환경 검증 중" ) );

    UvTask::Request request;
    request.program = scriptExe( projectRepairCandidateDir_, QStringLiteral( "python" ) );
    request.arguments = { QStringLiteral( "-I" ), QStringLiteral( "-c" ),
                          QStringLiteral( "import sys; print(sys.executable)" ) };
    request.workingDirectory = projectRepairRoot_;
    request.environment = utf8ProcessEnvironment();
    request.tag = QStringLiteral( "verify project repair" );

    auto* task = new UvTask( std::move( request ), this );
    projectRepairTask_ = task;
    connect( task, &UvTask::outputLine, this, &PythonEnvManager::bootstrapLog );
    connect( task, &UvTask::failedToStart, this, [this, task]( const QString& message ) {
        projectRepairTask_ = nullptr;
        task->deleteLater();
        finishProjectRepair( false, message );
    } );
    connect( task, &UvTask::finished, this, [this, task]( const int exitCode, const bool crashed ) {
        const bool cancelled = task->wasCancelled();
        projectRepairTask_ = nullptr;
        task->deleteLater();
        if( cancelled || crashed || exitCode != 0 )
        {
            finishProjectRepair( false,
                                 cancelled ? tr( "프로젝트 Python 환경 복구를 취소했습니다." )
                                           : tr( "새 프로젝트 Python 환경 검증에 실패했습니다." ) );
            return;
        }

        const QFileInfo currentCfg( QDir( projectRepairOriginalDir_ ).filePath(
            QStringLiteral( "pyvenv.cfg" ) ) );
        if( !currentCfg.exists() || currentCfg.size() != projectRepairOriginalCfgSize_
            || currentCfg.lastModified().toMSecsSinceEpoch() != projectRepairOriginalCfgMTimeMs_ )
        {
            finishProjectRepair( false,
                                 tr( "복구 중 원래 환경이 변경되어 교체하지 않았습니다." ) );
            return;
        }

        QDir parent( projectRepairRoot_ );
        const QString originalName = QFileInfo( projectRepairOriginalDir_ ).fileName();
        const QString candidateName = QFileInfo( projectRepairCandidateDir_ ).fileName();
        const QString backupName = QFileInfo( projectRepairBackupDir_ ).fileName();
        if( !parent.rename( originalName, backupName ) )
        {
            finishProjectRepair( false, tr( "기존 손상 환경을 백업할 수 없어 교체하지 않았습니다: %1" )
                                               .arg( nativePath( projectRepairOriginalDir_ ) ) );
            return;
        }
        if( !parent.rename( candidateName, originalName ) )
        {
            const bool restored = parent.rename( backupName, originalName );
            finishProjectRepair( false,
                                 restored
                                     ? tr( "새 환경을 적용하지 못해 기존 환경으로 되돌렸습니다." )
                                     : tr( "새 환경 적용과 기존 환경 복원에 실패했습니다. 백업: %1" )
                                           .arg( nativePath( projectRepairBackupDir_ ) ) );
            return;
        }

        emit projectRepairProgress( projectRepairKey_, 100, tr( "프로젝트 환경 복구 완료" ) );
        finishProjectRepair( true, tr( "프로젝트 Python 환경을 복구했습니다. 기존 환경 백업: %1" )
                                        .arg( nativePath( projectRepairBackupDir_ ) ) );
    } );
    task->start();
}

void PythonEnvManager::finishProjectRepair( const bool success, const QString& message )
{
    const QString key = projectRepairKey_;
    emit bootstrapLog( message );

    projectRepairTask_ = nullptr;
    projectRepairKey_.clear();
    projectRepairRoot_.clear();
    projectRepairOriginalDir_.clear();
    projectRepairCandidateDir_.clear();
    projectRepairBackupDir_.clear();
    projectRepairOriginalCfgSize_ = -1;
    projectRepairOriginalCfgMTimeMs_ = -1;

    emit projectRepairFinished( key, success, message );
}

void PythonEnvManager::requestUvVersionAsync()
{
    const QString uvPath = uvExecutable();
    if( !QFileInfo::exists( uvPath ) )
    {
        emit uvVersionReady( tr( "확인 실패" ) );
        return;
    }

    UvTask::Request request;
    request.program = uvPath;
    request.arguments = { QStringLiteral( "--version" ) };
    request.tag = QStringLiteral( "uv --version" );

    auto* task = new UvTask( std::move( request ), this );
    auto* captured = new QString;

    connect( task, &UvTask::outputLine, this, [captured]( const QString& line ) {
        if( captured->isEmpty() )
            *captured = line;
    } );
    connect( task, &UvTask::failedToStart, this, [this, task, captured]( const QString& ) {
        emit uvVersionReady( tr( "확인 실패" ) );
        delete captured;
        task->deleteLater();
    } );
    connect( task, &UvTask::finished, this, [this, task, captured]( const int exitCode, const bool crashed ) {
        emit uvVersionReady( ( crashed || exitCode != 0 || captured->isEmpty() ) ? tr( "확인 실패" ) : *captured );
        delete captured;
        task->deleteLater();
    } );

    task->start();
}

}  // namespace hc::rst
