#include "stdafx.h"
#include "workspaceDiscovery.hpp"

/// 모듈

/// UIs

/// 기타 헤더파일

/// 고유 라이브러리
#include "workspacePaths.hpp"
#include "UniqueLibs/document/explorer/remoteDrive.hpp"

/// 공통 라이브러리

/// 외부 라이브러리

/// 선언 헤더파일들

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutexLocker>
#include <QSet>

namespace hc::document::workspace
{
    namespace
    {
        QJsonObject ReadJson( const QString& Path )
        {
            QFile File( Path );
            if( !File.open( QIODevice::ReadOnly ) ) return {};
            const auto Json = QJsonDocument::fromJson( File.readAll() );
            return Json.isObject() ? Json.object() : QJsonObject{};
        }
    }

    MarkerKind ReadMarker( const QString& Folder )
    {
        const QDir Dir( QDir( Folder ).filePath( QStringLiteral( ".multiroot" ) ) );
        if( QFileInfo::exists( Dir.filePath( QStringLiteral( "hypereditor.json" ) ) ) )
            return MarkerKind::HyperEditor;
        // MRE 의 세션 파일은 schema 1 일 때만 워크스페이스로 친다(그 밖의 판은 MRE 도 버린다).
        const auto Mre = Dir.filePath( QStringLiteral( "workspace.json" ) );
        if( QFileInfo::exists( Mre ) && ReadJson( Mre ).value( "schema" ).toInt() == 1 )
            return MarkerKind::MultiRoot;
        return MarkerKind::None;
    }

    FindResult FindAncestorWorkspaces( const QString& FilePath, const QStringList& KnownRoots, int DeadlineMs )
    {
        FindResult Result;
        const QFileInfo Info( FilePath );
        const QString Folder = Info.isDir() ? NormalizePath( FilePath ) : NormalizePath( Info.absolutePath() );
        if( Folder.isEmpty() || explorer::IsDisconnectedRemoteDrivePath( Folder ) )
            return Result;
        QSet< QString > Seen;
        QElapsedTimer Clock; Clock.start();
        // 알려진 루트는 디스크를 읽지 않고 앞머리로만 맞춘다(프로필 상태 · 최근 워크스페이스).
        QHash< QString, QString > Known;
        for( const auto& Root : KnownRoots ) if( !Root.isEmpty() ) Known.insert( PathKey( Root ), NormalizeRoot( Root ) );
        for( const auto& Ancestor : AncestorFolders( Folder ) ) {
            const QString Key = PathKey( Ancestor );
            if( Seen.contains( Key ) ) continue;
            if( Clock.elapsed() > DeadlineMs ) { Result.timedOut_ = true; break; }
            Candidate Found; Found.root_ = Ancestor; bool Hit = false;
            const MarkerKind Marker = ReadMarker( Ancestor );
            MarkerCache::Instance().Put( Ancestor, Marker );
            if( Marker == MarkerKind::HyperEditor ) {
                const auto Json = ReadJson( QDir( Ancestor ).filePath( QStringLiteral( ".multiroot/hypereditor.json" ) ) );
                Found.kind_ = Candidate::Kind::HyperEditor;
                Found.savedAt_ = QDateTime::fromString( Json.value( "savedAt" ).toString(), Qt::ISODate );
                Found.documentCount_ = int( Json.value( "documents" ).toArray().size() );
                Hit = true;
            }
            else if( Marker == MarkerKind::MultiRoot ) {
                const auto Path = QDir( Ancestor ).filePath( QStringLiteral( ".multiroot/workspace.json" ) );
                Found.kind_ = Candidate::Kind::MultiRoot;
                Found.savedAt_ = QFileInfo( Path ).lastModified();
                Found.documentCount_ = int( ReadJson( Path ).value( "documents" ).toArray().size() );
                Hit = true;
            }
            else if( Known.contains( Key ) ) {
                Found.kind_ = Candidate::Kind::Known;
                Hit = true;
            }
            if( Hit ) { Seen.insert( Key ); Result.candidates_.append( Found ); }
        }
        return Result;
    }

    // ============================================================================================ MarkerCache

    MarkerCache& MarkerCache::Instance()
    {
        static MarkerCache Cache;
        return Cache;
    }

    MarkerKind MarkerCache::Get( const QString& Folder )
    {
        MarkerKind Kind{};
        if( Peek( Folder, &Kind ) ) return Kind;
        Kind = ReadMarker( Folder );
        Put( Folder, Kind );
        return Kind;
    }

    bool MarkerCache::Peek( const QString& Folder, MarkerKind* Kind ) const
    {
        QMutexLocker Lock( &mutex_ );
        const auto At = markers_.constFind( PathKey( Folder ) );
        if( At == markers_.cend() ) return false;
        if( Kind ) *Kind = At.value();
        return true;
    }

    void MarkerCache::Put( const QString& Folder, MarkerKind Kind )
    {
        QMutexLocker Lock( &mutex_ );
        markers_.insert( PathKey( Folder ), Kind );
    }

    void MarkerCache::Invalidate( const QString& Folder )
    {
        QMutexLocker Lock( &mutex_ );
        markers_.remove( PathKey( Folder ) );
    }

    void MarkerCache::Clear()
    {
        QMutexLocker Lock( &mutex_ );
        markers_.clear();
    }

    // ============================================================================================ Registry

    Registry& Registry::Instance()
    {
        static Registry Value;
        return Value;
    }

    bool Registry::Bind( const QString& Root, QObject* Window )
    {
        if( Root.isEmpty() || !Window ) return false;
        const QString Key = PathKey( Root );
        if( const auto At = windows_.constFind( Key ); At != windows_.cend() && At.value().second && At.value().second != Window )
            return false;
        Unbind( Window );
        windows_.insert( Key, { NormalizeRoot( Root ), Window } );
        connect( Window, &QObject::destroyed, this, &Registry::purge, Qt::UniqueConnection );
        return true;
    }

    void Registry::purge()
    {
        // destroyed 가 올 때 QPointer 는 이미 비어 있다.
        for( auto At = windows_.begin(); At != windows_.end(); )
            At = At.value().second ? std::next( At ) : windows_.erase( At );
    }

    void Registry::Unbind( QObject* Window )
    {
        for( auto At = windows_.begin(); At != windows_.end(); )
            At = ( !At.value().second || At.value().second == Window ) ? windows_.erase( At ) : std::next( At );
    }

    QObject* Registry::WindowFor( const QString& Root ) const
    {
        const auto At = windows_.constFind( PathKey( Root ) );
        return At == windows_.cend() ? nullptr : At.value().second.data();
    }

    QObject* Registry::WindowContaining( const QString& Path, QString* Root ) const
    {
        QObject* Best = nullptr; QString BestRoot;
        for( auto At = windows_.cbegin(); At != windows_.cend(); ++At ) {
            if( !At.value().second || !IsInside( Path, At.value().first, false ) ) continue;
            // 겹친 워크스페이스는 가장 깊은 루트를 고른다(설계 6.1).
            if( BestRoot.isEmpty() || At.value().first.size() > BestRoot.size() ) { BestRoot = At.value().first; Best = At.value().second.data(); }
        }
        if( Root ) *Root = BestRoot;
        return Best;
    }

    QString Registry::RootOf( const QObject* Window ) const
    {
        for( auto At = windows_.cbegin(); At != windows_.cend(); ++At )
            if( At.value().second == Window ) return At.value().first;
        return {};
    }

    QStringList Registry::Roots() const
    {
        QStringList Result;
        for( auto At = windows_.cbegin(); At != windows_.cend(); ++At ) if( At.value().second ) Result.append( At.value().first );
        return Result;
    }
}
