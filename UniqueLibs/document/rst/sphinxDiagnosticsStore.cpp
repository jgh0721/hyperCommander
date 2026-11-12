#include "stdafx.h"
#include "sphinxDiagnosticsStore.hpp"

/// 모듈

/// UIs

/// 기타 헤더파일

/// 고유 라이브러리

/// 공통 라이브러리

/// 외부 라이브러리

/// 선언 헤더파일들

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QFileInfo>
#include <QSet>

DECLARE_CMNLIBSV2_NAMESPACE
namespace hc::rst {

DiagnosticsStore::DiagnosticsStore( QObject* parent )
    : QObject( parent )
{
}

QString DiagnosticsStore::normalizeKey( const QString& path )
{
    if( path.isEmpty() )
        return {};
    // Windows 는 대소문자를 구분하지 않으므로 키를 접어서 맞춘다.
    return QFileInfo( path ).absoluteFilePath().toCaseFolded();
}

void DiagnosticsStore::replaceSource( const QString& source, const QVector< DiagnosticEntry >& entries )
{
    QHash< QString, QVector< DiagnosticEntry > > grouped;
    for( const DiagnosticEntry& entry : entries )
        grouped[ normalizeKey( entry.path ) ].push_back( entry );

    // 이번에 사라진 파일들도 알려야 편집기 스퀴글이 지워진다.
    QSet< QString > touched;
    const auto previous = bySource_.value( source );
    for( auto it = previous.constBegin(); it != previous.constEnd(); ++it )
        touched.insert( it.key() );
    for( auto it = grouped.constBegin(); it != grouped.constEnd(); ++it )
        touched.insert( it.key() );

    bySource_[ source ] = std::move( grouped );

    emit changed();
    for( const QString& key : touched )
        emit pathChanged( key );
}

void DiagnosticsStore::replaceSourceForPath( const QString& source, const QString& path,
                                            const QVector< DiagnosticEntry >& entries )
{
    const QString key = normalizeKey( path );
    if( key.isEmpty() )
        return;

    if( entries.isEmpty() )
        bySource_[ source ].remove( key );
    else
        bySource_[ source ][ key ] = entries;

    emit changed();
    emit pathChanged( key );
}

void DiagnosticsStore::replacePathsForSource(
    const QString& source, const QStringList& paths,
    const QHash< QString, QVector< DiagnosticEntry > >& entriesByPath )
{
    // 들어온 진단을 우리 키 규칙으로 다시 묶는다. 호출자가 정규화한 경로를
    // 넘겨도 대소문자 접기까지 맞춰야 조회가 들어맞는다.
    QHash< QString, QVector< DiagnosticEntry > > grouped;
    grouped.reserve( entriesByPath.size() );
    for( auto it = entriesByPath.constBegin(); it != entriesByPath.constEnd(); ++it )
    {
        const QString key = normalizeKey( it.key() );
        if( !key.isEmpty() )
            grouped[ key ] += it.value();
    }

    QHash< QString, QVector< DiagnosticEntry > >& forSource = bySource_[ source ];

    // 알릴 키를 모아 둔다. 같은 파일이 목록과 진단 양쪽에 있으면 한 번만 알린다.
    QStringList touched;
    touched.reserve( paths.size() + grouped.size() );
    QSet< QString > seen;

    const auto note = [ &touched, &seen ]( const QString& key ) {
        if( !seen.contains( key ) )
        {
            seen.insert( key );
            touched << key;
        }
    };

    for( const QString& path : paths )
    {
        const QString key = normalizeKey( path );
        if( key.isEmpty() )
            continue;

        const QVector< DiagnosticEntry > entries = grouped.value( key );
        if( entries.isEmpty() )
            forSource.remove( key );
        else
            forSource[ key ] = entries;
        note( key );
    }

    // 처리 목록에 **없는** 파일의 진단은 저장하지 않는다. 예전 반복 호출도
    // 그랬다(`grouped.value(key)` 를 목록 순회로만 꺼냈다). 이 함수는 신호를
    // 모으는 것이 목적이므로 저장 규칙은 한 글자도 바꾸지 않는다 — 넓히는 것은
    // 그 자체로 판단이 필요한 별개의 변경이다.

    emit changed();
    for( const QString& key : touched )
        emit pathChanged( key );
}

void DiagnosticsStore::clearSource( const QString& source )
{
    if( !bySource_.contains( source ) )
        return;

    const auto removed = bySource_.take( source );
    emit changed();
    for( auto it = removed.constBegin(); it != removed.constEnd(); ++it )
        emit pathChanged( it.key() );
}

void DiagnosticsStore::clear()
{
    bySource_.clear();
    emit changed();
}

QVector< DiagnosticEntry > DiagnosticsStore::all() const
{
    QVector< DiagnosticEntry > merged;
    for( auto source = bySource_.constBegin(); source != bySource_.constEnd(); ++source )
    {
        for( auto path = source.value().constBegin(); path != source.value().constEnd(); ++path )
            merged += path.value();
    }
    return deduplicateDiagnostics( merged );
}

QVector< DiagnosticEntry > DiagnosticsStore::forPath( const QString& path ) const
{
    const QString key = normalizeKey( path );
    QVector< DiagnosticEntry > merged;
    for( auto source = bySource_.constBegin(); source != bySource_.constEnd(); ++source )
        merged += source.value().value( key );

    return deduplicateDiagnostics( merged );
}

int DiagnosticsStore::count() const
{
    int total = 0;
    for( auto source = bySource_.constBegin(); source != bySource_.constEnd(); ++source )
    {
        for( auto path = source.value().constBegin(); path != source.value().constEnd(); ++path )
            total += static_cast< int >( path.value().size() );
    }
    return total;
}

}  // namespace hc::rst
