#pragma once

/// 모듈
#include "scratchSession.hpp"

/// UIs

/// 기타 헤더파일

/// 고유 라이브러리
#include "UniqueLibs/nativeMaterializer.hpp"

/// 공통 라이브러리

/// 외부 라이브러리

/// 선언 헤더파일들

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QStringList>
#include <limits>
#include <qt_windows.h>

/// Start-up clean-up of temporary files a crash left behind. Called only by HyperEditor and HyperCommander,
/// shortly after they started and off the GUI thread; never from a DLL, which also loads into test and host
/// processes. Everything it removes is either owned by a dead process (lock files) or an older release's
/// temporary folder past a day.
namespace hc::scratch
{
    struct CleanupReport { int sessions_ = 0, legacy_ = 0, leases_ = 0; };

    namespace detail
    {
        // A process that cannot be opened for lack of rights still runs.
        inline bool ProcessAlive( qint64 Pid )
        {
            if( Pid <= 0 ) return false;
            const auto Process = OpenProcess( SYNCHRONIZE, FALSE, static_cast< DWORD >( Pid ) );
            if( !Process ) return GetLastError() == ERROR_ACCESS_DENIED;
            const auto Wait = WaitForSingleObject( Process, 0 ); CloseHandle( Process );
            return Wait == WAIT_TIMEOUT;
        }
        inline bool RemoveEmptyTree( const QString& Path )
        {
            for( const auto& Child : QDir( Path ).entryInfoList( QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden ) ) RemoveEmptyTree( Child.absoluteFilePath() );
            return QDir().rmdir( Path );   // only an empty folder goes
        }
    }

    /// Folders older releases created directly in %TEMP% without an owner marker; each name pattern below was
    /// produced only by HyperCommander, HyperEditor or HyperViewer. Removed once older than a day.
    inline int SweepLegacy( const QString& Temp, const QString& RstShadow, const QString& LocalData, const QDateTime& Now )
    {
        const auto Before = Now.addDays( -1 );
        int Removed = 0;
        const auto Sweep = [ & ]( const QString& Folder, const QString& Pattern, bool Directories, const std::function< bool( const QString& ) >& Allowed = {} ) {
            const QRegularExpression Name( QRegularExpression::anchoredPattern( Pattern ) );
            for( const auto& Info : QDir( Folder ).entryInfoList( ( Directories ? QDir::Dirs : QDir::Files ) | QDir::NoDotAndDotDot | QDir::Hidden ) ) {
                if( !Name.match( Info.fileName() ).hasMatch() || Info.lastModified() >= Before ) continue;
                if( Allowed && !Allowed( Info.fileName() ) ) continue;
                if( Directories ? QDir( Info.absoluteFilePath() ).removeRecursively() : QFile::remove( Info.absoluteFilePath() ) ) ++Removed;
            }
        };
        const QString Random = QStringLiteral( "[A-Za-z0-9]{6}" );
        for( const auto& Prefix : { QStringLiteral( "HyperCommander-epub-preview-" ), QStringLiteral( "hc-index-" ), QStringLiteral( "hc-document-search-" ),
                 QStringLiteral( "HyperEditor-" ), QStringLiteral( "HyperViewer-" ) } )
            Sweep( Temp, QRegularExpression::escape( Prefix ) + Random, true );
        Sweep( Temp, QStringLiteral( "hc-outcome-index-" ) + Random, false );
        // RST single-document previews named their folder after the owner's process id.
        Sweep( Temp, QStringLiteral( "mrst-virtual-\\d+-" ) + Random, true, []( const QString& Name ) {
            return !detail::ProcessAlive( Name.section( '-', 2, 2 ).toLongLong() );
        } );
        const auto Indexes = QDir( Temp ).filePath( QStringLiteral( "HyperCommander/TransientIndexes" ) );
        Sweep( Indexes, QStringLiteral( "session-" ) + Random, true );
        QDir().rmdir( Indexes );
        if( !RstShadow.isEmpty() ) Sweep( RstShadow, QStringLiteral( "preview-" ) + Random + QStringLiteral( "\\.shadow" ), false );
        // Render helpers without an application identity made empty cache folders at the top of %LOCALAPPDATA%.
        if( !LocalData.isEmpty() )
            for( const auto& Name : { QStringLiteral( "HyperEpubWeb" ), QStringLiteral( "hc-markdown-render" ), QStringLiteral( "hc-rst-preview" ) } )
                if( const auto Path = QDir( LocalData ).filePath( Name ); QFileInfo( Path ).isDir() && detail::RemoveEmptyTree( Path ) ) ++Removed;
        return Removed;
    }

    /// Lease copies (files from archives, remote folders, HWP originals) whose process ended: each parent's
    /// hc-materialized-v1 store removes a lease only when its owner.lock is free.
    inline int RecoverLeases( const QStringList& Parents )
    {
        int Removed = 0;
        for( const auto& Parent : Parents ) {
            if( Parent.isEmpty() || !QDir( Parent ).exists( QStringLiteral( "hc-materialized-v1" ) ) ) continue;
            hc::storage::MaterializationLimits Limits; Limits.maxActiveFiles_ = std::numeric_limits< std::size_t >::max();
            auto Store = hc::storage::MakeNativeMaterializer( QDir::toNativeSeparators( Parent ).toStdString(), Limits );
            if( !Store.HasValue() ) continue;
            Removed += int( Store.Value()->Recover().removed_ );
        }
        return Removed;
    }

    inline CleanupReport RunStartupCleanup( const QStringList& LeaseParents, const QString& RstShadow )
    {
        CleanupReport Report;
        Report.sessions_ = SweepAbandoned();
        Report.legacy_ = SweepLegacy( QDir::tempPath(), RstShadow, QStandardPaths::writableLocation( QStandardPaths::GenericDataLocation ), QDateTime::currentDateTime() );
        Report.leases_ = RecoverLeases( LeaseParents );
        return Report;
    }
}
