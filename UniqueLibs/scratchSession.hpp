#pragma once

/// 모듈

/// UIs

/// 기타 헤더파일

/// 고유 라이브러리

/// 공통 라이브러리

/// 외부 라이브러리

/// 선언 헤더파일들

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QLockFile>
#include <QRegularExpression>
#include <QUuid>
#include <memory>

/// Temporary files that are worthless once their process ends: %TEMP%\HyperCommander\scratch\s-<pid>-<id>\,
/// one folder per process (per module), with owner.lock held while it lives. The folder goes when the process
/// exits normally; what a crash leaves behind is removed by the next HyperEditor or HyperCommander start
/// (SweepAbandoned), never by a folder whose owner still runs.
namespace hc::scratch
{
    inline QString Base() { return QDir( QDir::tempPath() ).filePath( QStringLiteral( "HyperCommander/scratch" ) ); }

    class Session final
    {
    public:
        static Session& Current() { static Session Instance( Base() ); return Instance; }
        explicit Session( const QString& Root )
        {
            const auto Name = QStringLiteral( "s-%1-%2" ).arg( QCoreApplication::applicationPid() ).arg( QUuid::createUuid().toString( QUuid::Id128 ).left( 12 ) );
            const auto Path = QDir( Root ).filePath( Name );
            if( !QDir().mkpath( Path ) ) return;
            lock_ = std::make_unique< QLockFile >( QDir( Path ).filePath( QStringLiteral( "owner.lock" ) ) ); lock_->setStaleLockTime( 0 );
            if( !lock_->tryLock( 0 ) ) { lock_.reset(); QDir( Path ).removeRecursively(); return; }
            path_ = Path;
        }
        ~Session()
        {
            if( lock_ ) lock_->unlock();
            if( !path_.isEmpty() ) QDir( path_ ).removeRecursively();
        }
        Session( const Session& ) = delete;
        Session& operator=( const Session& ) = delete;
        bool IsValid() const { return !path_.isEmpty(); }
        QString Path() const { return path_; }
        /// A folder inside the session for one purpose (created on demand).
        QString Folder( const QString& Name ) const
        {
            const auto Path = QDir( path_.isEmpty() ? QDir::tempPath() : path_ ).filePath( Name );
            QDir().mkpath( Path );
            return Path;
        }
        /// A QTemporaryDir / QTemporaryFile template inside the session; %TEMP% when the session could not start.
        QString Template( const QString& Prefix ) const
        { return QDir( path_.isEmpty() ? QDir::tempPath() : path_ ).filePath( Prefix + QStringLiteral( "-XXXXXX" ) ); }
    private:
        QString path_;
        std::unique_ptr< QLockFile > lock_;
    };

    /// Removes session folders whose process ended: only s-<pid>-<id> folders whose owner.lock can be taken.
    /// A folder without its lock (still being created) is left alone for an hour.
    inline int SweepAbandoned( const QString& Root = Base() )
    {
        static const QRegularExpression Name( QStringLiteral( "^s-\\d+-[0-9a-f]{12}$" ) );
        int Removed = 0;
        for( const auto& Info : QDir( Root ).entryInfoList( QDir::Dirs | QDir::NoDotAndDotDot ) ) {
            if( !Name.match( Info.fileName() ).hasMatch() ) continue;
            const auto Lock = QDir( Info.absoluteFilePath() ).filePath( QStringLiteral( "owner.lock" ) );
            if( QFileInfo::exists( Lock ) ) {
                QLockFile Probe( Lock ); Probe.setStaleLockTime( 0 );
                if( !Probe.tryLock( 0 ) ) continue;   // its process still runs (this one included)
                Probe.unlock();
            } else if( Info.lastModified() > QDateTime::currentDateTime().addSecs( -3600 ) ) continue;
            if( QDir( Info.absoluteFilePath() ).removeRecursively() ) ++Removed;
        }
        return Removed;
    }
}
