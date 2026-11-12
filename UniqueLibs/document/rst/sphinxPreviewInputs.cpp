#include "stdafx.h"
#include "sphinxPreviewInputs.hpp"

/// 모듈
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
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonDocument>
#include <QFileInfo>

DECLARE_CMNLIBSV2_NAMESPACE
namespace hc::rst {

bool previewInputsChanged( const QString& inputsFile )
{
    if( inputsFile.isEmpty() )
    {
        traceP( "preview.gate.miss", QStringLiteral( "no-inputs-path" ) );
        return true;
    }

    QFile file( inputsFile );
    if( !file.open( QIODevice::ReadOnly ) )
    {
        traceP( "preview.gate.miss", QStringLiteral( "no-inputs-file" ) );
        return true;
    }

    const QJsonObject root = QJsonDocument::fromJson( file.readAll() ).object();
    file.close();

    if( root.value( QStringLiteral( "schema" ) ).toInt() != 2 )
    {
        traceP( "preview.gate.miss", QStringLiteral( "schema" ) );
        return true;
    }

    if( !root.value( QStringLiteral( "templatesComplete" ) ).toBool()
        || !root.value( QStringLiteral( "templateDirs" ) ).isArray() )
        return true;
    for( const auto& value : root.value( QStringLiteral( "templateDirs" ) ).toArray() )
    {
        const auto entry = value.toObject();
        const QString path = entry.value( QStringLiteral( "p" ) ).toString();
        if( path.isEmpty() || !entry.value( QStringLiteral( "files" ) ).isArray() )
            return true;
        QStringList expected;
        for( const auto& name : entry.value( QStringLiteral( "files" ) ).toArray() )
            expected << name.toString();
        QStringList actual;
        const QDir directory( path );
        QDirIterator iterator( path, QDir::Files | QDir::Hidden | QDir::System,
                               QDirIterator::Subdirectories );
        while( iterator.hasNext() )
            actual << directory.relativeFilePath( iterator.next() );
        expected.sort();
        actual.sort();
        if( expected != actual )
        {
            traceP( "preview.gate.miss", QStringLiteral( "template-files %1" ).arg( path ) );
            return true;
        }
    }

    const QJsonArray files = root.value( QStringLiteral( "files" ) ).toArray();
    if( files.isEmpty() )
    {
        traceP( "preview.gate.miss", QStringLiteral( "empty-inputs" ) );
        return true;
    }

    for( const QJsonValue& value : files )
    {
        const QJsonObject entry = value.toObject();
        const QFileInfo info( entry.value( QStringLiteral( "p" ) ).toString() );
        if( !info.exists() )
        {
            traceP( "preview.gate.miss", QStringLiteral( "gone %1" ).arg( info.fileName() ) );
            return true;
        }
        if( info.size() != entry.value( QStringLiteral( "s" ) ).toInteger() )
        {
            traceP( "preview.gate.miss", QStringLiteral( "size %1" ).arg( info.fileName() ) );
            return true;
        }
        // 빌더는 ns 로 남긴다. Qt 는 ms 까지만 주므로 ms 로 맞춰 비교한다.
        const qint64 recordedMs = entry.value( QStringLiteral( "m" ) ).toInteger() / 1000000;
        if( info.lastModified().toMSecsSinceEpoch() != recordedMs )
        {
            traceP( "preview.gate.miss", QStringLiteral( "mtime %1" ).arg( info.fileName() ) );
            return true;
        }
    }

    // 목록에 없던 **새 문서**가 생기는 것은 위 비교로 잡히지 않는다. 개수로 잡는다.
    const QString sourceDir = root.value( QStringLiteral( "sourceDir" ) ).toString();
    QStringList suffixes;
    for( const QJsonValue& value : root.value( QStringLiteral( "sourceSuffixes" ) ).toArray() )
        suffixes << value.toString();

    if( !sourceDir.isEmpty() && !suffixes.isEmpty() )
    {
        int count = 0;
        QDirIterator it( sourceDir, QDir::Files, QDirIterator::Subdirectories );
        while( it.hasNext() )
        {
            it.next();
            if( suffixes.contains( QLatin1Char( '.' ) + it.fileInfo().suffix() ) )
                ++count;
        }
        if( count != root.value( QStringLiteral( "sourceCount" ) ).toInt( -1 ) )
        {
            traceP( "preview.gate.miss", QStringLiteral( "source-count %1" ).arg( count ) );
            return true;
        }
    }

    traceP( "preview.gate.hit", QStringLiteral( "%1 inputs" ).arg( files.size() ) );
    return false;
}


int prunePreviewInputs( const QString& stateDirectory, const int maxAgeDays )
{
    const QDir directory( stateDirectory );
    if( stateDirectory.isEmpty() || !directory.exists() )
        return 0;

    int removed = 0;
    const QDateTime now = QDateTime::currentDateTimeUtc();
    const QFileInfoList files =
        directory.entryInfoList( { QStringLiteral( "preview-inputs-*.json" ) }, QDir::Files | QDir::NoSymLinks );
    for( const QFileInfo& info : files )
    {
        bool stale = maxAgeDays > 0 && info.lastModified().toUTC().daysTo( now ) > maxAgeDays;
        if( !stale )
        {
            // 지문에 적힌 소스 폴더가 없으면 그 프로젝트는 지워졌거나 옮겨졌다.
            QFile file( info.absoluteFilePath() );
            QJsonObject root;
            if( file.open( QIODevice::ReadOnly ) && file.size() <= 64 * 1024 * 1024 )
                root = QJsonDocument::fromJson( file.readAll() ).object();
            const QString sourceDir = root.value( QStringLiteral( "sourceDir" ) ).toString();
            stale = sourceDir.isEmpty() || !QFileInfo( sourceDir ).isDir();
        }
        if( stale && QFile::remove( info.absoluteFilePath() ) )
            ++removed;
    }
    return removed;
}

} // namespace hc::rst
