#include "stdafx.h"
#include "documentRecoveryMaintenance.hpp"

/// 모듈

/// UIs

/// 기타 헤더파일

/// 고유 라이브러리
#include "UniqueLibs/ProductDefaults_Edit.hpp"
#include "UniqueLibs/settings/unifiedConfig.hpp"

/// 공통 라이브러리
#include "commonLibs/document/epub/epubArchive.hpp"
#include "commonLibs/document/epub/epubDraftStore.hpp"

/// 외부 라이브러리

/// 선언 헤더파일들

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLockFile>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QUuid>
#include <algorithm>

DECLARE_CMNLIBSV2_NAMESPACE

namespace hc::document::recovery
{
    namespace
    {
        QString Tr( const char* Text ) { return QCoreApplication::translate( "hc::document::recovery", Text ); }
        QString EpubRoot( const QString& Root ) { return Root + QStringLiteral( "/epub-drafts" ); }
        QString ArchiveRoot( const QString& Root ) { return Root + QStringLiteral( "/archive-drafts" ); }
        // Copies nobody reads any more are removed only after this long, so a write in progress is never hit.
        constexpr qint64 LeftoverSeconds = 3600;
        // The size cap never evicts copies written in the last day.
        constexpr qint64 CapacityGraceSeconds = 24 * 3600;
        // Temporary originals of untitled, captured or restored documents live while their tab does.
        constexpr int TemporaryOriginalDays = 30;

        struct SessionFacts
        {
            QString id_, profile_;
            bool live_ = false, clean_ = false, readable_ = false;
            QDateTime modified_;
            QStringList documents_, snapshots_, drafts_, paths_;
            QJsonObject json_;
        };
        std::vector< SessionFacts > ScanSessions( const DocumentRecoveryStore& Store, const QString& Root )
        {
            std::vector< SessionFacts > Result;
            for( const auto& Id : Store.Sessions() ) {
                SessionFacts Facts; Facts.id_ = Id; Facts.live_ = LockHeld( Store.SessionLockPath( Id ) );
                Facts.modified_ = QFileInfo( Root + QStringLiteral( "/sessions/" ) + Id + QStringLiteral( ".json" ) ).lastModified();
                const auto Session = Store.ReadSession( Id );
                if( Session.HasValue() ) {
                    Facts.readable_ = true; Facts.json_ = Session.Value();
                    Facts.clean_ = Facts.json_[ "cleanShutdown" ].toBool(); Facts.profile_ = Facts.json_[ "profileId" ].toString();
                    for( const auto& Value : Facts.json_[ "documents" ].toArray() ) {
                        const auto Item = Value.toObject();
                        Facts.documents_.append( Item[ "documentId" ].toString() );
                        if( !Item[ "snapshot" ].toString().isEmpty() ) Facts.snapshots_.append( Item[ "snapshot" ].toString() );
                        if( !Item[ "draft" ].toString().isEmpty() ) Facts.drafts_.append( QFileInfo( Item[ "draft" ].toString() ).fileName() );
                        if( !Item[ "path" ].toString().isEmpty() ) Facts.paths_.append( QFileInfo( Item[ "path" ].toString() ).absoluteFilePath() );
                    }
                }
                Result.push_back( std::move( Facts ) );
            }
            return Result;
        }
        QDateTime Expiry( const QDateTime& Saved, const Policy& Settings )
        { return Settings.retentionDays_ > 0 && Saved.isValid() ? Saved.addDays( Settings.retentionDays_ ) : QDateTime{}; }
        void Earliest( UsageRow& Row, const QDateTime& Expires, const QDateTime& Now )
        {
            if( !Expires.isValid() ) return;
            if( Expires <= Now ) ++Row.expired_;
            if( !Row.expires_.isValid() || Expires < Row.expires_ ) Row.expires_ = Expires;
        }
        qint64 TreeSize( const QString& Directory )
        {
            qint64 Bytes = 0;
            QDirIterator It( Directory, QDir::Files | QDir::Hidden | QDir::System, QDirIterator::Subdirectories );
            while( It.hasNext() ) { It.next(); Bytes += It.fileInfo().size(); }
            return Bytes;
        }
        // Folders other installations or processes may still read are copied, never moved.
        constexpr int LegacyGraceDays = 30;
        bool CopyTree( const QString& From, const QString& To )
        {
            QDirIterator It( From, QDir::Files | QDir::Hidden | QDir::System, QDirIterator::Subdirectories );
            while( It.hasNext() ) {
                It.next();
                if( It.fileName() == QStringLiteral( "owner.lock" ) ) continue;
                const auto Target = QDir( To ).filePath( QDir( From ).relativeFilePath( It.filePath() ) );
                if( !QDir().mkpath( QFileInfo( Target ).absolutePath() ) || !QFile::copy( It.filePath(), Target ) ) { QDir( To ).removeRecursively(); return false; }
            }
            return true;
        }
        QByteArray Sha256Hex( const QString& Path )
        {
            QFile File( Path ); if( !File.open( QIODevice::ReadOnly ) ) return {};
            QCryptographicHash Hash( QCryptographicHash::Sha256 ); if( !Hash.addData( &File ) ) return {};
            return Hash.result().toHex();
        }
        struct ArchiveFacts
        {
            QString directory_, content_;
            QJsonObject manifest_;
            bool live_ = false, pending_ = false, kept_ = false;
            QDateTime saved_;
            qint64 bytes_ = 0;
        };
        std::vector< ArchiveFacts > ScanArchiveDrafts( const QString& Root )
        {
            std::vector< ArchiveFacts > Result;
            for( const auto& Info : QDir( ArchiveRoot( Root ) ).entryInfoList( QDir::Dirs | QDir::NoDotAndDotDot ) ) {
                ArchiveFacts Facts; Facts.directory_ = Info.absoluteFilePath();
                Facts.live_ = LockHeld( QDir( Facts.directory_ ).filePath( QStringLiteral( "owner.lock" ) ) );
                Facts.manifest_ = ReadArchiveManifest( Facts.directory_ );
                Facts.content_ = ArchiveDraftContent( Facts.directory_ );
                const QFileInfo Content( Facts.content_ );
                Facts.saved_ = Content.exists() ? Content.lastModified() : Info.lastModified();
                Facts.bytes_ = TreeSize( Facts.directory_ );
                Facts.kept_ = Facts.manifest_[ "kept" ].toBool();
                // A plugin's copy is pending from the start; it is a real edit only when its content changed.
                const auto Baseline = Facts.manifest_[ "baselineSha256" ].toString().toLatin1();
                Facts.pending_ = !Facts.manifest_.isEmpty() && Content.exists() && Facts.manifest_[ "pending" ].toBool( true )
                    && ( Baseline.isEmpty() || Facts.manifest_[ "purpose" ].toString() != QStringLiteral( "external-archive-preview-edit" ) || Sha256Hex( Facts.content_ ) != Baseline );
                Result.push_back( std::move( Facts ) );
            }
            return Result;
        }
        QString Location( const QString& Path ) { return Path.isEmpty() ? QString{} : QDir::toNativeSeparators( QFileInfo( Path ).absolutePath() ); }
        QString ArchiveLocation( const QJsonObject& Manifest )
        {
            const auto Archive = Manifest[ "archivePath" ].toString();
            return Archive.isEmpty() ? Tr( "압축 파일 안" ) : Tr( "%1 안" ).arg( QDir::toNativeSeparators( Archive ) );
        }
        QString UntitledName( qint64 Number ) { return Number > 0 ? Tr( "새 문서 %1" ).arg( Number ) : Tr( "새 문서" ); }
    }

    bool LockHeld( const QString& Path )
    {
        if( Path.isEmpty() || !QFileInfo::exists( Path ) ) return false;
        QLockFile Probe( Path ); Probe.setStaleLockTime( 0 );
        if( !Probe.tryLock( 0 ) ) return true;
        Probe.unlock();   // a stale lock of an ended process is removed on the way
        return false;
    }

    Policy PolicyFrom( const QJsonObject& Configuration )
    {
        const auto Recovery = Configuration[ "documentViews" ].toObject()[ "recovery" ].toObject();
        const auto Default = [ & ]( const QString& Key ) { return hc::defaults::edit::ViewValue( QStringLiteral( "recovery/" ) + Key ); };
        Policy Result;
        const auto Action = Recovery[ "startupAction" ].toString( Default( "startupAction" ).toString() );
        Result.startupAction_ = QStringList{ "ask", "restore", "keep" }.contains( Action ) ? Action : Default( "startupAction" ).toString();
        Result.retentionDays_ = qBound( 0, Recovery[ "retentionDays" ].toInt( Default( "retentionDays" ).toInt() ), 36500 );
        Result.maxBytes_ = qint64( qBound( 0, Recovery[ "maxMiB" ].toInt( Default( "maxMiB" ).toInt() ), 1024 * 1024 ) ) * 1024 * 1024;
        return Result;
    }
    Policy LoadPolicy( const QString& ConfigDirectory )
    {
        QJsonObject Configuration; QString Error;
        hc::settings::UnifiedConfig::Load( hc::settings::UnifiedConfig::FileName( ConfigDirectory ), Configuration, Error );
        return PolicyFrom( Configuration );
    }

    MigrationReport MigrateLegacy( const QString& ConfigDirectory, const QString& ProfileId, const QStringList& LegacyRoots, const QString& TemporaryDirectory )
    {
        MigrationReport Report;
        const auto Root = hc::defaults::edit::RecoveryDirectory( ConfigDirectory );
        DocumentRecoveryStore Store( Root );
        for( const auto& Old : LegacyRoots ) {
            if( !QFileInfo( Old ).isDir() ) continue;
            // Shared by every installation: copied, never moved. The old folder gives up what was copied
            // only after LegacyGraceDays, so a lost destination never loses the user's copies.
            const auto Imported = Store.ImportFrom( Old, ProfileId );
            if( !Imported.HasValue() ) { Report.errors_.append( QString::fromStdString( Imported.Failure().code_ ) ); continue; }
            if( Imported.Value() > 0 ) { Report.moved_ += Imported.Value(); Report.sources_.append( QDir::toNativeSeparators( Old ) ); }
            DocumentRecoveryStore( Old ).RetireImported( QDateTime::currentDateTimeUtc().addDays( -LegacyGraceDays ) );
        }
        // EPUB drafts used to sit directly in the configuration folder.
        const auto OldDrafts = hc::defaults::edit::LegacyEpubDraftDirectory( ConfigDirectory ), Drafts = hc::defaults::edit::EpubDraftDirectory( ConfigDirectory );
        if( QFileInfo( OldDrafts ).isDir() && QDir().mkpath( Drafts + QStringLiteral( "/sources" ) ) ) {
            int Moved = 0;
            for( const auto& Folder : { QString{}, QStringLiteral( "/sources" ) } ) {
                for( const auto& Info : QDir( OldDrafts + Folder ).entryInfoList( QDir::Files ) ) {
                    if( Info.suffix() == QStringLiteral( "lock" ) ) { if( !LockHeld( Info.absoluteFilePath() ) ) QFile::remove( Info.absoluteFilePath() ); continue; }
                    if( LockHeld( Info.absoluteFilePath() + QStringLiteral( ".lock" ) ) ) continue;
                    const auto Target = Drafts + Folder + QStringLiteral( "/" ) + Info.fileName();
                    if( QFileInfo::exists( Target ) || !QFile::rename( Info.absoluteFilePath(), Target ) ) continue;
                    if( Info.suffix() == QStringLiteral( "hcedraft" ) ) ++Moved;
                }
            }
            QDir().rmdir( OldDrafts + QStringLiteral( "/sources" ) ); QDir().rmdir( OldDrafts );
            if( Moved ) { Report.moved_ += Moved; Report.sources_.append( QDir::toNativeSeparators( OldDrafts ) ); }
        }
        // Edited archive copies whose archive was never updated waited in %TEMP%. Copied as well; the original
        // folder keeps its content under recovery.migrated.json so no later start copies it again.
        if( !TemporaryDirectory.isEmpty() ) {
            int Moved = 0;
            for( const auto& Info : QDir( TemporaryDirectory ).entryInfoList( { QStringLiteral( "hc-preview-edit-*" ), QStringLiteral( "hc-preview-external-*" ) }, QDir::Dirs | QDir::NoDotAndDotDot ) ) {
                const auto Manifest = ReadArchiveManifest( Info.absoluteFilePath() );
                if( Manifest.isEmpty() || !Manifest[ "pending" ].toBool( true ) || LockHeld( QDir( Info.absoluteFilePath() ).filePath( QStringLiteral( "owner.lock" ) ) ) ) continue;
                const auto Target = ArchiveRoot( Root ) + QStringLiteral( "/" ) + QUuid::createUuid().toString( QUuid::Id128 );
                if( !QDir().mkpath( ArchiveRoot( Root ) ) || !CopyTree( Info.absoluteFilePath(), Target ) ) continue;
                QFile::rename( QDir( Info.absoluteFilePath() ).filePath( QStringLiteral( "recovery.json" ) ), QDir( Info.absoluteFilePath() ).filePath( QStringLiteral( "recovery.migrated.json" ) ) );
                ++Moved;
            }
            if( Moved ) { Report.moved_ += Moved; Report.sources_.append( QDir::toNativeSeparators( TemporaryDirectory ) ); }
        }
        if( Report.moved_ > 0 ) {
            auto Notice = MigrationNotice( Root );
            QJsonArray Sources = Notice[ "sources" ].toArray(); for( const auto& Source : Report.sources_ ) if( !Sources.contains( Source ) ) Sources.append( Source );
            Notice = QJsonObject{ { "count", Notice[ "count" ].toInt() + Report.moved_ }, { "sources", Sources }, { "utc", QDateTime::currentDateTimeUtc().toString( Qt::ISODate ) } };
            QSaveFile File( Root + QStringLiteral( "/migration.json" ) );
            if( QDir().mkpath( Root ) && File.open( QIODevice::WriteOnly ) ) { File.write( QJsonDocument( Notice ).toJson() ); File.commit(); }
        }
        return Report;
    }
    QJsonObject MigrationNotice( const QString& Root )
    {
        QFile File( Root + QStringLiteral( "/migration.json" ) );
        if( !File.open( QIODevice::ReadOnly ) || File.size() > 64 * 1024 ) return {};
        return QJsonDocument::fromJson( File.readAll() ).object();
    }
    bool DismissMigrationNotice( const QString& Root )
    {
        const auto Path = Root + QStringLiteral( "/migration.json" );
        return !QFileInfo::exists( Path ) || QFile::remove( Path );
    }

    std::vector< Item > ListItems( const QString& Root, const Policy& Settings, const QDateTime& Now )
    {
        std::vector< Item > Result;
        DocumentRecoveryStore Store( Root );
        QSet< QString > LiveDocuments, HotDocuments, LiveDrafts, HotDrafts;
        for( const auto& Session : ScanSessions( Store, Root ) ) {
            if( Session.live_ ) { for( const auto& Id : Session.documents_ ) LiveDocuments.insert( Id ); for( const auto& Draft : Session.drafts_ ) LiveDrafts.insert( Draft ); }
            else if( Session.clean_ ) { for( const auto& Id : Session.documents_ ) HotDocuments.insert( Id ); for( const auto& Draft : Session.drafts_ ) HotDrafts.insert( Draft ); }
        }
        const auto Finish = [ & ]( Item Entry ) {
            if( Entry.state_ == State::Kept || Entry.state_ == State::Pending ) {
                Entry.expires_ = Expiry( Entry.saved_, Settings ); Entry.expired_ = Entry.expires_.isValid() && Entry.expires_ <= Now;
            }
            Result.push_back( std::move( Entry ) );
        };
        if( auto Records = Store.List(); Records.HasValue() ) {
            auto Sorted = std::move( Records.Value() );
            std::sort( Sorted.begin(), Sorted.end(), []( const auto& Left, const auto& Right ) { return Left.utcMs_ > Right.utcMs_; } );
            for( const auto& Record : Sorted ) {
                Item Entry; Entry.key_ = QString::fromStdString( Record.document_.id_ ); Entry.path_ = QString::fromStdString( Record.snapshotPath_ );
                Entry.owner_ = QString::fromStdString( Record.ownerWindow_ );
                const auto Source = QString::fromStdString( Record.document_.source_.localPath_ );
                if( Record.text_ && Record.text_->untitled_ ) { Entry.kind_ = Kind::Untitled; Entry.name_ = UntitledName( Record.text_->untitledNumber_ ); Entry.location_ = Tr( "저장한 적 없음" ); }
                else { Entry.name_ = Source.isEmpty() ? Tr( "이름 없는 문서" ) : QFileInfo( Source ).fileName(); Entry.location_ = Location( Source ); }
                const QFileInfo Info( Entry.path_ ); Entry.bytes_ = Info.size(); Entry.saved_ = Info.lastModified();
                if( LiveDocuments.contains( Entry.key_ ) || ( !Entry.owner_.isEmpty() && LockHeld( Store.SessionLockPath( Entry.owner_ ) ) ) ) Entry.state_ = State::InUse;
                else if( HotDocuments.contains( Entry.key_ ) ) Entry.state_ = State::HotExit;
                Finish( std::move( Entry ) );
            }
        }
        for( const auto& Draft : epub::DraftStore::List( EpubRoot( Root ) ) ) {
            Item Entry; Entry.kind_ = Kind::Epub; Entry.key_ = Entry.path_ = Draft.path_;
            Entry.name_ = Draft.sourcePath_.isEmpty() ? Tr( "EPUB 편집 초안" ) : QFileInfo( Draft.sourcePath_ ).fileName();
            Entry.location_ = Location( Draft.sourcePath_ );
            const QFileInfo Info( Draft.path_ ); Entry.bytes_ = Info.size(); Entry.saved_ = Info.lastModified();
            if( LiveDrafts.contains( Info.fileName() ) || LockHeld( Draft.path_ + QStringLiteral( ".lock" ) ) ) Entry.state_ = State::InUse;
            else if( HotDrafts.contains( Info.fileName() ) ) Entry.state_ = State::HotExit;
            Finish( std::move( Entry ) );
        }
        for( const auto& Draft : ScanArchiveDrafts( Root ) ) {
            if( !Draft.pending_ ) continue;
            Item Entry; Entry.kind_ = Kind::Archive; Entry.key_ = Draft.directory_; Entry.path_ = Draft.content_;
            Entry.name_ = QFileInfo( Draft.content_ ).fileName(); Entry.location_ = ArchiveLocation( Draft.manifest_ );
            Entry.bytes_ = Draft.bytes_; Entry.saved_ = Draft.saved_;
            Entry.state_ = Draft.live_ ? State::InUse : State::Pending;
            Entry.applyBlocker_ = ArchiveApplyBlocker( Draft.manifest_ );
            Finish( std::move( Entry ) );
        }
        return Result;
    }

    QString RemoveCopy( const QString& Root, const Item& Entry )
    {
        if( Entry.kind_ == Kind::Archive ) return RemoveArchiveDraft( Entry.key_ ) ? QString{} : QStringLiteral( "recovery-copy-in-use" );
        DocumentRecoveryStore Store( Root );
        if( Entry.kind_ == Kind::Epub ) {
            if( LockHeld( Entry.path_ + QStringLiteral( ".lock" ) ) ) return QStringLiteral( "recovery-copy-in-use" );
            if( !epub::DraftStore::Remove( EpubRoot( Root ), Entry.path_ ) ) return QStringLiteral( "recovery-copy-remove" );
        } else if( !Entry.owner_.isEmpty() && LockHeld( Store.SessionLockPath( Entry.owner_ ) ) ) return QStringLiteral( "recovery-copy-in-use" );
        // A session that would reopen the copy forgets it: the document itself still reopens, without the edits.
        const auto Draft = QFileInfo( Entry.path_ ).fileName();
        for( const auto& Session : ScanSessions( Store, Root ) ) {
            if( Session.live_ || !Session.readable_ ) continue;
            auto Json = Session.json_; QJsonArray Items; bool Changed = false;
            for( const auto& Value : Json[ "documents" ].toArray() ) {
                auto Item = Value.toObject();
                if( Entry.kind_ == Kind::Epub && QFileInfo( Item[ "draft" ].toString() ).fileName() == Draft ) { Item.remove( "draft" ); Changed = true; }
                else if( Entry.kind_ != Kind::Epub && Item[ "documentId" ].toString() == Entry.key_ ) { Changed = true; continue; }
                Items.append( Item );
            }
            if( !Changed ) continue;
            Json[ "documents" ] = Items;
            if( !Store.StoreSession( Session.id_, Json ).HasValue() ) return QStringLiteral( "recovery-session-write" );
        }
        if( Entry.kind_ != Kind::Epub ) if( const auto Dropped = Store.Drop( { Entry.key_ } ); !Dropped.HasValue() ) return QString::fromStdString( Dropped.Failure().code_ );
        return {};
    }

    Usage Summarize( const QString& Root, const Policy& Settings, const QDateTime& Now )
    {
        Usage Result;
        DocumentRecoveryStore Store( Root );
        QSet< QString > LiveDocuments, HotDocuments, LiveDrafts, HotDrafts;
        for( const auto& Session : ScanSessions( Store, Root ) ) {
            if( Session.live_ ) { for( const auto& Id : Session.documents_ ) LiveDocuments.insert( Id ); for( const auto& Draft : Session.drafts_ ) LiveDrafts.insert( Draft ); }
            else if( Session.clean_ ) {
                ++Result.hotExit_.count_;
                for( const auto& Id : Session.documents_ ) HotDocuments.insert( Id );
                for( const auto& Draft : Session.drafts_ ) HotDrafts.insert( Draft );
            }
        }
        if( const auto Documents = Store.Documents(); Documents.HasValue() ) for( const auto& Document : Documents.Value() ) {
            const auto Id = QString::fromStdString( Document.id_ );
            if( HotDocuments.contains( Id ) ) { Result.hotExit_.bytes_ += Document.bytes_; continue; }
            ++Result.edits_.count_; Result.edits_.bytes_ += Document.bytes_;
            if( !LiveDocuments.contains( Id ) ) Earliest( Result.edits_, Expiry( Document.newest_, Settings ), Now );
        }
        for( const auto& Info : QDir( EpubRoot( Root ) ).entryInfoList( { QStringLiteral( "*.hcedraft" ) }, QDir::Files ) ) {
            if( HotDrafts.contains( Info.fileName() ) ) { Result.hotExit_.bytes_ += Info.size(); continue; }
            ++Result.epub_.count_; Result.epub_.bytes_ += Info.size();
            if( !LiveDrafts.contains( Info.fileName() ) ) Earliest( Result.epub_, Expiry( Info.lastModified(), Settings ), Now );
        }
        for( const auto& Draft : ScanArchiveDrafts( Root ) ) {
            if( !Draft.pending_ ) continue;
            ++Result.archive_.count_; Result.archive_.bytes_ += Draft.bytes_;
            if( !Draft.live_ ) Earliest( Result.archive_, Expiry( Draft.saved_, Settings ), Now );
        }
        return Result;
    }

    PurgeReport Purge( const QString& Root, const Policy& Settings, const QDateTime& Now, PurgeScope Scope )
    {
        PurgeReport Report;
        if( !QFileInfo( Root ).isDir() ) return Report;
        DocumentRecoveryStore Store( Root );
        const bool Startup = Scope == PurgeScope::Startup;
        const auto Cutoff = Settings.retentionDays_ > 0 ? Now.addDays( -Settings.retentionDays_ ) : QDateTime{};
        const auto Expired = [ & ]( const QDateTime& Time ) { return Cutoff.isValid() && Time.isValid() && Time < Cutoff; };
        const auto Leftover = Now.addSecs( -LeftoverSeconds );
        // Sessions pin their copies: a live window's, a HotExit session's and an unanswered abnormal exit's.
        // An abnormal exit older than the retention period expires with its copies.
        QSet< QString > PinnedDocuments, PinnedDrafts, SessionDocuments; QSet< QString > Paths;
        QStringList ExpiredSessions;
        const auto Sessions = ScanSessions( Store, Root );
        for( const auto& Session : Sessions ) {
            for( const auto& Id : Session.documents_ ) SessionDocuments.insert( Id );
            for( const auto& Path : Session.paths_ ) Paths.insert( Path.toCaseFolded() );
            const bool Ended = !Session.live_ && Session.readable_ && !Session.clean_ && Expired( Session.modified_ );
            if( Ended ) { ExpiredSessions.append( Session.id_ ); continue; }
            if( Startup && !Session.live_ && Session.readable_ && Session.documents_.isEmpty() ) { ExpiredSessions.append( Session.id_ ); continue; }
            for( const auto& Id : Session.documents_ ) PinnedDocuments.insert( Id );
            for( const auto& Draft : Session.drafts_ ) PinnedDrafts.insert( Draft );
        }
        for( const auto& Id : ExpiredSessions ) if( !Store.RemoveSession( Id ) ) Report.errors_.append( QStringLiteral( "session:" ) + Id );

        struct Candidate { enum Type { Snapshot, Epub, Archive } type_; QString key_; qint64 bytes_ = 0; QDateTime saved_; bool pinned_ = false; };
        std::vector< Candidate > Candidates;
        if( auto Documents = Store.Documents(); Documents.HasValue() ) for( const auto& Document : Documents.Value() ) {
            const auto Id = QString::fromStdString( Document.id_ );
            Candidates.push_back( { Candidate::Snapshot, Id, Document.bytes_, Document.newest_, PinnedDocuments.contains( Id ) } );
        }
        else Report.errors_.append( QString::fromStdString( Documents.Failure().code_ ) );
        for( const auto& Info : QDir( EpubRoot( Root ) ).entryInfoList( { QStringLiteral( "*.hcedraft" ) }, QDir::Files ) )
            Candidates.push_back( { Candidate::Epub, Info.absoluteFilePath(), Info.size(), Info.lastModified(),
                PinnedDrafts.contains( Info.fileName() ) || LockHeld( Info.absoluteFilePath() + QStringLiteral( ".lock" ) ) } );
        for( const auto& Draft : ScanArchiveDrafts( Root ) ) {
            if( Draft.live_ ) { Candidates.push_back( { Candidate::Archive, Draft.directory_, Draft.bytes_, Draft.saved_, true } ); continue; }
            // A copy that matches the archive (written back, discarded or never changed) has no reader.
            if( !Draft.pending_ ) { if( Startup && Draft.saved_ < Leftover && QDir( Draft.directory_ ).removeRecursively() ) ++Report.removed_; continue; }
            Candidates.push_back( { Candidate::Archive, Draft.directory_, Draft.bytes_, Draft.saved_, false } );
        }
        QStringList Dropped;
        const auto Remove = [ & ]( const Candidate& Item ) {
            bool Done = true;
            if( Item.type_ == Candidate::Snapshot ) Dropped.append( Item.key_ );
            else if( Item.type_ == Candidate::Epub ) Done = epub::DraftStore::Remove( EpubRoot( Root ), Item.key_ );
            else Done = RemoveArchiveDraft( Item.key_ );
            if( !Done ) { Report.errors_.append( Item.key_ ); return; }
            ++Report.removed_; Report.bytes_ += Item.bytes_;
        };
        std::vector< Candidate > Kept;
        for( const auto& Item : Candidates ) { if( !Item.pinned_ && Expired( Item.saved_ ) ) Remove( Item ); else Kept.push_back( Item ); }
        if( Startup && Settings.maxBytes_ > 0 ) {
            qint64 Total = 0; for( const auto& Item : Kept ) Total += Item.bytes_;
            std::sort( Kept.begin(), Kept.end(), []( const Candidate& Left, const Candidate& Right ) { return Left.saved_ < Right.saved_; } );
            const auto Recent = Now.addSecs( -CapacityGraceSeconds );
            for( const auto& Item : Kept ) {
                if( Total <= Settings.maxBytes_ ) break;
                if( Item.pinned_ || Item.saved_ >= Recent ) continue;
                Remove( Item ); Total -= Item.bytes_;
            }
        }
        if( !Dropped.isEmpty() ) if( const auto Done = Store.Drop( Dropped ); !Done.HasValue() ) Report.errors_.append( QString::fromStdString( Done.Failure().code_ ) );
        if( !Startup ) return Report;

        // Leftovers no reader uses: deletion fences of gone documents, unindexed snapshot files,
        // interrupted atomic writes, temporary originals and locks of ended windows.
        QSet< QString > Indexed, Live;
        if( const auto Documents = Store.Documents(); Documents.HasValue() ) for( const auto& Document : Documents.Value() ) {
            Live.insert( QString::fromStdString( Document.id_ ) );
            for( const auto& Name : Document.files_ ) Indexed.insert( Name );
        }
        static const QRegularExpression Fence( QStringLiteral( "^([0-9a-f]{32})\\.retired$" ) ), Snapshot( QStringLiteral( "^[0-9a-f]{32}-.+\\.snapshot\\.json$" ) ),
            Interrupted( QStringLiteral( "^.+\\.(json|retired|hcedraft)\\.[A-Za-z0-9]{6}$" ) ), Original( QStringLiteral( "^(untitled|capture|image-copy|restored)-[0-9a-f]{32}\\.[A-Za-z0-9]+$" ) );
        const auto Old = [ & ]( const QFileInfo& Info, const QDateTime& Before ) { return Info.lastModified() < Before; };
        const auto Erase = [ & ]( const QFileInfo& Info ) { if( QFile::remove( Info.absoluteFilePath() ) ) { ++Report.removed_; Report.bytes_ += Info.size(); } };
        for( const auto& Info : QDir( Root ).entryInfoList( QDir::Files ) ) {
            const auto Name = Info.fileName();
            if( const auto Match = Fence.match( Name ); Match.hasMatch() ) {
                if( !Live.contains( Match.captured( 1 ) ) && !SessionDocuments.contains( Match.captured( 1 ) ) && Old( Info, Leftover ) ) Erase( Info );
            } else if( Snapshot.match( Name ).hasMatch() ) { if( !Indexed.contains( Name ) && Old( Info, Leftover ) ) Erase( Info ); }
            else if( Interrupted.match( Name ).hasMatch() ) { if( Old( Info, Leftover ) ) Erase( Info ); }
            else if( Original.match( Name ).hasMatch() ) {
                if( !Paths.contains( Info.absoluteFilePath().toCaseFolded() ) && Old( Info, Now.addDays( -TemporaryOriginalDays ) ) ) Erase( Info );
            }
        }
        for( const auto& Info : QDir( Root + QStringLiteral( "/sessions" ) ).entryInfoList( { QStringLiteral( "*.lock" ) }, QDir::Files ) )
            if( !QFileInfo::exists( Info.absolutePath() + QStringLiteral( "/" ) + Info.completeBaseName() + QStringLiteral( ".json" ) ) && !LockHeld( Info.absoluteFilePath() ) && QFileInfo::exists( Info.absoluteFilePath() ) ) QFile::remove( Info.absoluteFilePath() );
        for( const auto& Info : QDir( Root + QStringLiteral( "/sessions" ) ).entryInfoList( QDir::Files ) )
            if( Interrupted.match( Info.fileName() ).hasMatch() && Old( Info, Leftover ) ) Erase( Info );
        for( const auto& Info : QDir( EpubRoot( Root ) ).entryInfoList( QDir::Files ) )
            if( Interrupted.match( Info.fileName() ).hasMatch() && Old( Info, Leftover ) ) Erase( Info );
        for( const auto& Info : QDir( EpubRoot( Root ) + QStringLiteral( "/sources" ) ).entryInfoList( QDir::Files ) )
            if( Old( Info, Now.addDays( -TemporaryOriginalDays ) ) ) Erase( Info );
        return Report;
    }

    std::vector< PromptGroup > CrashGroups( const QString& Root, const QString& ProfileId )
    {
        std::vector< PromptGroup > Result;
        DocumentRecoveryStore Store( Root );
        for( const auto& Session : ScanSessions( Store, Root ) ) {
            if( Session.live_ || !Session.readable_ || Session.clean_ ) continue;
            if( Session.profile_ != ProfileId ) continue;   // another installation's or executable's window
            PromptGroup Group; Group.sessionId_ = Session.id_; Group.saved_ = Session.modified_;
            for( const auto& Value : Session.json_[ "documents" ].toArray() ) {
                const auto Item = Value.toObject(); const auto Path = Item[ "path" ].toString();
                PromptEntry Entry; Entry.key_ = Item[ "documentId" ].toString();
                if( !Item[ "snapshot" ].toString().isEmpty() ) {
                    if( !Path.isEmpty() ) { Entry.name_ = QFileInfo( Path ).fileName(); Entry.location_ = Location( Path ); }
                    else {
                        Entry.type_ = PromptEntry::Untitled; Entry.location_ = Tr( "저장한 적 없는 문서" );
                        const auto Record = Store.Read( ( Root + QStringLiteral( "/" ) + Item[ "snapshot" ].toString() ).toStdString() );
                        Entry.name_ = UntitledName( Record.HasValue() && Record.Value().text_ ? Record.Value().text_->untitledNumber_ : 0 );
                    }
                } else if( !Item[ "draft" ].toString().isEmpty() ) { Entry.type_ = PromptEntry::Epub; Entry.name_ = QFileInfo( Path ).fileName(); Entry.location_ = Location( Path ); }
                else { ++Group.clean_; continue; }
                Group.entries_.push_back( std::move( Entry ) );
            }
            if( !Group.entries_.empty() || Group.clean_ ) Result.push_back( std::move( Group ) );
        }
        PromptGroup Archives;
        for( const auto& Draft : ScanArchiveDrafts( Root ) ) {
            if( Draft.live_ || !Draft.pending_ || Draft.kept_ ) continue;
            PromptEntry Entry; Entry.type_ = PromptEntry::Archive; Entry.key_ = Draft.directory_;
            Entry.name_ = QFileInfo( Draft.content_ ).fileName(); Entry.location_ = ArchiveLocation( Draft.manifest_ );
            if( !Archives.saved_.isValid() || Draft.saved_ > Archives.saved_ ) Archives.saved_ = Draft.saved_;
            Archives.entries_.push_back( std::move( Entry ) );
        }
        if( !Archives.entries_.empty() ) Result.push_back( std::move( Archives ) );
        return Result;
    }

    bool ReleaseSession( const QString& Root, const QString& SessionId, const QStringList& Delete )
    {
        DocumentRecoveryStore Store( Root );
        if( LockHeld( Store.SessionLockPath( SessionId ) ) ) return false;
        bool Complete = true;
        if( const auto Session = Store.ReadSession( SessionId ); Session.HasValue() && !Delete.isEmpty() ) {
            QStringList Snapshots;
            for( const auto& Value : Session.Value()[ "documents" ].toArray() ) {
                const auto Item = Value.toObject(); if( !Delete.contains( Item[ "documentId" ].toString() ) ) continue;
                if( !Item[ "snapshot" ].toString().isEmpty() ) Snapshots.append( Item[ "documentId" ].toString() );
                if( const auto Draft = QFileInfo( Item[ "draft" ].toString() ).fileName(); !Draft.isEmpty() )
                    Complete = epub::DraftStore::Remove( EpubRoot( Root ), EpubRoot( Root ) + QStringLiteral( "/" ) + Draft ) && Complete;
            }
            Complete = Store.Drop( Snapshots ).HasValue() && Complete;
        }
        return Store.RemoveSession( SessionId ) && Complete;
    }
    QJsonObject ReadArchiveManifest( const QString& Directory )
    {
        QFile File( QDir( Directory ).filePath( QStringLiteral( "recovery.json" ) ) );
        if( !File.open( QIODevice::ReadOnly ) || File.size() > 256 * 1024 ) return {};
        return QJsonDocument::fromJson( File.readAll() ).object();
    }
    QString ArchiveDraftContent( const QString& Directory )
    {
        const auto Name = QFileInfo( ReadArchiveManifest( Directory )[ "name" ].toString() ).fileName();
        const QDir Content( QDir( Directory ).filePath( QStringLiteral( "content" ) ) );
        if( !Name.isEmpty() ) return Content.filePath( Name );
        const auto Files = Content.entryList( QDir::Files );
        return Files.size() == 1 ? Content.filePath( Files.first() ) : QString{};
    }
    bool KeepArchiveDrafts( const QStringList& Directories )
    {
        bool Complete = true;
        for( const auto& Directory : Directories ) {
            auto Manifest = ReadArchiveManifest( Directory ); if( Manifest.isEmpty() ) { Complete = false; continue; }
            Manifest[ "kept" ] = true;
            QSaveFile File( QDir( Directory ).filePath( QStringLiteral( "recovery.json" ) ) );
            const auto Bytes = QJsonDocument( Manifest ).toJson();
            Complete = File.open( QIODevice::WriteOnly ) && File.write( Bytes ) == Bytes.size() && File.commit() && Complete;
        }
        return Complete;
    }
    bool RemoveArchiveDraft( const QString& Directory )
    {
        if( Directory.isEmpty() || LockHeld( QDir( Directory ).filePath( QStringLiteral( "owner.lock" ) ) ) ) return false;
        return QDir( Directory ).removeRecursively();
    }

    QString ArchiveApplyBlocker( const QJsonObject& Manifest )
    {
        if( Manifest.isEmpty() ) return QStringLiteral( "archive-draft-manifest" );
        if( Manifest[ "nesting" ].toInt() != 1 ) return QStringLiteral( "archive-nested" );
        const auto Path = Manifest[ "archivePath" ].toString();
        if( Path.isEmpty() || Manifest[ "entryPath" ].toString().isEmpty() ) return QStringLiteral( "archive-source-unknown" );
        const QFileInfo Info( Path );
        if( !Info.isFile() ) return QStringLiteral( "archive-missing" );
        if( Info.size() > epub::InputLimit ) return QStringLiteral( "archive-size" );
        QFile File( Path ); if( !File.open( QIODevice::ReadOnly ) ) return QStringLiteral( "archive-read" );
        const auto Signature = File.read( 4 );
        if( Signature != QByteArray( "PK\x03\x04", 4 ) && Signature != QByteArray( "PK\x05\x06", 4 ) ) return QStringLiteral( "archive-format" );
        return {};
    }
    QString ApplyArchiveDraft( const QString& Directory )
    {
        const auto Manifest = ReadArchiveManifest( Directory );
        if( const auto Blocker = ArchiveApplyBlocker( Manifest ); !Blocker.isEmpty() ) return Blocker;
        if( LockHeld( QDir( Directory ).filePath( QStringLiteral( "owner.lock" ) ) ) ) return QStringLiteral( "archive-draft-in-use" );
        QFile Draft( ArchiveDraftContent( Directory ) );
        if( !Draft.open( QIODevice::ReadOnly ) ) return QStringLiteral( "archive-draft-read" );
        const auto Content = Draft.readAll(); Draft.close();
        const auto Path = Manifest[ "archivePath" ].toString();
        QFile File( Path ); if( !File.open( QIODevice::ReadOnly ) ) return QStringLiteral( "archive-read" );
        const auto Bytes = File.readAll(); File.close();
        auto Entry = Manifest[ "entryPath" ].toString(); Entry.replace( QChar( '\\' ), QChar( '/' ) );
        while( Entry.startsWith( QChar( '/' ) ) ) Entry.remove( 0, 1 );
        try {
            using namespace hc::document::epub::detail;
            const Archive Zip( Bytes, {}, epub::InputLimit, ZipMode::General );
            const auto Name = Canonical( Entry );
            if( !Zip.Contains( Name ) ) return QStringLiteral( "archive-entry-missing" );
            // Someone else may have updated the archive since the edit began; never overwrite that.
            const auto Baseline = Manifest[ "baselineSha256" ].toString().toLatin1();
            if( !Baseline.isEmpty() && QCryptographicHash::hash( Zip.Read( Name ), QCryptographicHash::Sha256 ).toHex() != Baseline ) return QStringLiteral( "archive-entry-changed" );
            const auto Output = Zip.Rewrite( { { Name, Content } } );
            const Archive Check( Output, {}, epub::InputLimit, ZipMode::General );
            if( Check.Read( Name ) != Content ) return QStringLiteral( "archive-verify" );
            QSaveFile Target( Path ); Target.setDirectWriteFallback( false );
            if( !Target.open( QIODevice::WriteOnly ) || Target.write( Output ) != Output.size() || !Target.commit() ) return QStringLiteral( "archive-write" );
        } catch( const epub::detail::Rejected& Failure ) {
            return QStringLiteral( "archive-zip-" ) + Failure.code_;
        }
        QDir( Directory ).removeRecursively();
        return {};
    }
}
