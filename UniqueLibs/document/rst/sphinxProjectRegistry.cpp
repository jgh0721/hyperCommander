#include "stdafx.h"
#include "sphinxProjectRegistry.hpp"

/// 모듈

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
#include <QTimer>

#include <algorithm>

DECLARE_CMNLIBSV2_NAMESPACE

namespace hc::rst {
namespace fs = std::filesystem;

ProjectRegistry::ProjectRegistry( QObject* parent )
    : QObject( parent )
{
}

ProjectRegistry::~ProjectRegistry() = default;

void ProjectRegistry::setScannerSettings( ScannerSettings settings )
{
    const bool buildDirChanged = settings.buildDirName != settings_.buildDirName;
    settings_ = std::move( settings );
    // 빌드 폴더 이름이 바뀌면 이미 만든 프로젝트의 buildPath 도 따라가야 한다.
    if( buildDirChanged )
        for( SphinxProject& project : projects_ )
            project.buildPath = project.rootPath / fs::path( settings_.buildDirName );
}

const ScannerSettings& ProjectRegistry::scannerSettings() const
{
    return settings_;
}

const std::vector< SphinxProject >& ProjectRegistry::projects() const
{
    return projects_;
}

SphinxProject ProjectRegistry::makeProject( const fs::path& confPath ) const
{
    // 식별자는 프로젝트 폴더 이름에 경로 해시를 붙인 것이다. 같은 이름의 폴더( docs, source )가 여러 곳에
    // 있어도 겹치지 않고, 파일 이름에 그대로 쓸 수 있다.
    SphinxProject project = ProjectScanner( confPath.parent_path().parent_path(), settings_ ).projectFromConf( confPath );
    const QString canonical = toCanonicalQString( project.rootPath ).toCaseFolded();
    const QByteArray digest = QCryptographicHash::hash( canonical.toUtf8(), QCryptographicHash::Sha1 ).toHex().left( 8 );
    QString name = toQString( project.rootPath.filename() );
    if( name.isEmpty() )
        name = QStringLiteral( "project" );
    project.projectId = ( name + QLatin1Char( '-' ) + QString::fromLatin1( digest ) ).toStdWString();
    return project;
}

const SphinxProject* ProjectRegistry::resolveForFile( const QString& filePath )
{
    if( filePath.isEmpty() )
        return nullptr;

    // 알려진 프로젝트부터 보지 않는다. 바깥 프로젝트를 먼저 알고 있으면 그 안쪽에 따로 있는 프로젝트의
    // 문서까지 바깥 것에 붙는다. 가장 가까운 conf.py 가 언제나 답이다.
    QDir directory = QFileInfo( filePath ).absoluteDir();
    for( int depth = 0; depth < MaxAncestorDepth; ++depth )
    {
        const QString conf = directory.filePath( QStringLiteral( "conf.py" ) );
        if( QFileInfo( conf ).isFile() )
        {
            const QString canonical = toCanonicalQString( toPath( conf ) );
            for( const SphinxProject& known : projects_ )
            {
                if( toCanonicalQString( known.confPath ).compare( canonical, Qt::CaseInsensitive ) == 0 )
                    return &known;
            }
            SphinxProject project = makeProject( toPath( conf ) );
            emit logMessage( tr( "Sphinx 프로젝트: %1 — %2" )
                                .arg( QString::fromStdWString( project.projectId ), toQString( project.rootPath ) ) );
            projects_.push_back( std::move( project ) );
            return &projects_.back();
        }
        if( !directory.cdUp() )
            break;
    }
    return nullptr;
}

const SphinxProject* ProjectRegistry::findById( const QString& projectId ) const
{
    if( projectId.isEmpty() )
        return nullptr;

    for( const SphinxProject& project : projects_ )
    {
        if( QString::fromStdWString( project.projectId ) == projectId )
            return &project;
    }
    return nullptr;
}

void ProjectRegistry::rescanAsync()
{
    emit scanStarted();

    std::vector< SphinxProject > kept;
    kept.reserve( projects_.size() );
    for( const SphinxProject& project : projects_ )
    {
        if( !QFileInfo( toQString( project.confPath ) ).isFile() )
        {
            emit logMessage( tr( "Sphinx 프로젝트가 사라졌습니다: %1" ).arg( toQString( project.rootPath ) ) );
            continue;
        }
        kept.push_back( makeProject( project.confPath ) );
    }
    projects_ = std::move( kept );

    // 받는 쪽이 목록을 다시 읽는 동안 resolveForFile() 이 목록을 늘릴 수 있다. 부른 자리의 호출 스택을
    // 벗어난 뒤에 알린다.
    QTimer::singleShot( 0, this, [ this ] { emit scanFinished( static_cast< int >( projects_.size() ) ); } );
}

}  // namespace hc::rst
