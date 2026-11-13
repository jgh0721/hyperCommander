#include "stdafx.h"
#include "workspaceState.hpp"

/// 모듈

/// UIs

/// 기타 헤더파일

/// 고유 라이브러리
#include "workspacePaths.hpp"

/// 공통 라이브러리

/// 외부 라이브러리

/// 선언 헤더파일들

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QSet>

namespace hc::document::workspace
{
    namespace
    {
        const QStringList KnownKeys{ QStringLiteral( "schema" ), QStringLiteral( "app" ), QStringLiteral( "savedAt" ), QStringLiteral( "root" ),
            QStringLiteral( "documents" ), QStringLiteral( "active" ), QStringLiteral( "explorer" ), QStringLiteral( "dock" ),
            QStringLiteral( "window" ), QStringLiteral( "findInFiles" ) };

        std::optional< QJsonObject > ReadObject( const QString& Path, QString* Error = nullptr )
        {
            QFile File( Path );
            if( !File.exists() )
                return std::nullopt;
            if( !File.open( QIODevice::ReadOnly ) ) { if( Error ) *Error = File.errorString(); return std::nullopt; }
            QJsonParseError Parse{};
            const auto Json = QJsonDocument::fromJson( File.readAll(), &Parse );
            if( Parse.error != QJsonParseError::NoError || !Json.isObject() ) { if( Error ) *Error = Parse.errorString(); return QJsonObject{}; }
            return Json.object();
        }

        bool WriteObject( const QString& Path, const QJsonObject& Json, QString* Error )
        {
            if( !QDir().mkpath( QFileInfo( Path ).absolutePath() ) ) { if( Error ) *Error = QStringLiteral( "mkpath" ); return false; }
            QSaveFile File( Path );
            File.setDirectWriteFallback( false );
            if( !File.open( QIODevice::WriteOnly ) ) { if( Error ) *Error = File.errorString(); return false; }
            const auto Bytes = QJsonDocument( Json ).toJson( QJsonDocument::Indented );
            if( File.write( Bytes ) != Bytes.size() || !File.commit() ) { if( Error ) *Error = File.errorString(); return false; }
            return true;
        }

        QJsonObject PositionJson( const Position& Value )
        {
            QJsonObject Json;
            if( Value.caret_ >= 0 ) {
                Json[ "anchor" ] = double( qMax< qint64 >( 0, Value.anchor_ ) );
                Json[ "caret" ] = double( Value.caret_ );
                Json[ "firstLine" ] = qMax( 0, Value.firstLine_ );
                Json[ "firstSubLine" ] = qMax( 0, Value.firstSubLine_ );
                Json[ "xOffset" ] = qMax( 0, Value.xOffset_ );
            }
            else if( Value.caretLine_ > 0 ) {
                Json[ "caretLine" ] = Value.caretLine_;
                Json[ "caretColumn" ] = qMax( 1, Value.caretColumn_ );
                Json[ "firstLine" ] = qMax( 0, Value.firstLine_ );
            }
            return Json;
        }

        std::optional< Position > PositionFrom( const QJsonObject& Json )
        {
            Position Value;
            if( Json.contains( "caret" ) ) {
                // 음수는 버린다(MRE solWorkspaceSession.cpp:138-141 처럼 다듬는다).
                Value.caret_ = qint64( Json[ "caret" ].toDouble( -1 ) );
                if( Value.caret_ < 0 ) return std::nullopt;
                Value.anchor_ = qMax< qint64 >( 0, qint64( Json[ "anchor" ].toDouble( double( Value.caret_ ) ) ) );
                Value.firstLine_ = qMax( 0, Json[ "firstLine" ].toInt() );
                Value.firstSubLine_ = qMax( 0, Json[ "firstSubLine" ].toInt() );
                Value.xOffset_ = qMax( 0, Json[ "xOffset" ].toInt() );
                return Value;
            }
            if( Json.contains( "caretLine" ) ) {
                Value.caretLine_ = qMax( 1, Json[ "caretLine" ].toInt( 1 ) );
                Value.caretColumn_ = qMax( 1, Json[ "caretColumn" ].toInt( 1 ) );
                Value.firstLine_ = qMax( 0, Json[ "firstLine" ].toInt() );
                return Value;
            }
            return std::nullopt;
        }

        QString StoredOrEmpty( const QString& Root, const QString& Path )
        {
            return Path.isEmpty() ? QString{} : ToStored( Root, Path );
        }
    }

    QJsonObject ToJson( const State& Value, const QString& Root )
    {
        QJsonObject Json = Value.extra_;
        Json[ "schema" ] = State::kSchema;
        Json[ "app" ] = QStringLiteral( "HyperEditor" );
        Json[ "savedAt" ] = ( Value.savedAt_.isValid() ? Value.savedAt_ : QDateTime::currentDateTime() ).toOffsetFromUtc( QDateTime::currentDateTime().offsetFromUtc() ).toString( Qt::ISODate );
        Json[ "root" ] = NormalizeRoot( Root );
        QJsonArray Documents;
        for( const auto& Each : Value.documents_ ) {
            if( Each.path_.isEmpty() ) continue;
            bool Outside = false;
            QJsonObject Item{ { "path", ToStored( Root, Each.path_, &Outside ) } };
            if( Outside ) Item[ "outside" ] = true;
            if( !Each.kind_.isEmpty() ) Item[ "kind" ] = Each.kind_;
            if( Each.position_ && Each.position_->IsValid() ) Item[ "position" ] = PositionJson( *Each.position_ );
            if( !Each.view_.isEmpty() ) Item[ "view" ] = Each.view_;
            Documents.append( Item );
        }
        Json[ "documents" ] = Documents;
        if( !Value.active_.isEmpty() ) Json[ "active" ] = ToStored( Root, Value.active_ ); else Json.remove( "active" );
        QJsonArray Expanded;
        for( const auto& Each : Value.explorer_.expanded_ ) if( !Each.isEmpty() ) Expanded.append( ToStored( Root, Each ) );
        Json[ "explorer" ] = QJsonObject{ { "expanded", Expanded }, { "selected", StoredOrEmpty( Root, Value.explorer_.selected_ ) },
            { "topItem", StoredOrEmpty( Root, Value.explorer_.topItem_ ) } };
        if( !Value.dockLayout_.isEmpty() ) Json[ "dock" ] = QJsonObject{ { "layout", QString::fromLatin1( Value.dockLayout_.toBase64() ) }, { "version", Value.dockVersion_ } };
        else Json.remove( "dock" );
        if( !Value.geometry_.isEmpty() ) Json[ "window" ] = QJsonObject{ { "geometry", QString::fromLatin1( Value.geometry_.toBase64() ) } };
        else Json.remove( "window" );
        if( !Value.findInFiles_.isEmpty() ) Json[ "findInFiles" ] = Value.findInFiles_; else Json.remove( "findInFiles" );
        return Json;
    }

    State FromJson( const QJsonObject& Json, const QString& Root )
    {
        State Value;
        Value.schema_ = Json[ "schema" ].toInt( 0 );
        Value.newerSchema_ = Value.schema_ > State::kSchema;
        Value.savedAt_ = QDateTime::fromString( Json[ "savedAt" ].toString(), Qt::ISODate );
        Value.root_ = Json[ "root" ].toString();
        for( auto At = Json.begin(); At != Json.end(); ++At )
            if( !KnownKeys.contains( At.key() ) ) Value.extra_.insert( At.key(), At.value() );
        QSet< QString > Seen;
        for( const auto& Item : Json[ "documents" ].toArray() ) {
            const auto Object = Item.toObject();
            const auto Stored = Object[ "path" ].toString();
            if( Stored.isEmpty() ) continue;
            DocumentEntry Entry;
            Entry.path_ = FromStored( Root, Stored );
            if( Entry.path_.isEmpty() ) continue;
            // 같은 문서가 두 번 적혀 있으면 앞의 것만 쓴다(MRE solWorkspaceSession.cpp:122-127 의 중복 제거와 같은 뜻).
            const auto Key = PathKey( Entry.path_ );
            if( Seen.contains( Key ) ) continue;
            Seen.insert( Key );
            Entry.outside_ = !IsInside( Entry.path_, Root, false );
            Entry.kind_ = Object[ "kind" ].toString();
            Entry.position_ = PositionFrom( Object[ "position" ].toObject() );
            Entry.view_ = Object[ "view" ].toObject();
            Value.documents_.append( Entry );
        }
        if( const auto Active = Json[ "active" ].toString(); !Active.isEmpty() ) Value.active_ = FromStored( Root, Active );
        const auto Explorer = Json[ "explorer" ].toObject();
        for( const auto& Item : Explorer[ "expanded" ].toArray() )
            if( const auto Path = FromStored( Root, Item.toString() ); !Path.isEmpty() ) Value.explorer_.expanded_.append( Path );
        if( const auto Selected = Explorer[ "selected" ].toString(); !Selected.isEmpty() ) Value.explorer_.selected_ = FromStored( Root, Selected );
        if( const auto Top = Explorer[ "topItem" ].toString(); !Top.isEmpty() ) Value.explorer_.topItem_ = FromStored( Root, Top );
        const auto Dock = Json[ "dock" ].toObject();
        Value.dockLayout_ = QByteArray::fromBase64( Dock[ "layout" ].toString().toLatin1() );
        Value.dockVersion_ = Dock[ "version" ].toInt();
        Value.geometry_ = QByteArray::fromBase64( Json[ "window" ].toObject()[ "geometry" ].toString().toLatin1() );
        Value.findInFiles_ = Json[ "findInFiles" ].toObject();
        return Value;
    }

    // ============================================================================================ Store

    Store::Store( QString ConfigDirectory ) : configDirectory_( std::move( ConfigDirectory ) ) {}

    QString Store::FolderStatePath( const QString& Root )
    {
        return QDir( NormalizeRoot( Root ) ).filePath( QStringLiteral( ".multiroot/hypereditor.json" ) );
    }

    QString Store::ProfileStatePath( const QString& Root ) const
    {
        const auto Hash = QCryptographicHash::hash( PathKey( Root ).toUtf8(), QCryptographicHash::Sha1 ).toHex();
        return QDir( configDirectory_ ).filePath( QStringLiteral( "workspaces/" ) + QString::fromLatin1( Hash ) + QStringLiteral( ".json" ) );
    }

    LoadResult Store::Load( const QString& Root ) const
    {
        LoadResult Result;
        QString FolderError, ProfileError;
        const auto Folder = ReadObject( FolderStatePath( Root ), &FolderError );
        const auto Profile = configDirectory_.isEmpty() ? std::nullopt : ReadObject( ProfileStatePath( Root ), &ProfileError );
        std::optional< State > FromFolder, FromProfile;
        if( Folder && !Folder->isEmpty() ) FromFolder = FromJson( *Folder, Root );
        if( Profile && !Profile->isEmpty() ) FromProfile = FromJson( *Profile, Root );
        if( !FromFolder && !FromProfile ) { Result.error_ = !FolderError.isEmpty() ? FolderError : ProfileError; return Result; }
        Result.found_ = true;
        // 둘 다 있으면 savedAt 이 새 쪽(폴더에 못 써 프로필로 간 뒤 폴더가 다시 쓸 수 있게 된 경우).
        const bool UseProfile = FromProfile && ( !FromFolder || ( FromProfile->savedAt_.isValid() && ( !FromFolder->savedAt_.isValid() || FromProfile->savedAt_ > FromFolder->savedAt_ ) ) );
        Result.state_ = UseProfile ? *FromProfile : *FromFolder;
        Result.from_ = UseProfile ? Location::Profile : Location::Folder;
        return Result;
    }

    SaveResult Store::Save( const QString& Root, const State& Value, Location Preferred ) const
    {
        SaveResult Result;
        const auto Write = [ this, &Root, &Value ]( Location Where, QString* Error ) {
            const auto Path = Where == Location::Folder ? FolderStatePath( Root ) : ProfileStatePath( Root );
            if( Where == Location::Profile && configDirectory_.isEmpty() ) { *Error = QStringLiteral( "no-profile" ); return false; }
            // 더 새 판이 쓴 파일은 덮어쓰지 않는다(새 판의 상태를 옛 판이 망치지 않게).
            if( const auto Existing = ReadObject( Path ); Existing && Existing->value( "schema" ).toInt() > State::kSchema ) { *Error = QStringLiteral( "newer-schema" ); return false; }
            auto Json = ToJson( Value, Root );
            if( Where == Location::Profile ) Json[ "root" ] = NormalizeRoot( Root );
            return WriteObject( Path, Json, Error );
        };
        QString Error;
        if( Write( Preferred, &Error ) ) { Result.ok_ = true; Result.to_ = Preferred; return Result; }
        if( Error == QLatin1String( "newer-schema" ) || Preferred == Location::Profile ) { Result.error_ = Error; return Result; }
        QString Fallback;
        if( Write( Location::Profile, &Fallback ) ) { Result.ok_ = true; Result.to_ = Location::Profile; Result.fellBack_ = true; Result.error_ = Error; return Result; }
        Result.error_ = Error + QStringLiteral( "; " ) + Fallback;
        return Result;
    }

    QStringList Store::ProfileRoots() const
    {
        QStringList Roots;
        if( configDirectory_.isEmpty() ) return Roots;
        QDirIterator Files( QDir( configDirectory_ ).filePath( QStringLiteral( "workspaces" ) ), { QStringLiteral( "*.json" ) }, QDir::Files );
        while( Files.hasNext() ) {
            const auto Json = ReadObject( Files.next() );
            if( Json && !Json->value( "root" ).toString().isEmpty() ) Roots.append( NormalizeRoot( Json->value( "root" ).toString() ) );
        }
        return Roots;
    }

    // ============================================================================================ MRE 가져오기

    QString MultiRootSessionPath( const QString& Root )
    {
        return QDir( NormalizeRoot( Root ) ).filePath( QStringLiteral( ".multiroot/workspace.json" ) );
    }

    std::optional< State > ImportMultiRoot( const QString& Root )
    {
        const auto Json = ReadObject( MultiRootSessionPath( Root ) );
        // schema 1 만 읽는다. MRE 도 다른 판은 통째로 버린다(solWorkspaceSession.cpp:114-117).
        if( !Json || Json->isEmpty() || Json->value( "schema" ).toInt() != 1 )
            return std::nullopt;
        const QString Current = NormalizeRoot( Root );
        const QString Recorded = NormalizeRoot( Json->value( "workspaceRoot" ).toString() );
        const QString RecordedKey = PathKey( Recorded );
        // 기록된 루트로 시작하는 경로는 지금 루트로 다시 잇는다 — 저장소를 옮기면 MRE 는 하나도 되살리지 못했다.
        const auto Remap = [ & ]( const QString& Path ) -> QString {
            const QString Normalized = NormalizePath( Path );
            if( Recorded.isEmpty() ) return Normalized;
            const QString Key = PathKey( Normalized );
            if( Key == RecordedKey ) return Current;
            const QString Prefix = RecordedKey.endsWith( QLatin1Char( '/' ) ) ? RecordedKey : RecordedKey + QLatin1Char( '/' );
            if( Key.startsWith( Prefix ) ) return QDir( Current ).filePath( Normalized.mid( Prefix.size() ) );
            return Normalized;
        };
        State Value;
        Value.savedAt_ = QDateTime::currentDateTime();
        Value.root_ = Current;
        const auto Documents = Json->value( "documents" ).toArray();
        QSet< QString > Seen;
        for( const auto& Item : Documents ) {
            const auto Object = Item.toObject();
            const auto Raw = Object[ "path" ].toString();
            if( Raw.isEmpty() ) continue;
            DocumentEntry Entry;
            Entry.path_ = Remap( Raw );
            if( Seen.contains( PathKey( Entry.path_ ) ) ) continue;
            Seen.insert( PathKey( Entry.path_ ) );
            Entry.outside_ = !IsInside( Entry.path_, Current, false );
            Position Where;
            Where.caretLine_ = qMax( 1, Object[ "caretLine" ].toInt( 1 ) );
            Where.caretColumn_ = qMax( 1, Object[ "caretColumn" ].toInt( 1 ) );
            Where.firstLine_ = qMax( 0, Object[ "firstVisibleLine" ].toInt( 1 ) - 1 );
            Entry.position_ = Where;
            Value.documents_.append( Entry );
        }
        // activeIndex 는 documents 의 자리다(MRE 는 경로로 다시 맞춘다).
        const int Active = Json->value( "activeIndex" ).toInt( -1 );
        if( Active >= 0 && Active < Documents.size() ) Value.active_ = Remap( Documents.at( Active ).toObject()[ "path" ].toString() );
        return Value;
    }

    // ============================================================================================ projects.json

    QString ProjectsCachePath( const QString& Root )
    {
        return QDir( NormalizeRoot( Root ) ).filePath( QStringLiteral( ".multiroot/projects.json" ) );
    }

    QList< ProjectRecord > ReadProjectsCache( const QString& Root )
    {
        QList< ProjectRecord > Result;
        const auto Json = ReadObject( ProjectsCachePath( Root ) );
        if( !Json || Json->value( "version" ).toInt() != 2 ) return Result;
        for( const auto& Item : Json->value( "projects" ).toArray() ) {
            const auto Object = Item.toObject();
            ProjectRecord Record{ Object[ "projectId" ].toString(), Object[ "rootPath" ].toString(), Object[ "confPath" ].toString(),
                Object[ "sourcePath" ].toString(), Object[ "rootDoc" ].toString(), Object[ "buildPath" ].toString(), Object[ "mystMarkdown" ].toBool() };
            // conf.py 가 사라진 줄은 버린다(MRE solSphinxProjectRegistry.cpp:39-44).
            if( Record.confPath_.isEmpty() || !QFileInfo::exists( Record.confPath_ ) ) continue;
            Result.append( Record );
        }
        return Result;
    }

    bool WriteProjectsCache( const QString& Root, const QList< ProjectRecord >& Projects )
    {
        QJsonArray Items;
        for( const auto& Each : Projects )
            Items.append( QJsonObject{ { "projectId", Each.projectId_ }, { "rootPath", Each.rootPath_ }, { "confPath", Each.confPath_ },
                { "sourcePath", Each.sourcePath_ }, { "rootDoc", Each.rootDoc_ }, { "buildPath", Each.buildPath_ }, { "mystMarkdown", Each.mystMarkdown_ } } );
        QString Error;
        return WriteObject( ProjectsCachePath( Root ), QJsonObject{ { "version", 2 }, { "workspaceRoot", NormalizeRoot( Root ) }, { "projects", Items } }, &Error );
    }
}
