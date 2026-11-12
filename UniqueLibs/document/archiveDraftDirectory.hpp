#pragma once

/// 모듈

/// UIs

/// 기타 헤더파일

/// 고유 라이브러리
#include "UniqueLibs/ProductDefaults_Edit.hpp"
#include "UniqueLibs/settings/appSettings.hpp"

/// 공통 라이브러리

/// 외부 라이브러리

/// 선언 헤더파일들

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QSaveFile>
#include <QUuid>
#include <memory>

namespace hc::document
{
    /// The document configuration folder of this process: a hosted viewer's override, the plugin host's
    /// environment, else --config-dir or the executable folder.
    inline QString DocumentConfigDirectory()
    {
        if( auto* App = QCoreApplication::instance() ) {
            const auto Override = App->property( "documentViewsConfigDir" ).toString();
            if( !Override.isEmpty() ) return Override;
        }
        const auto Environment = qEnvironmentVariable( "HC_DOCUMENT_CONFIG_DIR" );
        return Environment.isEmpty() ? hc::settings::DocumentPaths().directory_ : Environment;
    }

    /// One edited copy of a file inside an archive that is written back only when its viewer tab closes:
    /// <archive-drafts>/<id>/{content/<name>, recovery.json, owner.lock}. The owner lock is held while the
    /// edit session lives, so the recovery list can tell a live draft from one whose process ended.
    /// A pending draft stays after its owner ends; the recovery manager writes it back or removes it.
    class ArchiveDraftDirectory final
    {
    public:
        explicit ArchiveDraftDirectory( const QString& Root )
        {
            // Without a configuration folder the copy falls back to the old temporary location.
            const auto Base = Root.isEmpty() ? QDir::tempPath() : Root;
            const auto Name = ( Root.isEmpty() ? QStringLiteral( "hc-preview-edit-" ) : QString{} ) + QUuid::createUuid().toString( QUuid::Id128 );
            const auto Path = QDir( Base ).filePath( Name );
            if( !QDir().mkpath( Path ) ) return;
            lock_ = std::make_unique< QLockFile >( QDir( Path ).filePath( QStringLiteral( "owner.lock" ) ) ); lock_->setStaleLockTime( 0 );
            if( !lock_->tryLock( 0 ) ) { lock_.reset(); QDir( Path ).removeRecursively(); return; }
            path_ = Path;
        }
        ~ArchiveDraftDirectory()
        {
            if( lock_ ) lock_->unlock();
            if( autoRemove_ && !path_.isEmpty() ) QDir( path_ ).removeRecursively();
        }
        ArchiveDraftDirectory( const ArchiveDraftDirectory& ) = delete;
        ArchiveDraftDirectory& operator=( const ArchiveDraftDirectory& ) = delete;
        bool isValid() const { return !path_.isEmpty(); }
        QString path() const { return path_; }
        QString filePath( const QString& Name ) const { return QDir( path_ ).filePath( Name ); }
        void setAutoRemove( bool Value ) { autoRemove_ = Value; }
    private:
        QString path_;
        std::unique_ptr< QLockFile > lock_;
        bool autoRemove_ = true;
    };

    inline QString ArchiveDraftRoot() { return hc::defaults::edit::ArchiveDraftDirectory( DocumentConfigDirectory() ); }

    inline bool WriteArchiveDraftManifest( const QString& Directory, const QJsonObject& Manifest )
    {
        QSaveFile File( QDir( Directory ).filePath( QStringLiteral( "recovery.json" ) ) ); File.setDirectWriteFallback( false );
        const auto Bytes = QJsonDocument( Manifest ).toJson();
        return File.open( QIODevice::WriteOnly ) && File.write( Bytes ) == Bytes.size() && File.commit();
    }
}
