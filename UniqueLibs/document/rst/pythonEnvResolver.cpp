#include "stdafx.h"
#include "pythonEnvResolver.hpp"

/// 모듈
#include "rstSettings.hpp"
#include "pythonEnvHealth.hpp"
#include "pythonEnvManager.hpp"

/// UIs

/// 기타 헤더파일

/// 고유 라이브러리

/// 공통 라이브러리

/// 외부 라이브러리

/// 선언 헤더파일들

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>

DECLARE_CMNLIBSV2_NAMESPACE
namespace hc::rst {
namespace {


/// venv 로 인정하는 디렉터리 이름들. 우선순위 순.
const QStringList& venvCandidateNames()
{
    static const QStringList names{ QStringLiteral( ".venv" ), QStringLiteral( "venv" ),
                                   QStringLiteral( "env" ) };
    return names;
}

QString venvPythonPath( const QString& venvDir )
{
#ifdef Q_OS_WIN
    return QDir( venvDir ).filePath( QStringLiteral( "Scripts/python.exe" ) );
#else
    return QDir( venvDir ).filePath( QStringLiteral( "bin/python" ) );
#endif
}

/// pyvenv.cfg 가 있으면 복구 대상이 될 수 있는 venv 로 본다.
/// 인터프리터 누락 여부는 pythonVenvDamageReason()에서 손상으로 판정한다.
bool looksLikeVenv( const QString& directory )
{
    return QFileInfo::exists( QDir( directory ).filePath( QStringLiteral( "pyvenv.cfg" ) ) );
}

QString venvRootForPython( const QString& pythonExe )
{
    QDir scriptsDir( QFileInfo( pythonExe ).absolutePath() );
#ifdef Q_OS_WIN
    if( scriptsDir.dirName().compare( QStringLiteral( "Scripts" ), Qt::CaseInsensitive ) != 0 )
        return {};
#else
    if( scriptsDir.dirName() != QStringLiteral( "bin" ) )
        return {};
#endif
    if( !scriptsDir.cdUp() )
        return {};

    const QString root = scriptsDir.absolutePath();
    return QFileInfo::exists( scriptsDir.filePath( QStringLiteral( "pyvenv.cfg" ) ) ) ? root : QString{};
}

ResolvedPythonEnv bundledEnvironment( PythonEnvManager* manager, const QString& key,
                                      const QString& reason )
{
    ResolvedPythonEnv resolved;
    resolved.projectKey = key;
    resolved.kind = EnvKind::Bundled;
    resolved.pythonExe = manager != nullptr ? manager->pythonExe() : QString{};
    resolved.originPath = manager != nullptr ? manager->venvDir() : QString{};
    resolved.reason = reason;
    return resolved;
}

}  // namespace

QString ResolvedPythonEnv::displayName() const
{
    switch( kind )
    {
        case EnvKind::Bundled:          return QObject::tr( "내장 환경" );
        case EnvKind::ProjectVenv:      return QFileInfo( originPath ).fileName();
        case EnvKind::ExplicitOverride: return QObject::tr( "지정한 환경" );
    }
    return {};
}

PythonEnvResolver::PythonEnvResolver( PythonEnvManager* manager, QObject* parent )
    : QObject( parent )
    , manager_( manager )
{
}

QString PythonEnvResolver::projectKeyFor( const std::filesystem::path& rootPath )
{
    const QString canonical = toCanonicalQString( rootPath ).toCaseFolded();
    const QByteArray digest = QCryptographicHash::hash( canonical.toUtf8(), QCryptographicHash::Sha1 );
    return QString::fromLatin1( digest.toHex().left( 16 ) );
}

void PythonEnvResolver::setWorkspaceRoot( const QString& root )
{
    if( workspaceRoot_ == root )
        return;

    workspaceRoot_ = root;
    invalidateAll();
}

void PythonEnvResolver::invalidate( const QString& projectKey )
{
    cache_.remove( projectKey );
    requirements_.remove( projectKey );
    if( damagedKeys_.remove( projectKey ) )
        emit environmentDamageCleared( projectKey );
}

void PythonEnvResolver::invalidateAll()
{
    cache_.clear();
    requirements_.clear();
    const QSet< QString > damaged = damagedKeys_;
    damagedKeys_.clear();
    for( const QString& key : damaged )
        emit environmentDamageCleared( key );
}

const ProjectRequirements& PythonEnvResolver::requirements( const SphinxProject& project )
{
    const QString key = projectKeyFor( project.rootPath );
    if( const auto it = requirements_.constFind( key ); it != requirements_.constEnd() )
        return it.value();

    // 프로젝트의 가상환경과 선언 파일은 읽기만 한다. 무엇을 읽었는지 로그로 남긴다.
    const ProjectRequirements found =
        readProjectRequirements( toCanonicalQString( project.rootPath ), projectVenvDir( project ) );
    if( !found.sources.isEmpty() )
        emit logMessage( tr( "프로젝트의 테마 · 확장 요구를 읽었습니다 [%1]: %2" )
                             .arg( QString::fromStdWString( project.projectId ), found.sources.join( QStringLiteral( ", " ) ) ) );
    return *requirements_.insert( key, found );
}

QString PythonEnvResolver::projectVenvDir( const SphinxProject& project ) const
{
    QString origin;
    return findProjectVenv( project.rootPath, &origin ).isEmpty() ? QString{} : origin;
}

QString PythonEnvResolver::findProjectVenv( const std::filesystem::path& rootPath,
                                           QString* originPath ) const
{
    // 프로젝트 루트에서 위로 올라가며 찾는다.
    //
    // 워크스페이스 경계에서 멈추지 않는다. 실제 저장소는 docs/ 아래에 conf.py 를
    // 두고 .venv 는 저장소 루트에 두는 배치가 흔한데, 사용자가 docs/source 를
    // 워크스페이스로 열면 경계에서 멈출 경우 그 venv 를 영영 못 찾는다.
    // 가장 가까운 조상의 venv 가 그 문서를 위한 것일 확률이 압도적으로 높다.
    // 대신 깊이를 제한하고, 어떤 것을 골랐는지 로그로 남긴다.
    QDir current( toCanonicalQString( rootPath ) );

    for( int depth = 0; depth < 8; ++depth )
    {
        for( const QString& name : venvCandidateNames() )
        {
            const QString candidate = current.filePath( name );
            if( looksLikeVenv( candidate ) )
            {
                if( originPath != nullptr )
                    *originPath = candidate;
                return venvPythonPath( candidate );
            }
        }

        if( !current.cdUp() )
            break;
    }
    return {};
}

ResolvedPythonEnv PythonEnvResolver::resolve( const SphinxProject& project )
{
    const QString key = projectKeyFor( project.rootPath );
    if( const auto it = cache_.constFind( key ); it != cache_.constEnd() )
        return it.value();

    ResolvedPythonEnv resolved;
    resolved.projectKey = key;
    QString damagedPath;
    QString damageReason;

    // 언제나 HyperEditor 가 구성한 환경으로 돌린다. 프로젝트의 가상환경은 실행하지 않는다 —
    // 그 프로젝트가 쓰는 테마와 확장을 알아내는 데만 쓴다( requirements() ).
    const QString overridePath;
    const QString projectVenv = projectVenvDir( project );
    resolved = bundledEnvironment( manager_, key,
                                   projectVenv.isEmpty()
                                       ? tr( "HyperEditor 환경을 사용합니다." )
                                       : tr( "HyperEditor 환경을 사용합니다. 프로젝트 가상환경(%1)은 테마 · 확장 요구를 읽는 데만 씁니다." )
                                             .arg( QDir::toNativeSeparators( projectVenv ) ) );

    cache_.insert( key, resolved );
    emit logMessage( tr( "Python 환경 [%1]: %2" )
                        .arg( QString::fromStdWString( project.projectId ), resolved.reason ) );
    if( !damageReason.isEmpty() && !damagedKeys_.contains( key ) )
    {
        damagedKeys_.insert( key );
        emit logMessage( tr( "Python 환경 손상 [%1]: %2" )
                             .arg( QString::fromStdWString( project.projectId ), damageReason ) );
        emit environmentDamaged( key, QString::fromStdWString( project.projectId ), damagedPath,
                                 overridePath.isEmpty() ? venvPythonPath( damagedPath ) : overridePath,
                                 damageReason );
    }
    return resolved;
}

void PythonEnvResolver::reportRuntimeFailure( const SphinxProject& project, const QString& pythonExe,
                                              const QString& reason )
{
    const QString key = projectKeyFor( project.rootPath );
    QString environmentPath;
    if( const auto it = cache_.constFind( key ); it != cache_.constEnd() && !it->isBundled() )
        environmentPath = QFileInfo( it->originPath ).isDir() ? it->originPath : venvRootForPython( pythonExe );
    if( environmentPath.isEmpty() )
        environmentPath = venvRootForPython( pythonExe );

    cache_.insert( key, bundledEnvironment( manager_, key,
                                           tr( "프로젝트 Python 실행에 실패하여 내장 환경을 사용합니다." ) ) );
    if( damagedKeys_.contains( key ) )
        return;

    damagedKeys_.insert( key );
    emit logMessage( tr( "Python 환경 손상 [%1]: %2" )
                         .arg( QString::fromStdWString( project.projectId ), reason ) );
    emit environmentDamaged( key, QString::fromStdWString( project.projectId ), environmentPath,
                             pythonExe, reason );
}

void PythonEnvResolver::clearDamage( const QString& projectKey )
{
    invalidate( projectKey );
}

}  // namespace hc::rst
